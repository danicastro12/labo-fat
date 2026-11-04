/*
 * fat_fuse_ops.c
 *
 * FAT32 filesystem operations for FUSE (Filesystem in Userspace)
 */

#include "fat_fuse_ops.h"

#include "audit.h"
#include "fat_file.h"
#include "fat_filename_util.h"
#include "fat_fs_tree.h"
#include "fat_types.h"
#include "fat_util.h"
#include "fat_volume.h"
#include <alloca.h>
#include <asm-generic/errno-base.h>
#include <errno.h>
#include <gmodule.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include "restore.h"

/* Loads directory children from the volume into in-memory tree.
 * Calls function fat_file_read_children which returns
 * a list of files inside a GList. The children were read from the directory
 * entries in the cluster of the directory. This function iterates over the
 * list of children and adds them to the file tree.
 * This operation should be performed only once per directory, the first time
 * readdir is called.
 */
void fat_fuse_load_directory_children(fat_volume vol,
                                             fat_tree_node dir_node) {
    fat_file dir = fat_tree_get_file(dir_node);
    GList *children_list = fat_file_read_children(dir);

    DEBUG("Loading children for directory: %s", dir->filepath);
    DEBUG("  Found %d children", g_list_length(children_list));

    // Add children to tree. TODO handle duplicates
    for (GList *l = children_list; l != NULL; l = l->next) {
        fat_file child = (fat_file)l->data;
        DEBUG("  - Loading child: name='%s', filepath='%s', is_dir=%d",
              child->name, child->filepath, fat_file_is_directory(child));

        vol->file_tree =
            fat_tree_insert(vol->file_tree, dir_node, child);
    }
}

fat_volume fat_fuse_init(fat_volume vol) {
    // Initialize root directory (orphan directory without parent)
    fat_dir_entry root_entry =
        fat_file_init_direntry(true, strdup("/"), vol->root_dir_start_cluster);
    if (root_entry == NULL) {
        errno = ENOSPC;
        return NULL;
    }

    fat_file root_dir = calloc(1, sizeof(struct fat_file_s));
    if (root_dir == NULL) {
        free(root_entry);
        errno = ENOMEM;
        return NULL;
    }

    // Initialize root directory fields
    root_dir->filepath = strdup("/");
    root_dir->dentry = root_entry;
    root_dir->start_cluster = vol->root_dir_start_cluster;
    root_dir->table = vol->table;
    root_dir->dir.nentries = 0;
    root_dir->pos_in_parent = 0;
    root_dir->num_times_opened = 0;
    root_dir->children_read = 0;

    char *filename = build_filename(root_entry->base_name, root_entry->extension);
    if (filename == NULL) {
        free(root_entry);
        free(root_dir->filepath);
        free(root_dir);
        errno = ENOMEM;
        return NULL;
    }
    strncpy(root_dir->name, filename, MAX_FILENAME - 1);
    root_dir->name[MAX_FILENAME - 1] = '\0';
    free(filename);

    vol->file_tree = fat_tree_init();
    vol->file_tree = fat_tree_insert(vol->file_tree, NULL, root_dir);

    // Load the content of / to the tree
    fat_tree_node root_dir_node = fat_tree_node_search(vol->file_tree, "/");
    fat_fuse_load_directory_children(vol, root_dir_node);
    fat_tree_print_preorder(vol->file_tree);

    return vol;
}

/* Get file attributes (file descriptor version) */
int fat_fuse_fgetattr(const char *path, struct stat *stbuf,
                      struct fuse_file_info *fi) {
    fat_file file = (fat_file)fat_tree_get_file((fat_tree_node)fi->fh);
    fat_file_to_stbuf(file, stbuf);
    return 0;
}

/* Load the entire hierarchy of parent directories in the in-memory tree.
 * IMPORTANT: Asumes the rood directory / has been already loaded
 */
static int fat_fuse_load_path(const char *path) {
    fat_volume vol = get_fat_volume();
    size_t len = strlen(path);
    char *prefix = calloc(len + 1, sizeof(char));
    for (size_t i = 1; i <= len; i++) {
        if (path[i] != '/')
            continue;
        if (path[i] == '\0')
            break; // End of path
        // We found a path delimiter /, we take everything up to there
        memcpy(prefix, path, i);
        prefix[i] = '\0';

        fat_tree_node dir_node = fat_tree_node_search(vol->file_tree, prefix);
        if (dir_node == NULL) {
            // This doesn't happen because the parent was added in the
            // previous iteration.
            DEBUG("Directory %s not found in tree\n", prefix);
            errno = ENOENT;
            return -errno;
        }
        fat_file dir = fat_tree_get_file(dir_node);
        if (!fat_file_is_directory(dir)) {
            DEBUG("Directory %s is not a directory\n", prefix);
            errno = ENOTDIR;
            return -errno;
        }
        if (dir->children_read != 1) {
            fat_fuse_load_directory_children(vol, dir_node);
        }
    }
    free(prefix);
    return 0;
}

/* Get file attributes (path version) */
int fat_fuse_getattr(const char *path, struct stat *stbuf) {
    fat_volume vol;
    fat_file file;

    vol = get_fat_volume();

    // Check all parent directories in path are loaded in the tree
    fat_fuse_load_path(path);

    file = fat_tree_search(vol->file_tree, path);
    if (file == NULL) {
        errno = ENOENT;
        return -errno;
    }
    fat_file_to_stbuf(file, stbuf);
    return 0;
}

/* Open a file */
int fat_fuse_open(const char *path, struct fuse_file_info *fi) {
    fat_volume vol;
    fat_tree_node file_node;
    fat_file file;

    vol = get_fat_volume();
    file_node = fat_tree_node_search(vol->file_tree, path);
    if (!file_node)
        return -errno;
    file = fat_tree_get_file(file_node);
    if (fat_file_is_directory(file))
        return -EISDIR;
    fat_tree_inc_num_times_opened(file_node);
    fi->fh = (uintptr_t)file_node;
    return 0;
}

/* Open a directory */
int fat_fuse_opendir(const char *path, struct fuse_file_info *fi) {
    fat_volume vol = NULL;
    fat_tree_node file_node = NULL;
    fat_file file = NULL;

    vol = get_fat_volume();
    file_node = fat_tree_node_search(vol->file_tree, path);
    if (file_node == NULL) {
        return -errno;
    }
    file = fat_tree_get_file(file_node);
    if (!fat_file_is_directory(file)) {
        return -ENOTDIR;
    }
    fat_tree_inc_num_times_opened(file_node);
    fi->fh = (uintptr_t)file_node;
    return 0;
}

/* Add entries of a directory in @fi to @buf using @filler function. */
int fat_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                     off_t offset, struct fuse_file_info *fi) {
    fat_volume vol = get_fat_volume();
    errno = 0;
    fat_tree_node dir_node = (fat_tree_node)fi->fh;
    fat_file dir = fat_tree_get_file(dir_node);
    fat_file *children = NULL, *child = NULL;
    int error = 0;

    // Insert first two filenames (. and ..)
    if ((*filler)(buf, ".", NULL, 0) || (*filler)(buf, "..", NULL, 0)) {
        return -errno;
    }
    if (!fat_file_is_directory(dir)) {
        errno = ENOTDIR;
        return -errno;
    }
    if (dir->children_read != 1) {
        fat_fuse_load_directory_children(vol, dir_node);
        if (errno < 0) {
            return -errno;
        }
    }

    children = fat_tree_flatten_h_children(dir_node);
    child = children;

    DEBUG("Reading directory: %s", path);
    DEBUG("  Directory has %d entries", dir->dir.nentries);

    while (*child != NULL) {
        DEBUG("  - Entry: name='%s', filepath='%s', is_dir=%d, start_cluster=%u",
              (*child)->name,
              (*child)->filepath,
              fat_file_is_directory(*child),
              (*child)->start_cluster);

        if ((*child)->dentry) {
            DEBUG("    base_name='%.8s', extension='%.3s'",
                  (*child)->dentry->base_name,
                  (*child)->dentry->extension);
        }

        error = (*filler)(buf, (*child)->name, NULL, 0);
        if (error != 0) {
            return -errno;
        }
        child++;
    }
    return 0;
}

/* Read data from a file */
int fat_fuse_read(const char *path, char *buf, size_t size, off_t offset,
                  struct fuse_file_info *fi) {
    errno = 0;
    int bytes_read;
    fat_tree_node file_node = (fat_tree_node)fi->fh;
    fat_file file = fat_tree_get_file(file_node);
    fat_file parent = fat_tree_get_parent(file_node);

    bytes_read = fat_file_pread(file, buf, size, offset, parent);

    return bytes_read;
}

/* Write data from a file */
int fat_fuse_write(const char *path, const char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi) {
    fat_tree_node file_node = (fat_tree_node)fi->fh;
    fat_file file = fat_tree_get_file(file_node);
    fat_file parent = fat_tree_get_parent(file_node);

    if (size == 0)
        return 0; // Nothing to write
    if (offset > file->dentry->file_size)
        return -EOVERFLOW;
    if (audit_is_protected_path(path))
        return -EPERM; //proteccion para no escribir en event.bin

    if(is_restore_path(path)){
     //buf seria nuestro path_to_recovery
      fat_volume vol = get_fat_volume();
      if(restore_file(vol->table, buf) != 0){
         return -EBUSY;
      };
    }

    int result = fat_file_pwrite(file, buf, size, offset, parent);

    //log
    if (result > 0)
        audit_log_write(path, file, size, offset);

    return result;
}

/* Close a file */
int fat_fuse_release(const char *path, struct fuse_file_info *fi) {
    fat_tree_node file = (fat_tree_node)fi->fh;
    fat_tree_dec_num_times_opened(file);
    return 0;
}

/* Close a directory */
int fat_fuse_releasedir(const char *path, struct fuse_file_info *fi) {
    fat_tree_node file = (fat_tree_node)fi->fh;
    fat_tree_dec_num_times_opened(file);
    return 0;
}

int fat_fuse_mkdir(const char *path, mode_t mode) {
    errno = 0;
    fat_volume vol = NULL;
    fat_file parent = NULL, new_file = NULL;
    fat_tree_node parent_node = NULL;

    // The system has already checked the path does not exist. We get the parent
    vol = get_fat_volume();
    parent_node = fat_tree_node_search(vol->file_tree, dirname(strdup(path)));
    if (parent_node == NULL) {
        errno = ENOENT;
        return -errno;
    }
    parent = fat_tree_get_file(parent_node);
    if (!fat_file_is_directory(parent)) {
        fat_error("Error! Parent is not directory\n");
        errno = ENOTDIR;
        return -errno;
    }

    // init child
    new_file = fat_file_init(vol->table, true, strdup(path));
    if (errno != 0) {
        return -errno;
    }
    // insert to directory tree representation
    vol->file_tree = fat_tree_insert(vol->file_tree, parent_node, new_file);
    // write file in parent's entry (disk)
    new_file->pos_in_parent = parent->dir.nentries;

    //log
    int errno_restore = errno;  //ver una forma mas bonita de hacer eso
    if(errno_restore == 0)
      audit_log_dir(path,OP_MKDIR,new_file);
    errno = errno_restore;

    return -errno;
}

/* Creates a new file in @path. @mode and @dev are ignored. */
int fat_fuse_mknod(const char *path, mode_t mode, dev_t dev) {
    errno = 0;
    fat_volume vol;
    fat_file parent, new_file;
    fat_tree_node parent_node;

    // The system has already checked the path does not exist. We get the parent
    vol = get_fat_volume();
    parent_node = fat_tree_node_search(vol->file_tree, dirname(strdup(path)));
    if (parent_node == NULL) {
        errno = ENOENT;
        return -errno;
    }
    parent = fat_tree_get_file(parent_node);
    if (!fat_file_is_directory(parent)) {
        fat_error("Error! Parent is not directory\n");
        errno = ENOTDIR;
        return -errno;
    }
    new_file = fat_file_init(vol->table, false, strdup(path));
    if (errno < 0) {
        return -errno;
    }
    // insert to directory tree representation
    vol->file_tree = fat_tree_insert(vol->file_tree, parent_node, new_file);
    // Write dentry in parent cluster
    new_file->pos_in_parent = parent->dir.nentries;
    fat_file_add_child_direntry(parent, new_file->dentry, new_file->pos_in_parent);

    //log
    int errno_restore = errno;  //ver una forma mas bonita de hacer eso
    if(errno_restore == 0)
        audit_log_mknod(path, new_file);
    errno = errno_restore;
    return -errno;
}

int fat_fuse_utime(const char *path, struct utimbuf *buf) {
    errno = 0;
    fat_file parent = NULL;
    fat_volume vol = get_fat_volume();
    fat_tree_node file_node = fat_tree_node_search(vol->file_tree, path);
    if (file_node == NULL || errno != 0) {
        errno = ENOENT;
        return -errno;
    }
    parent = fat_tree_get_parent(file_node);
    if (parent == NULL || errno != 0) {
        DEBUG("WARNING: Setting time for parent ignored");
        return 0; // We do nothing, no utime for parent
    }
    fat_utime(fat_tree_get_file(file_node), parent, buf);
    return -errno;
}

/* Shortens the file at the given offset.*/
int fat_fuse_truncate(const char *path, off_t offset) {
    errno = 0;
    //proteccion para no deletar .audit o events.bin
    if(audit_is_protected_path(path))
        return -EPERM;
    fat_volume vol = get_fat_volume();
    fat_file file = NULL, parent = NULL;
    fat_tree_node file_node = fat_tree_node_search(vol->file_tree, path);
    if (file_node == NULL || errno != 0) {
        errno = ENOENT;
        return -errno;
    }
    file = fat_tree_get_file(file_node);
    if (fat_file_is_directory(file))
        return -EISDIR;

    parent = fat_tree_get_parent(file_node);
    fat_tree_inc_num_times_opened(file_node);
    fat_file_truncate(file, offset, parent);
    return -errno;
}

/* Removes a file. */
int fat_fuse_unlink(const char *path) {
    errno = 0;
    //proteccion para no deletar .audit o events.bin
    if(audit_is_protected_path(path))
        return -EPERM;

    fat_volume vol = get_fat_volume();
    fat_file file = NULL, parent = NULL;
    // Search file in tree to get parent.
    fat_tree_node file_node = fat_tree_node_search(vol->file_tree, path);
    if (file_node == NULL || errno != 0) {
        errno = ENOENT;
        return -errno;
    }
    file = fat_tree_get_file(file_node);
    if (fat_file_is_directory(file))
        return -EISDIR;
    parent = fat_tree_get_parent(file_node);

    //hago el log antes de perder las referencias
    int errno_restore = errno;
    audit_log_unlink(path, file);
    errno = errno_restore;

    // Set clusters free and remove entry from dir
    fat_file_unlink(file, parent);

    // Remove file from tree. This destroys the file struct
    vol->file_tree = fat_tree_delete(vol->file_tree, path);
    return -errno;
}

/* Removes a directory. */
int fat_fuse_rmdir(const char *path) {
    errno = 0;
    //proteccion para no deletar .audit o events.bin
    if(audit_is_protected_path(path))
        return -EPERM;
    fat_volume vol = get_fat_volume();
    fat_file file = NULL, parent = NULL;
    // Search file in tree to get parent.
    fat_tree_node file_node = fat_tree_node_search(vol->file_tree, path);
    if (file_node == NULL || errno != 0) {
        errno = ENOENT;
        return -errno;
    }
    file = fat_tree_get_file(file_node);
    if (!fat_file_is_directory(file))
        return -ENOTDIR;
    parent = fat_tree_get_parent(file_node);

    int errno_restore = errno;  //ver una forma mas bonita de hacer eso
    //loggeo aca porque en la proxima funcion se le asinga otro valor al start_cluster
    if(errno_restore == 0){
      audit_log_dir(path,OP_RMDIR,file);
      audit_log_unlink(path,file);
    }
    errno = errno_restore;

    // Set clusters free and remove entry from dir
    fat_file_unlink(file, parent);

    // Remove file from tree. This destroys the file struct
    vol->file_tree = fat_tree_delete(vol->file_tree, path);
    return -errno;
}

/* Renames/moves a file or directory from @oldpath to @newpath. */
int fat_fuse_rename(const char *oldpath, const char *newpath) {
    errno = 0;
    //proteccion para no deletar .audit o events.bin
    if(audit_is_protected_path(oldpath) || audit_is_protected_path(newpath))
        return -EPERM;
    fat_volume vol = get_fat_volume();
    fat_file file = NULL;
    fat_file old_parent_file = NULL, new_parent_file = NULL;
    fat_tree_node file_node, new_parent_node;

    // Validation: i) Does the source file exist?
    file_node = fat_tree_node_search(vol->file_tree, oldpath);
    if (file_node == NULL || errno != 0) {
        errno = ENOENT;
        return -errno;
    }
    file = fat_tree_get_file(file_node);

    // Validation: ii) Can't rename root directory
    if (strcmp(oldpath, "/") == 0) {
        errno = EINVAL;
        return -errno;
    }

    // Validation: iii) Get new parent directory and verify it exists
    char *new_parent_path = dirname(strdup(newpath));
    new_parent_node = fat_tree_node_search(vol->file_tree, new_parent_path);
    if (new_parent_node == NULL) {
        errno = ENOENT;
        return -errno;
    }
    new_parent_file = fat_tree_get_file(new_parent_node);
    if (!fat_file_is_directory(new_parent_file)) {
        errno = ENOTDIR;
        return -errno;
    }

    // Validation: iv) Check if destination already exists
    fat_tree_node dest_node = fat_tree_node_search(vol->file_tree, newpath);
    if (dest_node != NULL) {
        errno = EEXIST;
        return -errno;
    }

    // Validation: v) Get old parent file reference
    old_parent_file = fat_tree_get_parent(file_node);
    if (old_parent_file == NULL) {
        errno = ENOENT;
        return -errno;
    }

    // vi) Make a copy of the directory entry to mark as deleted
    fat_dir_entry old_dentry_copy = calloc(1, sizeof(struct fat_dir_entry_s));
    if (old_dentry_copy == NULL) {
        errno = ENOMEM;
        return -errno;
    }
    memcpy(old_dentry_copy, file->dentry, sizeof(struct fat_dir_entry_s));

    // Mark the old entry as deleted in the old parent's directory
    fat_file_remove_child_direntry(old_parent_file, old_dentry_copy, file->pos_in_parent);

    // Free the temporary copy
    free(old_dentry_copy);

    // vii) Update the original directory entry with new name
    char *new_filename_base = basename(strdup(newpath));
    split_filepath_to_filename(new_filename_base, file->dentry->base_name,
                               file->dentry->extension);
    char *new_filename = build_filename(file->dentry->base_name, file->dentry->extension);
    if (new_filename == NULL) {
        errno = ENOMEM;
        return -errno;
    }
    strncpy(file->name, new_filename, MAX_FILENAME - 1);
    file->name[MAX_FILENAME - 1] = '\0';
    free(new_filename);

    // Update the filepath
    free(file->filepath);
    file->filepath = strdup(newpath);

    // viii) Add the entry to the new parent directory (or same if just renaming)
    file->pos_in_parent = new_parent_file->dir.nentries;
    fat_file_add_child_direntry(new_parent_file, file->dentry, file->pos_in_parent);

    // ix) Update the tree: remove from old path and insert at new path
    vol->file_tree = fat_tree_delete(vol->file_tree, oldpath);
    vol->file_tree = fat_tree_insert(vol->file_tree, new_parent_node, file);

    return -errno;
}

/* TODO: Implement ioctl handler for file restoration
 *
 * THIS IS ONLY A POSSIBLE IMPLEMENTATION USING IOCTL
 *
 * This function should handle custom ioctl commands for operations like
 * restoring deleted files.
 *
 * Example implementation:
 *   - Define a custom ioctl command (e.g., #define FAT_IOCTL_RESTORE 0xF001)
 *   - Extract the file path from the data parameter
 *   - Call your audit_restore_file() function
 *   - Return appropriate error codes
 *
 * Note: ioctl support may vary across FUSE implementations and platforms.
 * See CONSIGNA.md for alternative approaches.
 */
int fat_fuse_ioctl(const char *path, int cmd, void *arg,
                   struct fuse_file_info *fi, unsigned int flags, void *data) {
    (void)path;
    (void)cmd;
    (void)arg;
    (void)fi;
    (void)flags;
    (void)data;

    // TODO: Implement ioctl handling here
    return -ENOSYS;  // Not implemented
}

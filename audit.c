#include "fat_file.h"
#include "audit.h"
#include "fat_fs_tree.h"
#include "fat_types.h"
#include "fat_util.h"
#include "fat_volume.h"
#include <alloca.h>
#include <errno.h>
#include <gmodule.h>
#include <libgen.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>


int fat_fuse_internal_mkdir(const char *path,fat_volume vol) {
    errno = 0;
    if(vol == NULL){
      return -EINVAL;
    }
    fat_file parent = NULL, new_file = NULL;
    fat_tree_node parent_node = NULL;

    // The system has already checked the path does not exist. We get the parent
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
    printf("PRINT DESDE INTERNAL");
    fat_tree_print_preorder(vol->file_tree);
    // write file in parent's entry (disk)
    new_file->pos_in_parent = parent->dir.nentries;
    fat_file_add_child_direntry(parent, new_file->dentry, new_file->pos_in_parent);

    return -errno;
}

int fat_fuse_event_file(fat_volume vol){
  errno = 0;
  fat_file parent =NULL, new_file = NULL; 
  fat_tree_node parent_node = NULL;

  parent_node = fat_tree_node_search(vol->file_tree, AUDIT_DIR_PATH);

  //Chequeo si existe la carpeta padre /.audit
  if(parent_node == NULL){
    return -1;
  }

  //Chequeo si /.audit es un directorio(carpeta)
  parent = fat_tree_get_file(parent_node);
  if(!fat_file_is_directory(parent)){
    return -1;
  }

  new_file = fat_file_init(vol->table,false, strdup(AUDIT_FILE_PATH));
  if (errno != 0) {
    return -errno;
  }

  vol->file_tree = fat_tree_insert(vol->file_tree, parent_node,new_file);
  new_file->pos_in_parent = parent->dir.nentries;
  fat_file_add_child_direntry(parent, new_file->dentry, new_file->pos_in_parent);
 
  return -errno;
}

int audit_init(fat_volume vol){
    errno = 0;
    if (vol == NULL){
        return -EINVAL;
    }
    if (vol->mount_flags & FAT_MOUNT_FLAG_READONLY){
        return errno; //return sin hacer nada, no tiene permissos de escritura
    }
    
    bool exist_dir = false;
    bool exist_file = false;
    exist_dir = fat_tree_node_search(vol->file_tree, AUDIT_DIR_PATH);

    if(!exist_dir){
      int res_dir = fat_fuse_internal_mkdir(AUDIT_DIR_PATH, vol);
      if(res_dir != 0){
        return res_dir;
      }
    }

    exist_file = fat_tree_node_search(vol->file_tree, AUDIT_FILE_PATH);
    if(!exist_file){
      int res_file = fat_fuse_event_file(vol);
      if(res_file != 0){
        return res_file;
      }
    }
    return 0;
}


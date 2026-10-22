/*
 * fat_file.c
 *
 * Functions to operate over files and directories.
 */

#include <alloca.h>
#include <errno.h>
#include <gmodule.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "fat_file.h"
#include "fat_filename_util.h"
#include "fat_table.h"
#include "fat_util.h"

/* Directory entry special markers */
#define DENTRY_END_OF_DIR 0x00
#define DENTRY_DOT_ENTRY 0x2E

// static void fat_file_print_dentry(fat_dir_entry dentry) {
//     printf("\t Dentry: \n");
//     printf("\t Basename: %.*s\n", 8, dentry->base_name);
//     printf("\t Extension: %.*s\n", 3, dentry->extension);
//     printf("\t Attributes: %x\n", dentry->attribs);
// }

static int write_dir_entry(fat_file parent, fat_dir_entry dentry, u32 pos_in_parent);

/* Fills fields last_modified_time, last_modified_date, last_access_date,
 * create_date and create_time of a *in disk* file directory entry @dir_entry,
 * with the current time.
 * If @fill_creation or @fill_modified are false, it does not fill the
 * corresponding fields.
 * PRE: @fill_create => @fill_modified
 */
static int fill_dentry_time_now(fat_dir_entry dir_entry, bool fill_create,
                                bool fill_modified) {
    int ret = 0;
    le16 entry_date = 0, entry_time = 0;
    time_t now = time(NULL);

    if (!(!fill_create || fill_modified))
        return -1;
    if (fill_modified) {
        ret = fill_time(&entry_date, &entry_time, now);
        dir_entry->last_modified_date = entry_date;
        dir_entry->last_modified_time = entry_time;
        dir_entry->last_access_date = entry_date;
    } else {
        ret = fill_time(&entry_date, NULL, now);
        dir_entry->last_access_date = entry_date;
    }
    if (ret != 0)
        return ret;
    if (fill_create) {
        dir_entry->create_date = dir_entry->last_modified_date;
        dir_entry->create_time = dir_entry->last_modified_time;
    }
    return ret;
}

/* Set the information for first cluster in the directory entry @dir_entry.
 * Copied from libfat. */
static int set_first_cluster(fat_dir_entry dir_entry, u32 cluster) {
    if (dir_entry == NULL)
        return -1;
    cluster = cpu_to_le32(cluster); // cluster is now little endian

    char *src = (char *)&cluster;
    char *dst = (char *)&(dir_entry->start_cluster_low);

    dst[0] = src[0];
    dst[1] = src[1];

    dst = (char *)&(dir_entry->fat32_start_cluster_high);
    dst[0] = src[2];
    dst[1] = src[3];

    return 0;
}

/* Returns the start cluster from the @disk_entry */
static u32 file_start_cluster(fat_dir_entry disk_dentry) {
    u32 start_cluster;
    start_cluster = le16_to_cpu(disk_dentry->start_cluster_low);

    // FAT32 uses a 32-bit start cluster field, but it's split into
    // two different 16-bit locations on disk.
    start_cluster |= (u32)le16_to_cpu(disk_dentry->fat32_start_cluster_high)
                     << 16;
    return start_cluster;
}

/********************* INITIALIZERS *********************/

/* Returns an new directory entry with default values.
 * Values to fill depending on fat_file: start_cluster
 */
fat_dir_entry fat_file_init_direntry(bool is_dir, char *filepath,
                                     u32 start_cluster) {
    fat_dir_entry new_entry = calloc(1, sizeof(struct fat_dir_entry_s));
    if (new_entry == NULL) {
        errno = ENOSPC;
        return NULL;
    }
    // Calculate filename and extension. Save into disk entry structure
    split_filepath_to_filename(basename(strdup(filepath)), new_entry->base_name,
                               new_entry->extension);
    if (is_dir) {
        new_entry->attribs = FILE_ATTRIBUTE_DIRECTORY;
    } else {
        new_entry->attribs = FILE_ATTRIBUTE_ARCHIVE;
    }
    new_entry->reserved = 0;
    new_entry->create_time_fine_res = 0;
    fill_dentry_time_now(new_entry, true, true); // ignore error
    set_first_cluster(new_entry, start_cluster);
    new_entry->file_size = 0;
    return new_entry;
}

/* Returns an new directory entry with default values.
 * Values to fill depending on fat_file: start_cluster, pos_in_dir
 */
static fat_dir_entry init_direntry_from_buff(fat_dir_entry buff) {
    fat_dir_entry new_entry = calloc(1, sizeof(struct fat_dir_entry_s));
    if (new_entry == NULL) {
        errno = ENOSPC;
        return NULL;
    }
    memcpy(new_entry, buff, sizeof(struct fat_dir_entry_s));

    return new_entry;
}

/* Creates a fat_file from the information contained in @dentry.
 * @parent can't be None, since root directory does not have a dentry.*/
static fat_file init_file_from_dentry(fat_dir_entry dentry, fat_file parent) {
    fat_file new_file = NULL;
    bool is_dir = (dentry->attribs & FILE_ATTRIBUTE_DIRECTORY) != 0;

    new_file = calloc(1, sizeof(struct fat_file_s));
    if (new_file == NULL) {
        errno = ENOSPC;
        return NULL;
    }
    new_file->start_cluster = file_start_cluster(dentry);
    new_file->dentry = dentry;
    new_file->table = parent->table;
    char *filename = build_filename(dentry->base_name, dentry->extension);
    if (filename == NULL) {
        free(new_file);
        errno = ENOMEM;
        return NULL;
    }
    strncpy(new_file->name, filename, MAX_FILENAME - 1);
    new_file->name[MAX_FILENAME - 1] = '\0';
    new_file->filepath = build_filepath_from_name(parent->filepath, filename);
    free(filename);
    if (new_file->filepath == NULL) {
        free(new_file);
        errno = ENOMEM;
        return NULL;
    }
    if (is_dir) {
        new_file->dir.nentries = 0;
    } else {
        new_file->file.num_clusters = 0;
    }
    new_file->pos_in_parent = parent->dir.nentries;
    new_file->num_times_opened = 0;
    new_file->children_read = 0;
    return new_file;
}

fat_file fat_file_init(fat_table table, bool is_dir, char *filepath) {
    fat_file new_file = calloc(1, sizeof(struct fat_file_s));
    if (new_file == NULL) {
        return NULL;
    }
    new_file->filepath = filepath;
    new_file->table = table;
    if (is_dir) {
        new_file->dir.nentries = 0;
    } else {
        new_file->file.num_clusters = 0;
    }
    new_file->pos_in_parent = 0;
    new_file->num_times_opened = 0;
    new_file->children_read = 0;
    new_file->dentry = NULL;

    u32 start_cluster = fat_table_get_next_free_cluster(table);
    if (fat_table_is_cluster_used(table, start_cluster)) {
        DEBUG("Failed to initialize '%s': cluster %u marked as free but is in use",
              filepath, start_cluster);
        errno = ENOSPC;
        fat_file_destroy(new_file);
        return NULL;
    }
    new_file->dentry = fat_file_init_direntry(is_dir, filepath, start_cluster);
    if (new_file->dentry == NULL) {
        DEBUG("Failed to create directory entry for '%s' at cluster %u",
              filepath, start_cluster);
        fat_file_destroy(new_file);
        return NULL;
    }

    char *filename = build_filename(new_file->dentry->base_name, new_file->dentry->extension);
    if (filename == NULL) {
        fat_file_destroy(new_file);
        errno = ENOMEM;
        return NULL;
    }
    strncpy(new_file->name, filename, MAX_FILENAME - 1);
    new_file->name[MAX_FILENAME - 1] = '\0';
    free(filename);
    new_file->start_cluster = file_start_cluster(new_file->dentry);

    int error = fat_table_set_next_cluster(table, start_cluster,
                                           FAT_CLUSTER_END_OF_CHAIN_MAX);
    if (error != 0) {
        fat_file_destroy(new_file);
        return NULL;
    }
    return new_file;
}

/* Frees memory allocated for the fat_file_s structure. */
void fat_file_destroy(fat_file file) {
    free(file->filepath);
    free(file->dentry);
    free(file);
}

int fat_file_cmp(fat_file file1, fat_file file2) {
    return strcmp(file1->filepath, file2->filepath);
}

int fat_file_cmp_path(fat_file file1, char *filepath) {
    return strcmp(file1->filepath, filepath);
}

/********************* FILE METADATA *********************/

inline bool fat_file_is_directory(const fat_file file) {
    if (file->dentry == NULL) {
        return true; // root file
    }
    return (file->dentry->attribs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void fat_file_inc_num_times_opened(fat_file file) {
    if (file != NULL) {
        file->num_times_opened++;
    }
}

void fat_file_dec_num_times_opened(fat_file file) {
    if (file != NULL) {
        file->num_times_opened--;
    }
}

void fat_file_to_stbuf(fat_file file, struct stat *stbuf) {
    memset(stbuf, 0, sizeof(*stbuf));
    stbuf->st_nlink = 1;
    if (fat_file_is_directory(file)) {
        stbuf->st_mode |= S_IFDIR;
    } else {
        stbuf->st_mode |= S_IFREG;
    }
    if (file->dentry->attribs & FILE_ATTRIBUTE_READONLY) {
        stbuf->st_mode |= 0555;
    } else {
        stbuf->st_mode |= 0777;
    }
    stbuf->st_size = file->dentry->file_size;
    stbuf->st_blocks =
        fat_table_get_clusters_for_size(file->table, stbuf->st_size);
    stbuf->st_blksize = fat_table_bytes_per_cluster(file->table);
    stbuf->st_ctime = time_to_unix_time(file->dentry->create_date, // 0);
                                        file->dentry->create_time);
    stbuf->st_atime = time_to_unix_time(file->dentry->last_access_date, 0);
    stbuf->st_mtime = time_to_unix_time(file->dentry->last_modified_date, // 0);
                                        file->dentry->last_modified_time);
}

/********************* DIRECTORY ENTRY METADATA *********************/

/* Writes the @file's directory entry into the @parent's cluster.*/
static int write_dir_entry(fat_file parent, fat_dir_entry dentry,
                           u32 pos_in_parent) {
    // Calculate the starting position of the parent's cluster
    off_t parent_offset =
        fat_table_cluster_offset(parent->table, parent->start_cluster);

    // Check the position of the entry in the parent's cluster is valid
    u32 chunk_size = fat_table_bytes_per_cluster(parent->table);
    size_t entry_size = sizeof(struct fat_dir_entry_s);
    if (chunk_size <= pos_in_parent * entry_size) {
        // TODO we should add a new cluster to the directory.
        DEBUG("Cannot add entry to directory '%s': parent directory is full (%u entries)",
              parent->name, pos_in_parent);
        return -1;
    }
    DEBUG("Writing directory entry in '%s' at position %u",
          parent->name, pos_in_parent);

    // Calculate the position of the directory entry
    off_t entry_offset = (off_t)(pos_in_parent * entry_size) + parent_offset;
    ssize_t written_bytes =
        pwrite(parent->table->fd, dentry, entry_size, entry_offset);
    if (written_bytes < entry_size) {
        DEBUG("Failed to write directory entry: wrote %zd of %zu bytes at offset %lld",
              written_bytes, entry_size, (long long)entry_offset);
        return -1;
    }
    return 0;
}

void fat_utime(fat_file file, fat_file parent, const struct utimbuf *buf) {
    // Adapted from libfat
    le16 accdate, acctime, moddate, modtime;
    // All in little endians!
    fill_time(&accdate, &acctime, buf->actime);
    fill_time(&moddate, &modtime, buf->modtime);
    file->dentry->last_access_date = accdate;
    file->dentry->last_modified_date = moddate;
    file->dentry->last_modified_time = modtime;
    write_dir_entry(parent, file->dentry, file->pos_in_parent);
}

/********************* DIRECTORY FUNCTIONS *********************/

void fat_file_add_child_direntry(fat_file parent, fat_dir_entry child_dentry,
                                  u32 pos_in_parent) {
    DEBUG("Adding child entry to '%s' at position %u", parent->name, pos_in_parent);
    // FIXME this would be more efficient if we used the first free slot on the
    // cluster.
    write_dir_entry(parent, child_dentry, pos_in_parent);
    parent->dir.nentries++;
}

void fat_file_remove_child_direntry(fat_file parent, fat_dir_entry child_dentry,
                                     u32 pos_in_parent) {
    // Mark file to be deleted
    child_dentry->base_name[0] = FILE_FOR_DELETION;
    write_dir_entry(parent, child_dentry, pos_in_parent);
}

/* Returns %true iff the given FAT on-disk directory entry is the special
 * end-of-directory entry. */
static bool is_end_of_directory(const fat_dir_entry disk_dentry) {
    return disk_dentry->base_name[0] == DENTRY_END_OF_DIR;
}

/* Returns %true iff the filesystem driver should ignore the given directory
 * entry due to having invalid attributes or an invalid name. */
static bool ignore_dentry(const fat_dir_entry disk_dentry) {
    // Note: VFAT entries have FILE_ATTRIBUTE_VOLUME set, so they will be
    // correctly ignored by this long-name unaware code.
    return (disk_dentry->attribs & (FILE_ATTRIBUTE_VOLUME)) ||
           !file_basename_valid(disk_dentry->base_name) ||
           !file_extension_valid(disk_dentry->extension);
}

/* Fills @elems with the fat_dir_entry that's read form @buffer, and
 * updates @dir to mark the children have been read. @end_ptr is used
 * to mark the end of the @buffer.
 * @dir can't be NULL, since root directory does not have a dentry.
 */
static void parse_dir_entries_in_cluster(u8 *buffer, fat_dir_entry end_ptr,
                                         fat_file dir, GList **elems) {
    fat_dir_entry disk_dentry_ptr = NULL;
    u32 dir_entries_processed = 0;
    for (disk_dentry_ptr = (fat_dir_entry)buffer; disk_dentry_ptr <= end_ptr;
         disk_dentry_ptr++, dir_entries_processed++) {

        dir->dir.nentries = dir_entries_processed;

        if (is_end_of_directory(disk_dentry_ptr)) {
            dir->children_read = 1;
            break;
        }
        if (ignore_dentry(disk_dentry_ptr)) {
            continue;
        }
        // Create and fill new child structure
        fat_dir_entry new_entry = init_direntry_from_buff(disk_dentry_ptr);
        fat_file child = init_file_from_dentry(new_entry, dir);
        (*elems) = g_list_append((*elems), child);
    }
}

GList *fat_file_read_children(fat_file dir) {
    u32 bytes_per_cluster = 0, cur_cluster = 0;
    off_t cur_offset = 0;
    u8 *buf = NULL;
    GList *entry_list = NULL;

    DEBUG("Reading children of \"%s\"", dir->filepath);
    bytes_per_cluster = fat_table_bytes_per_cluster(dir->table);
    cur_cluster = dir->start_cluster;
    if (!fat_table_is_valid_cluster_number(dir->table, cur_cluster)) {
        fat_error("Cluster number %u is invalid", cur_cluster);
        errno = EIO;
        return NULL;
    }
    cur_offset = fat_table_cluster_offset(dir->table, cur_cluster);

    // This buffer will contain the content of the directory cluster
    buf = alloca(bytes_per_cluster);
    while (fat_table_cluster_is_valid(cur_cluster)) {
        fat_dir_entry end_ptr;
        end_ptr = (fat_dir_entry)(buf + bytes_per_cluster) - 1;
        if (full_pread(dir->table->fd, buf, bytes_per_cluster, cur_offset) !=
            bytes_per_cluster) {
            errno = EIO;
            return NULL;
        }
        parse_dir_entries_in_cluster(buf, end_ptr, dir, &entry_list);
        cur_cluster = fat_table_read_cluster_value(dir->table, cur_cluster);
        cur_offset = fat_table_cluster_offset(dir->table, cur_cluster);
    }
    dir->children_read = 1;
    return entry_list;
}

/********************* READ/WRITE OPERATIONS *********************/

ssize_t fat_file_pread(fat_file file, void *buf, size_t size, off_t offset,
                       fat_file parent) {
    if (offset > file->dentry->file_size) {
        errno = EOVERFLOW;
        return 0;
    }
    size = min(size, file->dentry->file_size - offset);
    if (size == 0) {
        return 0;
    }
    DEBUG("Reading %zu bytes from '%s' at offset %lld (file_size=%u)",
          size, file->filepath, (long long)offset, file->dentry->file_size);
    off_t cluster_off;
    ssize_t bytes_read = 0, bytes_remaining = size, bytes_to_read_cluster = 0;
    u32 cluster =
        fat_table_seek_cluster(file->table, file->start_cluster, offset);
    if (cluster == 0) {
        return 0;
    }

    while (bytes_remaining > 0) { // There are still bytes to read
        DEBUG("Reading from cluster %u (%zu bytes remaining)", cluster, bytes_remaining);
        if (!fat_table_cluster_is_valid(cluster)) {
            break;
        }
        bytes_to_read_cluster = fat_table_get_cluster_remaining_bytes(
            file->table, bytes_remaining, offset);
        cluster_off = fat_table_cluster_offset(file->table, cluster) +
                      fat_table_mask_offset(offset, file->table);
        bytes_read = full_pread(file->table->fd, buf, bytes_to_read_cluster,
                                cluster_off);
        if (bytes_read != bytes_to_read_cluster) {
            break;
        }
        buf += bytes_read; // Move pointer
        offset += bytes_read;
        bytes_remaining -= bytes_read;
        cluster = fat_table_read_cluster_value(file->table, cluster);
    }
    fill_dentry_time_now(file->dentry, false, false);
    write_dir_entry(parent, file->dentry, file->pos_in_parent);
    return size - bytes_remaining;
}

void fat_file_truncate(fat_file file, off_t offset, fat_file parent) {
    u32 new_num_clusters = 0, current_num_clusters = 0;
    u32 last_cluster = 0, next_cluster = 0;

    current_num_clusters = max(1, fat_table_get_clusters_for_size(
                                      file->table, file->dentry->file_size));
    new_num_clusters =
        max(1, fat_table_get_clusters_for_size(file->table, offset));

    // Calculate how many clusters to remove
    if (offset > file->dentry->file_size ||
        new_num_clusters >= current_num_clusters) {
        return; // Nothing to truncate
    }
    // TODO [optional]
    // If the file size is smaller than length, bytes between the old and
    // new lengths are read as zeros.
    last_cluster =
        fat_table_seek_cluster(file->table, file->start_cluster, offset);
    if (errno != 0) {
        return;
    }

    next_cluster = fat_table_read_cluster_value(file->table, last_cluster);
    // Mark current cluster as the last one
    int error = fat_table_set_next_cluster(file->table, last_cluster,
                                           FAT_CLUSTER_END_OF_CHAIN_MAX);
    last_cluster = next_cluster;
    if (error != 0) {
        return;
    }
    // Mark following clusters as not used
    while (fat_table_cluster_is_valid(last_cluster)) {
        // If there was an error, we continue with the function.
        next_cluster = fat_table_read_cluster_value(file->table, last_cluster);
        error = fat_table_set_next_cluster(file->table, last_cluster,
                                           FAT_CLUSTER_FREE);
        if (error != 0) {
            return;
        }
        last_cluster = next_cluster;
    }

    // Update entrance in directory
    file->dentry->file_size = offset; // Overwrite with new size
    fill_dentry_time_now(file->dentry, false, true);
    write_dir_entry(parent, file->dentry, file->pos_in_parent);
}

/* ####################### */
/* Returns the next cluster in the file, or adds a new one if EOC is reach.
 * Returns 0 if no free cluster can be found, or the FAT table can't be written
 * FIXME this could be a fat_table operation.
 */
static u32 next_or_new_cluster(fat_file file, u32 last_cluster) {
    int error = 0;
    u32 next = fat_table_read_cluster_value(file->table, last_cluster);
    if (fat_table_cluster_is_EOC(next)) {
        DEBUG("Allocating new cluster for '%s' (current last cluster: %u)",
              file->filepath, last_cluster);
        next = fat_table_get_next_free_cluster(file->table);
        if (fat_table_cluster_is_EOC(next)) {
            return 0;
        }
        error = fat_table_set_next_cluster(file->table, last_cluster, next);
        error = fat_table_set_next_cluster(file->table, next,
                                           FAT_CLUSTER_END_OF_CHAIN_MAX);
        if (error != 0) {
            return 0;
        }
    }
    return next;
}

ssize_t fat_file_pwrite(fat_file file, const void *buf, size_t size,
                        off_t offset, fat_file parent) {
    u32 cluster = 0;
    ssize_t bytes_written_cluster = 0, bytes_remaining = size;
    ssize_t bytes_to_write_cluster = 0;
    off_t original_offset = offset, cluster_off = 0, starting_point = 0;

    if (offset > file->dentry->file_size) {
        errno = EOVERFLOW;
        return 0;
    }
    DEBUG("Writing %zu bytes to '%s' at offset %lld (current_size=%u)",
          size, file->filepath, (long long)offset, file->dentry->file_size);
    // Move cluster to first cluster to write
    if (offset != 0) {
        starting_point = offset - 1;
    }
    cluster = fat_table_seek_cluster(
        file->table, file->start_cluster,
        starting_point); // originally offset - (offset != 0)
    if (offset % fat_table_bytes_per_cluster(file->table) == 0 &&
        cluster != 0 && offset != 0) {
        cluster = next_or_new_cluster(file, cluster);
    }
    if (cluster == 0) {
        errno = EIO;
        return 0;
    }
    while (bytes_remaining > 0) { // There are still bytes to write
        DEBUG("Writing to cluster %u (%zu bytes remaining)", cluster, bytes_remaining);
        bytes_to_write_cluster = fat_table_get_cluster_remaining_bytes(
            file->table, bytes_remaining, offset);
        cluster_off = fat_table_cluster_offset(file->table, cluster) +
                      fat_table_mask_offset(offset, file->table);
        bytes_written_cluster = full_pwrite(
            file->table->fd, buf, bytes_to_write_cluster, cluster_off);
        bytes_remaining -= bytes_written_cluster;
        if (bytes_written_cluster != bytes_to_write_cluster) {
            break;
        }
        buf += bytes_written_cluster; // Move pointer
        offset += bytes_written_cluster;

        if (bytes_remaining > 0) {
            cluster = next_or_new_cluster(file, cluster);
            if (cluster == 0 || !fat_table_cluster_is_valid(cluster)) {
                break;
            }
        }
    }
    // Update new file size
    if (original_offset + size - bytes_remaining > file->dentry->file_size) {
        file->dentry->file_size = original_offset + size - bytes_remaining;
    }
    // TODO if this operation fails, then the FAT table and the file's parent
    // entry are left on an incosistent state. FIXME
    // Update modified time
    fill_dentry_time_now(file->dentry, false, true);
    write_dir_entry(parent, file->dentry, file->pos_in_parent);

    return size - bytes_remaining;
}
/* ####################### */

ssize_t fat_file_pwrite_existing_clusters(fat_file file, const void *buf,
                                          size_t size, off_t offset,
                                          fat_file parent) {
    u32 cluster = 0;
    ssize_t bytes_written_cluster = 0, bytes_remaining = size;
    ssize_t bytes_to_write_cluster = 0;
    off_t original_offset = offset, cluster_off = 0;
    if (offset > file->dentry->file_size) {
        errno = EOVERFLOW;
        return 0;
    }
    DEBUG("Writing %zu bytes to existing clusters of '%s' at offset %lld",
          size, file->filepath, (long long)offset);
    // Move cluster to first cluster to write
    cluster = fat_table_seek_cluster(file->table, file->start_cluster, offset);
    if (cluster == 0) {
        errno = EOVERFLOW;
        return 0;
    }

    while (bytes_remaining > 0 && fat_table_cluster_is_valid(cluster)) {
        DEBUG("Writing to existing cluster %u (%zu bytes remaining)", cluster, bytes_remaining);
        bytes_to_write_cluster = fat_table_get_cluster_remaining_bytes(
            file->table, bytes_remaining, offset);
        cluster_off = fat_table_cluster_offset(file->table, cluster) +
                      fat_table_mask_offset(offset, file->table);
        bytes_written_cluster = full_pwrite(
            file->table->fd, buf, bytes_to_write_cluster, cluster_off);
        bytes_remaining -= bytes_written_cluster;
        if (bytes_written_cluster != bytes_to_write_cluster) {
            break;
        }
        buf += bytes_written_cluster; // Move pointer
        offset += bytes_written_cluster;

        if (bytes_remaining > 0) {
            cluster = fat_table_read_cluster_value(file->table, cluster);
        }
    }
    // Update new file size
    if (original_offset + size - bytes_remaining > file->dentry->file_size) {
        file->dentry->file_size = offset + size - bytes_remaining;
    }
    // TODO if this operation fails, then the FAT table and the file's parent
    // entry are left on an incosistent state. FIXME
    // Update modified time
    fill_dentry_time_now(file->dentry, false, true);
    write_dir_entry(parent, file->dentry, file->pos_in_parent);
    return size - bytes_remaining;
}

/* ####################### */
void fat_file_unlink(fat_file file, fat_file parent) {
    u32 new_start_cluster = 0, next_cluster = 0;
    off_t cluster_offset = 0;
    int error;
    new_start_cluster = file->start_cluster;

    // Mark following clusters as not used
    while (!fat_table_cluster_is_EOC(new_start_cluster)) {
        // Mark begining of this cluster as '\0' to avoid trash when creating
        // new directories
        // TODO this does not work!
        char buf = '\0';
        cluster_offset =
            fat_table_cluster_offset(file->table, new_start_cluster) +
            fat_table_mask_offset(0, file->table);
        full_pwrite(file->table->fd, &buf, 1, cluster_offset);
        // If there was an error, we continue with the function.
        next_cluster =
            fat_table_read_cluster_value(file->table, new_start_cluster);
        error = fat_table_set_next_cluster(file->table, new_start_cluster,
                                           FAT_CLUSTER_FREE);
        if (error != 0) {
            return;
        }
        new_start_cluster = next_cluster;
    }

    // Update entry in directory
    fat_file_remove_child_direntry(parent, file->dentry, file->pos_in_parent);
}

/* ####################### */
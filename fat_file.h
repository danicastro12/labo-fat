/*
 * fat_file.h
 *
 * This data sctructure is an abstraction of a file in a FAT file system.
 * It contains a copy of the directory entry that describes the file, and
 * other useful information, like a counter of times the file was opened or a
 * reference to the fat_table where it's stored.
 *
 * It also abstracts the functions to read and write the directory entries
 * into fat_table.
 */

#ifndef _FAT_FILE_H
#define _FAT_FILE_H

#include "fat_types.h"
#include <gmodule.h>
#include <sys/types.h>
#include <utime.h>

struct stat;

/* Flags that go in the @attribs field of FAT directory entries. */
#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_HIDDEN 0x00000002
#define FILE_ATTRIBUTE_SYSTEM 0x00000004
#define FILE_ATTRIBUTE_VOLUME 0x00000008
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020
#define FILE_ATTRIBUTE_DEVICE 0x00000040
#define FILE_ATTRIBUTE_RESERVED 0x00000080

#define FILE_FOR_DELETION 0xe5

/* Filename; with extension, if present; null-terminated */
#define MAX_FILENAME (8 + 1 + 3 + 1)
#define MAX_PATH_LEN 4096 /* Copied from libfat */

/********************* DATA STRUCTURES *********************/

/* Format of a FAT directory entry on disk (32 bytes) */
struct fat_dir_entry_s {
    u8 base_name[8];
    u8 extension[3];
    u8 attribs;
    u8 reserved;
    u8 create_time_fine_res;
    le16 create_time;      // BEWARE: little endian number
    le16 create_date;      // BEWARE: little endian number
    le16 last_access_date; // BEWARE: little endian number
    union {
        le16 file_access_bitmap; // Old formats
        le16 fat32_start_cluster_high;
    };
    le16 last_modified_time; // BEWARE: little endian number
    le16 last_modified_date; // BEWARE: little endian number
    le16 start_cluster_low;  // only last two bytes
    le32 file_size;
} __attribute__((packed));

#define FAT_DIR_ENTRY_BYTE_SIZE 32

/* Wrapper around a FAT directory entry that contains members used to put it in
 * data structures, and to use it as a file handle */
struct fat_file_s {
    // The data from the actual FAT directory entry
    fat_dir_entry dentry;
    // Full name of the file (with extension)
    char name[MAX_FILENAME];
    // Full path to file.
    char *filepath;
    // Full start cluster
    u32 start_cluster;

    union {
        // Valid only for directories
        struct {
            // Number of consecutive dir entries in disk (including ignored
            // ones). It also marks the position of the first free space for a
            // dir_entry.
            u32 nentries;
        } dir;
        // Valid only for non-directory files
        struct {
            // Number of clusters that this file is supposed to be,
            // based on the file_size given in the directory entry.
            u32 num_clusters;
        } file;
    };
    // Position in the parent directory entry table
    u32 pos_in_parent;
    // Pointer to the FAT table containing this file
    fat_table table;
    // Current number of open file descriptors to this file
    u32 num_times_opened : 31;
    // True iff the subdirectories of this file have been read into memory.
    // Always 0 for non-directories.
    u32 children_read : 1;
};

/**
 * @brief Create a new directory entry with default values
 *
 * @param is_dir True if this entry represents a directory, false for a file
 * @param filepath Path to the file or directory
 * @param start_cluster Starting cluster number for the file/directory data
 * @return Newly created directory entry, or NULL on error (errno set to ENOSPC)
 * @note Caller retains ownership of filepath reference
 */
fat_dir_entry fat_file_init_direntry(bool is_dir, char *filepath,
                                     u32 start_cluster);

/**
 * @brief Allocate and initialize a new FAT file structure
 *
 * Allocates memory and performs common initializations for a fat_file.
 * Sets the file at the next free cluster in the FAT table.
 *
 * @param table FAT table where the file will be stored
 * @param is_dir True if creating a directory, false for a regular file
 * @param filepath Path to the file or directory
 * @return Newly created fat_file, or NULL on error
 * @retval NULL errno set to ENOSPC if no free cluster available or inconsistent
 * @retval NULL errno set to EIO if setting next cluster fails
 */
fat_file fat_file_init(fat_table table, bool is_dir, char *filepath);

/**
 * @brief Destroy a FAT file structure and free its memory
 *
 * Frees the filepath and dentry fields, and finally the fat_file_s structure itself.
 *
 * @param file File to destroy
 */
void fat_file_destroy(fat_file file);

/**
 * @brief Compare two fat_file structures by filepath
 *
 * @param file1 First file to compare
 * @param file2 Second file to compare
 * @return strcmp result of the two filepaths
 */
int fat_file_cmp(fat_file file1, fat_file file2);

/**
 * @brief Compare a fat_file filepath with a string path
 *
 * @param file1 File to compare
 * @param filepath Path string to compare against
 * @return strcmp result between file1's filepath and the given filepath
 */
int fat_file_cmp_path(fat_file file1, char *filepath);

/********************* FILE METADATA *********************/

/**
 * @brief Check if a file is a directory
 *
 * @param file File to check
 * @return true if file is a directory, false otherwise
 */
bool fat_file_is_directory(const fat_file file);

/**
 * @brief Increment the open count for a file
 *
 * Increments the number of times a FAT file or directory has been opened.
 *
 * @param file File to increment open count
 */
void fat_file_inc_num_times_opened(fat_file file);

/**
 * @brief Decrement the open count for a file
 *
 * Decrements the number of times a FAT file or directory has been opened.
 *
 * @param file File to decrement open count
 */
void fat_file_dec_num_times_opened(fat_file file);

/**
 * @brief Transfer FAT file attributes to UNIX stat structure
 *
 * Converts FAT file attributes into standard UNIX format struct stat.
 *
 * @param file Source FAT file
 * @param stbuf Destination stat buffer to fill
 */
void fat_file_to_stbuf(fat_file file, struct stat *stbuf);

/**
 * @brief Update file access and modification times
 *
 * @param file File to update
 * @param parent Parent directory of the file
 * @param buf Time buffer containing access and modification times
 */
void fat_utime(fat_file file, fat_file parent, const struct utimbuf *buf);

/********************* DIRECTORY FUNCTIONS *********************/

/**
 * @brief Adds a child directory entry to a parent directory
 *
 * Writes the directory entry to the parent's cluster on disk at the specified
 * position and increments the parent's entry count.
 *
 * @param parent Parent directory
 * @param child_dentry Directory entry to add
 * @param pos_in_parent Position in parent directory where entry will be written
 * @retval void errno set to ENOSPC if no space in parent's data cluster
 * @retval void errno set to EIO if write operation fails
 */
void fat_file_add_child_direntry(fat_file parent, fat_dir_entry child_dentry,
                                  u32 pos_in_parent);

/**
 * @brief Removes a child directory entry from a parent directory
 *
 * Marks the directory entry as deleted (0xE5) by writing it to the parent's
 * cluster on disk at the specified position.
 *
 * @param parent Parent directory
 * @param child_dentry Directory entry to mark as deleted
 * @param pos_in_parent Position in parent directory of the entry
 */
void fat_file_remove_child_direntry(fat_file parent, fat_dir_entry child_dentry,
                                     u32 pos_in_parent);

/**
 * @brief Read all children entries from a directory
 *
 * Creates fat_file instances from directory entries in the directory's data cluster.
 * This operation is not recursive - it does not read files in subdirectories.
 *
 * @param dir Directory to read children from (must be a directory, not a file)
 * @return GList of newly created fat_file references, or NULL on error (errno set to EIO)
 */
GList *fat_file_read_children(fat_file dir);

/********************* DATA OPERATIONS *********************/

/**
 * @brief Read data from a FAT file at specified offset
 *
 * Reads size bytes from the file at the given offset, storing the result
 * in the buffer. The number of bytes read may be less than requested only
 * if EOF is encountered before completing the read operation.
 *
 * @param file File to read from
 * @param buf Buffer to store read data
 * @param size Number of bytes to read
 * @param offset Offset within file to start reading
 * @param parent Parent directory of the file
 * @return Number of bytes read on success, or negative error number on failure
 * @retval 0 errno set to EOVERFLOW if offset is greater than file size
 * @retval <0 errno set to EIO if read or write operations fail
 */
ssize_t fat_file_pread(fat_file file, void *buf, size_t size, off_t offset,
                       fat_file parent);

/**
 * @brief Truncate a file to specified size
 *
 * Truncates the file to offset bytes. Frees unused clusters and updates
 * the file size. If offset is greater than current file size, no operation
 * is performed.
 *
 * @param file File to truncate
 * @param offset New size in bytes
 * @param parent Parent directory of the file
 * @retval void errno set to EIO if read or write operations fail
 */
void fat_file_truncate(fat_file file, off_t offset, fat_file parent);

/**
 * @brief Write data to a FAT file at specified offset
 *
 * Writes size bytes to the file at the given offset, reading from the buffer.
 * Short count only occurs on EOF.
 *
 * @param file File to write to
 * @param buf Buffer containing data to write
 * @param size Number of bytes to write
 * @param offset Offset within file to start writing
 * @param parent Parent directory of the file
 * @return Number of bytes written on success, or negative error number on failure
 * @retval 0 errno set to EOVERFLOW if offset is greater than file size
 * @retval <0 errno set to EIO if read or write operations fail
 * @todo Fill with zeros the gap between file_size and offset
 */
ssize_t fat_file_pwrite(fat_file file, const void *buf, size_t size,
                        off_t offset, fat_file parent);

/**
 * @brief Write data to existing clusters only
 *
 * Writes data to the file without allocating new clusters. Only writes
 * to clusters that already exist in the file's cluster chain.
 *
 * @param file File to write to
 * @param buf Buffer containing data to write
 * @param size Number of bytes to write
 * @param offset Offset within file to start writing
 * @param parent Parent directory of the file
 * @return Number of bytes written on success, or negative error number on failure
 */
ssize_t fat_file_pwrite_existing_clusters(fat_file file, const void *buf,
                                          size_t size, off_t offset,
                                          fat_file parent);

/**
 * @brief Delete a file and free its clusters
 *
 * Removes the file from its parent directory and frees all clusters
 * used by the file in the FAT table.
 *
 * @param file File to delete
 * @param parent Parent directory of the file
 */
void fat_file_unlink(fat_file file, fat_file parent);
#endif /* _FAT_FILE_H */

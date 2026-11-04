#ifndef AUDIT_H
#define AUDIT_H

#include "fat_types.h"
#include <stdint.h>
#include <sys/types.h>
#include <stdbool.h>

#define AUDIT_DIR_PATH "/.audit"
#define AUDIT_FILE_PATH AUDIT_DIR_PATH "/events.bin"

/* Operation types for audit logging */
typedef enum {
    OP_WRITE = 0,
    OP_UNLINK = 1,
    OP_MKDIR = 2,
    OP_RMDIR = 3,
    OP_MKNOD = 4,
} operation_type_t;

/* Maximum number of clusters that can be tracked for different operations */
#define MAX_WRITE_CLUSTERS 8
#define MAX_UNLINK_CLUSTERS 16

/* Audit event structure with fixed size for easy reading/writing
 *
 * This is a SUGGESTED structure. You can modify it as needed, but keep in mind:
 * - All events should have the SAME SIZE (for easy indexing)
 * - Consider using __attribute__((packed)) to avoid padding
 * - Total size should be reasonable (e.g., 512 bytes = 1 cluster)
 *
 * Example calculation for 512 bytes:
 *   8 (event_id) + 8 (timestamp) + 4 (operation_type) + 64 (username) +
 *   256 (filepath) + union_size + padding = 512
 */
typedef struct __attribute__((packed)) {
    uint64_t event_id;
    uint64_t timestamp;
    uint32_t operation_type;
    char username[64];
    char filepath[256];

    /* Operation-specific data - use a union to save space */
    union {
        /* Data for OP_WRITE */
        struct {
            uint64_t offset;
            uint64_t size;
            uint32_t clusters_used[MAX_WRITE_CLUSTERS];
            uint32_t num_clusters;
        } write_data;

        /* Data for OP_UNLINK - store cluster chain for recovery */
        struct {
            uint32_t cluster_chain[MAX_UNLINK_CLUSTERS];
            uint32_t file_size;
            uint32_t pos_in_parent;
            uint8_t firs_char;
        } unlink_data;

        /* Data for OP_MKDIR/OP_RMDIR */
        struct {
            uint32_t dir_cluster;
        } dir_data;

        /* Data for OP_MKNOD */
        struct {
            uint32_t start_cluster;
        } mknod_data;
    } data;

    /* Padding to ensure fixed size of 512 bytes total
     * Adjust this value based on your structure size */
    uint8_t _padding[95];
} audit_event_t;


int fat_fuse_internal_mkdir(const char *path,fat_volume vol); //no deveriamos dejarlas privadas?
int fat_fuse_event_file(fat_volume vol);

bool audit_is_protected_path(const char *path);

/*
 * @brief creates dir and file for audit
 */
int audit_init(fat_volume vol);

void audit_log_write(const char *path, fat_file file, size_t size, off_t offset);

void audit_log_unlink(const char *path, fat_file file);

void audit_log_mknod(const char *path, fat_file file);

void audit_log_dir(const char* path,operation_type_t op_type, fat_file dir_file);
#endif /* AUDIT_H */

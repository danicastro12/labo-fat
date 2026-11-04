/*
 * fat_table.h
 *
 * The fat_table TAD is an abstraction the can be used to navigate, reand and
 * write the FAT table and the chains of data clusters.
 */
#ifndef _FAT_TABLE_H
#define _FAT_TABLE_H

#include "fat_types.h"
#include "fat_util.h"
#include <sys/types.h>
#include <glib.h> 

// Both values of EOC are valid in FAT32 systems
#define FAT_CLUSTER_END_OF_CHAIN_MIN 0x0FFFFFF8
#define FAT_CLUSTER_END_OF_CHAIN_MAX 0x0FFFFFFF
#define FAT_CLUSTER_FREE 0x00000000
#define FAT_CLUSTER_BAD_SECTOR 0x0FFFFFF7

/* Abstraction of the fat table that handles cluster information and
 * read/write operations
 */
struct fat_table_s {
    // FAT mapped into memory
    void *fat_map;
    // The offset to write the fat table
    off_t fat_offset;
    // Number of data clusters
    u32 num_data_clusters;
    // Byte offset of the "second" cluster (first in the actual layout)
    off_t data_start_offset;
    // Open file descriptor to the volume file or device
    int fd;
    u16 cluster_order;
    GHashTable *recoverable;
};

/**
 * @brief Check if a cluster number is valid
 *
 * @param table FAT table to check against
 * @param cluster Cluster number to validate
 * @return true if cluster number is within valid range, false otherwise
 */
bool fat_table_is_valid_cluster_number(const fat_table table, u32 cluster);

/**
 * @brief Read the FAT table value for a cluster
 *
 * Returns the next cluster in the chain or FAT_CLUSTER_END_OF_CHAIN if this
 * was the last cluster in the chain.
 *
 * @param table FAT table to read from
 * @param cur_cluster Current cluster to look up
 * @return Next cluster number, or FAT_CLUSTER_END_OF_CHAIN if end of chain
 */
u32 fat_table_read_cluster_value(fat_table table, u32 cur_cluster);

/**
 * @brief Calculate number of bytes per cluster
 *
 * Calculates bytes per cluster based on the cluster order (power of two).
 *
 * @param table FAT table containing cluster order
 * @return Number of bytes per cluster
 */
size_t fat_table_bytes_per_cluster(fat_table table);

/**
 * @brief Mask offset to prevent overflow
 *
 * Masks the offset so when subtracting it from max bytes in cluster
 * it doesn't overflow.
 *
 * @param offset Offset to mask
 * @param table FAT table containing cluster size info
 * @return Masked offset value
 */
size_t fat_table_mask_offset(off_t offset, fat_table table);

/**
 * @brief Calculate remaining bytes available in current cluster
 *
 * Calculates how many of bytes_remaining can be read/written in a cluster
 * starting from the given offset position.
 *
 * @param table FAT table containing cluster size info
 * @param bytes_remaining Total bytes remaining to process
 * @param offset Current offset within file
 * @return Number of bytes available in current cluster
 */
size_t fat_table_get_cluster_remaining_bytes(fat_table table,
                                             size_t bytes_remaining,
                                             off_t offset);

/**
 * @brief Calculate number of clusters needed for file size
 *
 * @param table FAT table containing cluster size info
 * @param file_size Size in bytes
 * @return Number of clusters needed to fit the file size
 */
u32 fat_table_get_clusters_for_size(fat_table table, size_t file_size);

/**
 * @brief Find the first free cluster in the data sector
 *
 * @param table FAT table to search
 * @return Cluster number of first unused cluster
 */
u32 fat_table_get_next_free_cluster(fat_table table);

/**
 * @brief Get byte offset where cluster data starts
 *
 * @param table FAT table containing offset information
 * @param cluster Cluster number
 * @return Byte offset to the start of cluster data
 */
off_t fat_table_cluster_offset(const fat_table table, u32 cluster);

/**
 * @brief Check if cluster is currently in use
 *
 * @param table FAT table to check
 * @param cluster Cluster number to check
 * @return true if cluster is not marked as free, false otherwise
 */
bool fat_table_is_cluster_used(fat_table table, u32 cluster);

/**
 * @brief Update FAT table to link clusters
 *
 * Writes the FAT table entry for cur_cluster, marking next_cluster as
 * the next cluster in the chain.
 *
 * @param table FAT table to update
 * @param cur_cluster Current cluster to update
 * @param next_cluster Next cluster in chain (or EOC marker)
 * @return 0 on success, -1 on write error
 */
int fat_table_set_next_cluster(fat_table table, u32 cur_cluster,
                               u32 next_cluster);

/**
 * @brief Seek to cluster at given offset in chain
 *
 * Finds the cluster number that contains data at the specified byte offset
 * from the start of the cluster chain.
 *
 * @param table FAT table to traverse
 * @param start_cluster First cluster in chain
 * @param offset Byte offset from start of chain
 * @return Cluster number at offset, or 0 if EOC encountered first
 */
u32 fat_table_seek_cluster(fat_table table, u32 start_cluster, off_t offset);

/**
 * @brief Check if cluster value marks end of chain
 *
 * @param cluster Cluster value to check
 * @return true if cluster is an end-of-chain marker, false otherwise
 */
bool fat_table_cluster_is_EOC(u32 cluster);

/**
 * @brief Check if cluster marks a bad sector
 *
 * @param cluster Cluster value to check
 * @return true if cluster marks a bad sector, false otherwise
 */
bool fat_table_cluster_is_bad_sector(u32 cluster);

/**
 * @brief Check if cluster value is valid (not EOC, free, or bad)
 *
 * @param cluster Cluster value to check
 * @return true if cluster is a valid data cluster, false otherwise
 */
bool fat_table_cluster_is_valid(u32 cluster);

/**
 * @brief Print cluster chain for debugging
 *
 * Prints index and cluster values stored in the FAT table from
 * start_cluster to end_cluster. Useful for debugging.
 *
 * @param table FAT table to print
 * @param start_cluster First cluster to print
 * @param end_cluster Last cluster to print
 */
void fat_table_print(fat_table table, u32 start_cluster, u32 end_cluster);

#endif /* _FAT_TABLE_H */

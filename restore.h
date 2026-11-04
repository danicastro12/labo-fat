#include "fat_types.h"

#define RESTORE_PATH "/.audit/.restore"

int restore_fat_table(fat_table table,int *cluster_chain,int num_clusters);

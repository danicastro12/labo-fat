#include "audit.h"
#include "fat_types.h"

#define RESTORE_PATH "/.audit/restore"

int restore_fat_table(fat_table table, uint32_t *cluster_chain,
                      uint8_t num_clusters, audit_event_t unlink_event);
int restore_check_recoverable(fat_table table,
                              const audit_event_t *unlink_event);
bool is_restore_path(const char *path);
int restore_file(fat_table table, const char *filepath);

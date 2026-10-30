#include "fat_file.h"
#include "audit.h"
#include "fat_fs_tree.h"
#include "fat_types.h"
#include "fat_util.h"
#include "fat_volume.h"
#include "fat_fuse_ops.h"
#include <alloca.h>
#include <errno.h>
#include <gmodule.h>
#include <libgen.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>


static uint64_t total_events;
static fat_tree_node audit_event_node;

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
    
    //bool exist_dir = false;
    //bool exist_file = false;
    fat_tree_node audit_dir_node = fat_tree_node_search(vol->file_tree, AUDIT_DIR_PATH);

    if(audit_dir_node == NULL){
      int res_dir = fat_fuse_internal_mkdir(AUDIT_DIR_PATH, vol);
      if(res_dir != 0){
        return res_dir;
      }
      audit_dir_node = fat_tree_node_search(vol->file_tree, AUDIT_DIR_PATH);
    }
    else {
      fat_file audit_dir_file = fat_tree_get_file(audit_dir_node);
      if (audit_dir_file->children_read != 1) {
          fat_fuse_load_directory_children(vol, audit_dir_node);
      }
    }

    audit_event_node = fat_tree_node_search(vol->file_tree, AUDIT_FILE_PATH);
    if(audit_event_node == NULL){
      int res_file = fat_fuse_event_file(vol);
      if(res_file != 0){
        return res_file;
      }
      audit_event_node = fat_tree_node_search(vol->file_tree, AUDIT_FILE_PATH);
    }

    //calculo de la cantidad de eventos en events.bin
    fat_file event_file = fat_tree_get_file(audit_event_node);
    total_events = event_file->dentry->file_size / sizeof(audit_event_t);

    return 0;
}

void audit_log_write(const char *path, fat_file file, size_t size, off_t offset){
    if (audit_event_node == NULL) //checkeo si es read_only
        return;
    audit_event_t event;

    memset(&event, 0, sizeof(event));

    fat_file event_file = fat_tree_get_file(audit_event_node);
    fat_file parent = fat_tree_get_parent(audit_event_node);
    off_t event_off = event_file->dentry->file_size;
    size_t event_size = sizeof(event);

    event.event_id = total_events;
    event.timestamp = time(NULL);
    event.operation_type = OP_WRITE;
    strncpy(event.username,"NaN", sizeof(event.username) - 1);
    strncpy(event.filepath, path, sizeof(event.filepath) - 1);
    event.data.write_data.offset = offset; //ver si hace falta casteo
    event.data.write_data.size = size;
    //event.data.write_data.clusters_used[MAX_WRITE_CLUSTERS]; //ver como saber el cluster
    //event.data.write_data.num_clusters;

    int result = fat_file_pwrite(event_file, &event, event_size, event_off, parent);
    if (result == sizeof(event))
        total_events++;

}

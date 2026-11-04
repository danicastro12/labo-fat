#include "restore.h"
#include "audit.h"
#include "fat_file.h"
#include "fat_fs_tree.h"
#include "fat_table.h"
#include "fat_types.h"
#include "fat_util.h"
#include "fat_volume.h"
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Busca el ultimo OP_UNLINK de @filepath en events.bin y lo copia en @event.
 * Retorna:
 *   0  -> Evento encontrado.
 *   1  -> No hay unlink registrado para ese path.
 *  -1  -> Error de lectura.
 */
static int find_unlink_event(const char *filepath, audit_event_t *event) {
  fat_volume vol = get_fat_volume();
  fat_tree_node audit_event_node =
      fat_tree_node_search(vol->file_tree, AUDIT_FILE_PATH);
  if (audit_event_node == NULL)
    return -1;
  fat_file event_file = fat_tree_get_file(audit_event_node);
  fat_file parent = fat_tree_get_parent(audit_event_node);

  // Recorremos de atras para adelante para quedarnos con el unlink mas reciente
  uint64_t total_events = event_file->dentry->file_size / sizeof(audit_event_t);
  for (uint64_t n = total_events; n > 0; n--) {
    off_t offset = (n - 1) * sizeof(audit_event_t);
    if (fat_file_pread(event_file, event, sizeof(audit_event_t), offset,
                       parent) != sizeof(audit_event_t))
      return -1;
    if (event->operation_type == OP_UNLINK &&
        strcmp(event->filepath, filepath) == 0)
      return 0;
  }
  return 1;
}

// Version con bitmap + escaneo de events.bin, reemplazada por
// restore_check_recoverable. Cambiar 0 por 1 para volver a usarla.
#if 0
static int check_clusters(fat_table table, uint8_t *bitmap, uint32_t cluster) {
  if (!fat_table_is_valid_cluster_number(table, cluster)) {
    return -1;
  }
  if (bitmap[cluster]) {
    return 1;
  }

  return 0;
}

/*
 * Retorna:
 *   0  -> No se encontraron conflictos.
 *   1  -> Se encontró una sobrescritura.
 *  -1  -> Error de lectura o estructura inválida.
 */
int restore_check_conflicts(fat_table table,
                            const audit_event_t *unlink_event) {
  uint32_t total_clusters = table->num_data_clusters + 2;
  // Bitmap: una posición por cluster.
  uint8_t *bitmap = calloc(total_clusters, sizeof(uint8_t));

  if (bitmap == NULL)
    return -1;

  uint32_t num_deleted = unlink_event->data.unlink_data.num_clusters;

  // Validar y marcar los clusters del archivo eliminado.
  if (num_deleted == 0 || num_deleted > MAX_UNLINK_CLUSTERS) {
    free(bitmap);
    return -1;
  }

  for (uint32_t i = 0; i < num_deleted; i++) {
    uint32_t cluster = unlink_event->data.unlink_data.cluster_chain[i];
    if (!fat_table_is_valid_cluster_number(table, cluster)) {
      free(bitmap);
      return -1;
    }
    bitmap[cluster] = 1;
  }

  // Recorrer los eventos binarios.
  audit_event_t event;
  ssize_t bytes_read;
  fat_file event_file;
  fat_tree_node audit_event_node;
  fat_volume vol = get_fat_volume();
  audit_event_node = fat_tree_node_search(vol->file_tree, AUDIT_FILE_PATH);
  if (audit_event_node == NULL) {
    free(bitmap);
    return -1;
  }
  event_file = fat_tree_get_file(audit_event_node);
  off_t offset = unlink_event->event_id * sizeof(audit_event_t);
  fat_file parent = fat_tree_get_parent(audit_event_node);

  uint32_t cluster;
  int result;
  while ((bytes_read = fat_file_pread(event_file, &event, sizeof(audit_event_t),
                                      offset, parent)) ==
         sizeof(audit_event_t)) {
    // Si el id es mas grande que el unlink(nos fijamos en lso eventos que
    // pasaron despues del unlink)
    if (unlink_event->event_id < event.event_id) {
      switch (event.operation_type) {
      case OP_MKDIR:
        cluster = event.data.dir_data.dir_cluster;
        if (cluster == 0) {
          break;
        }
        result = check_clusters(table, bitmap, cluster);
        if (result != 0) {
          free(bitmap);
          return result;
        }
        break;
      case OP_MKNOD:
        cluster = event.data.mknod_data.start_cluster;
        result = check_clusters(table, bitmap, cluster);
        if (result != 0) {
          free(bitmap);
          return result;
        }
        break;
      case OP_WRITE:
        if (event.data.write_data.num_clusters > MAX_WRITE_CLUSTERS) {
          free(bitmap);
          return -1;
        }

        for (int i = 0; i < event.data.write_data.num_clusters; i++) {
          cluster = event.data.write_data.clusters_used[i];
          result = check_clusters(table, bitmap, cluster);
          if (result != 0) {
            free(bitmap);
            return result;
          }
        }
        break;
      }
    }
    offset += sizeof(audit_event_t);
  }

  free(bitmap);
  return 0;
}
#endif

/*
 * Retorna:
 *   0  -> Todos los clusters siguen intactos.
 *   1  -> Algun cluster fue reutilizado despues del unlink.
 *  -1  -> Evento invalido.
 */
int restore_check_recoverable(fat_table table,
                              const audit_event_t *unlink_event) {
  uint32_t num_clusters = unlink_event->data.unlink_data.num_clusters;
  if (num_clusters == 0 || num_clusters > MAX_UNLINK_CLUSTERS)
    return -1;

  for (uint32_t i = 0; i < num_clusters; i++) {
    uint32_t cluster = unlink_event->data.unlink_data.cluster_chain[i];
    gpointer value = NULL;

    if (!fat_table_is_valid_cluster_number(table, cluster))
      return -1;
    // No esta en la tabla: se asigno a otro archivo despues del unlink
    if (!g_hash_table_lookup_extended(table->recoverable,
                                      GUINT_TO_POINTER(cluster), NULL, &value))
      return 1;
    // Lo libero otro unlink: tiene datos de otro archivo
    if (GPOINTER_TO_SIZE(value) != unlink_event->event_id + 1)
      return 1;
    if (fat_table_is_cluster_used(table, cluster))
      return 1;
  }
  return 0;
}

/*
 * Retorna:
 * 1 -> no se puede recuperar la fat_table
 * 0 -> fat_table recuperar
 * -1 -> Error al recuperar
 */
int restore_fat_table(fat_table table, uint32_t *cluster_chain,
                      uint8_t num_clusters, audit_event_t unlink_event) {
  // reconstruyo la cadena de clusters
  for (int i = 0; i < num_clusters - 1; i++) {
    if (fat_table_set_next_cluster(table, cluster_chain[i],
                                   cluster_chain[i + 1])) {
      // LIBERAR TODOS LOS CLUSTERS ANTES?
      return -1;
    }
  }

  // pongo el ultimo cluster como EOC
  if (fat_table_set_next_cluster(table, cluster_chain[num_clusters - 1],
                                 FAT_CLUSTER_END_OF_CHAIN_MIN) != 0) {
    return -1;
  };

  // devuelvo el primer byte de cada cluster que piso fat_file_unlink
  for (int i = 0; i < num_clusters; i++) {
    if (full_pwrite(table->fd, &unlink_event.data.unlink_data.first_bytes[i], 1,
                    fat_table_cluster_offset(table, cluster_chain[i])) != 1)
      return -1;
  }

  return 0;
}

int restore_entries(fat_volume vol, const audit_event_t *event) {
  fat_file parent = NULL, new_file = NULL;
  fat_tree_node parent_node = NULL;
  struct fat_dir_entry_s entry;
  u32 pos_in_parent = event->data.unlink_data.pos_in_parent;
  u8 first_char = event->data.unlink_data.first_char;

  // buscar padre, despues ver los casos de error
  char *path_copy = strdup(event->filepath); // liberar, eso para poder checkear
                                             // que el path no es vacio
  if (path_copy == NULL)
    return -1; // error
  parent_node = fat_tree_node_search(vol->file_tree, dirname(path_copy));
  free(path_copy);
  if (parent_node == NULL)
    return -1; // error
  parent = fat_tree_get_file(parent_node);

  // offset
  off_t offset =
      fat_table_cluster_offset(parent->table, parent->start_cluster) +
      pos_in_parent * sizeof(struct fat_dir_entry_s);

  if (full_pread(parent->table->fd, &entry, sizeof(entry), offset) !=
      sizeof(entry))
    return -1; // error

  // verificacion si sigue borrada y si es el que necesitamos
  if (entry.base_name[0] != FILE_FOR_DELETION ||
      file_start_cluster(&entry) !=
          event->data.unlink_data.cluster_chain[0]) // hacer public
    return -1; // error no era o ya no esta borrado

  // recuperar el primer caracter
  if (full_pwrite(parent->table->fd, &first_char, 1, offset) !=
      1)       // escribe 1byte en la posicion del fd con el buff first_char
    return -1; // error si no pudo
  entry.base_name[0] = first_char;

  // armar el nodo en memoria y ponerlo en la tree
  if (!parent->children_read) // eso para evitar que se dupliquen
    return 0;
  fat_dir_entry new_entry =
      init_direntry_from_buff(&entry); // hacer public en fat_file.c y .h
  // hacer checkeo si new_entry es NULL o no
  if (new_entry == NULL)
    return -1; // error

  new_file = init_file_from_dentry(
      new_entry, parent); // hay que hacer public en fat_file.c
  // hacer checkeo si new_file no vino con NULL y liberar new_entry con error
  if (new_file == NULL) {
    free(new_entry);
    return -1;
  }
  new_file->pos_in_parent = pos_in_parent; // recupero pos_in_parent
  vol->file_tree =
      fat_tree_insert(vol->file_tree, parent_node,
                      new_file); // vuelve a estar en el arbol asi no hace falta
                                 // desmontar y montar

  return 0;
}

/*
 * Retorna 0 si se restauro, o un -errno:
 *   -EEXIST -> Ya existe un archivo con ese path.
 *   -ENOENT -> No hay unlink registrado para ese path.
 *   -EBUSY  -> Algun cluster fue reutilizado, no se puede restaurar.
 *   -EINVAL -> El evento del unlink es invalido.
 *   -EIO    -> Error de lectura/escritura.
 */
int restore_file(fat_table table, const char *filepath) {
  audit_event_t unlink_event;
  fat_volume vol = get_fat_volume();

  if (fat_tree_node_search(vol->file_tree, filepath) != NULL) {
    DEBUG("restore: '%s' ya existe", filepath);
    return -EEXIST;
  }

  int result = find_unlink_event(filepath, &unlink_event);
  if (result != 0) {
    DEBUG("restore: no se encontro unlink de '%s'", filepath);
    return result == 1 ? -ENOENT : -EIO;
  }

  // Comprobar que los clusters no hayan sido sobrescritos.
  result = restore_check_recoverable(table, &unlink_event);
  if (result != 0) {
    DEBUG("restore: clusters de '%s' reutilizados o evento invalido", filepath);
    return result == 1 ? -EBUSY : -EINVAL;
  }

  uint32_t cluster_chain[MAX_UNLINK_CLUSTERS];
  memcpy(cluster_chain, unlink_event.data.unlink_data.cluster_chain,
         sizeof(cluster_chain));

  // Reconstruir la cadena FAT.
  result = restore_fat_table(table, cluster_chain,
                             unlink_event.data.unlink_data.num_clusters,
                             unlink_event);
  if (result != 0) {
    DEBUG("restore: error reconstruyendo la FAT de '%s'", filepath);
    return -EIO;
  }

  if (restore_entries(vol, &unlink_event) != 0) {
    DEBUG("restore: error restaurando la dentry de '%s'", filepath);
    return -EIO;
  }
  return 0;
}

bool is_restore_path(const char *path) {
  return strcmp(RESTORE_PATH, path) == 0;
}



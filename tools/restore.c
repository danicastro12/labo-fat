#include <stdbool.h>
#include <stdio.h>
#include "../audit.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv){
  if(argc > 4 || argc < 3){ //la peor validacion del mundo
      printf("Uso: restore /ruta_de_archivo_a_recuperar ./mnt/ruta_de_guardado\n");
      return 1;
  }

  bool debug = false;

  if(argc == 4){
    debug = strcmp(argv[3], "d") == 0;
  }

  char *filepath = "./mnt/.audit/events.bin";
  char *path_to_restore = argv[1];
  char *save_path = argv[2];
  printf("%s\n", argv[3]);
  printf("%d",debug);

  printf("%s\n",path_to_restore);
  printf("%s\n",save_path);

  //Filtrar el events.bin con el path_to_restore
    int fd;
    audit_event_t event;
    ssize_t bytes_read;
    uint64_t event_count = 0;

    fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Error: Cannot open file '%s': %s\n", filepath, strerror(errno));
        return 1;
    }


    while ((bytes_read = read(fd, &event, sizeof(audit_event_t))) == sizeof(audit_event_t)) {
        if(debug == true){
          printf("path pasado:%s\npath en el fs: %s\n",path_to_restore,event.filepath);
          printf("evento del tipo:%d\n",event.operation_type);
        }
        if(strcmp(event.filepath, path_to_restore) == 0)
          printf("coincide %s\n",event.filepath);
        event_count++;
    }


    if (bytes_read < 0) {
        fprintf(stderr, "Error reading file: %s\n", strerror(errno));
        close(fd);
        return 1;
    } else if (bytes_read > 0) {
        fprintf(stderr, "Warning: Incomplete event at end of file (%zd bytes)\n", bytes_read);
    }

    printf("\nTotal events: %lu\n", (unsigned long)event_count);

    close(fd);


  printf("restore ejecutado");
  return 0;
}

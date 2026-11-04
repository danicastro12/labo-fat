#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Must match the errno mapping chosen on the filesystem side. */
static const char *explain(int err) {
  switch (err) {
  case ENOENT:
    return "no deletion record found for that file";
  case EEXIST:
    return "a file with that name already exists";
  case EBUSY:
    return "clusters were reused after the deletion, data was overwritten";
  case ENOTDIR:
    return "the parent directory no longer exists";
  case EOVERFLOW:
    return "the deletion record is truncated, the file is not recoverable";
  default:
    return strerror(err);
  }
}

int main(int argc, char *argv[]) {
  char control_path[PATH_MAX];
  int fd;
  ssize_t written;

  fd = open("./mnt/.audit/restore", O_WRONLY);
  if (fd < 0) {
    fprintf(stderr, "Error: cannot open '%s': %s\n", control_path,
            strerror(errno));
    return 1;
  }

  written = write(fd, argv[1], strlen(argv[1]));
  int saved_errno = errno; /* close() could change it */
  close(fd);

  if (written < 0) {
    fprintf(stderr, "restore failed for '%s': %s\n", argv[1],
            explain(saved_errno));
    return 1;
  }

  printf("restored %s\n", argv[1]);
  return 0;
}

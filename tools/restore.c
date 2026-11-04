#include "../audit.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s MOUNTPOINT /path/of/deleted/file\n", prog);
}

/* Must match the errno mapping chosen on the filesystem side. */
static const char *explain(int err) {
    switch (err) {
    case ENOENT:   return "no deletion record found for that file";
    case EEXIST:   return "a file with that name already exists";
    case EBUSY:    return "clusters were reused after the deletion, data was overwritten";
    case ENOTDIR:  return "the parent directory no longer exists";
    case EOVERFLOW:return "the deletion record is truncated, the file is not recoverable";
    default:       return strerror(err);
    }
}

int main(int argc, char *argv[]) {
    char control_path[PATH_MAX];
    const char *mountpoint, *target;
    int fd, n;
    ssize_t written;

    if (argc != 3) {
        usage(argv[0]);
        return 2;
    }
    mountpoint = argv[1];
    target = argv[2];

    if (target[0] != '/' || strlen(target) >= AUDIT_PATH_MAX) {
        fprintf(stderr, "Error: path must be absolute and shorter than %d\n",
                AUDIT_PATH_MAX);
        return 2;
    }

    n = snprintf(control_path, sizeof(control_path), "%s%s", mountpoint,
                 AUDIT_RESTORE_PATH);
    if (n < 0 || (size_t)n >= sizeof(control_path)) {
        fprintf(stderr, "Error: mountpoint path too long\n");
        return 2;
    }

    fd = open(control_path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Error: cannot open '%s': %s\n", control_path,
                strerror(errno));
        return 1;
    }

    written = write(fd, target, strlen(target));
    int saved_errno = errno; /* close() could change it */
    close(fd);

    if (written < 0) {
        fprintf(stderr, "restore failed for '%s': %s\n", target,
                explain(saved_errno));
        return 1;
    }
    printf("restored %s\n", target);
    return 0;
}

/*
 * audit_reader.c
 *
 * Standalone tool to read and display FAT32 audit logs from events.bin
 */

#include "../audit.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Format timestamp as readable string */
static void format_timestamp(uint64_t timestamp, char* buffer, size_t size) {
    time_t t = (time_t)timestamp;
    struct tm* tm_info = localtime(&t);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

/* Get operation name as string */
static const char* operation_name(uint32_t op) {
    switch (op) {
        case OP_WRITE:  return "WRITE";
        case OP_UNLINK: return "UNLINK";
        case OP_MKDIR:  return "MKDIR";
        case OP_RMDIR:  return "RMDIR";
        case OP_MKNOD:  return "MKNOD";
        default:        return "UNKNOWN";
    }
}

/* Print table header */
static void print_header(void) {
    printf("┌────────┬─────────────────────┬─────────┬──────────┬──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐\n");
    printf("│ %-6s │ %-19s │ %-7s │ %-8s │ %-256s │\n", "ID", "Timestamp", "Op", "User", "Path");
    printf("├────────┼─────────────────────┼─────────┼──────────┼──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤\n");
}

/* Print table footer */
static void print_footer(void) {
    printf("└────────┴─────────────────────┴─────────┴──────────┴──────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘\n");
}

/* Print basic event info */
static void print_event_row(audit_event_t* event) {
    char timestamp_str[32];
    format_timestamp(event->timestamp, timestamp_str, sizeof(timestamp_str));

    printf("│ %-6lu │ %-19s │ %-7s │ %-8s │ %-256s │\n",
           (unsigned long)event->event_id,
           timestamp_str,
           operation_name(event->operation_type),
           event->username,
           event->filepath);
}

/* Print detailed operation data */
static void print_operation_details(audit_event_t* event) {
    printf("│ Details: ");

    switch (event->operation_type) {
        case OP_WRITE:
            printf("offset=%lu, size=%lu, clusters=[",
                   (unsigned long)event->data.write_data.offset,
                   (unsigned long)event->data.write_data.size);
            for (uint32_t i = 0; i < event->data.write_data.num_clusters && i < MAX_WRITE_CLUSTERS; i++) {
                if (i > 0) printf(",");
                printf("%u", event->data.write_data.clusters_used[i]);
            }
            printf("]");
            break;

        case OP_UNLINK: {
            printf("file_size=%u, clusters=[", event->data.unlink_data.file_size);
            uint32_t count = 0;
            for (uint32_t i = 0; i < MAX_UNLINK_CLUSTERS; i++) {
                if (event->data.unlink_data.cluster_chain[i] == 0) break;
                if (count > 0) printf(",");
                printf("%u", event->data.unlink_data.cluster_chain[i]);
                count++;
            }
            printf("]");
            if (count >= MAX_UNLINK_CLUSTERS) {
                printf(" (truncated, file may not be recoverable)");
            }
            break;
        }

        case OP_MKDIR:
        case OP_RMDIR:
            printf("cluster=%u", event->data.dir_data.dir_cluster);
            break;

        case OP_MKNOD:
            printf("start_cluster=%u", event->data.mknod_data.start_cluster);
            break;

        default:
            printf("(unknown operation)");
            break;
    }

    printf("\n");
}

/* Read and display all events from a file */
static int read_events(const char* filepath, int verbose) {
    int fd;
    audit_event_t event;
    ssize_t bytes_read;
    uint64_t event_count = 0;

    fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Error: Cannot open file '%s': %s\n", filepath, strerror(errno));
        return 1;
    }

    print_header();

    while ((bytes_read = read(fd, &event, sizeof(audit_event_t))) == sizeof(audit_event_t)) {
        print_event_row(&event);
        if (verbose) {
            print_operation_details(&event);
        }
        event_count++;
    }

    print_footer();

    if (bytes_read < 0) {
        fprintf(stderr, "Error reading file: %s\n", strerror(errno));
        close(fd);
        return 1;
    } else if (bytes_read > 0) {
        fprintf(stderr, "Warning: Incomplete event at end of file (%zd bytes)\n", bytes_read);
    }

    printf("\nTotal events: %lu\n", (unsigned long)event_count);

    close(fd);
    return 0;
}

/* Print usage information */
static void print_usage(const char* prog_name) {
    printf("Usage: %s [OPTIONS] <events.bin>\n\n", prog_name);
    printf("Read and display FAT32 audit logs.\n\n");
    printf("Options:\n");
    printf("  -v, --verbose    Show detailed operation data\n");
    printf("  -h, --help       Show this help message\n\n");
    printf("Examples:\n");
    printf("  %s /mnt/fuse/.audit/events.bin\n", prog_name);
    printf("  %s -v /mnt/fuse/.audit/events.bin\n", prog_name);
}

int main(int argc, char* argv[]) {
    int verbose = 0;
    const char* filepath = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option '%s'\n\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else {
            filepath = argv[i];
        }
    }

    if (filepath == NULL) {
        fprintf(stderr, "Error: No input file specified\n\n");
        print_usage(argv[0]);
        return 1;
    }

    return read_events(filepath, verbose);
}

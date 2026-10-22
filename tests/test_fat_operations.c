/**
 * @file test_fat_operations.c
 * @brief Functional tests for FAT filesystem operations
 *
 * Uses Check framework with hybrid fixtures:
 * - Unchecked fixtures: mount/unmount FS once per suite (slow)
 * - Checked fixtures: cleanup files between tests (fast)
 *
 * This ensures fast test execution while maintaining isolation.
 */

#include <check.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Global state for mounted filesystem
static char test_image[256];
static char mount_point[256];
static pid_t fuse_pid = 0;

// ============================================================================
// UNCHECKED FIXTURES - Execute once per suite (slow operations)
// ============================================================================

/**
 * @brief Setup: Copy base image and mount FAT32 filesystem
 *
 * This runs ONCE before all tests in the suite.
 * Copies the base empty FAT32 image from resources/ and mounts it.
 */
void setup_mount_fs(void) {
    printf("\n⏳ Setting up filesystem (once per suite)...\n");

    // Create unique temporary paths
    snprintf(test_image, sizeof(test_image), "/tmp/test_fat_%d.img",
             getpid());
    snprintf(mount_point, sizeof(mount_point), "/tmp/fuse_mnt_%d", getpid());

    // Copy base image from resources
    char cmd[512];
    printf("  Copying base image to: %s\n", test_image);
    snprintf(cmd, sizeof(cmd), "cp ../resources/test_empty.img %s",
             test_image);
    int ret = system(cmd);
    ck_assert_msg(ret == 0, "Failed to copy base image");

    // Create mount point directory
    printf("  Creating mount point: %s\n", mount_point);
    ret = mkdir(mount_point, 0755);
    ck_assert_msg(ret == 0, "Failed to create mount point: %s",
                  strerror(errno));

    // Mount filesystem in background
    printf("  Mounting filesystem...\n");
    fuse_pid = fork();
    ck_assert_msg(fuse_pid >= 0, "Failed to fork: %s", strerror(errno));

    if (fuse_pid == 0) {
        // Child process: execute fat-fuse
        // Redirect output to avoid clutter
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execlp("./fat-fuse", "fat-fuse", test_image, mount_point, NULL);
        exit(1); // Should not reach here
    }

    // Wait for filesystem to mount
    sleep(2);

    // Verify mount succeeded by trying to open the directory
    DIR *dir = opendir(mount_point);
    ck_assert_msg(dir != NULL, "Failed to open mount point: %s",
                  strerror(errno));
    closedir(dir);

    printf("✅ Filesystem mounted successfully\n\n");
}

/**
 * @brief Teardown: Unmount and cleanup filesystem
 *
 * This runs ONCE after all tests in the suite.
 */
void teardown_unmount_fs(void) {
    printf("\n🧹 Cleaning up filesystem...\n");

    // Unmount filesystem
    char cmd[512];
#ifdef __APPLE__
    snprintf(cmd, sizeof(cmd), "umount %s 2>/dev/null || "
                                "diskutil unmount force %s 2>/dev/null",
             mount_point, mount_point);
#else
    snprintf(cmd, sizeof(cmd), "fusermount -u %s 2>/dev/null || "
                                "umount %s 2>/dev/null",
             mount_point, mount_point);
#endif
    system(cmd);

    // Wait for fuse process to exit
    if (fuse_pid > 0) {
        int status;
        waitpid(fuse_pid, &status, 0);
    }

    // Remove mount point and image
    rmdir(mount_point);
    unlink(test_image);

    printf("✅ Cleanup complete\n\n");
}

// ============================================================================
// CHECKED FIXTURES - Execute before/after each test (fast operations)
// ============================================================================

/**
 * @brief Setup: Clean all files from previous test
 *
 * This runs BEFORE each test.
 * Removes all files/directories to ensure test isolation.
 */
void setup_clean_dir(void) {
    DIR *dir = opendir(mount_point);
    if (!dir)
        return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;

        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s/%s", mount_point,
                 entry->d_name);

        // Try to remove (file or empty directory)
        unlink(filepath);
        rmdir(filepath);

        // For non-empty directories, try recursive removal
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "rm -rf %s 2>/dev/null", filepath);
        system(cmd);
    }
    closedir(dir);
}

/**
 * @brief Teardown: Verify filesystem is still consistent
 *
 * This runs AFTER each test.
 * Optional verification step.
 */
void teardown_verify(void) {
    // Could add checks here to verify FS is still accessible
    // For now, just verify we can still read the directory
    DIR *dir = opendir(mount_point);
    ck_assert_msg(dir != NULL, "Filesystem became inaccessible");
    closedir(dir);
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Helper: Get full path for a file in the mount point
 */
static void get_test_path(char *buffer, size_t size, const char *filename) {
    snprintf(buffer, size, "%s/%s", mount_point, filename);
}

/**
 * @brief Helper: Read entire file content into buffer
 */
static int read_file_content(const char *filepath, char *buffer,
                              size_t buffer_size) {
    FILE *f = fopen(filepath, "r");
    if (!f)
        return -1;

    size_t bytes_read = fread(buffer, 1, buffer_size - 1, f);
    buffer[bytes_read] = '\0';
    fclose(f);
    return bytes_read;
}

// ============================================================================
// TESTS - Basic File Operations
// ============================================================================

START_TEST(test_create_empty_file) {
    char filepath[512];
    get_test_path(filepath, sizeof(filepath), "empty.txt");

    FILE *f = fopen(filepath, "w");
    ck_assert_msg(f != NULL, "Failed to create file: %s", strerror(errno));
    fclose(f);

    // Verify file exists
    ck_assert_msg(access(filepath, F_OK) == 0, "File does not exist");
}
END_TEST

START_TEST(test_write_read_small_data) {
    char filepath[512];
    get_test_path(filepath, sizeof(filepath), "small.txt");

    const char *data = "Hello FAT32 World!";

    // Write data
    FILE *f = fopen(filepath, "w");
    ck_assert_ptr_nonnull(f);
    fprintf(f, "%s", data);
    fclose(f);

    // Read and verify
    char buffer[256] = {0};
    int bytes = read_file_content(filepath, buffer, sizeof(buffer));
    ck_assert_int_gt(bytes, 0);
    ck_assert_str_eq(buffer, data);
}
END_TEST

START_TEST(test_write_read_with_newlines) {
    char filepath[512];
    get_test_path(filepath, sizeof(filepath), "multiline.txt");

    const char *data = "Line 1\nLine 2\nLine 3\n";

    // Write
    FILE *f = fopen(filepath, "w");
    ck_assert_ptr_nonnull(f);
    fprintf(f, "%s", data);
    fclose(f);

    // Read
    char buffer[256] = {0};
    read_file_content(filepath, buffer, sizeof(buffer));
    ck_assert_str_eq(buffer, data);
}
END_TEST

START_TEST(test_overwrite_file) {
    char filepath[512];
    get_test_path(filepath, sizeof(filepath), "overwrite.txt");

    // Write initial data
    FILE *f = fopen(filepath, "w");
    ck_assert_ptr_nonnull(f);
    fprintf(f, "Initial data");
    fclose(f);

    // Overwrite with new data
    const char *new_data = "New data";
    f = fopen(filepath, "w");
    ck_assert_ptr_nonnull(f);
    fprintf(f, "%s", new_data);
    fclose(f);

    // Verify new data
    char buffer[256] = {0};
    read_file_content(filepath, buffer, sizeof(buffer));
    ck_assert_str_eq(buffer, new_data);
}
END_TEST

// ============================================================================
// TESTS - Cluster Operations
// ============================================================================

START_TEST(test_write_one_cluster) {
    char filepath[512];
    get_test_path(filepath, sizeof(filepath), "onecluster.txt");

    // Write exactly 512 bytes (1 cluster for FAT32)
    char data[513];
    memset(data, 'A', 512);
    data[512] = '\0';

    FILE *f = fopen(filepath, "wb");
    ck_assert_ptr_nonnull(f);
    size_t written = fwrite(data, 1, 512, f);
    fclose(f);
    ck_assert_int_eq(written, 512);

    // Verify size
    struct stat st;
    ck_assert_int_eq(stat(filepath, &st), 0);
    ck_assert_int_eq(st.st_size, 512);
}
END_TEST

START_TEST(test_write_multi_cluster) {
    char filepath[512];
    get_test_path(filepath, sizeof(filepath), "multicluster.txt");

    // Write 2048 bytes (4 clusters)
    const size_t data_size = 2048;
    FILE *f = fopen(filepath, "wb");
    ck_assert_ptr_nonnull(f);

    for (size_t i = 0; i < data_size; i++) {
        fputc('B', f);
    }
    fclose(f);

    // Verify size
    struct stat st;
    ck_assert_int_eq(stat(filepath, &st), 0);
    ck_assert_int_eq(st.st_size, data_size);

    // Verify content
    f = fopen(filepath, "rb");
    ck_assert_ptr_nonnull(f);
    for (size_t i = 0; i < data_size; i++) {
        int c = fgetc(f);
        ck_assert_int_eq(c, 'B');
    }
    fclose(f);
}
END_TEST

START_TEST(test_write_large_file) {
    char filepath[512];
    get_test_path(filepath, sizeof(filepath), "large.txt");

    // Write 10KB (20 clusters)
    const size_t data_size = 10240;
    FILE *f = fopen(filepath, "wb");
    ck_assert_ptr_nonnull(f);

    // Write pattern that we can verify
    for (size_t i = 0; i < data_size; i++) {
        fputc(i % 256, f);
    }
    fclose(f);

    // Verify size
    struct stat st;
    ck_assert_int_eq(stat(filepath, &st), 0);
    ck_assert_int_eq(st.st_size, data_size);

    // Verify pattern
    f = fopen(filepath, "rb");
    ck_assert_ptr_nonnull(f);
    for (size_t i = 0; i < data_size; i++) {
        int c = fgetc(f);
        ck_assert_int_eq(c, i % 256);
    }
    fclose(f);
}
END_TEST

// ============================================================================
// TESTS - Directory Operations
// ============================================================================

START_TEST(test_mkdir) {
    char dirpath[512];
    get_test_path(dirpath, sizeof(dirpath), "testdir");

    int ret = mkdir(dirpath, 0755);
    ck_assert_msg(ret == 0, "mkdir failed: %s", strerror(errno));

    // Verify it's a directory
    struct stat st;
    ck_assert_int_eq(stat(dirpath, &st), 0);
    ck_assert(S_ISDIR(st.st_mode));
}
END_TEST

START_TEST(test_nested_directories) {
    char dir1[512], dir2[512], dir3[512];
    get_test_path(dir1, sizeof(dir1), "level1");
    snprintf(dir2, sizeof(dir2), "%s/level2", dir1);
    snprintf(dir3, sizeof(dir3), "%s/level3", dir2);

    // Create nested directories
    ck_assert_int_eq(mkdir(dir1, 0755), 0);
    ck_assert_int_eq(mkdir(dir2, 0755), 0);
    ck_assert_int_eq(mkdir(dir3, 0755), 0);

    // Verify all exist
    ck_assert_int_eq(access(dir1, F_OK), 0);
    ck_assert_int_eq(access(dir2, F_OK), 0);
    ck_assert_int_eq(access(dir3, F_OK), 0);
}
END_TEST

START_TEST(test_file_in_subdirectory) {
    char dirpath[512], filepath[512];
    get_test_path(dirpath, sizeof(dirpath), "subdir");
    snprintf(filepath, sizeof(filepath), "%s/file.txt", dirpath);

    // Create directory
    ck_assert_int_eq(mkdir(dirpath, 0755), 0);

    // Create file in subdirectory
    const char *data = "File in subdirectory";
    FILE *f = fopen(filepath, "w");
    ck_assert_ptr_nonnull(f);
    fprintf(f, "%s", data);
    fclose(f);

    // Verify content
    char buffer[256] = {0};
    read_file_content(filepath, buffer, sizeof(buffer));
    ck_assert_str_eq(buffer, data);
}
END_TEST

// ============================================================================
// TESTS - Delete Operations
// ============================================================================

START_TEST(test_unlink_file) {
    char filepath[512];
    get_test_path(filepath, sizeof(filepath), "todelete.txt");

    // Create file
    FILE *f = fopen(filepath, "w");
    ck_assert_ptr_nonnull(f);
    fprintf(f, "delete me");
    fclose(f);

    // Verify exists
    ck_assert_int_eq(access(filepath, F_OK), 0);

    // Delete
    ck_assert_int_eq(unlink(filepath), 0);

    // Verify doesn't exist
    ck_assert_int_eq(access(filepath, F_OK), -1);
    ck_assert_int_eq(errno, ENOENT);
}
END_TEST

START_TEST(test_rmdir_empty) {
    char dirpath[512];
    get_test_path(dirpath, sizeof(dirpath), "emptydir");

    // Create directory
    ck_assert_int_eq(mkdir(dirpath, 0755), 0);

    // Remove it
    ck_assert_int_eq(rmdir(dirpath), 0);

    // Verify doesn't exist
    ck_assert_int_eq(access(dirpath, F_OK), -1);
}
END_TEST

START_TEST(test_rmdir_with_file) {
    char dirpath[512], filepath[512];
    get_test_path(dirpath, sizeof(dirpath), "nonemptydir");
    snprintf(filepath, sizeof(filepath), "%s/file.txt", dirpath);

    // Create directory with file
    ck_assert_int_eq(mkdir(dirpath, 0755), 0);
    FILE *f = fopen(filepath, "w");
    ck_assert_ptr_nonnull(f);
    fprintf(f, "content");
    fclose(f);

    // Try to remove directory (should fail - not empty)
    int ret = rmdir(dirpath);
    ck_assert_msg(ret == -1, "rmdir should fail on non-empty directory");
    ck_assert_int_eq(errno, ENOTEMPTY);

    // Remove file first, then directory
    ck_assert_int_eq(unlink(filepath), 0);
    ck_assert_int_eq(rmdir(dirpath), 0);
}
END_TEST

// ============================================================================
// RENAME/MOVE OPERATIONS
// ============================================================================

START_TEST(test_rename_file_same_dir) {
    char oldpath[512], newpath[512];
    const char *content = "Test rename";

    snprintf(oldpath, sizeof(oldpath), "%s/oldname.txt", mount_point);
    snprintf(newpath, sizeof(newpath), "%s/newname.txt", mount_point);

    // Create file with old name
    int fd = open(oldpath, O_CREAT | O_WRONLY, 0644);
    ck_assert_msg(fd >= 0, "Failed to create file: %s", strerror(errno));
    write(fd, content, strlen(content));
    close(fd);

    // Rename file
    ck_assert_int_eq(rename(oldpath, newpath), 0);

    // Old name should not exist
    ck_assert_int_eq(access(oldpath, F_OK), -1);
    ck_assert_int_eq(errno, ENOENT);

    // New name should exist with same content
    fd = open(newpath, O_RDONLY);
    ck_assert_msg(fd >= 0, "Failed to open renamed file");
    char buf[100];
    ssize_t bytes = read(fd, buf, sizeof(buf));
    close(fd);
    ck_assert_int_eq(bytes, strlen(content));
    ck_assert_mem_eq(buf, content, strlen(content));
}
END_TEST

START_TEST(test_rename_file_to_subdir) {
    char filepath[512], dirpath[512], newpath[512];
    const char *content = "Move to subdir";

    snprintf(filepath, sizeof(filepath), "%s/file.txt", mount_point);
    snprintf(dirpath, sizeof(dirpath), "%s/subdir", mount_point);
    snprintf(newpath, sizeof(newpath), "%s/subdir/file.txt", mount_point);

    // Create file in root
    int fd = open(filepath, O_CREAT | O_WRONLY, 0644);
    ck_assert_msg(fd >= 0, "Failed to create file");
    write(fd, content, strlen(content));
    close(fd);

    // Create subdirectory
    ck_assert_int_eq(mkdir(dirpath, 0755), 0);

    // Move file to subdirectory
    ck_assert_int_eq(rename(filepath, newpath), 0);

    // Old location should not exist
    ck_assert_int_eq(access(filepath, F_OK), -1);

    // New location should exist with same content
    fd = open(newpath, O_RDONLY);
    ck_assert_msg(fd >= 0, "Failed to open moved file");
    char buf[100];
    ssize_t bytes = read(fd, buf, sizeof(buf));
    close(fd);
    ck_assert_int_eq(bytes, strlen(content));
    ck_assert_mem_eq(buf, content, strlen(content));
}
END_TEST

START_TEST(test_rename_directory) {
    char olddir[512], newdir[512], filepath[512], newfilepath[512];

    snprintf(olddir, sizeof(olddir), "%s/olddir", mount_point);
    snprintf(newdir, sizeof(newdir), "%s/newdir", mount_point);
    snprintf(filepath, sizeof(filepath), "%s/olddir/file.txt", mount_point);
    snprintf(newfilepath, sizeof(newfilepath), "%s/newdir/file.txt",
             mount_point);

    // Create directory with a file
    ck_assert_int_eq(mkdir(olddir, 0755), 0);
    int fd = open(filepath, O_CREAT | O_WRONLY, 0644);
    ck_assert_msg(fd >= 0, "Failed to create file in directory");
    write(fd, "test", 4);
    close(fd);

    // Rename directory
    ck_assert_int_eq(rename(olddir, newdir), 0);

    // Old directory should not exist
    ck_assert_int_eq(access(olddir, F_OK), -1);

    // New directory should exist
    struct stat st;
    ck_assert_int_eq(stat(newdir, &st), 0);
    ck_assert_msg(S_ISDIR(st.st_mode), "Renamed path should be a directory");

    // File should be accessible at new path
    ck_assert_int_eq(access(newfilepath, F_OK), 0);
}
END_TEST

START_TEST(test_rename_nonexistent_fails) {
    char oldpath[512], newpath[512];

    snprintf(oldpath, sizeof(oldpath), "%s/nonexist.txt", mount_point);
    snprintf(newpath, sizeof(newpath), "%s/newname.txt", mount_point);

    // Try to rename non-existent file (should fail)
    int ret = rename(oldpath, newpath);
    ck_assert_msg(ret == -1, "rename should fail for non-existent file");
    ck_assert_int_eq(errno, ENOENT);
}
END_TEST

// ============================================================================
// TEST SUITE SETUP
// ============================================================================

Suite *fat_operations_suite(void) {
    Suite *s = suite_create("FAT Operations");

    // Single test case with all tests
    // This ensures setup_mount_fs runs only ONCE for all tests
    TCase *tc_all = tcase_create("All Operations");
    tcase_add_unchecked_fixture(tc_all, setup_mount_fs, teardown_unmount_fs);
    tcase_add_checked_fixture(tc_all, setup_clean_dir, teardown_verify);
    tcase_set_timeout(tc_all, 60); // Allow up to 60s per test

    // Basic operations
    tcase_add_test(tc_all, test_create_empty_file);
    tcase_add_test(tc_all, test_write_read_small_data);
    tcase_add_test(tc_all, test_write_read_with_newlines);
    tcase_add_test(tc_all, test_overwrite_file);

    // Cluster operations
    tcase_add_test(tc_all, test_write_one_cluster);
    tcase_add_test(tc_all, test_write_multi_cluster);
    tcase_add_test(tc_all, test_write_large_file);

    // Directory operations
    tcase_add_test(tc_all, test_mkdir);
    tcase_add_test(tc_all, test_nested_directories);
    tcase_add_test(tc_all, test_file_in_subdirectory);

    // Delete operations
    tcase_add_test(tc_all, test_unlink_file);
    tcase_add_test(tc_all, test_rmdir_empty);
    tcase_add_test(tc_all, test_rmdir_with_file);

    // Rename/move operations
    tcase_add_test(tc_all, test_rename_file_same_dir);
    tcase_add_test(tc_all, test_rename_file_to_subdir);
    tcase_add_test(tc_all, test_rename_directory);
    tcase_add_test(tc_all, test_rename_nonexistent_fails);

    suite_add_tcase(s, tc_all);
    return s;
}

// ============================================================================
// MAIN
// ============================================================================

int main(void) {
    int number_failed;
    Suite *s = fat_operations_suite();
    SRunner *sr = srunner_create(s);

    // Run tests
    srunner_run_all(sr, CK_VERBOSE);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

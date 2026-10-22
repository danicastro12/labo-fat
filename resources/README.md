# Test Resources

This directory contains FAT32 filesystem images for testing and experimentation.

## Images

### test_empty.img
**Purpose:** Base image for functional tests

**Specifications:**
- Format: FAT32
- Size: 35 MB (minimum for FAT32)
- State: Empty filesystem (no files)
- Volume Label: TESTIMG

**Usage:**
- Used by `make test-ops` (functional tests)
- Tests copy this image to `/tmp/` and mount it
- Ensures tests start with clean, known state
- Git tracks if tests accidentally modify this file

**Regeneration:**
If you need to recreate this image:

```bash
# Create empty 35MB file
dd if=/dev/zero of=test_empty.img count=35 bs=1M

# Format as FAT32
# Linux:
mkfs.vfat -F 32 -v test_empty.img

# macOS:
hdiutil attach -nomount test_empty.img  # Note the /dev/diskN
newfs_msdos -F 32 -v TESTIMG /dev/diskN
hdiutil detach /dev/diskN
```

### fatfs.img
**Purpose:** General purpose test image for experimentation

**Specifications:**
- Format: FAT32
- Size: 35 MB
- State: May contain test files

**Usage:**
- Available for manual testing and experimentation
- Mount with: `./fat-fuse resources/fatfs.img ./mnt`
- Not used by automated tests

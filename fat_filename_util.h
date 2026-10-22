#ifndef _FAT_FILENAME_UTIL_H
#define _FAT_FILENAME_UTIL_H

#include "fat_types.h"
#include <string.h>
#include <sys/types.h>

#define FAT_FILENAME_DELETED_CHAR 0xe5

/* Compares the bytes of @s1 and @s2.
 * FAT names can be no more than 12 characters long, so we might as well just
 * inline strcmp() to make comparisons faster. */
int inline_strcmp(const char *s1, const char *s2);

/* Returns true iff @base_name of up to 8 bytes is valid. */
bool file_basename_valid(const u8 base_name[8]);

/* Returns true if @extension of up to 3 bytes is valid. */
bool file_extension_valid(const u8 extension[3]);

/* Return the actual length of a FAT base name or extension that may be up to
 * @max_len bytes long. Treats space and null character as ending the name.
 * If there is no ending character in name up to max_len position, the function
 * returns 0.*/
unsigned filename_len(const char *name, unsigned max_len);

/**
 * @brief Build a filename string from FAT base name and extension
 *
 * Constructs a filename from the 8-byte base name and 3-byte extension
 * stored in FAT directory entries. If the filename has an extension,
 * the result is "basename.ext", otherwise just "basename".
 *
 * @param src_name_p Base name (up to 8 bytes, space-padded)
 * @param src_extension_p Extension (up to 3 bytes, space-padded)
 * @return Newly allocated string containing the filename. Caller owns the
 *         pointer and must free it.
 */
char *build_filename(const u8 *src_name_p, const u8 *src_extension_p);

/**
 * @brief Split a filepath to extract FAT-compatible base name and extension
 *
 * Extracts the filename from a filepath and splits it into the 8-byte base name
 * and 3-byte extension format used in FAT directory entries. The base name and
 * extension are space-padded as required by the FAT specification.
 *
 * @param src_name_p Filepath or filename string
 * @param base Output buffer for base name (8 bytes, space-padded)
 * @param extension Output buffer for extension (3 bytes, space-padded)
 */
void split_filepath_to_filename(char *src_name_p, u8 *base, u8 *extension);

/**
 * @brief Build a full filepath from parent directory path and filename
 *
 * Concatenates a parent directory path with a filename, adding a path separator
 * if needed, to create a full filepath.
 *
 * @param parent_filepath Path to parent directory
 * @param file_name Filename to append
 * @return Newly allocated string containing the full filepath. Caller owns the
 *         pointer and must free it.
 */
char *build_filepath_from_name(char *parent_filepath, char *file_name);

#endif /* _FAT_FILENAME_UTIL_H */

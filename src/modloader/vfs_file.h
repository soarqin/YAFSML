#pragma once

#include "vfs.h"
#include "zip_package.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Internal constructor; path lookup and identity assignment belong to vfs.c.
 * Seekable files of the same identity share one decoded cache. */
vfs_file_t *vfs_file_create(zip_package_t *package, size_t entry, uint64_t identity, bool streaming);
/* Forgets every cache, before the identities that name them can change meaning
 * and at shutdown; a cache that open files still use is freed with the last of
 * them. */
void vfs_file_drop_caches(void);

#ifdef ML_VFS_FILE_TEST
void vfs_file_test_limits(size_t file_limit, size_t total_limit);
void vfs_file_test_idle_disk_limit(uint64_t limit);
uint64_t vfs_file_test_memory_used(void);
HANDLE vfs_file_test_cache_handle(const vfs_file_t *file);
bool vfs_file_test_is_stream(const vfs_file_t *file);
#endif

#ifdef __cplusplus
}
#endif

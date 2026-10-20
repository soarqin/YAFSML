#pragma once

#include "vfs.h"
#include "zip_package.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Internal constructor; path lookup and identity assignment belong to vfs.c. */
vfs_file_t *vfs_file_create(zip_package_t *package, size_t entry, uint64_t identity, bool streaming);

#ifdef ML_VFS_FILE_TEST
void vfs_file_test_limits(size_t file_limit, size_t total_limit);
uint64_t vfs_file_test_memory_used(void);
HANDLE vfs_file_test_cache_handle(const vfs_file_t *file);
bool vfs_file_test_is_stream(const vfs_file_t *file);
#endif

#ifdef __cplusplus
}
#endif

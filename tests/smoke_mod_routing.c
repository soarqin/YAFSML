#include <stdio.h>

#include "test_common.h"
#include "modloader/mod.h"
#include "modloader/vfs.h"

static LONG query_count;

wchar_t *config_full_path_alloc(const wchar_t *filename) {
    (void)filename;
    return NULL;
}

void vfs_init(void) {}
void vfs_uninit(void) {}
bool vfs_add_package(const wchar_t *path) { (void)path; return false; }

const wchar_t *vfs_lookup(const wchar_t *path) {
    (void)path;
    InterlockedIncrement(&query_count);
    return L"unexpected";
}

const wchar_t *vfs_lookup_prefixed_domain(const wchar_t *path, const wchar_t *game_root,
                                          vfs_lookup_domain_t domain) {
    (void)path;
    (void)game_root;
    (void)domain;
    InterlockedIncrement(&query_count);
    return L"unexpected";
}

const wchar_t *vfs_virtual_to_uid_prefixed(const wchar_t *path, const wchar_t *game_root) {
    (void)path;
    (void)game_root;
    InterlockedIncrement(&query_count);
    return L"unexpected";
}

const wchar_t *vfs_uid_to_path(const wchar_t *uid) {
    (void)uid;
    InterlockedIncrement(&query_count);
    return L"unexpected";
}

const wchar_t *vfs_route_read_path_prefixed(const wchar_t *path, const wchar_t *game_root,
                                            DWORD desired_access, DWORD creation_disposition) {
    (void)path;
    (void)game_root;
    (void)desired_access;
    (void)creation_disposition;
    InterlockedIncrement(&query_count);
    return L"unexpected";
}

int main(void) {
    EXPECT_EQ(mods_count(), 0);
    EXPECT_NULL(mods_file_search(L"parts/test.bin"));
    EXPECT_NULL(mods_file_search_prefixed_domain(
        L"C:\\game\\parts\\test.bin", VFS_LOOKUP_DISK_WIDE));
    EXPECT_NULL(mods_file_virtual_to_uid_prefixed(L"C:\\game\\parts\\test.bin"));
    EXPECT_NULL(mods_file_route_read(L"C:\\game\\parts\\test.bin", GENERIC_READ, OPEN_EXISTING));
    EXPECT_EQ(query_count, 0);

    printf("smoke_mod_routing: all tests passed\n");
    return 0;
}

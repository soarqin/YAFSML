#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>

#include "test_common.h"
#include "modloader/vfs.h"

static volatile LONG64 allocation_count;

void *ml_mem_alloc(UINT flags, size_t size) {
    void *result = LocalAlloc(flags, size);
    if (result != NULL) InterlockedIncrement64(&allocation_count);
    return result;
}

void *ml_mem_realloc(void *ptr, size_t size, UINT flags) {
    void *result = LocalReAlloc(ptr, size, flags);
    if (result != NULL) InterlockedIncrement64(&allocation_count);
    return result;
}

void ml_mem_free(void *ptr) {
    LocalFree(ptr);
}

char *ml_mem_strdup_a(const char *str) {
    char *result = StrDupA(str);
    if (result != NULL) InterlockedIncrement64(&allocation_count);
    return result;
}

wchar_t *ml_mem_strdup_w(const wchar_t *str) {
    wchar_t *result = StrDupW(str);
    if (result != NULL) InterlockedIncrement64(&allocation_count);
    return result;
}

int main(void) {
    wchar_t root[MAX_PATH];
    wchar_t first[MAX_PATH];
    wchar_t second[MAX_PATH];
    wchar_t late[MAX_PATH];
    wchar_t path[MAX_PATH];
    wchar_t hks_path[MAX_PATH];
    wchar_t late_path[MAX_PATH];
    wchar_t *normalized = NULL;
    HANDLE file;

    EXPECT_TRUE(GetTempPathW(MAX_PATH, root) != 0);
    EXPECT_TRUE(GetTempFileNameW(root, L"vfs", 0, root) != 0);
    DeleteFileW(root);
    EXPECT_TRUE(CreateDirectoryW(root, NULL));
    lstrcpyW(first, root); PathAppendW(first, L"first");
    lstrcpyW(second, root); PathAppendW(second, L"second");
    lstrcpyW(late, root); PathAppendW(late, L"late");
    EXPECT_TRUE(CreateDirectoryW(first, NULL));
    EXPECT_TRUE(CreateDirectoryW(second, NULL));
    EXPECT_TRUE(CreateDirectoryW(late, NULL));
    lstrcpyW(path, first); PathAppendW(path, L"parts"); EXPECT_TRUE(CreateDirectoryW(path, NULL));
    PathAppendW(path, L"test.bin");
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL); EXPECT_TRUE(file != INVALID_HANDLE_VALUE); CloseHandle(file);
    lstrcpyW(path, second); PathAppendW(path, L"parts"); EXPECT_TRUE(CreateDirectoryW(path, NULL));
    PathAppendW(path, L"test.bin");
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL); EXPECT_TRUE(file != INVALID_HANDLE_VALUE); CloseHandle(file);
    lstrcpyW(hks_path, second); PathAppendW(hks_path, L"action"); EXPECT_TRUE(CreateDirectoryW(hks_path, NULL));
    PathAppendW(hks_path, L"script"); EXPECT_TRUE(CreateDirectoryW(hks_path, NULL));
    PathAppendW(hks_path, L"modules"); EXPECT_TRUE(CreateDirectoryW(hks_path, NULL));
    PathAppendW(hks_path, L"convergence"); EXPECT_TRUE(CreateDirectoryW(hks_path, NULL));
    PathAppendW(hks_path, L"Update.hks");
    file = CreateFileW(hks_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL); EXPECT_TRUE(file != INVALID_HANDLE_VALUE); CloseHandle(file);

    EXPECT_TRUE(vfs_normalize_path(L"\\PARTS/./test.bin\0", &normalized));
    EXPECT_STREQ_W(normalized, L"parts\\test.bin");
    LocalFree(normalized);
    EXPECT_TRUE(vfs_normalize_path(L"data0:/PARAM/gameparam/gameparam.parambnd.dcx", &normalized));
    EXPECT_STREQ_W(normalized, L"param\\gameparam\\gameparam.parambnd.dcx");
    LocalFree(normalized);
    EXPECT_TRUE(!vfs_normalize_path(L"..\\test.bin", &normalized));
    {
        wchar_t deep[1600];
        size_t offset = 0;
        for (size_t i = 0; i < 600; i++) {
            deep[offset++] = L'a';
            deep[offset++] = L'/';
        }
        deep[offset] = L'\0';
        EXPECT_TRUE(vfs_normalize_path(deep, &normalized));
        EXPECT_EQ(wcslen(normalized), 1199);
        LocalFree(normalized);
        deep[offset++] = L'.';
        deep[offset++] = L'.';
        deep[offset++] = L'/';
        deep[offset++] = L'.';
        deep[offset++] = L'.';
        deep[offset] = L'\0';
        EXPECT_TRUE(vfs_normalize_path(deep, &normalized));
        EXPECT_EQ(wcslen(normalized), 1195);
        LocalFree(normalized);
    }

    vfs_init();
    EXPECT_TRUE(vfs_add_package(first));
    EXPECT_TRUE(vfs_add_package(second));
    EXPECT_EQ(vfs_generation(), 0);
    EXPECT_NOT_NULL(vfs_lookup(L"parts/test.bin"));
    EXPECT_NOT_NULL(vfs_lookup(L"data0:/parts/test.bin"));
    {
        const wchar_t *uncached_uid = vfs_virtual_to_uid(L"parts/test.bin");
        EXPECT_NOT_NULL(uncached_uid);
        EXPECT_TRUE(wcscmp(uncached_uid, L"\\\\me3??0") == 0);
        EXPECT_STREQ_W(vfs_uid_to_path(uncached_uid), vfs_lookup(L"parts/test.bin"));
    }
    lstrcpyW(late_path, late); PathAppendW(late_path, L"parts");
    EXPECT_TRUE(CreateDirectoryW(late_path, NULL));
    PathAppendW(late_path, L"late.bin");
    file = CreateFileW(late_path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    CloseHandle(file);
    EXPECT_NULL(vfs_virtual_to_uid(L"parts/late.bin"));
    EXPECT_TRUE(vfs_add_package(late));
    EXPECT_NOT_NULL(vfs_virtual_to_uid(L"parts/late.bin"));
    EXPECT_EQ(vfs_generation(), 0);
    EXPECT_EQ(vfs_reset_lookup_caches(), 1);
    EXPECT_EQ(vfs_generation(), 1);
    EXPECT_TRUE(wcsncmp(vfs_lookup(L"parts/test.bin"), second, wcslen(second)) == 0);
    {
        wchar_t absolute[MAX_PATH];
        const wchar_t *prefixed_uid;
        lstrcpyW(absolute, root); PathAppendW(absolute, L"parts/test.bin");
        EXPECT_STREQ_W(vfs_lookup_prefixed_domain(absolute, root, VFS_LOOKUP_DISK_WIDE),
                       vfs_lookup(L"parts/test.bin"));
        prefixed_uid = vfs_virtual_to_uid_prefixed(absolute, root);
        EXPECT_NOT_NULL(prefixed_uid);
        EXPECT_STREQ_W(vfs_uid_to_path(prefixed_uid), vfs_lookup(L"parts/test.bin"));
        lstrcpyW(absolute, root); lstrcatW(absolute, L"-outside\\parts\\test.bin");
        EXPECT_NULL(vfs_lookup_prefixed_domain(absolute, root, VFS_LOOKUP_DISK_WIDE));
        EXPECT_NULL(vfs_virtual_to_uid_prefixed(absolute, root));
    }
    {
        static const wchar_t game_root[] = L"D:\\Steam\\steamapps\\common\\ELDEN RING\\Game";
        static const wchar_t request[] = L"d:/steam/steamapps/common/elden ring/game/action/script/modules/convergence/Update.hks";
        const wchar_t *override = vfs_lookup(L"action/script/modules/convergence/Update.hks");
        const wchar_t *uid;
        EXPECT_NOT_NULL(override);
        EXPECT_STREQ_W(vfs_lookup_prefixed_domain(request, game_root, VFS_LOOKUP_DISK_WIDE), override);
        uid = vfs_virtual_to_uid_prefixed(request, game_root);
        EXPECT_NOT_NULL(uid);
        EXPECT_STREQ_W(vfs_uid_to_path(uid), override);
        EXPECT_STREQ_W(vfs_route_read_path_prefixed(request, game_root, GENERIC_READ, OPEN_EXISTING), override);
        EXPECT_NULL(vfs_route_read_path_prefixed(
            L"d:/steam/steamapps/common/elden ring/game-old/action/script/modules/convergence/Update.hks",
            game_root, GENERIC_READ, OPEN_EXISTING));
    }
    EXPECT_STREQ_W(vfs_lookup_domain(L"parts/test.bin", VFS_LOOKUP_VIRTUAL), vfs_lookup(L"parts/test.bin"));
    EXPECT_STREQ_W(vfs_lookup_domain(L"parts/test.bin", VFS_LOOKUP_DISK_WIDE), vfs_lookup(L"parts/test.bin"));
    EXPECT_STREQ_W(vfs_lookup_domain(L"parts/test.bin", VFS_LOOKUP_DISK_ANSI), vfs_lookup(L"parts/test.bin"));
    EXPECT_STREQ_W(vfs_lookup_domain(L"parts/test.bin", VFS_LOOKUP_WWISE), vfs_lookup(L"parts/test.bin"));
    InterlockedExchange64(&allocation_count, 0);
    EXPECT_NOT_NULL(vfs_lookup_domain(L"parts/test.bin", VFS_LOOKUP_WWISE));
    EXPECT_EQ(allocation_count, 0);
    EXPECT_NULL(vfs_lookup_domain(L"parts/test.bin", VFS_LOOKUP_DOMAIN_COUNT));
    EXPECT_NULL(vfs_lookup_domain(L"parts/domain-missing.bin", VFS_LOOKUP_VIRTUAL));
    EXPECT_NULL(vfs_lookup_domain(L"parts/domain-missing.bin", VFS_LOOKUP_WWISE));
    EXPECT_NOT_NULL(vfs_route_read_path(L"parts/test.bin", GENERIC_READ, OPEN_EXISTING));
    EXPECT_NULL(vfs_route_read_path(L"parts/test.bin", GENERIC_WRITE, OPEN_EXISTING));
    EXPECT_NULL(vfs_route_read_path(L"parts/test.bin", GENERIC_READ, CREATE_ALWAYS));
    EXPECT_TRUE(vfs_register_writable_path(L"settings/test.ini", L"C:\\profile\\test.ini"));
    EXPECT_TRUE(!vfs_register_writable_path(L"", L"C:\\profile\\test.ini"));
    EXPECT_TRUE(!vfs_register_writable_path(L"settings/empty.ini", L""));
    EXPECT_TRUE(vfs_has_writable_paths());
    EXPECT_STREQ_W(vfs_route_writable_path(L"settings/test.ini"), L"C:\\profile\\test.ini");
    InterlockedExchange64(&allocation_count, 0);
    EXPECT_STREQ_W(vfs_route_writable_path(L"settings/test.ini"), L"C:\\profile\\test.ini");
    EXPECT_EQ(allocation_count, 0);
    /* Drive-relative paths normalize by stripping the `C:` prefix, so the
     * writable-path pre-filter must not reject the raw final segment. */
    EXPECT_TRUE(vfs_register_writable_path(L"C:drive-relative.ini", L"C:\\profile\\drive-relative.ini"));
    EXPECT_STREQ_W(vfs_route_writable_path(L"C:drive-relative.ini"),
                   L"C:\\profile\\drive-relative.ini");
    EXPECT_TRUE(vfs_register_writable_path(L"SETTINGS\\test.ini", L"C:\\profile\\test.ini"));
    EXPECT_TRUE(!vfs_register_writable_path(L"settings/test.ini", L"C:\\other\\test.ini"));
    EXPECT_STREQ_W(vfs_route_read_path(L"settings/test.ini", GENERIC_WRITE, CREATE_ALWAYS), L"C:\\profile\\test.ini");
    vfs_recursion_guard_enter();
    EXPECT_NULL(vfs_route_read_path(L"parts/test.bin", GENERIC_READ, OPEN_EXISTING));
    vfs_recursion_guard_leave();
    EXPECT_NULL(vfs_lookup(L"parts/missing.bin"));
    {
        const wchar_t *uid = vfs_virtual_to_uid(L"parts/test.bin");
        const wchar_t *cached_uid;
        EXPECT_NOT_NULL(uid);
        EXPECT_TRUE(wcscmp(uid, L"\\\\me3??0") == 0);
        InterlockedExchange64(&allocation_count, 0);
        cached_uid = vfs_virtual_to_uid(L"parts/test.bin");
        EXPECT_NOT_NULL(cached_uid);
        EXPECT_STREQ_W(cached_uid, uid);
        EXPECT_TRUE(cached_uid == uid);
        EXPECT_EQ(allocation_count, 0);
        EXPECT_NULL(vfs_virtual_to_uid(L"parts/uid-missing.bin"));
        InterlockedExchange64(&allocation_count, 0);
        EXPECT_NULL(vfs_virtual_to_uid(L"parts/uid-missing.bin"));
        EXPECT_EQ(allocation_count, 0);
        EXPECT_STREQ_W(vfs_uid_to_path(uid), vfs_lookup(L"parts/test.bin"));
        EXPECT_STREQ_W(vfs_lookup(uid), vfs_lookup(L"parts/test.bin"));
        vfs_begin_lookup_reset();
        EXPECT_EQ(vfs_generation(), 0);
        EXPECT_STREQ_W(vfs_uid_to_path(uid), vfs_lookup(L"parts/test.bin"));
        EXPECT_EQ(vfs_reset_lookup_caches(), 2);
        EXPECT_EQ(vfs_generation(), 2);
        cached_uid = vfs_virtual_to_uid(L"parts/test.bin");
        EXPECT_NOT_NULL(cached_uid);
        EXPECT_TRUE(wcscmp(cached_uid, uid) == 0);
        EXPECT_NULL(vfs_uid_to_path(L"\\\\me3??ffffffff"));
        EXPECT_NULL(vfs_uid_to_path(L"\\\\me3??-2"));
        EXPECT_NULL(vfs_uid_to_path(L"\\\\me3?? 2"));
    }
    vfs_uninit();

    DeleteFileW(path);
    DeleteFileW(late_path);
    DeleteFileW(hks_path);
    lstrcpyW(hks_path, second); PathAppendW(hks_path, L"action\\script\\modules\\convergence"); RemoveDirectoryW(hks_path);
    lstrcpyW(hks_path, second); PathAppendW(hks_path, L"action\\script\\modules"); RemoveDirectoryW(hks_path);
    lstrcpyW(hks_path, second); PathAppendW(hks_path, L"action\\script"); RemoveDirectoryW(hks_path);
    lstrcpyW(hks_path, second); PathAppendW(hks_path, L"action"); RemoveDirectoryW(hks_path);
    lstrcpyW(path, first); PathAppendW(path, L"parts\\test.bin"); DeleteFileW(path);
    lstrcpyW(path, second); PathAppendW(path, L"parts"); RemoveDirectoryW(path);
    lstrcpyW(path, first); PathAppendW(path, L"parts"); RemoveDirectoryW(path);
    lstrcpyW(path, late); PathAppendW(path, L"parts"); RemoveDirectoryW(path);
    RemoveDirectoryW(late); RemoveDirectoryW(second); RemoveDirectoryW(first); RemoveDirectoryW(root);
    printf("smoke_vfs: all tests passed\n");
    return 0;
}

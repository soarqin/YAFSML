#include <stdio.h>

#include "test_common.h"
#include "modloader/patches/win32_hooks.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const wchar_t *save_fail_path;
static const wchar_t *read_route_path;
static const wchar_t *writable_route_path;
static const wchar_t *route_target;
static bool recursion_active;

bool ml_save_mapping_route(const wchar_t *path, const wchar_t **mapped) {
    if (mapped != NULL) *mapped = NULL;
    return path != NULL && save_fail_path != NULL && lstrcmpW(path, save_fail_path) == 0;
}

bool vfs_recursion_guard_active(void) {
    return recursion_active;
}

const wchar_t *vfs_route_writable_path(const wchar_t *path) {
    return path != NULL && writable_route_path != NULL && lstrcmpW(path, writable_route_path) == 0
        ? route_target : NULL;
}

const wchar_t *mods_file_route_read(const wchar_t *path, DWORD access, DWORD disposition) {
    if (path == NULL || read_route_path == NULL || lstrcmpW(path, read_route_path) != 0 ||
        (access & (GENERIC_WRITE | DELETE)) != 0 || disposition != OPEN_EXISTING) return NULL;
    return route_target;
}

wchar_t *ml_path_from_ansi(const char *path) {
    int length;
    wchar_t *wide;
    if (path == NULL) return NULL;
    length = MultiByteToWideChar(CP_ACP, 0, path, -1, NULL, 0);
    if (length <= 0) return NULL;
    wide = LocalAlloc(0, (size_t)length * sizeof(*wide));
    if (wide == NULL || MultiByteToWideChar(CP_ACP, 0, path, -1, wide, length) == 0) {
        LocalFree(wide);
        return NULL;
    }
    return wide;
}

int main(void) {
    wchar_t root[MAX_PATH];
    wchar_t source[MAX_PATH];
    wchar_t directory[MAX_PATH];
    wchar_t read_route[MAX_PATH];
    wchar_t writable_route[MAX_PATH];
    wchar_t save_fail[MAX_PATH];
    wchar_t copy[MAX_PATH];
    wchar_t moved[MAX_PATH];
    HANDLE file;
    typedef HANDLE (WINAPI *create_file_w_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES,
                                             DWORD, DWORD, HANDLE);
    create_file_w_t kernelbase_create_file_w;
    DWORD length = GetTempPathW(MAX_PATH, root);
    EXPECT_EQ(ml_win32_file_hooks_test_target("CreateFileW"),
              GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "CreateFileW"));
    EXPECT_TRUE(ml_win32_file_hooks_test_target("CreateFileW") != (void *)CreateFileW);
    EXPECT_TRUE(length != 0 && length < MAX_PATH);
    EXPECT_TRUE(GetTempFileNameW(root, L"mlh", 0, source) != 0);
    EXPECT_TRUE(GetTempFileNameW(root, L"mld", 0, directory) != 0);
    EXPECT_TRUE(DeleteFileW(directory));
    EXPECT_TRUE(GetTempFileNameW(root, L"mlr", 0, read_route) != 0);
    EXPECT_TRUE(DeleteFileW(read_route));
    EXPECT_TRUE(GetTempFileNameW(root, L"mlw", 0, writable_route) != 0);
    EXPECT_TRUE(DeleteFileW(writable_route));
    EXPECT_TRUE(GetTempFileNameW(root, L"mls", 0, save_fail) != 0);
    EXPECT_TRUE(DeleteFileW(save_fail));
    EXPECT_TRUE(GetTempFileNameW(root, L"mlc", 0, copy) != 0);
    EXPECT_TRUE(DeleteFileW(copy));
    EXPECT_TRUE(GetTempFileNameW(root, L"mlm", 0, moved) != 0);
    EXPECT_TRUE(DeleteFileW(moved));

    ml_win32_file_hooks_test_init();
    route_target = source;
    read_route_path = read_route;
    writable_route_path = writable_route;
    save_fail_path = save_fail;
    kernelbase_create_file_w = (create_file_w_t)ml_win32_file_hooks_test_target("CreateFileW");
    file = kernelbase_create_file_w(source, GENERIC_READ, FILE_SHARE_READ, NULL,
                                    OPEN_EXISTING, 0, NULL);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    CloseHandle(file);
    file = ml_win32_file_hooks_test_create_file_w(source, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    CloseHandle(file);
    file = ml_win32_file_hooks_test_create_file_w(read_route, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    CloseHandle(file);
    file = ml_win32_file_hooks_test_create_file_2(read_route, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    CloseHandle(file);
    file = ml_win32_file_hooks_test_create_file_w(writable_route, GENERIC_WRITE, 0, OPEN_EXISTING, 0);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    CloseHandle(file);
    /* An explicit save mapping that cannot produce its alternate path must not
     * fall through to the primary save. */
    file = ml_win32_file_hooks_test_create_file_w(save_fail, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0);
    EXPECT_TRUE(file == INVALID_HANDLE_VALUE);
    EXPECT_EQ(GetLastError(), ERROR_CANNOT_MAKE);
    recursion_active = true;
    file = ml_win32_file_hooks_test_create_file_w(read_route, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0);
    EXPECT_TRUE(file == INVALID_HANDLE_VALUE);
    recursion_active = false;
    file = ml_win32_file_hooks_test_create_file_w(source, GENERIC_WRITE, 0, OPEN_EXISTING, 0);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    CloseHandle(file);
    EXPECT_TRUE(ml_win32_file_hooks_test_create_directory_w(directory));
    EXPECT_TRUE(RemoveDirectoryW(directory));
    /* Copy/move route save paths transparently; source is not a mapped save
     * file here, so both fall through to the real API. */
    EXPECT_TRUE(ml_win32_file_hooks_test_copy_file_w(source, copy));
    EXPECT_TRUE(ml_win32_file_hooks_test_move_file_ex_w(copy, moved));
    EXPECT_TRUE(ml_win32_file_hooks_test_delete_file_w(moved));
    EXPECT_TRUE(ml_win32_file_hooks_test_delete_file_w(source));

    printf("smoke_win32_hooks: all tests passed\n");
    return 0;
}

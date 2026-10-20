#pragma once

#include "modloader/vfs.h"

/* Exercise ZIP file readers without replacing the OS path for loose files. */
static bool test_vfs_read(const wchar_t *path, void *buffer, size_t size, size_t *read) {
    bool result;
    if (path == NULL) return false;
    if (vfs_is_zip_path(path)) {
        vfs_file_t *stream = vfs_file_open(path);
        if (stream == NULL) return false;
        result = vfs_file_read(stream, buffer, size, read);
        vfs_file_close(stream);
    } else {
        DWORD received;
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (file == INVALID_HANDLE_VALUE) return false;
        result = ReadFile(file, buffer, (DWORD)size, &received, NULL) != FALSE;
        *read = received;
        CloseHandle(file);
    }
    return result;
}

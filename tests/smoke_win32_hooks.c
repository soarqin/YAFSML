#include <stdio.h>

#include "test_common.h"
#include "modloader/patches/win32_hooks.h"
#include "modloader/patches/zip_hooks.h"
#include "modloader/vfs.h"
#include <miniz.h>
#include <MinHook.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const wchar_t *save_fail_path;
static const wchar_t *read_route_path;
static const wchar_t *route_target;

bool ml_save_mapping_route(const wchar_t *path, const wchar_t **mapped) {
    if (mapped != NULL) *mapped = NULL;
    return path != NULL && save_fail_path != NULL && lstrcmpW(path, save_fail_path) == 0;
}

const wchar_t *mods_file_route_read(const wchar_t *path, DWORD access, DWORD disposition) {
    const wchar_t *uid = vfs_uid_to_path(path);
    if (uid != NULL) return uid;
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

static volatile LONG apc_calls;
static DWORD apc_error, apc_bytes;
static OVERLAPPED *apc_overlapped;
static void CALLBACK read_completed(DWORD error, DWORD bytes, LPOVERLAPPED overlapped) {
    apc_overlapped = overlapped;
    apc_error = error;
    apc_bytes = bytes;
    InterlockedIncrement(&apc_calls);
}

typedef HANDLE (WINAPI *test_create_file_w_t)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
static test_create_file_w_t competing_create_file;
static HANDLE WINAPI competing_create_file_hook(LPCWSTR path, DWORD access, DWORD share,
                                                 LPSECURITY_ATTRIBUTES security, DWORD disposition,
                                                 DWORD flags, HANDLE template_file) {
    return competing_create_file(path, access, share, security, disposition, flags, template_file);
}

static int compare_sync_io(const wchar_t *native_path, HANDLE stream) {
    HANDLE native = CreateFileW(native_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER distance, native_position, zip_position;
    DWORD native_result, zip_result, native_error, zip_error, native_read, zip_read;
    char native_data[7], zip_data[7];
    EXPECT_TRUE(native != INVALID_HANDLE_VALUE);
    distance.QuadPart = (LONGLONG)5 << 30;
    EXPECT_TRUE(SetFilePointerEx(native, distance, NULL, FILE_BEGIN));
    EXPECT_TRUE(SetFilePointerEx(stream, distance, NULL, FILE_BEGIN));
    SetLastError(0);
    native_result = SetFilePointer(native, 0, NULL, FILE_CURRENT);
    native_error = GetLastError();
    SetLastError(0);
    zip_result = SetFilePointer(stream, 0, NULL, FILE_CURRENT);
    zip_error = GetLastError();
    EXPECT_EQ(native_result, zip_result);
    EXPECT_EQ(native_error, zip_error);
    distance.QuadPart = 0;
    EXPECT_TRUE(SetFilePointerEx(native, distance, &native_position, FILE_CURRENT));
    EXPECT_TRUE(SetFilePointerEx(stream, distance, &zip_position, FILE_CURRENT));
    EXPECT_EQ(native_position.QuadPart, zip_position.QuadPart);
    for (DWORD offset = 8191; offset <= 8193; offset++) {
        OVERLAPPED native_overlapped = { 0 }, zip_overlapped = { 0 };
        native_overlapped.Offset = zip_overlapped.Offset = offset;
        SetLastError(0);
        native_result = ReadFile(native, native_data, sizeof(native_data), &native_read, &native_overlapped);
        native_error = GetLastError();
        SetLastError(0);
        zip_result = ReadFile(stream, zip_data, sizeof(zip_data), &zip_read, &zip_overlapped);
        zip_error = GetLastError();
        EXPECT_EQ(native_result, zip_result);
        if (!native_result) EXPECT_EQ(native_error, zip_error);
        EXPECT_EQ(native_read, zip_read);
        EXPECT_EQ(native_overlapped.Offset, zip_overlapped.Offset);
    }
    EXPECT_TRUE(CloseHandle(native));
    return 0;
}

static int compare_read_ex(const wchar_t *native_path, const wchar_t *uid) {
    BOOL results[2];
    DWORD errors[2], completion_errors[2], completion_bytes[2];
    LONG calls[2];
    for (size_t variant = 0; variant < 2; variant++) {
        HANDLE file = CreateFileW(variant == 0 ? native_path : uid, GENERIC_READ, FILE_SHARE_READ,
                                  NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        OVERLAPPED overlapped = { 0 };
        char byte;
        EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
        overlapped.Offset = 8192;
        apc_calls = 0;
        SetLastError(0);
        results[variant] = ReadFileEx(file, &byte, 1, &overlapped, read_completed);
        errors[variant] = GetLastError();
        SleepEx(results[variant] ? 1000 : 0, TRUE);
        calls[variant] = apc_calls;
        completion_errors[variant] = apc_error;
        completion_bytes[variant] = apc_bytes;
        EXPECT_TRUE(CloseHandle(file));
    }
    EXPECT_EQ(results[0], results[1]);
    EXPECT_EQ(errors[0], errors[1]);
    EXPECT_EQ(calls[0], calls[1]);
    if (calls[0] != 0) {
        EXPECT_EQ(completion_errors[0], completion_errors[1]);
        EXPECT_EQ(completion_bytes[0], completion_bytes[1]);
    }
    apc_calls = 0;
    return 0;
}

static int compare_duplicate_close(const wchar_t *native_path, const wchar_t *uid) {
    for (DWORD protection = 0; protection <= 1; protection++) {
        BOOL results[2], alive[2];
        DWORD errors[2];
        for (size_t variant = 0; variant < 2; variant++) {
            DWORD flags;
            HANDLE file = CreateFileW(variant == 0 ? native_path : uid, GENERIC_READ, FILE_SHARE_READ,
                                      NULL, OPEN_EXISTING, 0, NULL);
            EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
            EXPECT_TRUE(SetHandleInformation(file, HANDLE_FLAG_PROTECT_FROM_CLOSE,
                                             protection ? HANDLE_FLAG_PROTECT_FROM_CLOSE : 0));
            SetLastError(0);
            results[variant] = DuplicateHandle(GetCurrentProcess(), file, NULL, NULL, 0, FALSE,
                                                DUPLICATE_CLOSE_SOURCE);
            errors[variant] = GetLastError();
            alive[variant] = GetHandleInformation(file, &flags);
            if (alive[variant]) {
                EXPECT_TRUE(SetHandleInformation(file, HANDLE_FLAG_PROTECT_FROM_CLOSE, 0));
                EXPECT_TRUE(CloseHandle(file));
            }
        }
        EXPECT_EQ(results[0], results[1]);
        if (!results[0]) EXPECT_EQ(errors[0], errors[1]);
        EXPECT_EQ(alive[0], alive[1]);
    }
    return 0;
}

typedef struct shutdown_read_s { HANDLE file; BOOL result; char data[11]; } shutdown_read_t;
static DWORD WINAPI shutdown_reader(void *parameter) {
    shutdown_read_t *read = parameter;
    DWORD received;
    read->result = ReadFile(read->file, read->data, sizeof(read->data), &received, NULL) && received == sizeof(read->data);
    return 0;
}
static DWORD WINAPI shutdown_hooks(void *parameter) {
    (void)parameter;
    ml_win32_file_hooks_uninstall();
    return 0;
}

typedef struct stream_thread_s {
    const wchar_t *uid;
    const char *contents;
    volatile LONG failures;
} stream_thread_t;

static DWORD WINAPI stream_reader(void *parameter) {
    stream_thread_t *context = parameter;
    HANDLE file = CreateFileW(context->uid, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    char buffer[129];
    DWORD read;
    if (file == INVALID_HANDLE_VALUE) { InterlockedIncrement(&context->failures); return 1; }
    for (size_t i = 0; i < 40; i++) {
        LARGE_INTEGER offset;
        offset.QuadPart = (LONGLONG)((i * 193) % 7900);
        if (!SetFilePointerEx(file, offset, NULL, FILE_BEGIN) ||
            !ReadFile(file, buffer, sizeof(buffer), &read, NULL) || read != sizeof(buffer) ||
            memcmp(buffer, context->contents + offset.QuadPart, sizeof(buffer)) != 0)
            InterlockedIncrement(&context->failures);
    }
    if (!CloseHandle(file)) InterlockedIncrement(&context->failures);
    return 0;
}

static int test_zip_stream_hooks(const wchar_t *directory) {
    wchar_t path[MAX_PATH], native_path[MAX_PATH];
    char contents[8192], buffer[97], ansi_uid[64];
    mz_zip_archive archive = { 0 };
    FILE *output;
    const wchar_t *uid;
    HANDLE file, duplicate, event, port;
    LARGE_INTEGER offset, length;
    DWORD read, high, protected_close_error;
    BY_HANDLE_FILE_INFORMATION info;
    FILE_STANDARD_INFO standard;
    _snwprintf(path, MAX_PATH, L"%ls\\YAFSML-zip-hooks-%lu.zip", directory, GetCurrentProcessId());
    for (size_t i = 0; i < sizeof(contents); i++) contents[i] = (char)('a' + i % 26);
    output = _wfopen(path, L"wb");
    EXPECT_NOT_NULL(output);
    EXPECT_TRUE(mz_zip_writer_init_cfile(&archive, output, 0));
    EXPECT_TRUE(mz_zip_writer_add_mem(&archive, "asset.bin", contents, sizeof(contents), 6));
    EXPECT_TRUE(mz_zip_writer_add_mem(&archive, "empty.bin", "", 0, 0));
    EXPECT_TRUE(mz_zip_writer_finalize_archive(&archive));
    EXPECT_TRUE(mz_zip_writer_end(&archive));
    EXPECT_EQ(fclose(output), 0);
    _snwprintf(native_path, MAX_PATH, L"%ls.bin", path);
    output = _wfopen(native_path, L"wb");
    EXPECT_NOT_NULL(output);
    EXPECT_EQ(fwrite(contents, 1, sizeof(contents), output), sizeof(contents));
    EXPECT_EQ(fclose(output), 0);
    event = CreateEventW(NULL, TRUE, FALSE, NULL);
    EXPECT_NOT_NULL(event);
    EXPECT_TRUE(SetHandleInformation(event, HANDLE_FLAG_PROTECT_FROM_CLOSE, HANDLE_FLAG_PROTECT_FROM_CLOSE));
    EXPECT_TRUE(!CloseHandle(event));
    protected_close_error = GetLastError();
    EXPECT_TRUE(SetHandleInformation(event, HANDLE_FLAG_PROTECT_FROM_CLOSE, 0));
    EXPECT_TRUE(CloseHandle(event));
    vfs_init();
    EXPECT_EQ(MH_Initialize(), MH_OK);
    EXPECT_TRUE(ml_win32_file_hooks_install());
    EXPECT_EQ(MH_DisableHook(ml_win32_file_hooks_test_target("ReadFile")), MH_ERROR_NOT_CREATED);
    ml_win32_file_hooks_uninstall();
    EXPECT_TRUE(vfs_add_package(path));
    uid = vfs_virtual_to_uid(L"asset.bin");
    EXPECT_NOT_NULL(uid);
    {
        void *target = ml_win32_file_hooks_test_target("CreateFileW");
        EXPECT_EQ(MH_CreateHook(target, competing_create_file_hook, (void **)&competing_create_file), MH_OK);
        EXPECT_EQ(MH_EnableHook(target), MH_OK);
        EXPECT_TRUE(!ml_win32_file_hooks_install());
        EXPECT_EQ(MH_DisableHook(ml_win32_file_hooks_test_target("ReadFile")), MH_ERROR_NOT_CREATED);
        EXPECT_EQ(MH_RemoveHook(target), MH_OK);
    }
    EXPECT_TRUE(ml_win32_file_hooks_install());

    file = CreateFileW(uid, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    EXPECT_EQ(GetFileType(file), FILE_TYPE_DISK);
    EXPECT_TRUE(GetFileSizeEx(file, &length));
    EXPECT_EQ(length.QuadPart, sizeof(contents));
    EXPECT_EQ(GetFileSize(file, &high), sizeof(contents));
    EXPECT_EQ(high, 0);
    EXPECT_TRUE(GetFileInformationByHandle(file, &info));
    EXPECT_EQ(info.nFileSizeLow, sizeof(contents));
    EXPECT_TRUE(GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)));
    EXPECT_EQ(standard.EndOfFile.QuadPart, sizeof(contents));
    EXPECT_TRUE(ReadFile(file, buffer, sizeof(buffer), &read, NULL));
    EXPECT_EQ(read, sizeof(buffer));
    EXPECT_TRUE(memcmp(buffer, contents, read) == 0);
    EXPECT_TRUE(DuplicateHandle(GetCurrentProcess(), file, GetCurrentProcess(), &duplicate,
                                0, FALSE, DUPLICATE_SAME_ACCESS));
    EXPECT_TRUE(ReadFile(duplicate, buffer, 13, &read, NULL));
    EXPECT_TRUE(memcmp(buffer, contents + sizeof(buffer), 13) == 0);
    EXPECT_TRUE(CloseHandle(file));
    offset.QuadPart = -17;
    EXPECT_TRUE(SetFilePointerEx(duplicate, offset, &length, FILE_END));
    EXPECT_EQ(length.QuadPart, sizeof(contents) - 17);
    EXPECT_TRUE(ReadFile(duplicate, buffer, sizeof(buffer), &read, NULL));
    EXPECT_EQ(read, 17);
    EXPECT_TRUE(memcmp(buffer, contents + sizeof(contents) - 17, 17) == 0);
    EXPECT_TRUE(ReadFile(duplicate, buffer, sizeof(buffer), &read, NULL));
    EXPECT_EQ(read, 0);
    EXPECT_EQ(SetFilePointer(duplicate, 7, NULL, FILE_BEGIN), 7);
    EXPECT_TRUE(ReadFile(duplicate, buffer, 11, &read, NULL));
    EXPECT_TRUE(memcmp(buffer, contents + 7, 11) == 0);
    EXPECT_EQ(compare_sync_io(native_path, duplicate), 0);
    {
        HANDLE restricted;
        EXPECT_TRUE(DuplicateHandle(GetCurrentProcess(), duplicate, GetCurrentProcess(), &restricted, 0, FALSE, 0));
        EXPECT_TRUE(!ReadFile(restricted, buffer, 1, &read, NULL));
        EXPECT_EQ(GetLastError(), ERROR_ACCESS_DENIED);
        EXPECT_TRUE(CloseHandle(restricted));
        EXPECT_TRUE(DuplicateHandle(GetCurrentProcess(), duplicate, GetCurrentProcess(), &restricted,
                                    FILE_READ_DATA, FALSE, 0));
        EXPECT_EQ(SetFilePointer(restricted, 0, NULL, FILE_BEGIN), 0);
        EXPECT_TRUE(ReadFile(restricted, buffer, 3, &read, NULL));
        EXPECT_TRUE(memcmp(buffer, contents, 3) == 0);
        EXPECT_TRUE(DuplicateHandle(GetCurrentProcess(), restricted, NULL, NULL, 0, FALSE, DUPLICATE_CLOSE_SOURCE));
        EXPECT_EQ(GetFileType(restricted), FILE_TYPE_UNKNOWN);
        EXPECT_EQ(GetLastError(), ERROR_INVALID_HANDLE);
    }
    EXPECT_TRUE(SetHandleInformation(duplicate, HANDLE_FLAG_PROTECT_FROM_CLOSE, HANDLE_FLAG_PROTECT_FROM_CLOSE));
    EXPECT_TRUE(!CloseHandle(duplicate));
    EXPECT_EQ(GetLastError(), protected_close_error);
    EXPECT_TRUE(SetHandleInformation(duplicate, HANDLE_FLAG_PROTECT_FROM_CLOSE, 0));
    EXPECT_TRUE(!WriteFile(duplicate, "x", 1, &read, NULL));
    EXPECT_EQ(GetLastError(), ERROR_ACCESS_DENIED);
    EXPECT_TRUE(!SetEndOfFile(duplicate));
    EXPECT_EQ(GetLastError(), ERROR_ACCESS_DENIED);
    EXPECT_NULL(CreateFileMappingW(duplicate, NULL, PAGE_READONLY, 0, 0, NULL));
    EXPECT_EQ(GetLastError(), ERROR_NOT_SUPPORTED);
    EXPECT_TRUE(CloseHandle(duplicate));
    EXPECT_TRUE(!CloseHandle(duplicate));
    EXPECT_EQ(GetLastError(), ERROR_INVALID_HANDLE);

    EXPECT_TRUE(CreateFileW(uid, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE);
    EXPECT_EQ(GetLastError(), ERROR_ACCESS_DENIED);
    EXPECT_TRUE(CreateFileW(uid, GENERIC_READ, 0, NULL, CREATE_ALWAYS, 0, NULL) == INVALID_HANDLE_VALUE);
    EXPECT_EQ(GetLastError(), ERROR_ACCESS_DENIED);
    file = CreateFile2(uid, GENERIC_READ, 0, OPEN_EXISTING, NULL);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    EXPECT_TRUE(CreateFileW(uid, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE);
    EXPECT_EQ(GetLastError(), ERROR_SHARING_VIOLATION);
    EXPECT_TRUE(CloseHandle(file));
    EXPECT_TRUE(WideCharToMultiByte(CP_ACP, 0, uid, -1, ansi_uid, sizeof(ansi_uid), NULL, NULL) != 0);
    file = CreateFileA(ansi_uid, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    EXPECT_TRUE(ReadFile(file, buffer, 19, &read, NULL));
    EXPECT_TRUE(memcmp(buffer, contents, 19) == 0);
    EXPECT_TRUE(CloseHandle(file));

    file = CreateFileW(uid, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    event = CreateEventW(NULL, TRUE, FALSE, NULL);
    EXPECT_NOT_NULL(event);
    EXPECT_NULL(CreateIoCompletionPort(file, event, 1234, 0));
    EXPECT_EQ(GetLastError(), ERROR_INVALID_HANDLE);
    port = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    EXPECT_NOT_NULL(port);
    EXPECT_EQ(CreateIoCompletionPort(file, port, 1234, 0), port);
    EXPECT_NULL(CreateIoCompletionPort(file, port, 4321, 0));
    EXPECT_EQ(GetLastError(), ERROR_INVALID_PARAMETER);
    {
        HANDLE saved;
        EXPECT_TRUE(DuplicateHandle(GetCurrentProcess(), port, GetCurrentProcess(), &saved, 0, FALSE, DUPLICATE_SAME_ACCESS));
        EXPECT_TRUE(CloseHandle(port));
        HANDLE unrelated = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
        EXPECT_NOT_NULL(unrelated);
        OVERLAPPED overlapped = { 0 }, *completed = NULL;
        ULONG_PTR key;
        EXPECT_TRUE(ReadFile(file, buffer, 5, &read, &overlapped));
        EXPECT_TRUE(GetQueuedCompletionStatus(saved, &read, &key, &completed, 1000));
        EXPECT_EQ(key, 1234);
        EXPECT_TRUE(completed == &overlapped);
        EXPECT_TRUE(!GetQueuedCompletionStatus(unrelated, &read, &key, &completed, 0));
        EXPECT_EQ(GetLastError(), WAIT_TIMEOUT);
        EXPECT_TRUE(CloseHandle(unrelated));
        port = saved;
        EXPECT_TRUE(!ReadFileEx(file, buffer, 7, &overlapped, read_completed));
        EXPECT_EQ(GetLastError(), ERROR_INVALID_PARAMETER);
        EXPECT_EQ(SleepEx(0, TRUE), 0);
    }
    {
        OVERLAPPED overlapped = { 0 }, *completed = NULL;
        ULONG_PTR key;
        overlapped.Offset = 103;
        overlapped.hEvent = event;
        EXPECT_TRUE(ReadFile(file, buffer, 31, NULL, &overlapped));
        EXPECT_EQ(WaitForSingleObject(event, 0), WAIT_OBJECT_0);
        EXPECT_TRUE(memcmp(buffer, contents + 103, 31) == 0);
        EXPECT_TRUE(GetOverlappedResult(file, &overlapped, &read, FALSE));
        EXPECT_EQ(read, 31);
        EXPECT_TRUE(GetOverlappedResultEx(file, &overlapped, &read, 0, FALSE));
        EXPECT_TRUE(GetQueuedCompletionStatus(port, &read, &key, &completed, 1000));
        EXPECT_EQ(read, 31);
        EXPECT_EQ(key, 1234);
        EXPECT_TRUE(completed == &overlapped);
        overlapped.Offset = sizeof(contents);
        EXPECT_TRUE(!ReadFile(file, buffer, 1, &read, &overlapped));
        EXPECT_EQ(GetLastError(), ERROR_HANDLE_EOF);
        EXPECT_TRUE(!GetOverlappedResult(file, &overlapped, &read, FALSE));
        EXPECT_EQ(GetLastError(), ERROR_HANDLE_EOF);
        EXPECT_TRUE(SetFileCompletionNotificationModes(file, FILE_SKIP_COMPLETION_PORT_ON_SUCCESS));
        overlapped.Offset = 7;
        EXPECT_TRUE(ReadFile(file, buffer, 13, &read, &overlapped));
        EXPECT_TRUE(!GetQueuedCompletionStatus(port, &read, &key, &completed, 0));
        EXPECT_EQ(GetLastError(), WAIT_TIMEOUT);
        overlapped.Offset = 0;
        EXPECT_TRUE(ReadFile(file, buffer, 7, NULL, &overlapped));
        EXPECT_TRUE(memcmp(buffer, contents, 7) == 0);
    }
    EXPECT_TRUE(CloseHandle(file));
    EXPECT_TRUE(CloseHandle(event));
    EXPECT_TRUE(CloseHandle(port));

    file = CreateFileW(uid, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    {
        OVERLAPPED overlapped = { 0 };
        overlapped.Offset = 21;
        EXPECT_TRUE(ReadFileEx(file, buffer, 27, &overlapped, read_completed));
        EXPECT_EQ(apc_calls, 0);
        EXPECT_EQ(SleepEx(1000, TRUE), WAIT_IO_COMPLETION);
        EXPECT_EQ(apc_calls, 1);
        EXPECT_EQ(apc_error, ERROR_SUCCESS);
        EXPECT_EQ(apc_bytes, 27);
        EXPECT_TRUE(apc_overlapped == &overlapped);
        EXPECT_TRUE(memcmp(buffer, contents + 21, 27) == 0);
    }
    EXPECT_TRUE(CloseHandle(file));
    EXPECT_EQ(compare_read_ex(native_path, uid), 0);
    EXPECT_EQ(compare_duplicate_close(native_path, uid), 0);
    file = CreateFileW(vfs_virtual_to_uid(L"empty.bin"), GENERIC_READ, FILE_SHARE_READ,
                        NULL, OPEN_EXISTING, 0, NULL);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    EXPECT_TRUE(ReadFile(file, buffer, 1, &read, NULL));
    EXPECT_EQ(read, 0);
    EXPECT_TRUE(CloseHandle(file));

    {
        stream_thread_t context = { uid, contents, 0 };
        HANDLE threads[8];
        for (size_t i = 0; i < 8; i++) {
            threads[i] = CreateThread(NULL, 0, stream_reader, &context, 0, NULL);
            EXPECT_NOT_NULL(threads[i]);
        }
        EXPECT_EQ(WaitForMultipleObjects(8, threads, TRUE, 30000), WAIT_OBJECT_0);
        for (size_t i = 0; i < 8; i++) EXPECT_TRUE(CloseHandle(threads[i]));
        EXPECT_EQ(context.failures, 0);
    }

    /* Teardown waits for an in-flight reader before freeing its I/O trampolines. */
    {
        HANDLE entered = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE resume = CreateEventW(NULL, TRUE, FALSE, NULL);
        HANDLE shutdown = CreateEventW(NULL, TRUE, FALSE, NULL);
        shutdown_read_t context = { 0 };
        context.file = CreateFileW(uid, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        EXPECT_TRUE(context.file != INVALID_HANDLE_VALUE);
        EXPECT_TRUE(SetHandleInformation(context.file, HANDLE_FLAG_PROTECT_FROM_CLOSE, HANDLE_FLAG_PROTECT_FROM_CLOSE));
        ml_zip_file_hooks_test_read_gate(entered, resume, shutdown);
        HANDLE reader = CreateThread(NULL, 0, shutdown_reader, &context, 0, NULL);
        EXPECT_NOT_NULL(reader);
        EXPECT_EQ(WaitForSingleObject(entered, 5000), WAIT_OBJECT_0);
        HANDLE teardown = CreateThread(NULL, 0, shutdown_hooks, NULL, 0, NULL);
        EXPECT_NOT_NULL(teardown);
        EXPECT_EQ(WaitForSingleObject(shutdown, 5000), WAIT_OBJECT_0);
        EXPECT_EQ(WaitForSingleObject(teardown, 0), WAIT_TIMEOUT);
        EXPECT_TRUE(SetEvent(resume));
        HANDLE threads[] = { reader, teardown };
        EXPECT_EQ(WaitForMultipleObjects(2, threads, TRUE, 10000), WAIT_OBJECT_0);
        EXPECT_TRUE(context.result);
        EXPECT_TRUE(memcmp(context.data, contents, sizeof(context.data)) == 0);
        EXPECT_EQ(GetFileType(context.file), FILE_TYPE_UNKNOWN);
        EXPECT_EQ(GetLastError(), ERROR_INVALID_HANDLE);
        ml_zip_file_hooks_test_read_gate(NULL, NULL, NULL);
        EXPECT_TRUE(CloseHandle(reader));
        EXPECT_TRUE(CloseHandle(teardown));
        EXPECT_TRUE(CloseHandle(entered));
        EXPECT_TRUE(CloseHandle(resume));
        EXPECT_TRUE(CloseHandle(shutdown));
    }
    EXPECT_EQ(MH_Uninitialize(), MH_OK);
    vfs_uninit();
    EXPECT_TRUE(DeleteFileW(path));
    EXPECT_TRUE(DeleteFileW(native_path));
    return 0;
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
    vfs_init();
    route_target = source;
    read_route_path = read_route;
    EXPECT_TRUE(vfs_register_writable_path(writable_route, source));
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
    vfs_recursion_guard_enter();
    file = ml_win32_file_hooks_test_create_file_w(read_route, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0);
    EXPECT_TRUE(file == INVALID_HANDLE_VALUE);
    vfs_recursion_guard_leave();
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
    vfs_uninit();
    EXPECT_EQ(test_zip_stream_hooks(root), 0);

    printf("smoke_win32_hooks: all tests passed\n");
    return 0;
}

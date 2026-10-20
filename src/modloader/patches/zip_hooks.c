/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 * Use of this source code is governed by the MIT license in the LICENSE file.
 */

#include "zip_hooks.h"

#include "common/allocator.h"
#include "modloader/vfs.h"
#include <winternl.h>

#define kcalloc(N,Z) ml_mem_alloc(LPTR, (N) * (Z))
#define kmalloc(Z) ml_mem_alloc(0, (Z))
#define krealloc(P,Z) ((P) ? ml_mem_realloc((P), (Z), LMEM_MOVEABLE) : ml_mem_alloc(0, (Z)))
#define kfree(P) ml_mem_free(P)
#include "khash.h"

/* ZIP handles are event handles tagged in the low bits that the system ignores
 * (OBJ_HANDLE_TAGBITS in ntdef.h). The tag matches no kernel or console handle
 * and no common pseudo handle, so calls on any other handle reach the real API
 * without a lock. Code that suspends threads and then calls CloseHandle, such as
 * MinHook toggling a hook, must not wait for a lock a suspended thread holds. */
#define ZIP_HANDLE_TAG ((uintptr_t)1)

typedef struct zip_file_s {
    vfs_file_t *reader;
    volatile LONG references;
    DWORD access;
    DWORD share;
    DWORD flags;
    uint64_t identity;
    /* The file's own reference to the handles' event: a caller may close its
     * handle, and the value be reused, while a read is still completing. */
    HANDLE event;
    HANDLE completion_port;
    ULONG_PTR completion_key;
    UCHAR completion_modes;
    SRWLOCK lock;
} zip_file_t;

typedef struct zip_handle_s {
    zip_file_t *file;
    DWORD access;
} zip_handle_t;

KHASH_MAP_INIT_INT64(zip_handles, zip_handle_t)
static khash_t(zip_handles) *handles;
static SRWLOCK handles_lock = SRWLOCK_INIT;
static volatile LONG handle_count;
/* Written under handles_lock; cleared while the hooks are torn down. */
static volatile LONG accepting;
static volatile LONG active_operations;
static CONDITION_VARIABLE operations_idle = CONDITION_VARIABLE_INIT;

#ifdef ML_WIN32_HOOKS_TEST
static HANDLE test_read_entered, test_read_resume, test_shutdown_entered;
void ml_zip_file_hooks_test_read_gate(HANDLE entered, HANDLE resume, HANDLE shutdown) {
    test_read_entered = entered;
    test_read_resume = resume;
    test_shutdown_entered = shutdown;
}
#endif

static BOOL (WINAPI *old_read_file)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL (WINAPI *old_read_file_ex)(HANDLE, LPVOID, DWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE);
static BOOL (WINAPI *old_write_file)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
static BOOL (WINAPI *old_close_handle)(HANDLE);
static BOOL (WINAPI *old_set_pointer_ex)(HANDLE, LARGE_INTEGER, PLARGE_INTEGER, DWORD);
static DWORD (WINAPI *old_set_pointer)(HANDLE, LONG, PLONG, DWORD);
static BOOL (WINAPI *old_get_size_ex)(HANDLE, PLARGE_INTEGER);
static DWORD (WINAPI *old_get_size)(HANDLE, LPDWORD);
static DWORD (WINAPI *old_get_type)(HANDLE);
static BOOL (WINAPI *old_get_info)(HANDLE, LPBY_HANDLE_FILE_INFORMATION);
static BOOL (WINAPI *old_get_info_ex)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);
static BOOL (WINAPI *old_get_overlapped)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
static BOOL (WINAPI *old_get_overlapped_ex)(HANDLE, LPOVERLAPPED, LPDWORD, DWORD, BOOL);
static BOOL (WINAPI *old_duplicate)(HANDLE, HANDLE, HANDLE, LPHANDLE, DWORD, BOOL, DWORD);
static HANDLE (WINAPI *old_completion_port)(HANDLE, HANDLE, ULONG_PTR, DWORD);
static BOOL (WINAPI *old_completion_modes)(HANDLE, UCHAR);
static BOOL (WINAPI *old_cancel_io)(HANDLE);
static BOOL (WINAPI *old_cancel_io_ex)(HANDLE, LPOVERLAPPED);
static BOOL (WINAPI *old_flush)(HANDLE);
static BOOL (WINAPI *old_set_end)(HANDLE);
static HANDLE (WINAPI *old_mapping_w)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCWSTR);
static HANDLE (WINAPI *old_mapping_a)(HANDLE, LPSECURITY_ATTRIBUTES, DWORD, DWORD, DWORD, LPCSTR);
static BOOL (WINAPI *old_set_info)(HANDLE, FILE_INFO_BY_HANDLE_CLASS, LPVOID, DWORD);
static void *targets[ML_ZIP_HOOK_COUNT];
static NTSTATUS (NTAPI *query_object)(HANDLE, OBJECT_INFORMATION_CLASS, PVOID, ULONG, PULONG);
/* On the supported 64-bit ABI, a file completion routine consumes the low
 * DWORDs of the first two native APC arguments and the OVERLAPPED pointer. */
static NTSTATUS (NTAPI *queue_file_apc)(HANDLE, LPOVERLAPPED_COMPLETION_ROUTINE, ULONG_PTR, ULONG_PTR, LPOVERLAPPED);
static ULONG (WINAPI *status_to_error)(NTSTATUS);

static DWORD file_access(DWORD access) {
    GENERIC_MAPPING mapping = { FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
    /* As for a read-only disk file, the most a caller can be granted is reading. */
    if ((access & MAXIMUM_ALLOWED) != 0) access = (access & ~MAXIMUM_ALLOWED) | FILE_GENERIC_READ;
    MapGenericMask(&access, &mapping);
    return access;
}

static bool zip_handle_tagged(HANDLE handle) {
    return ((uintptr_t)handle & 3) == ZIP_HANDLE_TAG;
}

static void zip_file_release(zip_file_t *file) {
    if (InterlockedDecrement(&file->references) != 0) return;
    vfs_file_close(file->reader);
    if (file->event != NULL) old_close_handle(file->event);
    if (file->completion_port != NULL) old_close_handle(file->completion_port);
    ml_mem_free(file);
}

static void zip_operation_leave(void) {
    InterlockedDecrement(&active_operations);
    if (!ReadAcquire(&accepting)) {
        AcquireSRWLockExclusive(&handles_lock);
        WakeAllConditionVariable(&operations_idle);
        ReleaseSRWLockExclusive(&handles_lock);
    }
}

static void zip_handle_release(zip_file_t *file) {
    zip_file_release(file);
    zip_operation_leave();
}

/* Records `handle` with handles_lock held exclusively. An entry that a handle
 * closed behind the hooks' back (NtClose) left for the same value is replaced;
 * the caller releases its file, returned in `stale`, after unlocking. */
static bool zip_handle_insert(HANDLE handle, zip_file_t *file, DWORD access, zip_file_t **stale) {
    khiter_t slot;
    int ret;
    *stale = NULL;
    if (!accepting || handles == NULL) return false;
    slot = kh_put(zip_handles, handles, (khint64_t)(uintptr_t)handle, &ret);
    if (ret < 0) return false;
    if (ret == 0) *stale = kh_value(handles, slot).file;
    else InterlockedIncrement(&handle_count);
    kh_value(handles, slot) = (zip_handle_t){ file, access };
    return true;
}

static zip_file_t *zip_handle_get(HANDLE handle, DWORD *access) {
    zip_file_t *file = NULL;
    khiter_t slot;
    if (!zip_handle_tagged(handle) || ReadAcquire(&handle_count) == 0 || !ReadAcquire(&accepting)) return NULL;
    AcquireSRWLockShared(&handles_lock);
    if (handles != NULL) {
        slot = kh_get(zip_handles, handles, (khint64_t)(uintptr_t)handle);
        if (slot != kh_end(handles)) {
            file = kh_value(handles, slot).file;
            if (access != NULL) *access = kh_value(handles, slot).access;
            InterlockedIncrement(&file->references);
            InterlockedIncrement(&active_operations);
        }
    }
    ReleaseSRWLockShared(&handles_lock);
    return file;
}

/* For calls that a ZIP handle answers without touching its file. */
static bool zip_handle_known(HANDLE handle) {
    zip_file_t *file = zip_handle_get(handle, NULL);
    if (file == NULL) return false;
    zip_handle_release(file);
    return true;
}

static BOOL WINAPI close_handle_hooked(HANDLE handle) {
    zip_file_t *file = NULL;
    BOOL result = FALSE;
    DWORD error = ERROR_SUCCESS;
    bool handled = false;
    khiter_t slot;
    if (!zip_handle_tagged(handle) || ReadAcquire(&handle_count) == 0) return old_close_handle(handle);
    AcquireSRWLockExclusive(&handles_lock);
    if (handles != NULL) {
        slot = kh_get(zip_handles, handles, (khint64_t)(uintptr_t)handle);
        if (slot != kh_end(handles)) {
            handled = true;
            InterlockedIncrement(&active_operations);
            file = kh_value(handles, slot).file;
            result = old_close_handle(handle);
            error = GetLastError();
            if (result) {
                kh_del(zip_handles, handles, slot);
                InterlockedDecrement(&handle_count);
            } else {
                file = NULL;
            }
        }
    }
    ReleaseSRWLockExclusive(&handles_lock);
    if (!handled) return old_close_handle(handle);
    if (file != NULL) zip_file_release(file);
    zip_operation_leave();
    SetLastError(error);
    return result;
}

HANDLE ml_zip_file_open(const wchar_t *path, DWORD access, DWORD share,
                        LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags) {
    zip_file_t *file, *stale;
    HANDLE event = NULL, handle;
    DWORD error = ERROR_NOT_ENOUGH_MEMORY;
    khiter_t slot;
    access = file_access(access);
    if ((access & (DELETE | WRITE_DAC | WRITE_OWNER |
                   FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES)) != 0 ||
        (flags & FILE_FLAG_DELETE_ON_CLOSE) != 0) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    if (disposition != OPEN_EXISTING && disposition != OPEN_ALWAYS) {
        SetLastError(disposition == CREATE_NEW ? ERROR_FILE_EXISTS : ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    AcquireSRWLockShared(&handles_lock);
    if (!accepting) {
        ReleaseSRWLockShared(&handles_lock);
        SetLastError(ERROR_OPERATION_ABORTED);
        return INVALID_HANDLE_VALUE;
    }
    InterlockedIncrement(&active_operations);
    ReleaseSRWLockShared(&handles_lock);
    file = ml_mem_alloc(LPTR, sizeof(*file));
    if (file == NULL) { zip_operation_leave(); SetLastError(error); return INVALID_HANDLE_VALUE; }
    file->references = 1;
    file->access = access;
    file->share = share;
    file->flags = flags;
    InitializeSRWLock(&file->lock);
    file->reader = vfs_file_open(path);
    if (file->reader == NULL) { error = GetLastError(); goto failed; }
    file->identity = vfs_file_identity(file->reader);
    /* A real kernel event supplies a valid, inheritable HANDLE without creating
     * a disk file. All file operations on this handle are dispatched below. */
    event = CreateEventW(security, TRUE, TRUE, NULL);
    if (event == NULL || !old_duplicate(GetCurrentProcess(), event, GetCurrentProcess(), &file->event,
                                        0, FALSE, DUPLICATE_SAME_ACCESS)) {
        error = GetLastError();
        goto failed;
    }
    handle = (HANDLE)((uintptr_t)event | ZIP_HANDLE_TAG);
    AcquireSRWLockExclusive(&handles_lock);
    if (!accepting) {
        error = ERROR_OPERATION_ABORTED;
        goto failed_locked;
    }
    if (handles == NULL) handles = kh_init(zip_handles);
    if (handles != NULL) {
        for (slot = kh_begin(handles); slot != kh_end(handles); slot++) {
            zip_file_t *other;
            if (!kh_exist(handles, slot)) continue;
            other = kh_value(handles, slot).file;
            if (file->identity == other->identity &&
                (((access & (FILE_READ_DATA | FILE_EXECUTE)) != 0 && (other->share & FILE_SHARE_READ) == 0) ||
                 ((other->access & (FILE_READ_DATA | FILE_EXECUTE)) != 0 && (share & FILE_SHARE_READ) == 0))) {
                error = ERROR_SHARING_VIOLATION;
                goto failed_locked;
            }
        }
    }
    if (zip_handle_insert(handle, file, access, &stale)) {
        ReleaseSRWLockExclusive(&handles_lock);
        if (stale != NULL) zip_file_release(stale);
        zip_operation_leave();
        SetLastError(disposition == OPEN_ALWAYS ? ERROR_ALREADY_EXISTS : ERROR_SUCCESS);
        return handle;
    }
failed_locked:
    ReleaseSRWLockExclusive(&handles_lock);
failed:
    if (event != NULL) old_close_handle(event);
    zip_file_release(file);
    zip_operation_leave();
    SetLastError(error);
    return INVALID_HANDLE_VALUE;
}

/* OVERLAPPED.Internal holds an NTSTATUS. End of file keeps its native status;
 * other errors use the NTWIN32 facility, which RtlNtStatusToDosError maps back
 * to the error ReadFile reported. */
static ULONG_PTR status_from_error(DWORD error) {
    if (error == ERROR_SUCCESS) return 0;
    return error == ERROR_HANDLE_EOF ? 0xC0000011UL : 0xC0070000UL | (error & 0xFFFF);
}

static DWORD read_file_data(zip_file_t *file, DWORD access, void *buffer, DWORD size, DWORD *read,
                          OVERLAPPED *overlapped, bool apc) {
    size_t received = 0;
    DWORD error = ERROR_SUCCESS;
    bool result;
    AcquireSRWLockExclusive(&file->lock);
#ifdef ML_WIN32_HOOKS_TEST
    if (test_read_entered != NULL) {
        SetEvent(test_read_entered);
        WaitForSingleObject(test_read_resume, INFINITE);
    }
#endif
    if (apc && file->completion_port != NULL) {
        error = ERROR_INVALID_PARAMETER;
    } else if ((access & FILE_READ_DATA) == 0) {
        error = ERROR_ACCESS_DENIED;
    } else if (overlapped == NULL && (file->flags & FILE_FLAG_OVERLAPPED) != 0) {
        error = ERROR_INVALID_PARAMETER;
    } else {
        if (overlapped != NULL) {
            uint64_t offset = ((uint64_t)overlapped->OffsetHigh << 32) | overlapped->Offset;
            if ((file->flags & FILE_FLAG_OVERLAPPED) != 0) {
                result = vfs_file_read_at(file->reader, offset, buffer, size, &received);
            } else {
                if (offset > INT64_MAX) {
                    result = false;
                    SetLastError(ERROR_INVALID_PARAMETER);
                } else {
                    result = vfs_file_seek(file->reader, (int64_t)offset, FILE_BEGIN, NULL);
                    if (result) result = vfs_file_read(file->reader, buffer, size, &received);
                }
            }
        } else {
            result = vfs_file_read(file->reader, buffer, size, &received);
        }
        if (!result) error = GetLastError();
        else if (overlapped != NULL && size != 0 && received == 0) error = ERROR_HANDLE_EOF;
    }
    *read = error == ERROR_SUCCESS ? (DWORD)received : 0;
    if (overlapped != NULL) {
        overlapped->Internal = status_from_error(error);
        overlapped->InternalHigh = *read;
    }
    ReleaseSRWLockExclusive(&file->lock);
    return error;
}

static BOOL WINAPI read_file_hooked(HANDLE handle, LPVOID buffer, DWORD size,
                                     LPDWORD read, LPOVERLAPPED overlapped) {
    DWORD access;
    zip_file_t *file = zip_handle_get(handle, &access);
    DWORD received, error;
    HANDLE event;
    bool skip_port;
    if (file == NULL) return old_read_file(handle, buffer, size, read, overlapped);
    event = overlapped == NULL ? NULL : (HANDLE)((uintptr_t)overlapped->hEvent & ~(uintptr_t)1);
    skip_port = overlapped != NULL && ((uintptr_t)overlapped->hEvent & 1) != 0;
    if (event != NULL && !ResetEvent(event)) {
        error = GetLastError();
        if (read != NULL) *read = 0;
        zip_handle_release(file);
        SetLastError(error);
        return FALSE;
    }
    ResetEvent(file->event);
    error = read_file_data(file, access, buffer, size, &received, overlapped, false);
    if (read != NULL) *read = received;
    AcquireSRWLockShared(&file->lock);
    if ((file->completion_modes & FILE_SKIP_SET_EVENT_ON_HANDLE) == 0) SetEvent(file->event);
    if (overlapped != NULL) {
        /* A waiter may release the OVERLAPPED or close the file as soon as it
         * observes completion. Finish all updates before publishing it. */
        if (event != NULL) SetEvent(event);
        if (error == ERROR_SUCCESS && file->completion_port != NULL && !skip_port &&
            (file->completion_modes & FILE_SKIP_COMPLETION_PORT_ON_SUCCESS) == 0)
            if (!PostQueuedCompletionStatus(file->completion_port, received, file->completion_key, overlapped))
                error = GetLastError();
    }
    ReleaseSRWLockShared(&file->lock);
    zip_handle_release(file);
    SetLastError(error);
    return error == ERROR_SUCCESS;
}

static BOOL WINAPI read_file_ex_hooked(HANDLE handle, LPVOID buffer, DWORD size,
                                        LPOVERLAPPED overlapped, LPOVERLAPPED_COMPLETION_ROUTINE callback) {
    DWORD access;
    zip_file_t *file = zip_handle_get(handle, &access);
    DWORD error, received;
    NTSTATUS status;
    if (file == NULL) return old_read_file_ex(handle, buffer, size, overlapped, callback);
    if (overlapped == NULL || callback == NULL || (size != 0 && buffer == NULL) ||
        (file->flags & FILE_FLAG_OVERLAPPED) == 0) {
        zip_handle_release(file); SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
    }
    error = read_file_data(file, access, buffer, size, &received, overlapped, true);
    zip_handle_release(file);
    if (error != ERROR_SUCCESS && error != ERROR_HANDLE_EOF) { SetLastError(error); return FALSE; }
    /* Queue the caller's routine directly: no heap context to leak if its
     * thread exits, and no loader thunk left queued after hooks are removed. */
    status = queue_file_apc(GetCurrentThread(), callback, error, received, overlapped);
    SetLastError(status < 0 ? status_to_error(status) : ERROR_SUCCESS);
    return status >= 0;
}

static BOOL WINAPI write_file_hooked(HANDLE handle, LPCVOID buffer, DWORD size,
                                      LPDWORD written, LPOVERLAPPED overlapped) {
    if (!zip_handle_known(handle)) return old_write_file(handle, buffer, size, written, overlapped);
    if (written != NULL) *written = 0;
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

static BOOL seek_file_position(zip_file_t *file, int64_t distance, DWORD origin, uint64_t limit, uint64_t *next) {
    uint64_t previous;
    BOOL result;
    DWORD error;
    AcquireSRWLockExclusive(&file->lock);
    vfs_file_seek(file->reader, 0, FILE_CURRENT, &previous);
    result = vfs_file_seek(file->reader, distance, origin, next);
    error = GetLastError();
    if (result && *next > limit) {
        vfs_file_seek(file->reader, (int64_t)previous, FILE_BEGIN, NULL);
        result = FALSE;
        error = ERROR_INVALID_PARAMETER;
    }
    ReleaseSRWLockExclusive(&file->lock);
    SetLastError(error);
    return result;
}

static BOOL WINAPI set_pointer_ex_hooked(HANDLE handle, LARGE_INTEGER distance,
                                          PLARGE_INTEGER position, DWORD origin) {
    zip_file_t *file = zip_handle_get(handle, NULL);
    uint64_t next;
    BOOL result;
    DWORD error;
    if (file == NULL) return old_set_pointer_ex(handle, distance, position, origin);
    result = seek_file_position(file, distance.QuadPart, origin, INT64_MAX, &next);
    error = GetLastError();
    zip_handle_release(file);
    if (result && position != NULL) position->QuadPart = (LONGLONG)next;
    SetLastError(error);
    return result;
}

static DWORD WINAPI set_pointer_hooked(HANDLE handle, LONG low, PLONG high, DWORD origin) {
    zip_file_t *file = zip_handle_get(handle, NULL);
    LARGE_INTEGER distance;
    uint64_t next;
    BOOL result;
    DWORD error;
    if (file == NULL) return old_set_pointer(handle, low, high, origin);
    if (high != NULL) { distance.LowPart = (DWORD)low; distance.HighPart = *high; }
    else distance.QuadPart = low;
    result = seek_file_position(file, distance.QuadPart, origin, high != NULL ? INT64_MAX : MAXDWORD, &next);
    error = GetLastError();
    zip_handle_release(file);
    SetLastError(error);
    if (!result) return INVALID_SET_FILE_POINTER;
    if (high != NULL) *high = (LONG)(next >> 32);
    SetLastError(ERROR_SUCCESS);
    return (DWORD)next;
}

static BOOL WINAPI get_size_ex_hooked(HANDLE handle, PLARGE_INTEGER size) {
    zip_file_t *file = zip_handle_get(handle, NULL);
    if (file == NULL) return old_get_size_ex(handle, size);
    if (size != NULL) size->QuadPart = (LONGLONG)vfs_file_size(file->reader);
    zip_handle_release(file);
    if (size == NULL) SetLastError(ERROR_INVALID_PARAMETER);
    return size != NULL;
}

static DWORD WINAPI get_size_hooked(HANDLE handle, LPDWORD high) {
    zip_file_t *file = zip_handle_get(handle, NULL);
    uint64_t size;
    if (file == NULL) return old_get_size(handle, high);
    size = vfs_file_size(file->reader);
    zip_handle_release(file);
    if (high != NULL) *high = (DWORD)(size >> 32);
    SetLastError(ERROR_SUCCESS);
    return (DWORD)size;
}

static DWORD WINAPI get_type_hooked(HANDLE handle) {
    return zip_handle_known(handle) ? FILE_TYPE_DISK : old_get_type(handle);
}

static BOOL WINAPI get_info_hooked(HANDLE handle, LPBY_HANDLE_FILE_INFORMATION info) {
    zip_file_t *file = zip_handle_get(handle, NULL);
    uint64_t size, identity;
    if (file == NULL) return old_get_info(handle, info);
    size = vfs_file_size(file->reader);
    identity = file->identity;
    zip_handle_release(file);
    if (info == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(info, 0, sizeof(*info));
    info->dwFileAttributes = FILE_ATTRIBUTE_READONLY;
    info->nFileSizeHigh = (DWORD)(size >> 32);
    info->nFileSizeLow = (DWORD)size;
    info->nNumberOfLinks = 1;
    info->nFileIndexHigh = (DWORD)(identity >> 32);
    info->nFileIndexLow = (DWORD)identity;
    return TRUE;
}

static BOOL WINAPI get_info_ex_hooked(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID info, DWORD size) {
    zip_file_t *file = zip_handle_get(handle, NULL);
    DWORD required = 0;
    uint64_t length, identity;
    if (file == NULL) return old_get_info_ex(handle, kind, info, size);
    length = vfs_file_size(file->reader);
    identity = file->identity;
    switch (kind) {
        case FileBasicInfo: required = sizeof(FILE_BASIC_INFO); break;
        case FileStandardInfo: required = sizeof(FILE_STANDARD_INFO); break;
        case FileAttributeTagInfo: required = sizeof(FILE_ATTRIBUTE_TAG_INFO); break;
        case FileIdInfo: required = sizeof(FILE_ID_INFO); break;
        default: break;
    }
    zip_handle_release(file);
    if (required == 0) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
    if (info == NULL || size < required) { SetLastError(ERROR_BAD_LENGTH); return FALSE; }
    memset(info, 0, required);
    if (kind == FileBasicInfo) ((FILE_BASIC_INFO *)info)->FileAttributes = FILE_ATTRIBUTE_READONLY;
    if (kind == FileStandardInfo) {
        FILE_STANDARD_INFO *standard = info;
        standard->AllocationSize.QuadPart = (LONGLONG)length;
        standard->EndOfFile.QuadPart = (LONGLONG)length;
        standard->NumberOfLinks = 1;
    }
    if (kind == FileAttributeTagInfo) ((FILE_ATTRIBUTE_TAG_INFO *)info)->FileAttributes = FILE_ATTRIBUTE_READONLY;
    if (kind == FileIdInfo) memcpy(((FILE_ID_INFO *)info)->FileId.Identifier, &identity, sizeof(identity));
    return TRUE;
}

static BOOL overlapped_result(LPOVERLAPPED overlapped, LPDWORD read) {
    DWORD error;
    if (overlapped == NULL || read == NULL) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *read = (DWORD)overlapped->InternalHigh;
    error = overlapped->Internal == 0x103 ? ERROR_IO_INCOMPLETE
          : overlapped->Internal == 0 ? ERROR_SUCCESS : status_to_error((NTSTATUS)overlapped->Internal);
    SetLastError(error);
    return error == ERROR_SUCCESS;
}

static BOOL WINAPI get_overlapped_hooked(HANDLE handle, LPOVERLAPPED overlapped, LPDWORD read, BOOL wait) {
    if (!zip_handle_known(handle)) return old_get_overlapped(handle, overlapped, read, wait);
    return overlapped_result(overlapped, read);
}

static BOOL WINAPI get_overlapped_ex_hooked(HANDLE handle, LPOVERLAPPED overlapped, LPDWORD read,
                                            DWORD milliseconds, BOOL alertable) {
    if (!zip_handle_known(handle)) return old_get_overlapped_ex(handle, overlapped, read, milliseconds, alertable);
    return overlapped_result(overlapped, read);
}

static BOOL WINAPI duplicate_hooked(HANDLE source_process, HANDLE source, HANDLE target_process,
                                     LPHANDLE target, DWORD access, BOOL inherit, DWORD options) {
    DWORD source_access;
    zip_file_t *file = zip_handle_tagged(source) && GetProcessId(source_process) == GetCurrentProcessId()
        ? zip_handle_get(source, &source_access) : NULL;
    zip_file_t *stale = NULL;
    HANDLE duplicate = NULL;
    BOOL result = FALSE;
    DWORD error = ERROR_NOT_SUPPORTED;
    if (file == NULL) return old_duplicate(source_process, source, target_process, target, access, inherit, options);
    if (target_process == NULL && (options & DUPLICATE_CLOSE_SOURCE) != 0) { result = TRUE; goto done; }
    if (target == NULL || (options & ~(DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) != 0) {
        error = ERROR_INVALID_PARAMETER; goto done;
    }
    access = (options & DUPLICATE_SAME_ACCESS) != 0 ? source_access : file_access(access);
    if ((access & ~source_access) != 0) { error = ERROR_ACCESS_DENIED; goto done; }
    if (GetProcessId(target_process) != GetCurrentProcessId()) goto done;
    if (!old_duplicate(source_process, source, target_process, &duplicate, 0, inherit, DUPLICATE_SAME_ACCESS)) {
        error = GetLastError(); goto done;
    }
    duplicate = (HANDLE)((uintptr_t)duplicate | ZIP_HANDLE_TAG);
    AcquireSRWLockExclusive(&handles_lock);
    result = zip_handle_insert(duplicate, file, access, &stale);
    if (result) InterlockedIncrement(&file->references);
    ReleaseSRWLockExclusive(&handles_lock);
    if (stale != NULL) zip_file_release(stale);
    if (result) *target = duplicate;
    else { old_close_handle(duplicate); error = ERROR_NOT_ENOUGH_MEMORY; }
done:
    if ((options & DUPLICATE_CLOSE_SOURCE) != 0) close_handle_hooked(source);
    zip_handle_release(file);
    SetLastError(result ? ERROR_SUCCESS : error);
    return result;
}

static HANDLE WINAPI completion_port_hooked(HANDLE handle, HANDLE port, ULONG_PTR key, DWORD threads) {
    zip_file_t *file = zip_handle_get(handle, NULL);
    HANDLE result = NULL, retained = NULL;
    DWORD error = ERROR_INVALID_PARAMETER;
    bool created = false;
    struct {
        PUBLIC_OBJECT_TYPE_INFORMATION type;
        wchar_t name[32];
    } information;
    if (file == NULL) return old_completion_port(handle, port, key, threads);
    AcquireSRWLockExclusive(&file->lock);
    if (file->completion_port != NULL || (file->flags & FILE_FLAG_OVERLAPPED) == 0) goto done;
    if (port == NULL) {
        port = old_completion_port(INVALID_HANDLE_VALUE, NULL, 0, threads);
        if (port == NULL) { error = GetLastError(); goto done; }
        created = true;
    }
    /* Retain the object, not the caller's reusable handle value. Query its type
     * without posting a probe packet into the application's completion queue. */
    if (!old_duplicate(GetCurrentProcess(), port, GetCurrentProcess(), &retained, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        error = GetLastError(); goto done;
    }
    if (query_object(retained, ObjectTypeInformation, &information, sizeof(information), NULL) < 0 ||
        information.type.TypeName.Length != sizeof(L"IoCompletion") - sizeof(wchar_t) ||
        memcmp(information.type.TypeName.Buffer, L"IoCompletion", information.type.TypeName.Length) != 0) {
        error = ERROR_INVALID_HANDLE; goto done;
    }
    file->completion_port = retained;
    file->completion_key = key;
    result = port;
    retained = NULL;
done:
    ReleaseSRWLockExclusive(&file->lock);
    if (retained != NULL) old_close_handle(retained);
    if (result == NULL && created) old_close_handle(port);
    zip_handle_release(file);
    SetLastError(result == NULL ? error : ERROR_SUCCESS);
    return result;
}

static BOOL WINAPI completion_modes_hooked(HANDLE handle, UCHAR modes) {
    zip_file_t *file = zip_handle_get(handle, NULL);
    if (file == NULL) return old_completion_modes(handle, modes);
    if ((modes & ~(FILE_SKIP_COMPLETION_PORT_ON_SUCCESS | FILE_SKIP_SET_EVENT_ON_HANDLE)) != 0) {
        zip_handle_release(file); SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
    }
    AcquireSRWLockExclusive(&file->lock);
    file->completion_modes |= modes;
    ReleaseSRWLockExclusive(&file->lock);
    zip_handle_release(file);
    return TRUE;
}

static BOOL WINAPI cancel_io_hooked(HANDLE handle) {
    return zip_handle_known(handle) ? TRUE : old_cancel_io(handle);
}

static BOOL WINAPI cancel_io_ex_hooked(HANDLE handle, LPOVERLAPPED overlapped) {
    if (!zip_handle_known(handle)) return old_cancel_io_ex(handle, overlapped);
    SetLastError(ERROR_NOT_FOUND);
    return FALSE;
}

static BOOL WINAPI flush_hooked(HANDLE handle) {
    if (!zip_handle_known(handle)) return old_flush(handle);
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

static BOOL WINAPI set_end_hooked(HANDLE handle) {
    if (!zip_handle_known(handle)) return old_set_end(handle);
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

static BOOL WINAPI set_info_hooked(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID info, DWORD size) {
    if (!zip_handle_known(handle)) return old_set_info(handle, kind, info, size);
    /* As on a read-only disk file, an I/O priority hint needs no write access. */
    if (kind == FileIoPriorityHintInfo) {
        if (info != NULL && size >= sizeof(FILE_IO_PRIORITY_HINT_INFO)) return TRUE;
        SetLastError(ERROR_BAD_LENGTH);
        return FALSE;
    }
    SetLastError(ERROR_ACCESS_DENIED);
    return FALSE;
}

static HANDLE WINAPI mapping_w_hooked(HANDLE handle, LPSECURITY_ATTRIBUTES security,
                                       DWORD protect, DWORD high, DWORD low, LPCWSTR name) {
    if (!zip_handle_known(handle)) return old_mapping_w(handle, security, protect, high, low, name);
    SetLastError(ERROR_NOT_SUPPORTED);
    return NULL;
}

static HANDLE WINAPI mapping_a_hooked(HANDLE handle, LPSECURITY_ATTRIBUTES security,
                                       DWORD protect, DWORD high, DWORD low, LPCSTR name) {
    if (!zip_handle_known(handle)) return old_mapping_a(handle, security, protect, high, low, name);
    SetLastError(ERROR_NOT_SUPPORTED);
    return NULL;
}

void ml_zip_file_hook_specs(ml_hook_spec_t *specs, bool resolve_targets) {
    HMODULE kernelbase = resolve_targets ? GetModuleHandleW(L"kernelbase.dll") : NULL;
    size_t i = 0;
#define ZIP_HOOK(NAME, OLD, DETOUR) do { \
    if (resolve_targets) { \
        targets[i] = kernelbase == NULL ? NULL : (void *)GetProcAddress(kernelbase, #NAME); \
        if (targets[i] == NULL) targets[i] = (void *)NAME; \
    } \
    specs[i] = (ml_hook_spec_t){ targets[i], DETOUR, (void **)&OLD }; \
    i++; \
} while (0)
    ZIP_HOOK(ReadFile, old_read_file, read_file_hooked);
    ZIP_HOOK(ReadFileEx, old_read_file_ex, read_file_ex_hooked);
    ZIP_HOOK(WriteFile, old_write_file, write_file_hooked);
    ZIP_HOOK(CloseHandle, old_close_handle, close_handle_hooked);
    ZIP_HOOK(SetFilePointerEx, old_set_pointer_ex, set_pointer_ex_hooked);
    ZIP_HOOK(SetFilePointer, old_set_pointer, set_pointer_hooked);
    ZIP_HOOK(GetFileSizeEx, old_get_size_ex, get_size_ex_hooked);
    ZIP_HOOK(GetFileSize, old_get_size, get_size_hooked);
    ZIP_HOOK(GetFileType, old_get_type, get_type_hooked);
    ZIP_HOOK(GetFileInformationByHandle, old_get_info, get_info_hooked);
    ZIP_HOOK(GetFileInformationByHandleEx, old_get_info_ex, get_info_ex_hooked);
    ZIP_HOOK(GetOverlappedResult, old_get_overlapped, get_overlapped_hooked);
    ZIP_HOOK(GetOverlappedResultEx, old_get_overlapped_ex, get_overlapped_ex_hooked);
    ZIP_HOOK(DuplicateHandle, old_duplicate, duplicate_hooked);
    ZIP_HOOK(CreateIoCompletionPort, old_completion_port, completion_port_hooked);
    ZIP_HOOK(SetFileCompletionNotificationModes, old_completion_modes, completion_modes_hooked);
    ZIP_HOOK(CancelIo, old_cancel_io, cancel_io_hooked);
    ZIP_HOOK(CancelIoEx, old_cancel_io_ex, cancel_io_ex_hooked);
    ZIP_HOOK(FlushFileBuffers, old_flush, flush_hooked);
    ZIP_HOOK(SetEndOfFile, old_set_end, set_end_hooked);
    ZIP_HOOK(CreateFileMappingW, old_mapping_w, mapping_w_hooked);
    ZIP_HOOK(CreateFileMappingA, old_mapping_a, mapping_a_hooked);
    ZIP_HOOK(SetFileInformationByHandle, old_set_info, set_info_hooked);
#undef ZIP_HOOK
}

bool ml_zip_file_hooks_prepare(void) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    query_object = ntdll == NULL ? NULL : (void *)GetProcAddress(ntdll, "NtQueryObject");
    queue_file_apc = ntdll == NULL ? NULL : (void *)GetProcAddress(ntdll, "NtQueueApcThread");
    status_to_error = ntdll == NULL ? NULL : (void *)GetProcAddress(ntdll, "RtlNtStatusToDosError");
    if (query_object == NULL || queue_file_apc == NULL || status_to_error == NULL) return false;
    AcquireSRWLockExclusive(&handles_lock);
    InterlockedExchange(&accepting, TRUE);
    ReleaseSRWLockExclusive(&handles_lock);
    return true;
}

void ml_zip_file_close_all(void) {
    khash_t(zip_handles) *remaining;
    AcquireSRWLockExclusive(&handles_lock);
    InterlockedExchange(&accepting, FALSE);
#ifdef ML_WIN32_HOOKS_TEST
    if (test_shutdown_entered != NULL) SetEvent(test_shutdown_entered);
#endif
    while (ReadAcquire(&active_operations) != 0)
        SleepConditionVariableSRW(&operations_idle, &handles_lock, INFINITE, 0);
    remaining = handles;
    handles = NULL;
    InterlockedExchange(&handle_count, 0);
    ReleaseSRWLockExclusive(&handles_lock);
    if (remaining == NULL) return;
    /* The application still owns its handles: closing them here would make its
     * later CloseHandle close whatever reused the value. Without the hooks, the
     * events fail file I/O and close normally. */
    for (khiter_t slot = kh_begin(remaining); slot != kh_end(remaining); slot++)
        if (kh_exist(remaining, slot)) zip_file_release(kh_value(remaining, slot).file);
    kh_destroy(zip_handles, remaining);
}

#ifdef ML_WIN32_HOOKS_TEST
void ml_zip_file_hooks_test_init(void) {
    old_close_handle = CloseHandle;
    InterlockedExchange(&accepting, TRUE);
}
#endif

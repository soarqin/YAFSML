/*
 * C11 implementation of dearxan v0.5.3 functionality.
 *
 * Original project copyright (c) 2025 William Tremblay.
 * Licensed under the MIT license; see THIRD_PARTY_NOTICES.md.
 */

#include "dearxan.h"
#include "analysis.h"
#include "entry_point.h"
#include "image.h"
#include "patch.h"
#include "steamstub.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define DEARXAN_SCHEDULE_MAGIC UINT32_C(0x53584144)
#define DEARXAN_SCHEDULE_VERSION 1u

typedef void (*security_init_cookie_t)(void);

typedef struct schedule_callback_node {
    DearxanScheduleCallback callback;
    void *opaque;
    struct schedule_callback_node *next;
} schedule_callback_node_t;

typedef struct dearxan_schedule_shared {
    uint32_t magic;
    uint32_t version;
    size_t size;
    DWORD pid;
    volatile LONG state;
    volatile LONG result_ready;
    volatile LONG is_arxan_detected;
    volatile LONG is_executing_entrypoint;
    volatile LONG hook_ready;
    security_init_cookie_t original_security_init_cookie;
    unsigned char *security_init_cookie_call;
    schedule_callback_node_t *head;
    schedule_callback_node_t *tail;
    volatile LONG neuter_state;
    DearxanResult neuter_result;
    char neuter_error[256];
    dearxan_image_t scheduled_image;
    uint64_t steamstub_original_entrypoint;
    dearxan_steamstub_context_t *steamstub_context;
} dearxan_schedule_shared_t;

typedef struct neuter_context {
    DearxanUserCallback callback;
    void *opaque;
} neuter_context_t;

static INIT_ONCE schedule_once = INIT_ONCE_STATIC_INIT;
static dearxan_schedule_shared_t *schedule_shared;
static HANDLE schedule_mapping;
static HANDLE schedule_mutex;

static void flush_callbacks(bool is_arxan_detected,
                            bool is_executing_entrypoint);
static bool install_schedule_hook(void);

static void *allocate_near(uintptr_t origin, size_t size) {
    SYSTEM_INFO information;
    uintptr_t minimum;
    uintptr_t maximum;
    uintptr_t address;
    GetSystemInfo(&information);
    minimum = origin > INT32_MAX
        ? origin - INT32_MAX
        : (uintptr_t)information.lpMinimumApplicationAddress;
    if (minimum < (uintptr_t)information.lpMinimumApplicationAddress) {
        minimum = (uintptr_t)information.lpMinimumApplicationAddress;
    }
    maximum = origin <= UINTPTR_MAX - INT32_MAX
        ? origin + INT32_MAX
        : (uintptr_t)information.lpMaximumApplicationAddress;
    if (maximum > (uintptr_t)information.lpMaximumApplicationAddress) {
        maximum = (uintptr_t)information.lpMaximumApplicationAddress;
    }
    address = minimum - minimum % information.dwAllocationGranularity;
    if (address < minimum) address += information.dwAllocationGranularity;
    while (address <= maximum && size <= maximum - address) {
        MEMORY_BASIC_INFORMATION region;
        uintptr_t next;
        if (VirtualQuery((void *)address, &region, sizeof(region)) == 0) break;
        if (region.State == MEM_FREE && region.RegionSize >= size) {
            void *result = VirtualAlloc((void *)address, size,
                                        MEM_COMMIT | MEM_RESERVE,
                                        PAGE_EXECUTE_READWRITE);
            if (result != NULL) return result;
        }
        if ((uintptr_t)region.BaseAddress >
            UINTPTR_MAX - region.RegionSize) break;
        next = (uintptr_t)region.BaseAddress + region.RegionSize;
        if (next > UINTPTR_MAX - information.dwAllocationGranularity + 1) break;
        next = (next + information.dwAllocationGranularity - 1) -
               (next + information.dwAllocationGranularity - 1) %
                   information.dwAllocationGranularity;
        if (next <= address) break;
        address = next;
    }
    return NULL;
}

static void *make_absolute_jump_near(uintptr_t origin, const void *target) {
    unsigned char *bridge = allocate_near(origin, 14);
    if (bridge == NULL) return NULL;
    bridge[0] = 0xff;
    bridge[1] = 0x25;
    memset(bridge + 2, 0, 4);
    memcpy(bridge + 6, &target, sizeof(target));
    FlushInstructionCache(GetCurrentProcess(), bridge, 14);
    return bridge;
}

static BOOL CALLBACK initialize_schedule_mapping(PINIT_ONCE once, PVOID parameter,
                                                 PVOID *context) {
    wchar_t name[96];
    wchar_t mutex_name[104];
    dearxan_schedule_shared_t *shared;
    DWORD create_error;
    DWORD wait_result;
    (void)once;
    (void)parameter;
    (void)context;
    swprintf_s(name, sizeof(name) / sizeof(name[0]),
               L"Local\\DEARXAN_SCHEDULED_AFTER_ARXAN_%lu",
               (unsigned long)GetCurrentProcessId());
    swprintf_s(mutex_name, sizeof(mutex_name) / sizeof(mutex_name[0]),
               L"Local\\DEARXAN_SCHEDULED_AFTER_ARXAN_LOCK_%lu",
               (unsigned long)GetCurrentProcessId());
    schedule_mutex = CreateMutexW(NULL, FALSE, mutex_name);
    if (schedule_mutex == NULL) return FALSE;
    wait_result = WaitForSingleObject(schedule_mutex, INFINITE);
    if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_ABANDONED) {
        CloseHandle(schedule_mutex);
        schedule_mutex = NULL;
        return FALSE;
    }
    schedule_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL,
                                          PAGE_READWRITE, 0,
                                          sizeof(*shared), name);
    if (schedule_mapping == NULL) goto fail;
    create_error = GetLastError();
    shared = MapViewOfFile(schedule_mapping, FILE_MAP_ALL_ACCESS,
                           0, 0, sizeof(*shared));
    if (shared == NULL) {
        CloseHandle(schedule_mapping);
        schedule_mapping = NULL;
        goto fail;
    }
    if (create_error != ERROR_ALREADY_EXISTS) {
        memset(shared, 0, sizeof(*shared));
        shared->size = sizeof(*shared);
        shared->pid = GetCurrentProcessId();
        shared->version = DEARXAN_SCHEDULE_VERSION;
        MemoryBarrier();
        shared->magic = DEARXAN_SCHEDULE_MAGIC;
    } else if (shared->magic != DEARXAN_SCHEDULE_MAGIC ||
               shared->version != DEARXAN_SCHEDULE_VERSION ||
               shared->size < sizeof(*shared) ||
               shared->pid != GetCurrentProcessId()) {
        UnmapViewOfFile(shared);
        CloseHandle(schedule_mapping);
        schedule_mapping = NULL;
        goto fail;
    }
    schedule_shared = shared;
    ReleaseMutex(schedule_mutex);
    return TRUE;
fail:
    ReleaseMutex(schedule_mutex);
    CloseHandle(schedule_mutex);
    schedule_mutex = NULL;
    return FALSE;
}

static dearxan_schedule_shared_t *get_schedule_shared(void) {
    if (!InitOnceExecuteOnce(&schedule_once, initialize_schedule_mapping,
                             NULL, NULL)) return NULL;
    return schedule_shared;
}

static void invoke_callbacks(schedule_callback_node_t *callbacks,
                             bool is_arxan_detected,
                             bool is_executing_entrypoint) {
    while (callbacks != NULL) {
        schedule_callback_node_t *next = callbacks->next;
        callbacks->callback(is_arxan_detected, is_executing_entrypoint,
                            callbacks->opaque);
        HeapFree(GetProcessHeap(), 0, callbacks);
        callbacks = next;
    }
}

static void pin_callback_module(DearxanScheduleCallback callback) {
    HMODULE module;
    (void)GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                             (LPCWSTR)(uintptr_t)callback, &module);
}

static void flush_callbacks(bool is_arxan_detected,
                            bool is_executing_entrypoint) {
    dearxan_schedule_shared_t *shared = get_schedule_shared();
    schedule_callback_node_t *callbacks;
    if (shared == NULL) return;
    WaitForSingleObject(schedule_mutex, INFINITE);
    shared->is_arxan_detected = is_arxan_detected;
    if (shared->result_ready == 0) {
        shared->is_executing_entrypoint = is_executing_entrypoint;
    }
    shared->result_ready = 1;
    callbacks = shared->head;
    shared->head = NULL;
    shared->tail = NULL;
    InterlockedExchange(&shared->state, 2); /* callback drain in progress */
    ReleaseMutex(schedule_mutex);
    for (;;) {
        invoke_callbacks(callbacks, is_arxan_detected,
                         is_executing_entrypoint);
        WaitForSingleObject(schedule_mutex, INFINITE);
        callbacks = shared->head;
        shared->head = NULL;
        shared->tail = NULL;
        if (callbacks == NULL) {
            InterlockedExchange(&shared->state, 3); /* complete */
            ReleaseMutex(schedule_mutex);
            break;
        }
        ReleaseMutex(schedule_mutex);
    }
}

static bool write_call_target(unsigned char *call, const void *target) {
    intptr_t displacement;
    int32_t relative;
    DWORD old_protection;
    if (call == NULL || target == NULL || call[0] != 0xe8) return false;
    displacement = (const unsigned char *)target - (call + 5);
    if (displacement < INT32_MIN || displacement > INT32_MAX) return false;
    relative = (int32_t)displacement;
    if (!VirtualProtect(call + 1, sizeof(relative), PAGE_EXECUTE_READWRITE,
                        &old_protection)) return false;
    memcpy(call + 1, &relative, sizeof(relative));
    if (!VirtualProtect(call + 1, sizeof(relative), old_protection,
                        &old_protection)) return false;
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    return true;
}

static void security_init_cookie_detour(void) {
    bool detected = false;
    dearxan_schedule_shared_t *shared = get_schedule_shared();
    if (shared == NULL || shared->original_security_init_cookie == NULL) {
        flush_callbacks(false, true);
        return;
    }
    (void)write_call_target(shared->security_init_cookie_call,
                            (const void *)shared->original_security_init_cookie);
    shared->original_security_init_cookie();
    if (shared != NULL) detected = shared->is_arxan_detected != 0;
    flush_callbacks(detected, true);
}

static bool install_schedule_hook(void) {
    dearxan_msvc_entrypoint_t entrypoint;
    uint64_t entrypoint_va;
    dearxan_schedule_shared_t *shared = get_schedule_shared();
    if (shared == NULL) return false;
    if (shared->scheduled_image.base == NULL &&
        !dearxan_image_from_module(GetModuleHandleW(NULL),
                                   &shared->scheduled_image)) {
        return false;
    }
    entrypoint_va = shared->steamstub_original_entrypoint != 0
        ? shared->steamstub_original_entrypoint
        : shared->scheduled_image.base_va +
          shared->scheduled_image.entrypoint_rva;
    if (!dearxan_parse_msvc_entrypoint(&shared->scheduled_image, entrypoint_va,
                                       &entrypoint) ||
        entrypoint.security_init_cookie_call == NULL ||
        entrypoint.security_init_cookie_call[0] != 0xe8) return false;
    shared->security_init_cookie_call = entrypoint.security_init_cookie_call;
    shared->original_security_init_cookie =
        (security_init_cookie_t)(uintptr_t)entrypoint.security_init_cookie_va;
    InterlockedExchange(&shared->is_arxan_detected,
                        entrypoint.is_arxan_hooked);
    {
        void *bridge = make_absolute_jump_near(
            (uintptr_t)shared->security_init_cookie_call + 5,
            (const void *)security_init_cookie_detour);
        bool hooked = bridge != NULL &&
                      write_call_target(shared->security_init_cookie_call,
                                        bridge);
        if (hooked) InterlockedExchange(&shared->hook_ready, 1);
        return hooked;
    }
}

static uint64_t steamstub_entrypoint_detour(void) {
    typedef uint64_t (*entrypoint_t)(void);
    dearxan_schedule_shared_t *shared = get_schedule_shared();
    if (shared == NULL) return 0;
    if (InterlockedCompareExchange(&shared->hook_ready, 0, 0) != 0) {
        return ((entrypoint_t)(uintptr_t)
                shared->steamstub_original_entrypoint)();
    }
    dearxan_steamstub_uninit(shared->steamstub_context);
    free(shared->steamstub_context);
    shared->steamstub_context = NULL;
    if (!install_schedule_hook()) flush_callbacks(false, true);
    return ((entrypoint_t)(uintptr_t)shared->steamstub_original_entrypoint)();
}

static DWORD WINAPI post_entry_worker(LPVOID parameter) {
    dearxan_schedule_shared_t *shared = get_schedule_shared();
    bool detected = false;
    (void)parameter;
    if (shared == NULL) return 0;
    if (!dearxan_wait_for_gs_cookie(&shared->scheduled_image, INFINITE)) Sleep(1000);
    if (shared != NULL) detected = shared->is_arxan_detected != 0;
    flush_callbacks(detected, false);
    return 0;
}

static bool start_post_entry_worker(void) {
    HANDLE thread = CreateThread(NULL, 0, post_entry_worker, NULL, 0, NULL);
    if (thread == NULL) return false;
    CloseHandle(thread);
    return true;
}

static bool initialize_schedule(void) {
    dearxan_steamstub_probe_result_t steamstub;
    dearxan_schedule_shared_t *shared = get_schedule_shared();
    if (shared == NULL ||
        !dearxan_image_from_module(GetModuleHandleW(NULL),
                                   &shared->scheduled_image)) return false;
    shared->steamstub_context = calloc(1, sizeof(*shared->steamstub_context));
    if (shared->steamstub_context == NULL) return false;
    steamstub = dearxan_steamstub_probe(&shared->scheduled_image,
                                        shared->steamstub_context);
    if (steamstub == DEARXAN_STEAMSTUB_ERROR) {
        free(shared->steamstub_context);
        shared->steamstub_context = NULL;
        return false;
    }
    if (steamstub == DEARXAN_STEAMSTUB_PRESENT) {
        shared->steamstub_original_entrypoint =
            shared->scheduled_image.base_va +
            shared->steamstub_context->header.original_entry_point;
        if (!dearxan_is_pre_entry_point(&shared->scheduled_image)) {
            dearxan_msvc_entrypoint_t entrypoint;
            if (shared != NULL && dearxan_parse_msvc_entrypoint(
                    &shared->scheduled_image,
                    shared->steamstub_original_entrypoint, &entrypoint)) {
                InterlockedExchange(&shared->is_arxan_detected,
                                    entrypoint.is_arxan_hooked);
            }
            dearxan_steamstub_uninit(shared->steamstub_context);
            free(shared->steamstub_context);
            shared->steamstub_context = NULL;
            return start_post_entry_worker();
        }
        {
            if (!dearxan_steamstub_patch(
                &shared->scheduled_image, shared->steamstub_context,
                (uint64_t)(uintptr_t)steamstub_entrypoint_detour)) {
                dearxan_steamstub_uninit(shared->steamstub_context);
                free(shared->steamstub_context);
                shared->steamstub_context = NULL;
                return false;
            }
        }
        return true;
    }
    free(shared->steamstub_context);
    shared->steamstub_context = NULL;
    if (!dearxan_is_pre_entry_point(&shared->scheduled_image)) {
        dearxan_msvc_entrypoint_t entrypoint;
        if (!dearxan_parse_msvc_entrypoint(
                &shared->scheduled_image,
                shared->scheduled_image.base_va +
                shared->scheduled_image.entrypoint_rva,
                &entrypoint)) return false;
        if (shared != NULL) {
            InterlockedExchange(&shared->is_arxan_detected,
                                entrypoint.is_arxan_hooked);
        }
        return start_post_entry_worker();
    }
    return install_schedule_hook();
}

void dearxan_schedule_after_arxan(DearxanScheduleCallback callback, void *opaque) {
    dearxan_schedule_shared_t *shared;
    schedule_callback_node_t *node;
    bool initialize = false;
    bool run_now = false;
    bool detected = false;
    bool pre_entry_point;
    if (callback == NULL) return;
    shared = get_schedule_shared();
    if (shared == NULL) {
        callback(false, false, opaque);
        return;
    }
    node = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*node));
    if (node == NULL) {
        callback(false, false, opaque);
        return;
    }
    pre_entry_point = shared->scheduled_image.base == NULL ||
                      dearxan_is_pre_entry_point(&shared->scheduled_image);
    node->callback = callback;
    node->opaque = opaque;
    pin_callback_module(callback);
    WaitForSingleObject(schedule_mutex, INFINITE);
    if (shared->state == 3) {
        run_now = true;
        detected = shared->is_arxan_detected != 0;
    } else {
        if (shared->tail != NULL) shared->tail->next = node;
        else shared->head = node;
        shared->tail = node;
        if (shared->state == 0) {
            shared->state = 1;
            initialize = true;
        }
    }
    ReleaseMutex(schedule_mutex);
    if (run_now) {
        HeapFree(GetProcessHeap(), 0, node);
        callback(detected, false, opaque);
        return;
    }
    if (!initialize && !pre_entry_point &&
        InterlockedCompareExchange(&shared->state, 0, 0) != 2) {
        while (InterlockedCompareExchange(&shared->state, 3, 3) != 3) {
            Sleep(0);
        }
        {
            schedule_callback_node_t *callbacks;
            bool present;
            WaitForSingleObject(schedule_mutex, INFINITE);
            callbacks = shared->head;
            shared->head = NULL;
            shared->tail = NULL;
            present = shared->is_arxan_detected != 0;
            ReleaseMutex(schedule_mutex);
            invoke_callbacks(callbacks, present, false);
        }
        return;
    }
    if (initialize && !initialize_schedule()) {
        /* Preserve BeforeMain ordering even when binary analysis cannot install
           a synchronized hook. */
        flush_callbacks(false, false);
    }
}

static DearxanResult make_result(int status, const char *error_message,
                                 bool is_present,
                                 bool is_executing_entrypoint) {
    DearxanResult result = {
        DEARXAN_RESULT_SIZE, status, error_message, 0,
        is_present, is_executing_entrypoint, 0
    };
    if (error_message != NULL) result.error_msg_size = strlen(error_message);
    return result;
}

static DearxanResult neuter_once(bool is_executing_entrypoint) {
    dearxan_image_t image;
    dearxan_stub_list_t stubs;
    dearxan_suspend_guard_t guard = { 0 };
    wchar_t image_path[32768];
    DWORD image_path_size;
    const char *error_message = NULL;
    bool suspended = false;
    DearxanResult result;
    if (!is_executing_entrypoint) {
        suspended = dearxan_suspend_other_threads(&guard);
        if (!suspended) {
            return make_result(DearxanError,
                               "failed to suspend process threads", true,
                               false);
        }
    }
    if (!dearxan_image_from_module(GetModuleHandleW(NULL), &image)) {
        result = make_result(DearxanError,
                             "failed to initialize executable image", true,
                             is_executing_entrypoint);
    } else if ((image_path_size = GetModuleFileNameW(
                    NULL, image_path,
                    (DWORD)(sizeof(image_path) / sizeof(image_path[0])))) == 0 ||
               image_path_size >= sizeof(image_path) / sizeof(image_path[0]) ||
               !dearxan_image_set_preferred_base_from_file(&image, image_path)) {
        result = make_result(DearxanError,
                             "failed to read executable preferred base", true,
                             is_executing_entrypoint);
    } else if (!dearxan_image_make_sections_rwe(&image)) {
        result = make_result(DearxanError,
                             "failed to make executable sections writable", true,
                             is_executing_entrypoint);
    } else if (!dearxan_analyze_all_stubs(&image, &stubs, &error_message)) {
        result = make_result(DearxanError,
                             error_message != NULL ? error_message
                                                   : "failed to analyze Arxan stubs",
                             true, is_executing_entrypoint);
    } else if (stubs.count == 0) {
        dearxan_free_stub_list(&stubs);
        result = make_result(DearxanError,
                             "Arxan was detected but no valid stubs were found",
                             true, is_executing_entrypoint);
    } else {
        bool patched = dearxan_apply_stub_patches(&image, &stubs,
                                                   &error_message);
        dearxan_free_stub_list(&stubs);
        result = patched
            ? make_result(DearxanSuccess, NULL, true,
                          is_executing_entrypoint)
            : make_result(DearxanError,
                          error_message != NULL ? error_message
                                                : "failed to apply Arxan patches",
                          true, is_executing_entrypoint);
    }
    if (suspended) dearxan_resume_threads(&guard);
    return result;
}

static void publish_neuter_result(dearxan_schedule_shared_t *shared,
                                  const DearxanResult *result) {
    shared->neuter_result = *result;
    if (result->error_msg != NULL && result->error_msg_size != 0) {
        size_t size = result->error_msg_size;
        if (size >= sizeof(shared->neuter_error)) {
            size = sizeof(shared->neuter_error) - 1;
        }
        memcpy(shared->neuter_error, result->error_msg, size);
        shared->neuter_error[size] = '\0';
        shared->neuter_result.error_msg = shared->neuter_error;
        shared->neuter_result.error_msg_size = size;
    } else {
        shared->neuter_error[0] = '\0';
        shared->neuter_result.error_msg = NULL;
        shared->neuter_result.error_msg_size = 0;
    }
    MemoryBarrier();
    InterlockedExchange(&shared->neuter_state, 2);
}

static DearxanResult get_neuter_result(bool is_executing_entrypoint) {
    dearxan_schedule_shared_t *shared = get_schedule_shared();
    DearxanResult result;
    bool initialize = false;
    if (shared == NULL) {
        return make_result(DearxanError,
                           "failed to access process-wide neutralization state",
                           true, is_executing_entrypoint);
    }
    WaitForSingleObject(schedule_mutex, INFINITE);
    if (shared->neuter_state == 0) {
        shared->neuter_state = 1;
        initialize = true;
    }
    ReleaseMutex(schedule_mutex);
    if (initialize) {
        result = neuter_once(is_executing_entrypoint);
        WaitForSingleObject(schedule_mutex, INFINITE);
        publish_neuter_result(shared, &result);
        ReleaseMutex(schedule_mutex);
    } else {
        while (InterlockedCompareExchange(&shared->neuter_state, 2, 2) != 2) {
            Sleep(0);
        }
    }
    WaitForSingleObject(schedule_mutex, INFINITE);
    result = shared->neuter_result;
    ReleaseMutex(schedule_mutex);
    if (result.result_size > DEARXAN_RESULT_SIZE) {
        result.result_size = DEARXAN_RESULT_SIZE;
    }
    result.is_executing_entrypoint = is_executing_entrypoint;
    return result;
}

static void neuter_after_arxan(bool is_present, bool is_executing_entrypoint,
                               void *opaque) {
    neuter_context_t *context = opaque;
    DearxanResult result;
    if (!is_present) {
        result = make_result(DearxanSuccess, NULL, false,
                             is_executing_entrypoint);
        if (context->callback != NULL) {
            context->callback(&result, context->opaque);
        }
        HeapFree(GetProcessHeap(), 0, context);
        return;
    }
    result = get_neuter_result(is_executing_entrypoint);
    if (context->callback != NULL) {
        context->callback(&result, context->opaque);
    }
    HeapFree(GetProcessHeap(), 0, context);
}

void dearxan_neuter_arxan(DearxanUserCallback callback, void *opaque) {
    neuter_context_t *context;
    context = HeapAlloc(GetProcessHeap(), 0, sizeof(*context));
    if (context == NULL) {
        if (callback != NULL) {
            DearxanResult result = make_result(
                DearxanError, "failed to allocate callback context", false,
                false);
            callback(&result, opaque);
        }
        return;
    }
    context->callback = callback;
    context->opaque = opaque;
    dearxan_schedule_after_arxan(neuter_after_arxan, context);
}

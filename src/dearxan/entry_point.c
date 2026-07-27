/*
 * C11 implementation of dearxan v0.5.3 functionality.
 *
 * Original project copyright (c) 2025 William Tremblay.
 * Licensed under the MIT license; see THIRD_PARTY_NOTICES.md.
 */

#include "entry_point.h"
#include "vm.h"

#include <Zydis/Zydis.h>

#include <stdlib.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string.h>

#define DEARXAN_UNINITIALIZED_GS_COOKIE UINT64_C(0x2b992ddfa232)

typedef LONG NTSTATUS;
typedef NTSTATUS (NTAPI *nt_get_next_thread_t)(HANDLE, HANDLE, ACCESS_MASK,
                                               ULONG, ULONG, PHANDLE);
typedef NTSTATUS (NTAPI *nt_query_information_thread_t)(HANDLE, ULONG, PVOID,
                                                        ULONG, PULONG);

static volatile LONG gs_cookie_state;
static uint64_t gs_cookie_address;

typedef struct cookie_scan_context {
    const dearxan_image_t *image;
    uint64_t addresses[256];
    unsigned short counts[256];
    size_t count;
    uint64_t found;
} cookie_scan_context_t;

static bool scan_cookie_references(uint64_t va, const unsigned char *bytes,
                                   size_t size, void *opaque) {
    static const unsigned char pattern[] = { 0x48, 0x33, 0xc4 };
    cookie_scan_context_t *context = opaque;
    for (size_t i = 4; i + sizeof(pattern) <= size; i++) {
        int32_t displacement;
        uint64_t address;
        size_t index;
        if (memcmp(bytes + i, pattern, sizeof(pattern)) != 0) continue;
        memcpy(&displacement, bytes + i - 4, sizeof(displacement));
        address = va + i + displacement;
        for (index = 0; index < context->count; index++) {
            if (context->addresses[index] == address) break;
        }
        if (index == context->count) {
            if (context->count == sizeof(context->addresses) / sizeof(context->addresses[0])) continue;
            context->addresses[index] = address;
            context->counts[index] = 0;
            context->count++;
        }
        if (++context->counts[index] >= 16) {
            context->found = address;
            return false;
        }
    }
    return true;
}

static uint64_t find_gs_cookie(const dearxan_image_t *image) {
    cookie_scan_context_t context = { 0 };
    (void)dearxan_image_for_each_section(image, scan_cookie_references, &context);
    return context.found;
}

static uint64_t get_gs_cookie(const dearxan_image_t *image) {
    LONG state = InterlockedCompareExchange(&gs_cookie_state, 1, 0);
    if (state == 0) {
        gs_cookie_address = find_gs_cookie(image);
        InterlockedExchange(&gs_cookie_state, 2);
    } else {
        while (InterlockedCompareExchange(&gs_cookie_state, 2, 2) != 2) {
            YieldProcessor();
        }
    }
    return gs_cookie_address;
}

static HANDLE process_main_thread(const dearxan_image_t *image) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    nt_get_next_thread_t get_next = ntdll != NULL
        ? (nt_get_next_thread_t)GetProcAddress(ntdll, "NtGetNextThread") : NULL;
    nt_query_information_thread_t query = ntdll != NULL
        ? (nt_query_information_thread_t)GetProcAddress(ntdll, "NtQueryInformationThread") : NULL;
    HANDLE previous = NULL;
    if (get_next == NULL || query == NULL) return NULL;
    for (;;) {
        HANDLE next = NULL;
        uint64_t start_address = 0;
        NTSTATUS status = get_next(GetCurrentProcess(), previous, THREAD_ALL_ACCESS,
                                   0, 0, &next);
        if (previous != NULL) CloseHandle(previous);
        if (status < 0 || next == NULL) return NULL;
        if (query(next, 9, &start_address, sizeof(start_address), NULL) >= 0 &&
            start_address == image->base_va + image->entrypoint_rva) return next;
        previous = next;
    }
}

static bool thread_created_suspended(HANDLE thread, const dearxan_image_t *image) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    nt_query_information_thread_t query = ntdll != NULL
        ? (nt_query_information_thread_t)GetProcAddress(ntdll, "NtQueryInformationThread") : NULL;
    ULONG suspend_count = 0;
    CONTEXT context = { 0 };
    FARPROC rtl_user_thread_start = ntdll != NULL
        ? GetProcAddress(ntdll, "RtlUserThreadStart") : NULL;
    if (query == NULL || query(thread, 35, &suspend_count,
                               sizeof(suspend_count), NULL) < 0 || suspend_count == 0) return false;
    context.ContextFlags = CONTEXT_FULL;
    if (!GetThreadContext(thread, &context)) return false;
    return context.Rip == (DWORD64)(uintptr_t)rtl_user_thread_start ||
           context.Rcx == image->base_va + image->entrypoint_rva;
}

static BOOL CALLBACK initialize_decoder(PINIT_ONCE once, PVOID parameter,
                                        PVOID *context) {
    ZydisDecoder *decoder = parameter;
    (void)once;
    (void)context;
    return ZYAN_SUCCESS(ZydisDecoderInit(decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                         ZYDIS_STACK_WIDTH_64));
}

static bool decode_at(const dearxan_image_t *image, uint64_t va,
                      ZydisDecodedInstruction *instruction,
                      ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]) {
    static ZydisDecoder decoder;
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    size_t available;
    const unsigned char *bytes;
    if (!InitOnceExecuteOnce(&once, initialize_decoder, &decoder, NULL)) return false;
    bytes = dearxan_image_read(image, va, 1, &available);
    if (bytes == NULL) return false;
    return ZYAN_SUCCESS(ZydisDecoderDecodeFull(
        &decoder, bytes, available, instruction, operands));
}

static bool relative_target(uint64_t ip, const ZydisDecodedInstruction *instruction,
                            const ZydisDecodedOperand *operand, uint64_t *target) {
    ZyanU64 address;
    if (operand->type != ZYDIS_OPERAND_TYPE_IMMEDIATE ||
        !operand->imm.is_relative) return false;
    if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(instruction, operand, ip, &address))) return false;
    *target = address;
    return true;
}

static bool detect_arxan_hook(const dearxan_image_t *image, uint64_t start_va) {
    dearxan_vm_t vm;
    size_t steps = 0;
    dearxan_vm_init(&vm, image, start_va, 0x10000);
    while (vm.rip_known && steps++ < 0x100) {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        dearxan_vm_t forked;
        bool has_fork = false;
        if (!decode_at(image, vm.rip, &instruction, operands)) break;
        if (instruction.mnemonic == ZYDIS_MNEMONIC_TEST &&
            instruction.operand_count_visible == 2 &&
            operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
            operands[0].reg.value == ZYDIS_REGISTER_RSP &&
            operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
            operands[1].imm.value.u == 0x0f) {
            dearxan_vm_uninit(&vm);
            return true;
        }
        if (!dearxan_vm_step(&vm, &instruction, operands,
                             &forked, &has_fork)) break;
        if (has_fork) dearxan_vm_uninit(&forked);
    }
    dearxan_vm_uninit(&vm);
    return false;
}

bool dearxan_parse_msvc_entrypoint(const dearxan_image_t *image,
                                   uint64_t entrypoint_va,
                                   dearxan_msvc_entrypoint_t *result) {
    static const ZydisMnemonic expected[] = {
        ZYDIS_MNEMONIC_SUB,
        ZYDIS_MNEMONIC_CALL,
        ZYDIS_MNEMONIC_ADD,
        ZYDIS_MNEMONIC_JMP
    };
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    uint64_t ip = entrypoint_va;
    dearxan_msvc_entrypoint_t parsed = { 0 };
    if (image == NULL || result == NULL) return false;
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        if (!decode_at(image, ip, &instruction, operands) ||
            instruction.mnemonic != expected[i]) return false;
        if (i == 0 || i == 2) {
            if (instruction.operand_count_visible != 2 ||
                operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
                operands[0].reg.value != ZYDIS_REGISTER_RSP ||
                operands[1].type != ZYDIS_OPERAND_TYPE_IMMEDIATE) return false;
        } else {
            uint64_t target;
            if (instruction.operand_count_visible != 1 ||
                !relative_target(ip, &instruction, &operands[0], &target)) return false;
            if (i == 1) {
                parsed.security_init_cookie_va = target;
                parsed.security_init_cookie_call =
                    (unsigned char *)image->base + (size_t)(ip - image->base_va);
            } else {
                parsed.scrt_common_main_seh_va = target;
            }
        }
        ip += instruction.length;
    }
    *result = parsed;
    result->is_arxan_hooked = detect_arxan_hook(image,
                                                parsed.security_init_cookie_va);
    return true;
}

bool dearxan_is_pre_entry_point(const dearxan_image_t *image) {
    HANDLE main_thread;
    DWORD main_thread_id;
    uint64_t cookie;
    if (image == NULL) return true;
    main_thread = process_main_thread(image);
    if (main_thread == NULL) return true;
    main_thread_id = GetThreadId(main_thread);
    if (main_thread_id == GetCurrentThreadId()) {
        cookie = get_gs_cookie(image);
        CloseHandle(main_thread);
        if (cookie == 0) return false;
        return *(volatile uint64_t *)(uintptr_t)cookie ==
               DEARXAN_UNINITIALIZED_GS_COOKIE;
    }
    {
        bool suspended = thread_created_suspended(main_thread, image);
        CloseHandle(main_thread);
        return suspended;
    }
}

bool dearxan_wait_for_gs_cookie(const dearxan_image_t *image,
                                unsigned long timeout_ms) {
    uint64_t cookie;
    ULONGLONG start = GetTickCount64();
    if (image == NULL) return false;
    cookie = get_gs_cookie(image);
    if (cookie == 0) return false;
    while (timeout_ms == INFINITE || GetTickCount64() - start < timeout_ms) {
        if (*(volatile uint64_t *)(uintptr_t)cookie !=
            DEARXAN_UNINITIALIZED_GS_COOKIE) return true;
        Sleep(10);
    }
    return false;
}

bool dearxan_suspend_other_threads(dearxan_suspend_guard_t *guard) {
    HMODULE ntdll;
    nt_get_next_thread_t get_next;
    HANDLE previous = NULL;
    HANDLE *threads = NULL;
    size_t count = 0;
    size_t capacity = 0;
    DWORD current_thread;
    bool complete = false;
    if (guard == NULL) return false;
    memset(guard, 0, sizeof(*guard));
    ntdll = GetModuleHandleW(L"ntdll.dll");
    get_next = ntdll != NULL
        ? (nt_get_next_thread_t)GetProcAddress(ntdll, "NtGetNextThread") : NULL;
    if (get_next == NULL) return false;
    current_thread = GetCurrentThreadId();
    for (;;) {
        HANDLE next = NULL;
        NTSTATUS status = get_next(GetCurrentProcess(), previous,
                                   THREAD_SUSPEND_RESUME |
                                   THREAD_QUERY_INFORMATION, 0, 0, &next);
        if (previous != NULL) CloseHandle(previous);
        previous = NULL;
        if (status < 0 || next == NULL) {
            complete = true;
            break;
        }
        if (GetThreadId(next) == current_thread || SuspendThread(next) == (DWORD)-1) {
            previous = next;
            continue;
        }
        {
            HANDLE owned = NULL;
            if (count == capacity) {
                size_t next_capacity = capacity == 0 ? 16 : capacity * 2;
                HANDLE *next_threads;
                if (next_capacity < capacity ||
                    next_capacity > SIZE_MAX / sizeof(*next_threads)) {
                    ResumeThread(next);
                    CloseHandle(next);
                    for (size_t i = 0; i < count; i++) {
                        ResumeThread(threads[i]);
                        CloseHandle(threads[i]);
                    }
                    free(threads);
                    return false;
                }
                next_threads = realloc(threads, next_capacity * sizeof(*next_threads));
                if (next_threads == NULL) {
                    ResumeThread(next);
                    CloseHandle(next);
                    for (size_t i = 0; i < count; i++) {
                        ResumeThread(threads[i]);
                        CloseHandle(threads[i]);
                    }
                    free(threads);
                    return false;
                }
                threads = next_threads;
                capacity = next_capacity;
            }
            if (!DuplicateHandle(GetCurrentProcess(), next, GetCurrentProcess(),
                                 &owned, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
                ResumeThread(next);
                previous = next;
                continue;
            }
            threads[count++] = owned;
        }
        previous = next;
    }
    if (!complete) {
        for (size_t i = 0; i < count; i++) {
            ResumeThread(threads[i]);
            CloseHandle(threads[i]);
        }
        free(threads);
        return false;
    }
    guard->threads = (void **)threads;
    guard->count = count;
    return true;
}

void dearxan_resume_threads(dearxan_suspend_guard_t *guard) {
    HANDLE *threads;
    if (guard == NULL) return;
    threads = (HANDLE *)guard->threads;
    for (size_t i = 0; i < guard->count; i++) {
        ResumeThread(threads[i]);
        CloseHandle(threads[i]);
    }
    free(threads);
    memset(guard, 0, sizeof(*guard));
}

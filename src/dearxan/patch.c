/*
 * C11 implementation of dearxan v0.5.3 functionality.
 *
 * Original project copyright (c) 2025 William Tremblay.
 * Licensed under the MIT license; see THIRD_PARTY_NOTICES.md.
 */

#include "patch.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define DEARXAN_CODE_BUFFER_SIZE (16u * 1024u * 1024u)

static unsigned char *code_buffer;
static volatile LONG64 code_buffer_offset;

static void *allocate_near_code(const dearxan_image_t *image, size_t size) {
    SYSTEM_INFO system_info;
    uintptr_t min_address;
    uintptr_t max_address;
    uintptr_t address;
    LONG64 old_offset;
    LONG64 next_offset;
    if (code_buffer == NULL) {
        GetSystemInfo(&system_info);
        min_address = image->base_va + image->size > (uint64_t)INT32_MAX
            ? (uintptr_t)(image->base_va + image->size - INT32_MAX)
            : (uintptr_t)system_info.lpMinimumApplicationAddress;
        if (min_address < (uintptr_t)system_info.lpMinimumApplicationAddress) {
            min_address = (uintptr_t)system_info.lpMinimumApplicationAddress;
        }
        max_address = image->base_va > UINTPTR_MAX - INT32_MAX
            ? (uintptr_t)system_info.lpMaximumApplicationAddress
            : (uintptr_t)image->base_va + INT32_MAX;
        if (max_address > (uintptr_t)system_info.lpMaximumApplicationAddress) {
            max_address = (uintptr_t)system_info.lpMaximumApplicationAddress;
        }
        address = (min_address + system_info.dwAllocationGranularity - 1) -
                  ((min_address + system_info.dwAllocationGranularity - 1) %
                   system_info.dwAllocationGranularity);
        while (max_address >= DEARXAN_CODE_BUFFER_SIZE &&
               address <= max_address - DEARXAN_CODE_BUFFER_SIZE) {
            MEMORY_BASIC_INFORMATION information;
            if (VirtualQuery((void *)address, &information, sizeof(information)) == 0) break;
            if (information.State == MEM_FREE &&
                information.RegionSize >= DEARXAN_CODE_BUFFER_SIZE) {
                unsigned char *allocated = VirtualAlloc((void *)address,
                    DEARXAN_CODE_BUFFER_SIZE, MEM_COMMIT | MEM_RESERVE,
                    PAGE_EXECUTE_READWRITE);
                if (allocated != NULL) {
                    code_buffer = allocated;
                    break;
                }
            }
            if ((uintptr_t)information.BaseAddress > UINTPTR_MAX - information.RegionSize) break;
            address = (uintptr_t)information.BaseAddress + information.RegionSize;
            address = (address + system_info.dwAllocationGranularity - 1) -
                      ((address + system_info.dwAllocationGranularity - 1) %
                       system_info.dwAllocationGranularity);
        }
    }
    if (code_buffer == NULL || size > DEARXAN_CODE_BUFFER_SIZE) return NULL;
    do {
        old_offset = InterlockedCompareExchange64(&code_buffer_offset, 0, 0);
        if (old_offset < 0 || (uint64_t)old_offset > DEARXAN_CODE_BUFFER_SIZE - size) return NULL;
        next_offset = old_offset + (LONG64)size;
    } while (InterlockedCompareExchange64(&code_buffer_offset, next_offset,
                                          old_offset) != old_offset);
    return code_buffer + old_offset;
}

static bool write_memory(void *target, const void *source, size_t size) {
    DWORD old_protection;
    if (!VirtualProtect(target, size, PAGE_EXECUTE_READWRITE, &old_protection)) return false;
    memcpy(target, source, size);
    VirtualProtect(target, size, old_protection, &old_protection);
    FlushInstructionCache(GetCurrentProcess(), target, size);
    return true;
}

static bool emit_stub_patch(const dearxan_stub_info_t *stub,
                            unsigned char *bytes, size_t capacity,
                            size_t *size) {
    size_t offset = 0;
    if (capacity < 64) return false;
    if (stub->has_return_gadget) {
        intptr_t low_slot = (intptr_t)stub->return_gadget.stack_offset - 16;
        size_t lea_displacement_offset;
        size_t return_gadget_offset;
        bytes[offset++] = 0x48; bytes[offset++] = 0x8d; bytes[offset++] = 0x05;
        lea_displacement_offset = offset;
        offset += 4;
        if (low_slot >= INT8_MIN && low_slot <= INT8_MAX) {
            bytes[offset++] = 0x48; bytes[offset++] = 0x89; bytes[offset++] = 0x44;
            bytes[offset++] = 0x24; bytes[offset++] = (unsigned char)(int8_t)low_slot;
        } else if (low_slot >= INT32_MIN && low_slot <= INT32_MAX) {
            bytes[offset++] = 0x48; bytes[offset++] = 0x89; bytes[offset++] = 0x84;
            bytes[offset++] = 0x24;
            int32_t displacement = (int32_t)low_slot;
            memcpy(bytes + offset, &displacement, sizeof(displacement));
            offset += 4;
        } else {
            return false;
        }
        bytes[offset++] = 0x48; bytes[offset++] = 0x83;
        bytes[offset++] = 0xec; bytes[offset++] = 0x08;
        bytes[offset++] = 0x48; bytes[offset++] = 0xb8;
        memcpy(bytes + offset, &stub->context_pop_va, sizeof(stub->context_pop_va));
        offset += sizeof(stub->context_pop_va);
        bytes[offset++] = 0xff; bytes[offset++] = 0xe0;
        return_gadget_offset = offset;
        {
            int32_t lea_displacement =
                (int32_t)(return_gadget_offset - (lea_displacement_offset + 4));
            memcpy(bytes + lea_displacement_offset, &lea_displacement,
                   sizeof(lea_displacement));
        }
        bytes[offset++] = 0x48; bytes[offset++] = 0x83;
        bytes[offset++] = 0xc4; bytes[offset++] = 0x10;
        bytes[offset++] = 0xff; bytes[offset++] = 0x25;
        memset(bytes + offset, 0, sizeof(int32_t));
        offset += sizeof(int32_t);
        memcpy(bytes + offset, &stub->return_gadget.address,
               sizeof(stub->return_gadget.address));
        offset += sizeof(stub->return_gadget.address);
        *size = offset;
        return true;
    }
    bytes[offset++] = 0x48; bytes[offset++] = 0x83; bytes[offset++] = 0xec; bytes[offset++] = 0x08;
    bytes[offset++] = 0x48; bytes[offset++] = 0xb8;
    memcpy(bytes + offset, &stub->context_pop_va, sizeof(stub->context_pop_va));
    offset += sizeof(stub->context_pop_va);
    bytes[offset++] = 0xff; bytes[offset++] = 0xe0;
    *size = offset;
    return true;
}

bool dearxan_apply_stub_patches(const dearxan_image_t *image,
                                const dearxan_stub_list_t *stubs,
                                const char **error_message) {
    const dearxan_encrypted_region_list_t **region_lists = NULL;
    dearxan_encrypted_region_list_t *resolved = NULL;
    size_t region_list_count = 0;
    size_t resolved_count = 0;
    if (image == NULL || stubs == NULL) return false;
    if (stubs->count != 0) {
        region_lists = malloc(stubs->count * sizeof(*region_lists));
        if (region_lists == NULL) {
            if (error_message != NULL) *error_message = "failed to allocate patch list";
            return false;
        }
    }
    for (size_t i = 0; i < stubs->count; i++) {
        if (stubs->items[i].has_encrypted_regions) {
            region_lists[region_list_count++] = &stubs->items[i].encrypted_regions;
        }
    }
    if (!dearxan_resolve_encrypted_region_lists(image, region_lists,
            region_list_count, &resolved, &resolved_count)) {
        free(region_lists);
        if (error_message != NULL) *error_message = "failed to process encrypted regions";
        return false;
    }
    free(region_lists);
    for (size_t i = 0; i < resolved_count; i++) {
        const dearxan_encrypted_region_list_t *list = &resolved[i];
        for (size_t j = 0; j < list->region_count; j++) {
            const dearxan_encrypted_region_t *region =
                &list->regions[j];
            if (region->stream_offset > list->decrypted_size ||
                region->size > list->decrypted_size - region->stream_offset ||
                dearxan_image_read(image, image->base_va + region->rva,
                                   region->size, NULL) == NULL ||
                !write_memory((void *)(uintptr_t)(image->base_va + region->rva),
                               list->decrypted_stream + region->stream_offset,
                               region->size)) {
                dearxan_free_encrypted_region_lists(resolved, resolved_count);
                if (error_message != NULL) *error_message = "failed to write decrypted region";
                return false;
            }
        }
    }
    dearxan_free_encrypted_region_lists(resolved, resolved_count);
    for (size_t i = 0; i < stubs->count; i++) {
        unsigned char patch[64];
        size_t patch_size;
        unsigned char jump[5];
        intptr_t displacement;
        void *buffer;
        if (!emit_stub_patch(&stubs->items[i], patch, sizeof(patch), &patch_size)) {
            if (error_message != NULL) *error_message = "unsupported Arxan return gadget";
            return false;
        }
        buffer = allocate_near_code(image, patch_size);
        if (buffer == NULL) {
            if (error_message != NULL) *error_message = "failed to allocate patch code";
            return false;
        }
        memcpy(buffer, patch, patch_size);
        displacement = (unsigned char *)buffer -
                       ((unsigned char *)(uintptr_t)stubs->items[i].test_rsp_va + 5);
        if (displacement < INT32_MIN || displacement > INT32_MAX) {
            if (error_message != NULL) *error_message = "patch code is outside rel32 range";
            return false;
        }
        jump[0] = 0xe9;
        {
            int32_t displacement32 = (int32_t)displacement;
            memcpy(jump + 1, &displacement32, sizeof(displacement32));
        }
        if (!write_memory((void *)(uintptr_t)stubs->items[i].test_rsp_va,
                          jump, sizeof(jump))) {
            if (error_message != NULL) *error_message = "failed to install stub jump";
            return false;
        }
    }
    if (error_message != NULL) *error_message = NULL;
    return true;
}

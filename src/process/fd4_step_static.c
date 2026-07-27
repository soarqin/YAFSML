#include "fd4_step_static.h"

#include <stdbool.h>
#include <string.h>

enum {
    FD4_STEP_STATIC_INITIALIZER_SIZE = 28,
};

static const uint8_t initializer_pattern[] = {
    0x48, 0x8d, 0x05, 0, 0, 0, 0,
    0x48, 0x89, 0x05, 0, 0, 0, 0,
    0x48, 0x8d, 0x05, 0, 0, 0, 0,
    0x48, 0x89, 0x05, 0, 0, 0, 0,
};

static const uint8_t initializer_mask[] = {
    1, 1, 1, 0, 0, 0, 0,
    1, 1, 1, 0, 0, 0, 0,
    1, 1, 1, 0, 0, 0, 0,
    1, 1, 1, 0, 0, 0, 0,
};

static bool initializer_matches(const uint8_t *code) {
    for (size_t i = 0; i < FD4_STEP_STATIC_INITIALIZER_SIZE; i++) {
        if (initializer_mask[i] != 0 && code[i] != initializer_pattern[i]) {
            return false;
        }
    }
    return true;
}

static bool range_contains(uintptr_t start, size_t size, uintptr_t address,
                           size_t needed) {
    return address >= start && needed <= size && address - start <= size - needed;
}

static uintptr_t rip_target(uintptr_t instruction_va, const uint8_t *disp32) {
    int32_t displacement;
    memcpy(&displacement, disp32, sizeof(displacement));
    return instruction_va + 7 + (intptr_t)displacement;
}

static bool step_name_equals(const fd4_step_static_sections_t *sections,
                             uintptr_t name_va, const wchar_t *step_name) {
    size_t expected_len = wcslen(step_name);
    size_t expected_size;
    size_t name_offset;

    if (expected_len > (SIZE_MAX / sizeof(wchar_t)) - 1) return false;
    expected_size = (expected_len + 1) * sizeof(wchar_t);
    if (!range_contains(sections->rdata_va, sections->rdata_size,
                        name_va, expected_size)) return false;
    name_offset = (size_t)(name_va - sections->rdata_va);
    return memcmp(sections->rdata + name_offset, step_name, expected_size) == 0;
}

bool fd4_step_static_find(const fd4_step_static_sections_t *sections,
                          const wchar_t *step_name,
                          fd4_step_static_result_t *result) {
    if (sections == NULL || step_name == NULL || sections->text == NULL ||
        result == NULL || sections->rdata == NULL ||
        sections->text_size < FD4_STEP_STATIC_INITIALIZER_SIZE) return false;

    result->step = NULL;
    result->slot = NULL;

    for (size_t i = 0; i <= sections->text_size - FD4_STEP_STATIC_INITIALIZER_SIZE; i++) {
        const uint8_t *code = sections->text + i;
        uintptr_t instruction_va;
        uintptr_t step_va;
        uintptr_t slot_va;
        uintptr_t name_va;
        uintptr_t name_slot_va;

        if (!initializer_matches(code)) continue;
        instruction_va = sections->text_va + i;
        step_va = rip_target(instruction_va, code + 3);
        slot_va = rip_target(instruction_va + 7, code + 10);
        name_va = rip_target(instruction_va + 14, code + 17);
        name_slot_va = rip_target(instruction_va + 21, code + 24);
        if ((step_va & 15) != 0 || (slot_va & (sizeof(void *) - 1)) != 0 ||
            (name_va & (sizeof(wchar_t) - 1)) != 0 ||
            (name_slot_va & (sizeof(void *) - 1)) != 0 ||
            name_slot_va != slot_va + sizeof(void *) ||
            !range_contains(sections->text_va, sections->text_size,
                            step_va, 1) ||
            !range_contains(sections->data_va, sections->data_size,
                            slot_va, sizeof(void *)) ||
            !range_contains(sections->data_va, sections->data_size,
                            name_slot_va, sizeof(void *)) ||
            !step_name_equals(sections, name_va, step_name)) continue;
        result->step = (void *)step_va;
        result->slot = (void **)slot_va;
        return true;
    }
    return false;
}

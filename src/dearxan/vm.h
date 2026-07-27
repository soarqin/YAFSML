/*
 * C11 implementation of dearxan v0.5.3 functionality.
 *
 * Original project copyright (c) 2025 William Tremblay.
 * Licensed under the MIT license; see THIRD_PARTY_NOTICES.md.
 */

#pragma once

#include "image.h"

#include <Zydis/Zydis.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dearxan_vm_registers {
    uint64_t values[16];
    uint16_t known;
} dearxan_vm_registers_t;

typedef struct dearxan_vm_memory_block dearxan_vm_memory_block_t;

typedef struct dearxan_vm {
    const dearxan_image_t *image;
    uint64_t rip;
    bool rip_known;
    dearxan_vm_registers_t registers;
    dearxan_vm_memory_block_t *memory;
} dearxan_vm_t;

void dearxan_vm_init(dearxan_vm_t *vm, const dearxan_image_t *image,
                     uint64_t rip, uint64_t rsp);
void dearxan_vm_uninit(dearxan_vm_t *vm);
bool dearxan_vm_clone(dearxan_vm_t *destination, const dearxan_vm_t *source);
bool dearxan_vm_read_register(const dearxan_vm_t *vm, ZydisRegister reg,
                              uint64_t *value);
void dearxan_vm_write_register(dearxan_vm_t *vm, ZydisRegister reg,
                               bool known, uint64_t value);
bool dearxan_vm_read_memory(const dearxan_vm_t *vm, uint64_t address,
                            void *bytes, size_t size);
bool dearxan_vm_write_memory(dearxan_vm_t *vm, uint64_t address,
                             const void *bytes, size_t size);
void dearxan_vm_invalidate_memory(dearxan_vm_t *vm, uint64_t address,
                                  size_t size);
bool dearxan_vm_operand_address(const dearxan_vm_t *vm,
                                const ZydisDecodedInstruction *instruction,
                                const ZydisDecodedOperand *operand,
                                uint64_t *address);
bool dearxan_vm_operand_value(const dearxan_vm_t *vm,
                              const ZydisDecodedInstruction *instruction,
                              const ZydisDecodedOperand *operand,
                              uint64_t *value);
bool dearxan_vm_step(dearxan_vm_t *vm,
                     const ZydisDecodedInstruction *instruction,
                     const ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT],
                     dearxan_vm_t *forked, bool *has_fork);

#ifdef __cplusplus
}
#endif

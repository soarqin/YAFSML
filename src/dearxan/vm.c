/*
 * C11 implementation of dearxan v0.5.3 functionality.
 *
 * Original project copyright (c) 2025 William Tremblay.
 * Licensed under the MIT license; see THIRD_PARTY_NOTICES.md.
 */

#include "vm.h"

#include <stdlib.h>
#include <string.h>

struct dearxan_vm_memory_block {
    uint64_t index;
    unsigned char bytes[64];
    uint64_t known;
    struct dearxan_vm_memory_block *next;
};

static int register_index(ZydisRegister reg, unsigned int *width) {
    ZydisRegister enclosing = ZydisRegisterGetLargestEnclosing(
        ZYDIS_MACHINE_MODE_LONG_64, reg);
    int id;
    if (enclosing < ZYDIS_REGISTER_RAX || enclosing > ZYDIS_REGISTER_R15) return -1;
    id = ZydisRegisterGetId(enclosing);
    if (id < 0 || id >= 16) return -1;
    if (width != NULL) *width = ZydisRegisterGetWidth(
        ZYDIS_MACHINE_MODE_LONG_64, reg) / 8;
    return id;
}

static unsigned int register_bit_offset(ZydisRegister reg) {
    return reg >= ZYDIS_REGISTER_AH && reg <= ZYDIS_REGISTER_BH ? 8 : 0;
}

static uint64_t size_mask(size_t size) {
    if (size == 0) return 0;
    return size >= 8 ? UINT64_MAX : (UINT64_C(1) << (size * 8)) - 1;
}

static dearxan_vm_memory_block_t *find_block(dearxan_vm_t *vm, uint64_t index,
                                              bool create) {
    dearxan_vm_memory_block_t **cursor = &vm->memory;
    while (*cursor != NULL && (*cursor)->index != index) cursor = &(*cursor)->next;
    if (*cursor == NULL && create) {
        const unsigned char *image_bytes;
        size_t available;
        *cursor = calloc(1, sizeof(**cursor));
        if (*cursor == NULL) return NULL;
        (*cursor)->index = index;
        image_bytes = dearxan_image_read(vm->image, index * 64, 64, &available);
        if (image_bytes != NULL) {
            memcpy((*cursor)->bytes, image_bytes, 64);
            (*cursor)->known = UINT64_MAX;
        }
    }
    return *cursor;
}

static const dearxan_vm_memory_block_t *find_existing_block(
    const dearxan_vm_t *vm, uint64_t index) {
    const dearxan_vm_memory_block_t *block = vm->memory;
    while (block != NULL && block->index != index) block = block->next;
    return block;
}

void dearxan_vm_init(dearxan_vm_t *vm, const dearxan_image_t *image,
                     uint64_t rip, uint64_t rsp) {
    memset(vm, 0, sizeof(*vm));
    vm->image = image;
    vm->rip = rip;
    vm->rip_known = true;
    dearxan_vm_write_register(vm, ZYDIS_REGISTER_RSP, true, rsp);
}

void dearxan_vm_uninit(dearxan_vm_t *vm) {
    dearxan_vm_memory_block_t *block = vm->memory;
    while (block != NULL) {
        dearxan_vm_memory_block_t *next = block->next;
        free(block);
        block = next;
    }
    memset(vm, 0, sizeof(*vm));
}

bool dearxan_vm_clone(dearxan_vm_t *destination, const dearxan_vm_t *source) {
    dearxan_vm_memory_block_t **tail;
    *destination = *source;
    destination->memory = NULL;
    tail = &destination->memory;
    for (const dearxan_vm_memory_block_t *block = source->memory;
         block != NULL; block = block->next) {
        *tail = malloc(sizeof(**tail));
        if (*tail == NULL) {
            dearxan_vm_uninit(destination);
            return false;
        }
        **tail = *block;
        (*tail)->next = NULL;
        tail = &(*tail)->next;
    }
    return true;
}

bool dearxan_vm_read_register(const dearxan_vm_t *vm, ZydisRegister reg,
                              uint64_t *value) {
    unsigned int width;
    int index = register_index(reg, &width);
    if (index < 0 || (vm->registers.known & (1u << index)) == 0) return false;
    *value = (vm->registers.values[index] >> register_bit_offset(reg)) & size_mask(width);
    return true;
}

void dearxan_vm_write_register(dearxan_vm_t *vm, ZydisRegister reg,
                               bool known, uint64_t value) {
    unsigned int width;
    int index = register_index(reg, &width);
    if (index < 0) return;
    if (!known) {
        vm->registers.known &= (uint16_t)~(1u << index);
        return;
    }
    if (width == 8) vm->registers.values[index] = value;
    else if (width == 4) vm->registers.values[index] = (uint32_t)value;
    else if ((vm->registers.known & (1u << index)) != 0) {
        unsigned int offset = register_bit_offset(reg);
        uint64_t mask = size_mask(width) << offset;
        vm->registers.values[index] = (vm->registers.values[index] & ~mask) |
                                      ((value << offset) & mask);
    } else return;
    vm->registers.known |= (uint16_t)(1u << index);
}

bool dearxan_vm_read_memory(const dearxan_vm_t *vm, uint64_t address,
                            void *bytes, size_t size) {
    unsigned char *output = bytes;
    if (size != 0 && bytes == NULL) return false;
    while (size != 0) {
        uint64_t index = address / 64;
        size_t offset = (size_t)(address % 64);
        size_t chunk = 64 - offset;
        const dearxan_vm_memory_block_t *block = find_existing_block(vm, index);
        if (chunk > size) chunk = size;
        if (block != NULL) {
            uint64_t mask = chunk == 64 ? UINT64_MAX
                : ((UINT64_C(1) << chunk) - 1) << offset;
            if ((block->known & mask) != mask) return false;
            memcpy(output, block->bytes + offset, chunk);
        } else {
            const unsigned char *image_bytes = dearxan_image_read(
                vm->image, address, chunk, NULL);
            if (image_bytes == NULL) return false;
            memcpy(output, image_bytes, chunk);
        }
        address += chunk;
        output += chunk;
        size -= chunk;
    }
    return true;
}

bool dearxan_vm_write_memory(dearxan_vm_t *vm, uint64_t address,
                             const void *bytes, size_t size) {
    const unsigned char *input = bytes;
    if (size != 0 && bytes == NULL) return false;
    while (size != 0) {
        size_t offset = (size_t)(address % 64);
        size_t chunk = 64 - offset;
        dearxan_vm_memory_block_t *block = find_block(vm, address / 64, true);
        if (block == NULL) return false;
        if (chunk > size) chunk = size;
        memcpy(block->bytes + offset, input, chunk);
        block->known |= chunk == 64 ? UINT64_MAX
            : ((UINT64_C(1) << chunk) - 1) << offset;
        address += chunk;
        input += chunk;
        size -= chunk;
    }
    return true;
}

void dearxan_vm_invalidate_memory(dearxan_vm_t *vm, uint64_t address,
                                  size_t size) {
    while (size != 0) {
        size_t offset = (size_t)(address % 64);
        size_t chunk = 64 - offset;
        dearxan_vm_memory_block_t *block = find_block(vm, address / 64, true);
        if (chunk > size) chunk = size;
        if (block != NULL) {
            uint64_t mask = chunk == 64 ? UINT64_MAX
                : ((UINT64_C(1) << chunk) - 1) << offset;
            block->known &= ~mask;
        }
        address += chunk;
        size -= chunk;
    }
}

bool dearxan_vm_operand_address(const dearxan_vm_t *vm,
                                const ZydisDecodedInstruction *instruction,
                                const ZydisDecodedOperand *operand,
                                uint64_t *address) {
    uint64_t result = 0;
    uint64_t value;
    if (operand->type != ZYDIS_OPERAND_TYPE_MEMORY) return false;
    if (operand->mem.base == ZYDIS_REGISTER_RIP) result = vm->rip + instruction->length;
    else if (operand->mem.base != ZYDIS_REGISTER_NONE &&
             !dearxan_vm_read_register(vm, operand->mem.base, &result)) return false;
    if (operand->mem.index != ZYDIS_REGISTER_NONE) {
        if (!dearxan_vm_read_register(vm, operand->mem.index, &value)) return false;
        result += value * operand->mem.scale;
    }
    if (operand->mem.disp.has_displacement) result += operand->mem.disp.value;
    *address = result;
    return true;
}

bool dearxan_vm_operand_value(const dearxan_vm_t *vm,
                              const ZydisDecodedInstruction *instruction,
                              const ZydisDecodedOperand *operand,
                              uint64_t *value) {
    uint64_t address;
    size_t size = operand->size / 8;
    switch (operand->type) {
        case ZYDIS_OPERAND_TYPE_REGISTER:
            return dearxan_vm_read_register(vm, operand->reg.value, value);
        case ZYDIS_OPERAND_TYPE_MEMORY:
            if (size == 0 || size > 8 ||
                !dearxan_vm_operand_address(vm, instruction, operand, &address)) return false;
            *value = 0;
            return dearxan_vm_read_memory(vm, address, value, size);
        case ZYDIS_OPERAND_TYPE_IMMEDIATE:
            if (operand->imm.is_relative) {
                ZyanU64 absolute;
                if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(instruction, operand,
                                                           vm->rip, &absolute))) return false;
                *value = absolute;
            } else {
                *value = operand->imm.value.u;
            }
            return true;
        default:
            return false;
    }
}

static void set_operand(dearxan_vm_t *vm,
                        const ZydisDecodedInstruction *instruction,
                        const ZydisDecodedOperand *operand,
                        bool known, uint64_t value) {
    uint64_t address;
    size_t size = operand->size / 8;
    if (operand->type == ZYDIS_OPERAND_TYPE_REGISTER) {
        dearxan_vm_write_register(vm, operand->reg.value, known, value);
    } else if (operand->type == ZYDIS_OPERAND_TYPE_MEMORY && size != 0 &&
               dearxan_vm_operand_address(vm, instruction, operand, &address)) {
        if (known && size <= sizeof(value)) dearxan_vm_write_memory(vm, address, &value, size);
        else dearxan_vm_invalidate_memory(vm, address, size);
    }
}

static bool is_cmov(ZydisMnemonic mnemonic) {
    return mnemonic >= ZYDIS_MNEMONIC_CMOVB && mnemonic <= ZYDIS_MNEMONIC_CMOVZ;
}

static bool operand_mentions_rsp(const ZydisDecodedOperand *operand) {
    return (operand->type == ZYDIS_OPERAND_TYPE_REGISTER &&
            ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64,
                                             operand->reg.value) == ZYDIS_REGISTER_RSP) ||
           (operand->type == ZYDIS_OPERAND_TYPE_MEMORY &&
            (operand->mem.base == ZYDIS_REGISTER_RSP ||
             operand->mem.index == ZYDIS_REGISTER_RSP));
}

static int64_t generic_stack_increment(const ZydisDecodedInstruction *instruction) {
    switch (instruction->meta.category) {
        case ZYDIS_CATEGORY_PUSH:
            return -8;
        case ZYDIS_CATEGORY_POP:
            return 8;
        default:
            return 0;
    }
}

static void handle_generic(dearxan_vm_t *vm,
                           const ZydisDecodedInstruction *instruction,
                           const ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]) {
    bool rsp_is_explicit = false;
    int64_t stack_increment = generic_stack_increment(instruction);

    for (ZyanU8 i = 0; i < instruction->operand_count_visible; i++) {
        if (operand_mentions_rsp(&operands[i])) rsp_is_explicit = true;
    }
    for (ZyanU8 i = 0; i < instruction->operand_count; i++) {
        const ZydisDecodedOperand *operand = &operands[i];
        if ((operand->actions & ZYDIS_OPERAND_ACTION_WRITE) == 0) continue;

        if (stack_increment != 0 && !rsp_is_explicit &&
            operand->type == ZYDIS_OPERAND_TYPE_REGISTER &&
            ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64,
                                             operand->reg.value) == ZYDIS_REGISTER_RSP) {
            continue;
        }
        set_operand(vm, instruction, operand, false, 0);
    }
    if (stack_increment != 0 && !rsp_is_explicit) {
        uint64_t rsp;
        if (dearxan_vm_read_register(vm, ZYDIS_REGISTER_RSP, &rsp)) {
            dearxan_vm_write_register(vm, ZYDIS_REGISTER_RSP, true,
                                      rsp + (uint64_t)stack_increment);
        }
    }
}

bool dearxan_vm_step(dearxan_vm_t *vm,
                     const ZydisDecodedInstruction *instruction,
                     const ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT],
                     dearxan_vm_t *forked, bool *has_fork) {
    uint64_t lhs, rhs, address, rsp;
    bool lhs_known, rhs_known;
    *has_fork = false;
    lhs_known = instruction->operand_count_visible > 0 &&
                dearxan_vm_operand_value(vm, instruction, &operands[0], &lhs);
    rhs_known = instruction->operand_count_visible > 1 &&
                dearxan_vm_operand_value(vm, instruction, &operands[1], &rhs);
    switch (instruction->mnemonic) {
        case ZYDIS_MNEMONIC_MOV:
        case ZYDIS_MNEMONIC_MOVZX:
            set_operand(vm, instruction, &operands[0], rhs_known, rhs);
            break;
        case ZYDIS_MNEMONIC_MOVSX:
        case ZYDIS_MNEMONIC_MOVSXD:
            if (rhs_known && operands[1].size != 0 && operands[1].size < 64) {
                uint64_t mask = (UINT64_C(1) << operands[1].size) - 1;
                uint64_t sign = UINT64_C(1) << (operands[1].size - 1);
                rhs &= mask;
                rhs = (rhs ^ sign) - sign;
            }
            set_operand(vm, instruction, &operands[0], rhs_known, rhs);
            break;
        case ZYDIS_MNEMONIC_LEA:
            rhs_known = dearxan_vm_operand_address(vm, instruction, &operands[1], &address);
            set_operand(vm, instruction, &operands[0], rhs_known, address);
            break;
        case ZYDIS_MNEMONIC_ADD:
            set_operand(vm, instruction, &operands[0], lhs_known && rhs_known, lhs + rhs);
            break;
        case ZYDIS_MNEMONIC_SUB:
            set_operand(vm, instruction, &operands[0], lhs_known && rhs_known, lhs - rhs);
            break;
        case ZYDIS_MNEMONIC_XCHG:
            {
                uint64_t addresses[2] = { 0, 0 };
                bool has_address[2] = { false, false };
                for (size_t i = 0; i < 2; i++) {
                    if (operands[i].type == ZYDIS_OPERAND_TYPE_MEMORY) {
                        has_address[i] = dearxan_vm_operand_address(
                            vm, instruction, &operands[i], &addresses[i]);
                    }
                }
                for (size_t i = 0; i < 2; i++) {
                    bool known = i == 0 ? rhs_known : lhs_known;
                    uint64_t value = i == 0 ? rhs : lhs;
                    size_t size = operands[i].size / 8;
                    if (operands[i].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                        dearxan_vm_write_register(vm, operands[i].reg.value,
                                                  known, value);
                    } else if (has_address[i] && size != 0) {
                        if (known && size <= sizeof(value)) {
                            dearxan_vm_write_memory(vm, addresses[i], &value,
                                                    size);
                        } else {
                            dearxan_vm_invalidate_memory(vm, addresses[i], size);
                        }
                    }
                }
            }
            break;
        case ZYDIS_MNEMONIC_PUSH:
            if (dearxan_vm_read_register(vm, ZYDIS_REGISTER_RSP, &rsp)) {
                rsp -= 8;
                dearxan_vm_write_register(vm, ZYDIS_REGISTER_RSP, true, rsp);
                if (lhs_known) dearxan_vm_write_memory(vm, rsp, &lhs, 8);
                else dearxan_vm_invalidate_memory(vm, rsp, 8);
            }
            break;
        case ZYDIS_MNEMONIC_POP:
            if (dearxan_vm_read_register(vm, ZYDIS_REGISTER_RSP, &rsp)) {
                lhs_known = dearxan_vm_read_memory(vm, rsp, &lhs, operands[0].size / 8);
                dearxan_vm_write_register(vm, ZYDIS_REGISTER_RSP, true, rsp + 8);
                set_operand(vm, instruction, &operands[0], lhs_known, lhs);
            }
            break;
        case ZYDIS_MNEMONIC_CALL:
            if (dearxan_vm_read_register(vm, ZYDIS_REGISTER_RSP, &rsp)) {
                uint64_t return_ip = vm->rip + instruction->length;
                rsp -= 8;
                dearxan_vm_write_register(vm, ZYDIS_REGISTER_RSP, true, rsp);
                dearxan_vm_write_memory(vm, rsp, &return_ip, 8);
            }
            if (lhs_known) vm->rip = lhs;
            else vm->rip_known = false;
            return true;
        case ZYDIS_MNEMONIC_RET:
            if (dearxan_vm_read_register(vm, ZYDIS_REGISTER_RSP, &rsp)) {
                bool target_known = dearxan_vm_read_memory(vm, rsp, &address, 8);
                uint64_t increment = 8;
                if (instruction->operand_count_visible != 0 &&
                    operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                    increment += operands[0].imm.value.u;
                }
                dearxan_vm_write_register(vm, ZYDIS_REGISTER_RSP, true, rsp + increment);
                if (target_known) vm->rip = address;
                else vm->rip_known = false;
            } else {
                vm->rip_known = false;
            }
            return true;
        default:
            if (is_cmov(instruction->mnemonic)) {
                vm->rip += instruction->length;
                if (lhs_known && rhs_known) {
                    if (!dearxan_vm_clone(forked, vm)) return false;
                    set_operand(forked, instruction, &operands[0], true, rhs);
                    *has_fork = true;
                } else if (rhs_known) {
                    set_operand(vm, instruction, &operands[0], true, rhs);
                }
                return true;
            }
            handle_generic(vm, instruction, operands);
            break;
    }
    if (instruction->meta.category == ZYDIS_CATEGORY_COND_BR &&
        instruction->operand_count_visible == 1 &&
        dearxan_vm_operand_value(vm, instruction, &operands[0], &address)) {
        if (!dearxan_vm_clone(forked, vm)) return false;
        forked->rip = address;
        forked->rip_known = true;
        *has_fork = true;
    }
    if (instruction->meta.category == ZYDIS_CATEGORY_UNCOND_BR) {
        if (lhs_known) vm->rip = lhs;
        else vm->rip_known = false;
    } else {
        vm->rip += instruction->length;
    }
    return true;
}

/*
 * C11 implementation of dearxan v0.5.3 functionality.
 *
 * Original project copyright (c) 2025 William Tremblay.
 * Licensed under the MIT license; see THIRD_PARTY_NOTICES.md.
 */

#include "analysis.h"
#include "vm.h"

#include <Zydis/Zydis.h>

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define ANALYSIS_MAX_STEPS 0x100000u
#define ANALYSIS_INITIAL_RSP UINT64_C(0x10000)
#define CFG_CMOV_DETACHED (UINT64_MAX >> 1)

typedef struct candidate_list {
    uint64_t *items;
    size_t count;
    size_t capacity;
    bool valid;
} candidate_list_t;

typedef struct address_list {
    uint64_t *items;
    size_t count;
    size_t capacity;
} address_list_t;

typedef struct cfg_info {
    uint64_t cmov_id;
    bool unresolved_branch;
} cfg_info_t;

typedef struct cfg_visited {
    uint64_t address;
    cfg_info_t info;
} cfg_visited_t;

typedef struct analysis_state {
    dearxan_vm_t vm;
    address_list_t path;
    size_t fork_index;
    size_t basic_block_index;
    size_t branch_count;
    cfg_info_t cfg_info;
} analysis_state_t;

typedef struct analysis_work {
    analysis_state_t *items;
    size_t count;
    size_t capacity;
} analysis_work_t;

typedef struct visited_slot {
    uint64_t address;
    size_t index_plus_one;
} visited_slot_t;

typedef struct visited_map {
    visited_slot_t *slots;
    size_t count;
    size_t capacity;
} visited_map_t;

typedef struct tea_candidate {
    uint64_t decrypt_va;
    uint64_t key_va;
    const unsigned char *key;
    const unsigned char *ciphertext;
} tea_candidate_t;

typedef struct tea_key_candidate {
    uint64_t decrypt_va;
    size_t next_lea;
    uint64_t key_va;
    const unsigned char *key;
} tea_key_candidate_t;

typedef struct lea_value {
    uint64_t value;
    uint64_t va;
} lea_value_t;

typedef enum tea_phase {
    TEA_SEARCH_REGIONS,
    TEA_SEARCH_CIPHERTEXT,
    TEA_FOUND
} tea_phase_t;

typedef struct tea_state {
    tea_phase_t phase;
    tea_key_candidate_t *keys;
    size_t key_count;
    size_t key_capacity;
    uint64_t *decrypt_vas;
    size_t decrypt_count;
    size_t decrypt_capacity;
    tea_candidate_t *candidates;
    size_t candidate_count;
    size_t candidate_capacity;
    uint64_t region_decrypt_va;
    uint64_t region_key_va;
    dearxan_encrypted_region_t *regions;
    size_t region_count;
    size_t stream_size;
    dearxan_encrypted_region_list_t found;
} tea_state_t;

typedef struct rmx_state {
    bool has_key;
    uint32_t key;
    dearxan_encrypted_region_t *regions;
    size_t region_count;
    size_t stream_size;
    const unsigned char *ciphertext;
    size_t ciphertext_available;
    bool found;
    dearxan_encrypted_region_list_t list;
} rmx_state_t;

typedef enum sub_phase {
    SUB_SEARCH_REGIONS,
    SUB_SEARCH_CIPHERTEXT,
    SUB_CHECK_NEG,
    SUB_CHECK_KEY,
    SUB_FOUND
} sub_phase_t;

typedef struct sub_state {
    sub_phase_t phase;
    dearxan_encrypted_region_t *regions;
    size_t region_count;
    size_t stream_size;
    ZydisRegister reg;
    const unsigned char *ciphertext;
    size_t ciphertext_available;
    dearxan_encrypted_region_list_t list;
} sub_state_t;

typedef struct scan_state {
    const dearxan_image_t *image;
    dearxan_stub_info_t *stub;
    uint64_t initial_rsp;
    bool has_context_pop;
    tea_state_t tea;
    rmx_state_t rmx;
    sub_state_t sub;
    lea_value_t *leas;
    size_t lea_count;
    size_t lea_capacity;
} scan_state_t;

typedef struct call_info {
    uint64_t target_rsp;
    uint64_t return_rsp;
    bool has_target_ip;
    uint64_t target_ip;
    bool has_return_ip;
    uint64_t return_ip;
} call_info_t;

static bool reserve_array(void **items, size_t *capacity, size_t count,
                          size_t item_size) {
    size_t next_capacity;
    void *next;
    if (count <= *capacity) return true;
    next_capacity = *capacity == 0 ? 8 : *capacity;
    while (next_capacity < count) {
        if (next_capacity > SIZE_MAX / 2) return false;
        next_capacity *= 2;
    }
    if (next_capacity > SIZE_MAX / item_size) return false;
    next = realloc(*items, next_capacity * item_size);
    if (next == NULL) return false;
    *items = next;
    *capacity = next_capacity;
    return true;
}

static bool append_address(address_list_t *list, uint64_t value) {
    if (!reserve_array((void **)&list->items, &list->capacity,
                       list->count + 1, sizeof(*list->items))) return false;
    list->items[list->count++] = value;
    return true;
}

static bool clone_address_list(address_list_t *destination,
                               const address_list_t *source) {
    memset(destination, 0, sizeof(*destination));
    if (source->count == 0) return true;
    destination->items = malloc(source->count * sizeof(*source->items));
    if (destination->items == NULL) return false;
    memcpy(destination->items, source->items,
           source->count * sizeof(*source->items));
    destination->count = source->count;
    destination->capacity = source->count;
    return true;
}

static void uninit_analysis_state(analysis_state_t *state) {
    dearxan_vm_uninit(&state->vm);
    free(state->path.items);
    memset(state, 0, sizeof(*state));
}

static bool clone_analysis_state(analysis_state_t *destination,
                                 const analysis_state_t *source) {
    *destination = *source;
    destination->path.items = NULL;
    destination->path.count = 0;
    destination->path.capacity = 0;
    destination->vm.memory = NULL;
    if (!dearxan_vm_clone(&destination->vm, &source->vm)) return false;
    if (!clone_address_list(&destination->path, &source->path)) {
        dearxan_vm_uninit(&destination->vm);
        return false;
    }
    return true;
}

static bool clone_analysis_state_with_vm(analysis_state_t *destination,
                                         const analysis_state_t *source,
                                         dearxan_vm_t *vm) {
    *destination = *source;
    destination->path.items = NULL;
    destination->path.count = 0;
    destination->path.capacity = 0;
    destination->vm = *vm;
    memset(vm, 0, sizeof(*vm));
    if (!clone_address_list(&destination->path, &source->path)) {
        dearxan_vm_uninit(&destination->vm);
        return false;
    }
    return true;
}

static bool push_state(analysis_work_t *work, analysis_state_t *state) {
    if (!reserve_array((void **)&work->items, &work->capacity,
                       work->count + 1, sizeof(*work->items))) return false;
    work->items[work->count++] = *state;
    memset(state, 0, sizeof(*state));
    return true;
}

static void pop_state(analysis_work_t *work) {
    if (work->count == 0) return;
    uninit_analysis_state(&work->items[work->count - 1]);
    work->count--;
}

static bool collect_candidates(uint64_t va, const unsigned char *bytes,
                               size_t size, void *opaque) {
    static const unsigned char pattern[] = { 0x48, 0xf7, 0xc4, 0x0f, 0, 0, 0 };
    candidate_list_t *list = opaque;
    const unsigned char *cursor = bytes;
    size_t remaining = size;
    while (remaining >= sizeof(pattern)) {
        const unsigned char *match = memchr(cursor, pattern[0],
                                            remaining - sizeof(pattern) + 1);
        size_t consumed;
        if (match == NULL) break;
        consumed = (size_t)(match - cursor);
        cursor = match + 1;
        remaining -= consumed + 1;
        if (memcmp(match, pattern, sizeof(pattern)) != 0) continue;
        if (!reserve_array((void **)&list->items, &list->capacity,
                           list->count + 1, sizeof(*list->items))) {
            list->valid = false;
            return false;
        }
        list->items[list->count++] = va + (uint64_t)(match - bytes);
    }
    return true;
}

static bool decode(const dearxan_image_t *image, const ZydisDecoder *decoder,
                   uint64_t rip, ZydisDecodedInstruction *instruction,
                   ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]) {
    const unsigned char *bytes;
    size_t available;
    bytes = dearxan_image_read(image, rip, 1, &available);
    if (bytes == NULL) return false;
    if (available > ZYDIS_MAX_INSTRUCTION_LENGTH) {
        available = ZYDIS_MAX_INSTRUCTION_LENGTH;
    }
    return ZYAN_SUCCESS(ZydisDecoderDecodeFull(
        decoder, bytes, available, instruction, operands));
}

static bool is_direct_relative_branch(const ZydisDecodedInstruction *instruction,
                                      const ZydisDecodedOperand *operand) {
    return instruction->meta.category == ZYDIS_CATEGORY_UNCOND_BR &&
           operand->type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
           operand->imm.is_relative;
}

static uint64_t ret_increment(const ZydisDecodedInstruction *instruction,
                              const ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]) {
    uint64_t increment = 8;
    if (instruction->operand_count_visible != 0 &&
        operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
        increment += operands[0].imm.value.u;
    }
    return increment;
}

static bool extract_call_info(const analysis_state_t *state,
                              const ZydisDecodedInstruction *instruction,
                              const ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT],
                              call_info_t *call) {
    uint64_t rsp;
    memset(call, 0, sizeof(*call));
    if (!dearxan_vm_read_register(&state->vm, ZYDIS_REGISTER_RSP, &rsp)) return false;
    if (instruction->meta.category == ZYDIS_CATEGORY_RET) {
        call->target_rsp = rsp + ret_increment(instruction, operands);
        if ((call->target_rsp & 0xf) != 8) return false;
        call->return_rsp = call->target_rsp + 8;
        call->has_target_ip = dearxan_vm_read_memory(
            &state->vm, rsp, &call->target_ip, sizeof(call->target_ip));
        call->has_return_ip = dearxan_vm_read_memory(
            &state->vm, call->target_rsp, &call->return_ip, sizeof(call->return_ip));
        return true;
    }
    if (instruction->meta.category == ZYDIS_CATEGORY_UNCOND_BR &&
        instruction->operand_count_visible != 0 &&
        !is_direct_relative_branch(instruction, &operands[0])) {
        if ((rsp & 0xf) != 8) return false;
        call->target_rsp = rsp;
        call->return_rsp = rsp + 8;
        call->has_target_ip = dearxan_vm_operand_value(
            &state->vm, instruction, &operands[0], &call->target_ip);
        call->has_return_ip = dearxan_vm_read_memory(
            &state->vm, rsp, &call->return_ip, sizeof(call->return_ip));
        return true;
    }
    if (instruction->meta.category == ZYDIS_CATEGORY_CALL) {
        call->target_rsp = rsp - 8;
        call->return_rsp = rsp;
        call->has_return_ip = true;
        call->return_ip = state->vm.rip + instruction->length;
        call->has_target_ip = instruction->operand_count_visible != 0 &&
            dearxan_vm_operand_value(&state->vm, instruction, &operands[0],
                                     &call->target_ip);
        return true;
    }
    return false;
}

static bool is_rmx_key_instruction(const ZydisDecodedInstruction *instruction,
                                   const ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]) {
    return instruction->mnemonic == ZYDIS_MNEMONIC_AND &&
           instruction->operand_count_visible == 2 &&
           operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
           operands[0].size == 32 &&
           operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
           (instruction->raw.imm[0].size == 8 ||
            (operands[0].reg.value == ZYDIS_REGISTER_EAX &&
             instruction->raw.imm[0].size == 32)) &&
           operands[1].imm.value.u == 0x1f;
}

static bool is_movzx_r32_rm8(const ZydisDecodedInstruction *instruction,
                            const ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]) {
    return instruction->mnemonic == ZYDIS_MNEMONIC_MOVZX &&
           instruction->operand_count_visible == 2 &&
           operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
           operands[0].size == 32 && operands[1].size == 8 &&
           operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY;
}

static bool is_mov_r32_rm32(const ZydisDecodedInstruction *instruction,
                           const ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]) {
    return instruction->mnemonic == ZYDIS_MNEMONIC_MOV &&
           instruction->operand_count_visible == 2 &&
           operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
           operands[0].size == 32 && operands[1].size == 32 &&
           operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY;
}

static bool is_add_r32_rm32(const ZydisDecodedInstruction *instruction,
                           const ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]) {
    return instruction->mnemonic == ZYDIS_MNEMONIC_ADD &&
           instruction->operand_count_visible == 2 &&
           operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
           operands[0].size == 32 && operands[1].size == 32 &&
           operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY;
}

static bool parse_regions_at(const dearxan_image_t *image, uint64_t va,
                             dearxan_encrypted_region_t **regions,
                             size_t *region_count, size_t *stream_size) {
    const unsigned char *bytes;
    size_t available;
    bytes = dearxan_image_read(image, va, 2, &available);
    return bytes != NULL && dearxan_parse_encrypted_regions(
        bytes, available, regions, region_count, stream_size);
}

static bool decrypt_stream(dearxan_decryption_kind_t kind,
                           const unsigned char *ciphertext, size_t available,
                           size_t stream_size, const unsigned char *key_bytes,
                           uint32_t key32, unsigned char **plaintext) {
    size_t block_size = kind == DEARXAN_DECRYPTION_TEA ? 8 : 4;
    size_t encrypted_size;
    unsigned char *output;
    uint32_t rmx_key = key32;
    uint32_t rotation = key32 & 0x1f;
    uint32_t tea_key[4] = { 0 };
    if (stream_size > SIZE_MAX - (block_size - 1)) return false;
    encrypted_size = (stream_size + block_size - 1) / block_size * block_size;
    if (ciphertext == NULL || available < encrypted_size) return false;
    output = malloc(encrypted_size == 0 ? 1 : encrypted_size);
    if (output == NULL) return false;
    memcpy(output, ciphertext, encrypted_size);
    if (kind == DEARXAN_DECRYPTION_TEA) {
        memcpy(tea_key, key_bytes, sizeof(tea_key));
        for (size_t offset = 0; offset < encrypted_size; offset += 8) {
            uint32_t block[2];
            memcpy(block, output + offset, sizeof(block));
            dearxan_tea_decrypt(block, tea_key);
            memcpy(output + offset, block, sizeof(block));
        }
    } else {
        for (size_t offset = 0; offset < encrypted_size; offset += 4) {
            uint32_t block;
            memcpy(&block, output + offset, sizeof(block));
            block = kind == DEARXAN_DECRYPTION_RMX
                ? dearxan_rmx_decrypt(block, &rmx_key, &rotation)
                : dearxan_sub_decrypt(block, key32);
            memcpy(output + offset, &block, sizeof(block));
        }
    }
    *plaintext = output;
    return true;
}

typedef struct tea_reader {
    const unsigned char *ciphertext;
    size_t available;
    size_t offset;
    unsigned char block[8];
    size_t block_offset;
    uint32_t key[4];
} tea_reader_t;

static bool tea_reader_byte(tea_reader_t *reader, unsigned char *value) {
    if (reader->block_offset == sizeof(reader->block)) {
        uint32_t block[2];
        if (reader->offset > reader->available ||
            sizeof(block) > reader->available - reader->offset) return false;
        memcpy(block, reader->ciphertext + reader->offset, sizeof(block));
        dearxan_tea_decrypt(block, reader->key);
        memcpy(reader->block, block, sizeof(block));
        reader->offset += sizeof(block);
        reader->block_offset = 0;
    }
    *value = reader->block[reader->block_offset++];
    return true;
}

static bool tea_reader_varint(tea_reader_t *reader, uint32_t *value) {
    uint32_t result = 0;
    unsigned int shift = 0;
    for (;;) {
        unsigned char byte;
        uint32_t payload;
        if (!tea_reader_byte(reader, &byte)) return false;
        payload = byte & 0x7f;
        if (shift >= 32 || (payload != 0 && payload > (UINT32_MAX >> shift))) return false;
        payload <<= shift;
        if (result > UINT32_MAX - payload) return false;
        result += payload;
        if (byte < 0x80) {
            *value = result;
            return true;
        }
        shift += 7;
    }
}

static bool parse_tea_regions(const unsigned char *ciphertext, size_t available,
                              const unsigned char *key_bytes,
                              dearxan_encrypted_region_t **regions,
                              size_t *region_count, size_t *stream_size) {
    tea_reader_t reader;
    dearxan_encrypted_region_t *items = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t output_offset = 0;
    uint32_t rva = 0;
    memset(&reader, 0, sizeof(reader));
    reader.ciphertext = ciphertext;
    reader.available = available;
    reader.block_offset = sizeof(reader.block);
    memcpy(reader.key, key_bytes, sizeof(reader.key));
    for (;;) {
        uint32_t delta;
        uint32_t size;
        if (!tea_reader_varint(&reader, &delta) || delta == 0 ||
            rva > UINT32_MAX - delta) goto fail;
        rva += delta;
        if (rva == UINT32_MAX) {
            *regions = items;
            *region_count = count;
            *stream_size = output_offset;
            return true;
        }
        if (!tea_reader_varint(&reader, &size) || size == 0 ||
            rva > UINT32_MAX - size || output_offset > SIZE_MAX - size) goto fail;
        if (!reserve_array((void **)&items, &capacity, count + 1, sizeof(*items))) goto fail;
        items[count++] = (dearxan_encrypted_region_t){ output_offset, size, rva };
        rva += size;
        output_offset += size;
    }
fail:
    free(items);
    return false;
}

static bool build_region_list(dearxan_encrypted_region_list_t *list,
                              dearxan_decryption_kind_t kind,
                              dearxan_encrypted_region_t **regions,
                              size_t *region_count, size_t stream_size,
                              const unsigned char *ciphertext, size_t available,
                              const unsigned char *key_bytes, uint32_t key32) {
    unsigned char *plaintext;
    if (!decrypt_stream(kind, ciphertext, available, stream_size,
                        key_bytes, key32, &plaintext)) return false;
    memset(list, 0, sizeof(*list));
    list->kind = kind;
    list->regions = *regions;
    list->region_count = *region_count;
    list->decrypted_stream = plaintext;
    list->decrypted_size = stream_size;
    *regions = NULL;
    *region_count = 0;
    return true;
}

static const lea_value_t *find_lea_value(const scan_state_t *scan, uint64_t value) {
    for (size_t i = 0; i < scan->lea_count; i++) {
        if (scan->leas[i].value == value) return &scan->leas[i];
    }
    return NULL;
}

static bool tea_has_decrypt_va(const tea_state_t *tea, uint64_t va) {
    for (size_t i = 0; i < tea->decrypt_count; i++) {
        if (tea->decrypt_vas[i] == va) return true;
    }
    return false;
}

static bool append_decrypt_va(tea_state_t *tea, uint64_t va) {
    if (tea_has_decrypt_va(tea, va)) return true;
    if (!reserve_array((void **)&tea->decrypt_vas, &tea->decrypt_capacity,
                       tea->decrypt_count + 1, sizeof(*tea->decrypt_vas))) return false;
    tea->decrypt_vas[tea->decrypt_count++] = va;
    return true;
}

static void search_tea_ciphertext(scan_state_t *scan) {
    tea_state_t *tea = &scan->tea;
    if (tea->phase != TEA_SEARCH_CIPHERTEXT) return;
    for (size_t i = 0; i < tea->candidate_count;) {
        tea_candidate_t *candidate = &tea->candidates[i];
        size_t available;
        size_t offset;
        if (candidate->decrypt_va != tea->region_decrypt_va ||
            candidate->key_va == tea->region_key_va) {
            memmove(candidate, candidate + 1,
                    (tea->candidate_count - i - 1) * sizeof(*candidate));
            tea->candidate_count--;
            continue;
        }
        offset = (size_t)(candidate->ciphertext - scan->image->base);
        if (offset >= scan->image->size) {
            i++;
            continue;
        }
        available = scan->image->size - offset;
        if (build_region_list(&tea->found, DEARXAN_DECRYPTION_TEA,
                              &tea->regions, &tea->region_count,
                              tea->stream_size, candidate->ciphertext, available,
                              candidate->key, 0)) {
            tea->phase = TEA_FOUND;
        }
        return;
    }
}

static void search_tea_regions(scan_state_t *scan, bool has_decrypt_va,
                               uint64_t decrypt_va) {
    tea_state_t *tea = &scan->tea;
    if (tea->phase != TEA_SEARCH_REGIONS) return;
    if (has_decrypt_va && !append_decrypt_va(tea, decrypt_va)) return;
    for (size_t i = 0; i < tea->key_count; i++) {
        tea_key_candidate_t *candidate = &tea->keys[i];
        if (!tea_has_decrypt_va(tea, candidate->decrypt_va)) continue;
        for (size_t j = candidate->next_lea; j < scan->lea_count; j++) {
            const unsigned char *ciphertext;
            size_t available;
            dearxan_encrypted_region_t *regions = NULL;
            size_t region_count = 0;
            size_t stream_size = 0;
            ciphertext = dearxan_image_read(scan->image, scan->leas[j].va, 8, &available);
            if (ciphertext != NULL && parse_tea_regions(ciphertext, available,
                    candidate->key, &regions, &region_count, &stream_size)) {
                tea->region_decrypt_va = candidate->decrypt_va;
                tea->region_key_va = candidate->key_va;
                tea->regions = regions;
                tea->region_count = region_count;
                tea->stream_size = stream_size;
                tea->phase = TEA_SEARCH_CIPHERTEXT;
                search_tea_ciphertext(scan);
                return;
            }
        }
        candidate->next_lea = scan->lea_count;
    }
}

static void on_tea_call(scan_state_t *scan, const analysis_state_t *state,
                        uint64_t decrypt_va) {
    uint64_t key_va;
    uint64_t stack_va;
    uint64_t block;
    const unsigned char *key;
    const lea_value_t *lea;
    tea_state_t *tea = &scan->tea;
    key = dearxan_vm_read_register(&state->vm, ZYDIS_REGISTER_RDX, &key_va)
        ? dearxan_image_read(scan->image, key_va, 16, NULL) : NULL;
    if (key == NULL) return;
    if (tea->phase == TEA_SEARCH_REGIONS) {
        if (reserve_array((void **)&tea->keys, &tea->key_capacity,
                          tea->key_count + 1, sizeof(*tea->keys))) {
            tea->keys[tea->key_count++] = (tea_key_candidate_t){
                decrypt_va, 0, key_va, key
            };
            search_tea_regions(scan, false, 0);
        }
    }
    if (!dearxan_vm_read_register(&state->vm, ZYDIS_REGISTER_RCX, &stack_va) ||
        stack_va >= scan->initial_rsp ||
        !dearxan_vm_read_memory(&state->vm, stack_va, &block, sizeof(block))) return;
    lea = find_lea_value(scan, block);
    if (lea == NULL || !reserve_array((void **)&tea->candidates,
            &tea->candidate_capacity, tea->candidate_count + 1,
            sizeof(*tea->candidates))) return;
    tea->candidates[tea->candidate_count++] = (tea_candidate_t){
        decrypt_va, key_va, key, dearxan_image_read(scan->image, lea->va, 8, NULL)
    };
    if (tea->candidates[tea->candidate_count - 1].ciphertext == NULL) {
        tea->candidate_count--;
        return;
    }
    search_tea_regions(scan, true, decrypt_va);
    search_tea_ciphertext(scan);
}

static void update_rmx(scan_state_t *scan, const analysis_state_t *state,
                       const ZydisDecodedInstruction *instruction,
                       const ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]) {
    rmx_state_t *rmx = &scan->rmx;
    uint64_t value;
    uint64_t va;
    if (rmx->found) return;
    if (!rmx->has_key && is_rmx_key_instruction(instruction, operands) &&
        dearxan_vm_read_register(&state->vm, operands[0].reg.value, &value)) {
        rmx->has_key = true;
        rmx->key = (uint32_t)value;
    }
    if (rmx->regions == NULL && is_movzx_r32_rm8(instruction, operands) &&
        dearxan_vm_operand_address(&state->vm, instruction, &operands[1], &va)) {
        parse_regions_at(scan->image, va, &rmx->regions, &rmx->region_count,
                         &rmx->stream_size);
    }
    if (rmx->ciphertext == NULL && instruction->mnemonic == ZYDIS_MNEMONIC_IMUL &&
        instruction->operand_count_visible == 2 &&
        operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
        operands[0].reg.value == ZYDIS_REGISTER_EDX &&
        operands[0].size == 32 && operands[1].size == 32 &&
        operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
        dearxan_vm_read_register(&state->vm, ZYDIS_REGISTER_RAX, &va)) {
        rmx->ciphertext = dearxan_image_read(scan->image, va, 4,
                                             &rmx->ciphertext_available);
    }
    if (rmx->has_key && rmx->regions != NULL && rmx->ciphertext != NULL &&
        build_region_list(&rmx->list, DEARXAN_DECRYPTION_RMX,
                          &rmx->regions, &rmx->region_count, rmx->stream_size,
                          rmx->ciphertext, rmx->ciphertext_available,
                          NULL, rmx->key)) {
        rmx->found = true;
    }
}

static void update_sub(scan_state_t *scan, const analysis_state_t *state,
                       const ZydisDecodedInstruction *instruction,
                       const ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]) {
    sub_state_t *sub = &scan->sub;
    uint64_t va;
    uint64_t key;
    if (sub->phase == SUB_FOUND ||
        (instruction->operand_count_visible != 0 &&
         is_direct_relative_branch(instruction, &operands[0]))) return;
    switch (sub->phase) {
        case SUB_SEARCH_REGIONS:
            if (is_movzx_r32_rm8(instruction, operands) &&
                dearxan_vm_operand_address(&state->vm, instruction, &operands[1], &va) &&
                parse_regions_at(scan->image, va, &sub->regions,
                                 &sub->region_count, &sub->stream_size)) {
                sub->phase = SUB_SEARCH_CIPHERTEXT;
            }
            break;
        case SUB_SEARCH_CIPHERTEXT:
            if (is_mov_r32_rm32(instruction, operands) &&
                dearxan_vm_operand_address(&state->vm, instruction, &operands[1], &va)) {
                sub->ciphertext = dearxan_image_read(scan->image, va, 1,
                                                      &sub->ciphertext_available);
                if (sub->ciphertext != NULL) {
                    sub->reg = operands[0].reg.value;
                    sub->phase = SUB_CHECK_NEG;
                }
            }
            break;
        case SUB_CHECK_NEG:
            if (instruction->mnemonic == ZYDIS_MNEMONIC_NEG &&
                instruction->operand_count_visible == 1 &&
                operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
                operands[0].reg.value == sub->reg) {
                sub->phase = SUB_CHECK_KEY;
            } else {
                sub->phase = SUB_SEARCH_CIPHERTEXT;
            }
            break;
        case SUB_CHECK_KEY:
            if (is_add_r32_rm32(instruction, operands) &&
                operands[0].reg.value == sub->reg &&
                operands[1].mem.base == ZYDIS_REGISTER_RIP &&
                dearxan_vm_operand_value(&state->vm, instruction, &operands[1], &key) &&
                build_region_list(&sub->list, DEARXAN_DECRYPTION_SUB,
                                  &sub->regions, &sub->region_count, sub->stream_size,
                                  sub->ciphertext, sub->ciphertext_available,
                                  NULL, (uint32_t)key)) {
                sub->phase = SUB_FOUND;
            } else {
                sub->phase = SUB_SEARCH_CIPHERTEXT;
            }
            break;
        default:
            break;
    }
}

static void update_scan_state(scan_state_t *scan, const analysis_state_t *state,
                              const ZydisDecodedInstruction *instruction,
                              const ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT],
                              const call_info_t *call, bool has_call) {
    uint64_t write_address;
    uint64_t return_address;
    uint64_t va;
    uint64_t value;
    if (!scan->stub->has_return_gadget && instruction->mnemonic == ZYDIS_MNEMONIC_MOV &&
        instruction->operand_count_visible == 2 &&
        operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY && operands[0].size == 64 &&
        operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
        dearxan_vm_operand_address(&state->vm, instruction, &operands[0], &write_address) &&
        dearxan_vm_operand_value(&state->vm, instruction, &operands[1], &return_address) &&
        write_address >= scan->initial_rsp &&
        write_address - scan->initial_rsp < 0x400) {
        scan->stub->has_return_gadget = true;
        scan->stub->return_gadget.stack_offset =
            (size_t)(write_address - scan->initial_rsp);
        scan->stub->return_gadget.address = return_address;
    }
    update_rmx(scan, state, instruction, operands);
    update_sub(scan, state, instruction, operands);
    if (instruction->mnemonic == ZYDIS_MNEMONIC_LEA &&
        instruction->operand_count_visible == 2 && operands[0].size == 64 &&
        operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
        operands[1].mem.base == ZYDIS_REGISTER_RIP &&
        dearxan_vm_operand_address(&state->vm, instruction, &operands[1], &va) &&
        dearxan_vm_read_memory(&state->vm, va, &value, sizeof(value))) {
        size_t existing;
        for (existing = 0; existing < scan->lea_count; existing++) {
            if (scan->leas[existing].value == value) break;
        }
        if (existing == scan->lea_count && reserve_array((void **)&scan->leas,
                &scan->lea_capacity, scan->lea_count + 1, sizeof(*scan->leas))) {
            scan->leas[scan->lea_count++] = (lea_value_t){ value, va };
        } else if (existing < scan->lea_count) {
            scan->leas[existing].va = va;
        }
    }
    if (!has_call) return;
    if (call->has_return_ip && !scan->has_context_pop) {
        scan->has_context_pop = true;
        scan->stub->context_pop_va = call->return_ip;
    } else if (call->has_target_ip) {
        on_tea_call(scan, state, call->target_ip);
    }
}

static bool is_cmp_rax_18(const ZydisDecodedInstruction *instruction,
                         const ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]) {
    return instruction->mnemonic == ZYDIS_MNEMONIC_CMP &&
           instruction->operand_count_visible == 2 &&
           operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER && operands[0].size == 64 &&
           ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64,
                                            operands[0].reg.value) == ZYDIS_REGISTER_RAX &&
           operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
           operands[1].imm.value.u == 0x18;
}

static bool is_int_2d(const ZydisDecodedInstruction *instruction,
                      const ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]) {
    return instruction->mnemonic == ZYDIS_MNEMONIC_INT &&
           instruction->operand_count_visible == 1 &&
           operands[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
           operands[0].imm.value.u == 0x2d;
}

static bool is_cmov(ZydisMnemonic mnemonic) {
    return mnemonic >= ZYDIS_MNEMONIC_CMOVB && mnemonic <= ZYDIS_MNEMONIC_CMOVZ;
}

static size_t find_visited(const cfg_visited_t *visited, size_t visited_count,
                           uint64_t address) {
    for (size_t i = 0; i < visited_count; i++) {
        if (visited[i].address == address) return i;
    }
    return SIZE_MAX;
}

static uint64_t hash_address(uint64_t value) {
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static size_t find_visited_map(const visited_map_t *map, uint64_t address) {
    size_t slot;
    if (map->capacity == 0) return SIZE_MAX;
    slot = (size_t)hash_address(address) & (map->capacity - 1);
    while (map->slots[slot].index_plus_one != 0) {
        if (map->slots[slot].address == address) {
            return map->slots[slot].index_plus_one - 1;
        }
        slot = (slot + 1) & (map->capacity - 1);
    }
    return SIZE_MAX;
}

static bool resize_visited_map(visited_map_t *map, size_t capacity) {
    visited_slot_t *slots;
    if (capacity < 16) capacity = 16;
    if ((capacity & (capacity - 1)) != 0 ||
        capacity > SIZE_MAX / sizeof(*slots)) return false;
    slots = calloc(capacity, sizeof(*slots));
    if (slots == NULL) return false;
    for (size_t i = 0; i < map->capacity; i++) {
        if (map->slots[i].index_plus_one != 0) {
            size_t slot = (size_t)hash_address(map->slots[i].address) &
                          (capacity - 1);
            while (slots[slot].index_plus_one != 0) {
                slot = (slot + 1) & (capacity - 1);
            }
            slots[slot] = map->slots[i];
        }
    }
    free(map->slots);
    map->slots = slots;
    map->capacity = capacity;
    return true;
}

static bool insert_visited_map(visited_map_t *map, uint64_t address,
                               size_t index) {
    size_t slot;
    if (map->capacity == 0 || map->count + 1 > map->capacity / 2) {
        size_t capacity = map->capacity == 0 ? 16 : map->capacity * 2;
        if (capacity < map->capacity || !resize_visited_map(map, capacity)) {
            return false;
        }
    }
    slot = (size_t)hash_address(address) & (map->capacity - 1);
    while (map->slots[slot].index_plus_one != 0) {
        if (map->slots[slot].address == address) return true;
        slot = (slot + 1) & (map->capacity - 1);
    }
    map->slots[slot].address = address;
    map->slots[slot].index_plus_one = index + 1;
    map->count++;
    return true;
}

static bool target_oob_or_visited(const dearxan_image_t *image,
                                  const visited_map_t *visited_map,
                                  const call_info_t *call) {
    return !call->has_target_ip ||
           dearxan_image_read(image, call->target_ip, 1, NULL) == NULL ||
           find_visited_map(visited_map, call->target_ip) != SIZE_MAX;
}

static void invalidate_volatile_registers(dearxan_vm_t *vm) {
    static const ZydisRegister volatile_regs[] = {
        ZYDIS_REGISTER_RAX, ZYDIS_REGISTER_RCX, ZYDIS_REGISTER_RDX,
        ZYDIS_REGISTER_R8, ZYDIS_REGISTER_R9, ZYDIS_REGISTER_R10,
        ZYDIS_REGISTER_R11
    };
    for (size_t i = 0; i < sizeof(volatile_regs) / sizeof(volatile_regs[0]); i++) {
        dearxan_vm_write_register(vm, volatile_regs[i], false, 0);
    }
}

static bool scan_can_stop(const scan_state_t *scan) {
    return scan->has_context_pop && scan->stub->has_return_gadget &&
           (scan->tea.phase == TEA_FOUND || scan->rmx.found ||
            scan->sub.phase == SUB_FOUND);
}

static void move_encryption_result(scan_state_t *scan) {
    if (scan->tea.phase == TEA_FOUND) {
        scan->stub->encrypted_regions = scan->tea.found;
        memset(&scan->tea.found, 0, sizeof(scan->tea.found));
        scan->stub->has_encrypted_regions = true;
    } else if (scan->rmx.found) {
        scan->stub->encrypted_regions = scan->rmx.list;
        memset(&scan->rmx.list, 0, sizeof(scan->rmx.list));
        scan->stub->has_encrypted_regions = true;
    } else if (scan->sub.phase == SUB_FOUND) {
        scan->stub->encrypted_regions = scan->sub.list;
        memset(&scan->sub.list, 0, sizeof(scan->sub.list));
        scan->stub->has_encrypted_regions = true;
    }
}

static void uninit_scan_state(scan_state_t *scan) {
    dearxan_free_encrypted_region_list(&scan->tea.found);
    dearxan_free_encrypted_region_list(&scan->rmx.list);
    dearxan_free_encrypted_region_list(&scan->sub.list);
    free(scan->tea.keys);
    free(scan->tea.decrypt_vas);
    free(scan->tea.candidates);
    free(scan->tea.regions);
    free(scan->rmx.regions);
    free(scan->sub.regions);
    free(scan->leas);
}

static bool analyze_candidate(const dearxan_image_t *image, uint64_t start,
                              dearxan_stub_info_t *stub, bool *fatal_error) {
    ZydisDecoder decoder;
    analysis_work_t work = { 0 };
    cfg_visited_t *visited = NULL;
    visited_map_t visited_map = { 0 };
    size_t visited_count = 0;
    size_t visited_capacity = 0;
    size_t step_count = 0;
    uint64_t cmov_pair_gen = 0;
    size_t ignored_test_rsp_branch = 1;
    size_t bad_cmp_rax_branch = SIZE_MAX;
    bool is_double_stepping = false;
    bool max_steps_reached = false;
    scan_state_t scan;
    analysis_state_t initial;

    *fatal_error = false;
    memset(stub, 0, sizeof(*stub));
    stub->test_rsp_va = start;
    memset(&scan, 0, sizeof(scan));
    scan.image = image;
    scan.stub = stub;
    scan.initial_rsp = ANALYSIS_INITIAL_RSP;
    scan.tea.phase = TEA_SEARCH_REGIONS;
    scan.sub.phase = SUB_SEARCH_REGIONS;
    memset(&initial, 0, sizeof(initial));
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                       ZYDIS_STACK_WIDTH_64))) return false;
    dearxan_vm_init(&initial.vm, image, start, ANALYSIS_INITIAL_RSP);
    {
        uint64_t initial_stack = 0x10;
        if (!dearxan_vm_write_memory(&initial.vm, ANALYSIS_INITIAL_RSP,
                                     &initial_stack, sizeof(initial_stack))) {
            uninit_analysis_state(&initial);
            *fatal_error = true;
            return false;
        }
    }
    initial.cfg_info.cmov_id = CFG_CMOV_DETACHED;
    if (!push_state(&work, &initial)) {
        uninit_analysis_state(&initial);
        *fatal_error = true;
        return false;
    }

    while (work.count != 0) {
        analysis_state_t *state = &work.items[work.count - 1];
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        call_info_t call;
        bool has_call;
        size_t visited_index;
        bool allow_visited = false;
        uint64_t rsp;

        if (!state->vm.rip_known || !decode(image, &decoder, state->vm.rip,
                                            &instruction, operands)) {
            pop_state(&work);
            continue;
        }
        if (!append_address(&state->path, state->vm.rip)) {
            *fatal_error = true;
            break;
        }
        if (state->branch_count == 1 && work.count - 1 == ignored_test_rsp_branch) {
            pop_state(&work);
            continue;
        }
        if (state->branch_count == 1 && is_cmp_rax_18(&instruction, operands)) {
            bad_cmp_rax_branch = 2 * ignored_test_rsp_branch;
        }
        if (state->branch_count == 2 && work.count - 1 == bad_cmp_rax_branch) {
            pop_state(&work);
            continue;
        }

        visited_index = find_visited_map(&visited_map, state->vm.rip);
        if (visited_index != SIZE_MAX) {
            cfg_info_t *known = &visited[visited_index].info;
            allow_visited = known->unresolved_branch;
            known->unresolved_branch = false;
            if (instruction.meta.category != ZYDIS_CATEGORY_COND_BR &&
                state->cfg_info.cmov_id != CFG_CMOV_DETACHED &&
                state->cfg_info.cmov_id + 1 == known->cmov_id) {
                is_double_stepping = true;
                allow_visited = true;
            } else if (allow_visited && is_double_stepping) {
                is_double_stepping = false;
                state->cfg_info.cmov_id = CFG_CMOV_DETACHED;
            }
            if (!allow_visited) {
                pop_state(&work);
                continue;
            }
        } else {
            if (is_double_stepping) {
                is_double_stepping = false;
                state->cfg_info.cmov_id = CFG_CMOV_DETACHED;
            }
            if (!reserve_array((void **)&visited, &visited_capacity,
                               visited_count + 1, sizeof(*visited))) {
                *fatal_error = true;
                break;
            }
            if (!insert_visited_map(&visited_map, state->vm.rip,
                                    visited_count)) {
                *fatal_error = true;
                break;
            }
            visited[visited_count++] = (cfg_visited_t){ state->vm.rip, state->cfg_info };
        }

        step_count++;
        if (step_count > ANALYSIS_MAX_STEPS) {
            max_steps_reached = true;
            break;
        }
        has_call = extract_call_info(state, &instruction, operands, &call);
        update_scan_state(&scan, state, &instruction, operands, &call, has_call);
        if (scan_can_stop(&scan)) break;
        if (dearxan_vm_read_register(&state->vm, ZYDIS_REGISTER_RSP, &rsp) &&
            rsp > ANALYSIS_INITIAL_RSP) {
            pop_state(&work);
            continue;
        }
        if (is_int_2d(&instruction, operands)) {
            state->vm.rip += instruction.length;
            continue;
        }
        if (is_cmov(instruction.mnemonic)) {
            dearxan_vm_t forked_vm;
            bool has_fork = false;
            if (!dearxan_vm_step(&state->vm, &instruction, operands,
                                 &forked_vm, &has_fork)) {
                *fatal_error = true;
                break;
            }
            if (has_fork) {
                analysis_state_t forked;
                if (!clone_analysis_state_with_vm(&forked, state,
                                                  &forked_vm)) {
                    dearxan_vm_uninit(&forked_vm);
                    *fatal_error = true;
                    break;
                }
                cmov_pair_gen += 3;
                state->cfg_info.cmov_id = cmov_pair_gen;
                forked.cfg_info.cmov_id = cmov_pair_gen + 1;
                state->branch_count++;
                forked.branch_count = state->branch_count;
                state->basic_block_index = state->path.count;
                forked.fork_index = forked.path.count;
                forked.basic_block_index = forked.path.count;
                if (!push_state(&work, &forked)) {
                    uninit_analysis_state(&forked);
                    *fatal_error = true;
                    break;
                }
            }
            continue;
        }
        if (has_call && call.has_return_ip &&
            target_oob_or_visited(image, &visited_map, &call)) {
            dearxan_vm_write_register(&state->vm, ZYDIS_REGISTER_RSP, true,
                                      call.return_rsp);
            state->vm.rip = call.return_ip;
            state->vm.rip_known = true;
            invalidate_volatile_registers(&state->vm);
            continue;
        }
        {
            dearxan_vm_t forked_vm;
            bool has_fork = false;
            if (!dearxan_vm_step(&state->vm, &instruction, operands,
                                 &forked_vm, &has_fork)) {
                *fatal_error = true;
                break;
            }
            if (has_fork) {
                analysis_state_t forked;
                if (!clone_analysis_state_with_vm(&forked, state,
                                                  &forked_vm)) {
                    dearxan_vm_uninit(&forked_vm);
                    *fatal_error = true;
                    break;
                }
                state->branch_count++;
                forked.branch_count = state->branch_count;
                state->basic_block_index = state->path.count;
                forked.fork_index = forked.path.count;
                forked.basic_block_index = forked.path.count;
                if (!push_state(&work, &forked)) {
                    uninit_analysis_state(&forked);
                    *fatal_error = true;
                    break;
                }
            } else if ((instruction.operand_count_visible != 0 &&
                        instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR &&
                        !is_direct_relative_branch(&instruction, &operands[0])) ||
                       instruction.meta.category == ZYDIS_CATEGORY_RET) {
                if (!state->vm.rip_known) {
                    size_t start_index = state->basic_block_index;
                    if (start_index > state->path.count) start_index = 0;
                    for (size_t i = start_index; i < state->path.count; i++) {
                        size_t index = find_visited_map(
                            &visited_map, state->path.items[i]);
                        if (index != SIZE_MAX) visited[index].info.unresolved_branch = true;
                    }
                }
            }
        }
    }

    while (work.count != 0) pop_state(&work);
    free(work.items);
    free(visited);
    free(visited_map.slots);
    if (!*fatal_error && !max_steps_reached && scan.has_context_pop) {
        const unsigned char *context = dearxan_image_read(
            image, stub->context_pop_va, 5, NULL);
        if (context == NULL ||
            (context[0] != 0xe9 && memcmp(context, "\x48\x03\x64\x24\x08", 5) != 0)) {
            scan.has_context_pop = false;
        }
    }
    if (!*fatal_error && !max_steps_reached && scan.has_context_pop) {
        move_encryption_result(&scan);
    }
    uninit_scan_state(&scan);
    if (*fatal_error || max_steps_reached || !scan.has_context_pop) {
        dearxan_free_encrypted_region_list(&stub->encrypted_regions);
        memset(stub, 0, sizeof(*stub));
        if (max_steps_reached) *fatal_error = true;
        return false;
    }
    return true;
}

typedef struct candidate_result {
    dearxan_stub_info_t stub;
    bool matched;
    bool fatal_error;
} candidate_result_t;

typedef struct candidate_work {
    const dearxan_image_t *image;
    const candidate_list_t *candidates;
    candidate_result_t *results;
    volatile LONG next_index;
} candidate_work_t;

static DWORD WINAPI analyze_candidate_worker(void *opaque) {
    candidate_work_t *work = opaque;
    for (;;) {
        LONG index = InterlockedIncrement(&work->next_index) - 1;
        candidate_result_t *result;
        if (index < 0 || (size_t)index >= work->candidates->count) break;
        result = &work->results[index];
        result->matched = analyze_candidate(
            work->image, work->candidates->items[index], &result->stub,
            &result->fatal_error);
    }
    return 0;
}

bool dearxan_analyze_all_stubs(const dearxan_image_t *image,
                               dearxan_stub_list_t *stubs,
                               const char **error_message) {
    candidate_list_t candidates = { 0 };
    candidate_result_t *results = NULL;
    dearxan_stub_info_t *items = NULL;
    size_t count = 0;
    size_t errors = 0;
    if (stubs == NULL || image == NULL) return false;
    memset(stubs, 0, sizeof(*stubs));
    if (error_message != NULL) *error_message = NULL;
    candidates.valid = true;
    if (!dearxan_image_for_each_section(image, collect_candidates, &candidates) ||
        !candidates.valid) {
        free(candidates.items);
        if (error_message != NULL) *error_message = "failed to scan executable sections";
        return false;
    }
    if (candidates.count != 0) {
        candidate_work_t work;
        HANDLE handles[MAXIMUM_WAIT_OBJECTS - 1];
        DWORD handle_count = 0;
        DWORD worker_count;
        results = calloc(candidates.count, sizeof(*results));
        items = calloc(candidates.count, sizeof(*items));
        if (results == NULL || items == NULL) {
            free(results);
            free(items);
            free(candidates.items);
            if (error_message != NULL) *error_message = "failed to allocate stub list";
            return false;
        }
        work.image = image;
        work.candidates = &candidates;
        work.results = results;
        work.next_index = 0;
        worker_count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) / 4;
        if (worker_count == 0) worker_count = 1;
        if (worker_count > 8) worker_count = 8;
        if (worker_count > candidates.count) worker_count = (DWORD)candidates.count;
        if (worker_count > MAXIMUM_WAIT_OBJECTS) worker_count = MAXIMUM_WAIT_OBJECTS;
        for (DWORD i = 1; i < worker_count; i++) {
            HANDLE thread = CreateThread(NULL, 0, analyze_candidate_worker,
                                         &work, 0, NULL);
            if (thread != NULL) handles[handle_count++] = thread;
        }
        analyze_candidate_worker(&work);
        if (handle_count != 0) {
            DWORD wait_result = WaitForMultipleObjects(
                handle_count, handles, TRUE, INFINITE);
            if (wait_result == WAIT_FAILED) {
                for (DWORD i = 0; i < handle_count; i++) {
                    WaitForSingleObject(handles[i], INFINITE);
                }
            }
            for (DWORD i = 0; i < handle_count; i++) {
                CloseHandle(handles[i]);
            }
        }
        for (size_t i = 0; i < candidates.count; i++) {
            if (results[i].matched) {
                items[count++] = results[i].stub;
                memset(&results[i].stub, 0, sizeof(results[i].stub));
            } else if (results[i].fatal_error) {
                errors++;
            }
        }
    }
    free(results);
    free(candidates.items);
    if (errors != 0) {
        dearxan_stub_list_t partial = { items, count };
        dearxan_free_stub_list(&partial);
        if (error_message != NULL) *error_message = "failed to analyze all Arxan stubs";
        return false;
    }
    stubs->items = items;
    stubs->count = count;
    return true;
}

void dearxan_free_stub_list(dearxan_stub_list_t *stubs) {
    if (stubs == NULL) return;
    for (size_t i = 0; i < stubs->count; i++) {
        dearxan_free_encrypted_region_list(&stubs->items[i].encrypted_regions);
    }
    free(stubs->items);
    memset(stubs, 0, sizeof(*stubs));
}

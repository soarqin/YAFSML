/*
 * C11 implementation of dearxan v0.5.3 functionality.
 *
 * Original project copyright (c) 2025 William Tremblay.
 * Licensed under the MIT license; see THIRD_PARTY_NOTICES.md.
 */

#include "encryption.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rotate_left32(uint32_t value, uint32_t count) {
    count &= 31;
    return count == 0 ? value : (value << count) | (value >> (32 - count));
}

void dearxan_tea_decrypt(uint32_t block[2], const uint32_t key[4]) {
    uint32_t sum = 0xC6EF3720u;
    for (unsigned int i = 0; i < 32; i++) {
        block[1] -= ((block[0] << 4) + key[2]) ^ (block[0] + sum) ^
                    ((block[0] >> 5) + key[3]);
        block[0] -= ((block[1] << 4) + key[0]) ^ (block[1] + sum) ^
                    ((block[1] >> 5) + key[1]);
        sum -= 0x9E3779B9u;
    }
}

uint32_t dearxan_rmx_decrypt(uint32_t block, uint32_t *key, uint32_t *key_rotation) {
    *key = rotate_left32(*key, *key_rotation);
    block -= *key * *key_rotation;
    *key_rotation ^= ~block;
    return block;
}

uint32_t dearxan_sub_decrypt(uint32_t block, uint32_t key) {
    return key - block;
}

bool dearxan_read_varint(const unsigned char *bytes, size_t length,
                         size_t *consumed, uint32_t *value) {
    uint32_t result = 0;
    unsigned int shift = 0;
    if (bytes == NULL || consumed == NULL || value == NULL) return false;
    for (size_t i = 0; i < length; i++) {
        uint32_t payload = bytes[i] & 0x7fu;
        if (shift >= 32 || (payload != 0 && payload > (UINT32_MAX >> shift))) return false;
        payload <<= shift;
        if (result > UINT32_MAX - payload) return false;
        result += payload;
        if (bytes[i] < 0x80) {
            *consumed = i + 1;
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool dearxan_parse_encrypted_regions(const unsigned char *bytes, size_t length,
                                     dearxan_encrypted_region_t **regions,
                                     size_t *region_count,
                                     size_t *stream_size) {
    dearxan_encrypted_region_t *items = NULL;
    size_t count = 0;
    size_t capacity = 0;
    size_t offset = 0;
    size_t stream_offset = 0;
    uint32_t rva = 0;
    if (bytes == NULL || regions == NULL || region_count == NULL || stream_size == NULL) return false;
    while (offset < length) {
        uint32_t delta;
        uint32_t size;
        size_t consumed;
        if (!dearxan_read_varint(bytes + offset, length - offset, &consumed, &delta) ||
            delta == 0) goto fail;
        offset += consumed;
        if (rva > UINT32_MAX - delta) goto fail;
        rva += delta;
        if (rva == UINT32_MAX) {
            *regions = items;
            *region_count = count;
            *stream_size = stream_offset;
            return true;
        }
        if (!dearxan_read_varint(bytes + offset, length - offset, &consumed, &size) ||
            size == 0 || rva > UINT32_MAX - size || stream_offset > SIZE_MAX - size) goto fail;
        offset += consumed;
        if (count == capacity) {
            size_t next_capacity = capacity == 0 ? 8 : capacity * 2;
            dearxan_encrypted_region_t *next;
            if (next_capacity < capacity ||
                next_capacity > SIZE_MAX / sizeof(*items)) goto fail;
            next = realloc(items, next_capacity * sizeof(*items));
            if (next == NULL) goto fail;
            items = next;
            capacity = next_capacity;
        }
        items[count++] = (dearxan_encrypted_region_t){ stream_offset, size, rva };
        rva += size;
        stream_offset += size;
    }
fail:
    free(items);
    return false;
}

void dearxan_free_encrypted_region_list(dearxan_encrypted_region_list_t *list) {
    if (list == NULL) return;
    free(list->regions);
    free(list->decrypted_stream);
    memset(list, 0, sizeof(*list));
}

typedef struct relocation_context {
    const dearxan_image_t *image;
    dearxan_encrypted_region_list_t *list;
    bool valid;
} relocation_context_t;

typedef struct relocation_list {
    uint32_t *items;
    size_t count;
    size_t capacity;
    bool valid;
} relocation_list_t;

static bool apply_relocation(uint32_t rva, void *opaque) {
    relocation_context_t *context = opaque;
    for (size_t i = 0; i < context->list->region_count; i++) {
        const dearxan_encrypted_region_t *region = &context->list->regions[i];
        uint64_t end = (uint64_t)region->rva + region->size;
        if (rva < region->rva || (uint64_t)rva + sizeof(uint64_t) > end) continue;
        {
            size_t offset = region->stream_offset + (rva - region->rva);
            uint64_t value;
            int64_t delta = (int64_t)(context->image->base_va - context->image->preferred_base);
            memcpy(&value, context->list->decrypted_stream + offset, sizeof(value));
            value += delta;
            if (dearxan_image_read(context->image, value, 1, NULL) == NULL) {
                context->valid = false;
                return false;
            }
            memcpy(context->list->decrypted_stream + offset, &value, sizeof(value));
        }
    }
    return true;
}

bool dearxan_apply_relocations(const dearxan_image_t *image,
                               dearxan_encrypted_region_list_t *list) {
    relocation_context_t context = { image, list, true };
    if (image == NULL || list == NULL ||
        (list->decrypted_size != 0 && list->decrypted_stream == NULL)) return false;
    return dearxan_image_for_each_relocation64(image, apply_relocation, &context) &&
           context.valid;
}

static bool collect_relocation(uint32_t rva, void *opaque) {
    relocation_list_t *list = opaque;
    if (list->count == list->capacity) {
        size_t capacity = list->capacity == 0 ? 256 : list->capacity * 2;
        uint32_t *items;
        if (capacity < list->capacity || capacity > SIZE_MAX / sizeof(*items)) {
            list->valid = false;
            return false;
        }
        items = realloc(list->items, capacity * sizeof(*items));
        if (items == NULL) {
            list->valid = false;
            return false;
        }
        list->items = items;
        list->capacity = capacity;
    }
    list->items[list->count++] = rva;
    return true;
}

static bool has_valid_pe_headers(const dearxan_image_t *image) {
    const unsigned char *base = image->base;
    int32_t pe_offset;
    uint32_t signature;
    if (base == NULL || image->size < 0x40 || base[0] != 'M' || base[1] != 'Z') return false;
    memcpy(&pe_offset, base + 0x3c, sizeof(pe_offset));
    if (pe_offset <= 0 || (size_t)pe_offset > image->size - sizeof(signature)) return false;
    memcpy(&signature, base + pe_offset, sizeof(signature));
    return signature == UINT32_C(0x00004550);
}

static int compare_u32(const void *left, const void *right) {
    uint32_t lhs = *(const uint32_t *)left;
    uint32_t rhs = *(const uint32_t *)right;
    return lhs < rhs ? -1 : lhs > rhs;
}

typedef struct region_reference {
    size_t list_index;
    dearxan_encrypted_region_t *region;
} region_reference_t;

static int compare_region_references(const void *left, const void *right) {
    const dearxan_encrypted_region_t *lhs = ((const region_reference_t *)left)->region;
    const dearxan_encrypted_region_t *rhs = ((const region_reference_t *)right)->region;
    if (lhs->rva != rhs->rva) return lhs->rva < rhs->rva ? -1 : 1;
    if (lhs->size != rhs->size) return lhs->size < rhs->size ? -1 : 1;
    return 0;
}

static bool clone_region_list(dearxan_encrypted_region_list_t *destination,
                              const dearxan_encrypted_region_list_t *source) {
    memset(destination, 0, sizeof(*destination));
    destination->kind = source->kind;
    destination->region_count = source->region_count;
    destination->decrypted_size = source->decrypted_size;
    if (source->region_count != 0) {
        if (source->regions == NULL ||
            source->region_count > SIZE_MAX / sizeof(*source->regions)) return false;
        destination->regions = malloc(source->region_count * sizeof(*source->regions));
        if (destination->regions == NULL) return false;
        memcpy(destination->regions, source->regions,
               source->region_count * sizeof(*source->regions));
    }
    if (source->decrypted_size != 0) {
        if (source->decrypted_stream == NULL) goto fail;
        destination->decrypted_stream = malloc(source->decrypted_size);
        if (destination->decrypted_stream == NULL) goto fail;
        memcpy(destination->decrypted_stream, source->decrypted_stream,
               source->decrypted_size);
    }
    return true;
fail:
    dearxan_free_encrypted_region_list(destination);
    return false;
}

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

static bool regions_intersect(const dearxan_encrypted_region_t *left,
                              const dearxan_encrypted_region_t *right) {
    uint64_t left_end = (uint64_t)left->rva + left->size;
    uint64_t right_end = (uint64_t)right->rva + right->size;
    return left_end < right_end ? left_end > right->rva : right_end > left->rva;
}

static double image_region_entropy(const dearxan_image_t *image,
                                   const dearxan_encrypted_region_list_t *list) {
    size_t distribution[256] = { 0 };
    size_t length = 0;
    double length_log2;
    double sum = 0.0;
    for (size_t i = 0; i < list->region_count; i++) {
        const dearxan_encrypted_region_t *region = &list->regions[i];
        const unsigned char *bytes = dearxan_image_read(
            image, image->base_va + region->rva, region->size, NULL);
        if (bytes == NULL) continue;
        for (size_t j = 0; j < region->size; j++) distribution[bytes[j]]++;
        length += region->size;
    }
    if (length == 0) return NAN;
    length_log2 = log2((double)length);
    for (size_t i = 0; i < 256; i++) {
        if (distribution[i] != 0) {
            sum += (double)distribution[i] *
                   (length_log2 - log2((double)distribution[i]));
        }
    }
    return sum / (double)length;
}

bool dearxan_resolve_encrypted_region_lists(
    const dearxan_image_t *image,
    const dearxan_encrypted_region_list_t *const *lists, size_t list_count,
    dearxan_encrypted_region_list_t **resolved, size_t *resolved_count) {
    dearxan_encrypted_region_list_t *processed = NULL;
    double *entropies = NULL;
    bool *eliminated = NULL;
    region_reference_t *regions = NULL;
    size_t region_count = 0;
    size_t region_capacity = 0;
    size_t count = 0;
    size_t output_count = 0;
    relocation_list_t relocations = { 0 };

    if (image == NULL || resolved == NULL || resolved_count == NULL ||
        (list_count != 0 && lists == NULL)) return false;
    *resolved = NULL;
    *resolved_count = 0;
    relocations.valid = true;
    if (has_valid_pe_headers(image) &&
        (!dearxan_image_for_each_relocation64(image, collect_relocation, &relocations) ||
         !relocations.valid)) goto fail;
    qsort(relocations.items, relocations.count, sizeof(*relocations.items), compare_u32);
    if (list_count == 0) {
        free(relocations.items);
        return true;
    }

    processed = calloc(list_count, sizeof(*processed));
    entropies = malloc(list_count * sizeof(*entropies));
    eliminated = calloc(list_count, sizeof(*eliminated));
    if (processed == NULL || entropies == NULL || eliminated == NULL) goto fail;

    for (size_t i = 0; i < list_count; i++) {
        const dearxan_encrypted_region_list_t *source = lists[i];
        if (source == NULL || source->region_count == 0) continue;
        if (!clone_region_list(&processed[count], source)) goto fail;
        if (source->region_count > SIZE_MAX - region_count ||
            !reserve_array((void **)&regions, &region_capacity,
                           region_count + source->region_count,
                           sizeof(*regions))) goto fail;
        for (size_t j = 0; j < source->region_count; j++) {
            regions[region_count++] = (region_reference_t){
                count, &processed[count].regions[j]
            };
        }
        count++;
    }

    qsort(regions, region_count, sizeof(*regions), compare_region_references);
    {
        size_t relocation_index = 0;
        uint64_t base_diff = image->base_va - image->preferred_base;
        for (size_t i = 0; i < region_count; i++) {
            dearxan_encrypted_region_list_t *parent =
                &processed[regions[i].list_index];
            const dearxan_encrypted_region_t *region = regions[i].region;
            uint64_t end = (uint64_t)region->rva + region->size;
            if (eliminated[regions[i].list_index]) continue;
            while (relocation_index < relocations.count &&
                   relocations.items[relocation_index] < region->rva) {
                relocation_index++;
            }
            if (relocation_index == relocations.count) break;
            for (size_t j = relocation_index; j < relocations.count &&
                 (uint64_t)relocations.items[j] + sizeof(uint64_t) <= end; j++) {
                uint32_t rva = relocations.items[j];
                size_t offset = region->stream_offset + (rva - region->rva);
                uint64_t value;
                if (offset > parent->decrypted_size ||
                    sizeof(value) > parent->decrypted_size - offset) {
                    eliminated[regions[i].list_index] = true;
                    break;
                }
                memcpy(&value, parent->decrypted_stream + offset, sizeof(value));
                value += base_diff;
                if (dearxan_image_read(image, value, 1, NULL) == NULL) {
                    eliminated[regions[i].list_index] = true;
                    break;
                }
                memcpy(parent->decrypted_stream + offset, &value, sizeof(value));
            }
        }
    }

    for (size_t i = 0; i < count; i++) {
        double base_entropy;
        if (eliminated[i]) continue;
        entropies[i] = dearxan_shannon_entropy(
            processed[i].decrypted_stream, processed[i].decrypted_size);
        base_entropy = image_region_entropy(image, &processed[i]);
        eliminated[i] = entropies[i] >= base_entropy;
    }

    {
        size_t best = SIZE_MAX;
        for (size_t i = 0; i < region_count; i++) {
            if (!eliminated[regions[i].list_index]) {
                best = i;
                break;
            }
        }
        if (best != SIZE_MAX) {
            for (size_t i = best + 1; i < region_count; i++) {
                size_t current_list = regions[i].list_index;
                size_t best_list = regions[best].list_index;
                if (current_list == best_list) {
                    best = i;
                    continue;
                }
                if (eliminated[current_list]) continue;
                if (!regions_intersect(regions[best].region, regions[i].region)) {
                    best = i;
                    continue;
                }
                if (entropies[best_list] > entropies[current_list]) {
                    eliminated[best_list] = true;
                    best = i;
                } else {
                    eliminated[current_list] = true;
                }
            }
        }
    }
    for (size_t i = 0; i < count; i++) if (!eliminated[i]) output_count++;
    if (output_count != 0) {
        dearxan_encrypted_region_list_t *output = calloc(output_count, sizeof(*output));
        size_t output_index = 0;
        if (output == NULL) goto fail;
        for (size_t i = 0; i < count; i++) {
            if (eliminated[i]) continue;
            output[output_index++] = processed[i];
            memset(&processed[i], 0, sizeof(processed[i]));
        }
        *resolved = output;
    }
    *resolved_count = output_count;
    dearxan_free_encrypted_region_lists(processed, count);
    free(entropies);
    free(eliminated);
    free(regions);
    free(relocations.items);
    return true;
fail:
    dearxan_free_encrypted_region_lists(processed, count + (count < list_count ? 1 : 0));
    free(entropies);
    free(eliminated);
    free(regions);
    free(relocations.items);
    return false;
}

void dearxan_free_encrypted_region_lists(dearxan_encrypted_region_list_t *lists,
                                         size_t list_count) {
    if (lists == NULL) return;
    for (size_t i = 0; i < list_count; i++) {
        dearxan_free_encrypted_region_list(&lists[i]);
    }
    free(lists);
}

double dearxan_shannon_entropy(const unsigned char *bytes, size_t length) {
    size_t distribution[256] = { 0 };
    double length_log2;
    double sum = 0.0;
    if (bytes == NULL || length == 0) return NAN;
    for (size_t i = 0; i < length; i++) distribution[bytes[i]]++;
    length_log2 = log2((double)length);
    for (size_t i = 0; i < 256; i++) {
        if (distribution[i] != 0) {
            sum += (double)distribution[i] *
                   (length_log2 - log2((double)distribution[i]));
        }
    }
    return sum / (double)length;
}

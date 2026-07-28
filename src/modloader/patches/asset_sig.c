/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "asset_sig.h"

#include "common/allocator.h"
#include "log.h"

#include <string.h>

bool ml_asset_sig_match_mount_ebl(const uint8_t *p, size_t remaining, mount_pattern_match_t *match) {
    bool stack_compare;
    if (p == NULL || match == NULL || remaining == 0) return false;
    /* First-byte gate. find_mount_ebl calls this for every byte of the game's
     * .text (tens of millions of offsets), so reject on the one byte both
     * patterns pin before touching the 46-byte memcmp below: pattern one needs
     * 0x48 (`mov rax, [rbp+..]`), pattern two needs 0x53 (`push rbx`). */
    if (p[0] != 0x48 && p[0] != 0x53) return false;
    if (remaining >= 42 && p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x45 &&
        memcmp(p + 4, "\x48\x89\x44\x24\x28", 5) == 0 &&
        (p[9] == 0x48 || p[9] == 0x4C || p[9] == 0x7C) && p[10] == 0x89 &&
        p[11] >= 0x44 && p[11] <= 0x7C && ((p[11] - 0x44) % 8) == 0 &&
        p[12] == 0x24 && p[13] == 0x20 && memcmp(p + 14, "\x4C\x8B\x0D", 3) == 0 &&
        (p[21] == 0x48 || p[21] == 0x49 || p[21] == 0x7C) && p[22] == 0x8B &&
        p[23] >= 0xD0 && p[23] <= 0xD7 &&
        (p[24] == 0x48 || p[24] == 0x49 || p[24] == 0x7C) && p[25] == 0x8B &&
        p[26] >= 0xC8 && p[26] <= 0xCF && p[27] == 0xE8 &&
        memcmp(p + 32, "\x0F\xB6\xD8", 3) == 0) {
        stack_compare = memcmp(p + 35, "\x48\x83\x7D", 3) == 0 && p[39] == 0x08 && p[40] == 0x72;
        if (!stack_compare && remaining >= 43) {
            stack_compare = memcmp(p + 35, "\x48\x83\x7C\x24", 4) == 0 && p[40] == 0x08 && p[41] == 0x72;
        }
        if (stack_compare) {
            match->displacement_offset = 28;
            match->instruction_end_offset = 32;
            return true;
        }
    }
    if (remaining >= 56 &&
        memcmp(p, "\x53\x48\x83\xEC\x30\x48\x8B\x44\x24\x70\x4D\x8B\xD1\x4C\x8B\x4C\x24\x60"
                  "\x4D\x8B\xD8\x48\x89\x44\x24\x28\x48\x8B\xCA\x48\x8B\x44\x24\x68\x4D\x8B\xC2"
                  "\x49\x8B\xD3\x48\x89\x44\x24\x20\xE8", 46) == 0 &&
        memcmp(p + 50, "\x48\x83\xC4\x30\x5B\xC3", 6) == 0) {
        match->displacement_offset = 46;
        match->instruction_end_offset = 50;
        return true;
    }
    return false;
}

static int base64_value(char value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

static bool der_read_length(const uint8_t **cursor, const uint8_t *end, size_t *length) {
    uint8_t first;
    if (cursor == NULL || *cursor == NULL || end == NULL || length == NULL || *cursor >= end) return false;
    first = *(*cursor)++;
    if ((first & 0x80) == 0) {
        *length = first;
        return true;
    }
    {
        size_t bytes = first & 0x7f;
        size_t value = 0;
        if (bytes == 0 || bytes > sizeof(size_t) || (size_t)(end - *cursor) < bytes) return false;
        for (size_t i = 0; i < bytes; i++) value = (value << 8) | *(*cursor)++;
        *length = value;
        return true;
    }
}

bool ml_asset_sig_rsa_block_size(const char *pem, size_t pem_length, size_t *block_size) {
    static const char begin[] = "-----BEGIN RSA PUBLIC KEY-----";
    static const char pem_end[] = "-----END RSA PUBLIC KEY-----";
    uint8_t *der;
    size_t begin_offset = SIZE_MAX;
    size_t end_offset = SIZE_MAX;
    size_t der_length = 0;
    uint32_t accumulator = 0;
    unsigned bits = 0;
    const uint8_t *cursor;
    const uint8_t *end;
    const uint8_t *sequence_end;
    size_t sequence_length;
    size_t modulus_length;
    ML_LOG_TRACE(L"asset-hooks", L"RSA parse begin: pem=%p length=%zu", pem, pem_length);
    if (pem == NULL || block_size == NULL || pem_length < sizeof(begin) + sizeof(pem_end)) return false;
    for (size_t i = 0; i + sizeof(begin) - 1 <= pem_length; i++) {
        if (memcmp(pem + i, begin, sizeof(begin) - 1) == 0) { begin_offset = i + sizeof(begin) - 1; break; }
    }
    if (begin_offset == SIZE_MAX) return false;
    for (size_t i = begin_offset; i + sizeof(pem_end) - 1 <= pem_length; i++) {
        if (memcmp(pem + i, pem_end, sizeof(pem_end) - 1) == 0) { end_offset = i; break; }
    }
    if (end_offset == SIZE_MAX) return false;
    ML_LOG_TRACE(L"asset-hooks", L"RSA PEM markers: begin=%zu end=%zu", begin_offset, end_offset);
    der = ml_mem_alloc(0, end_offset - begin_offset);
    if (der == NULL) return false;
    for (size_t i = begin_offset; i < end_offset; i++) {
        int value = base64_value(pem[i]);
        if (value < 0) continue;
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            der[der_length++] = (uint8_t)(accumulator >> bits);
            accumulator &= (UINT32_C(1) << bits) - 1;
        }
    }
    cursor = der;
    end = der + der_length;
    ML_LOG_TRACE(L"asset-hooks", L"RSA DER length: %zu", der_length);
    if (cursor >= end || *cursor++ != 0x30 || !der_read_length(&cursor, end, &sequence_length) ||
        sequence_length > (size_t)(end - cursor)) {
        ml_mem_free(der);
        return false;
    }
    sequence_end = cursor + sequence_length;
    if (cursor >= sequence_end || *cursor++ != 0x02 || !der_read_length(&cursor, sequence_end, &modulus_length) ||
        modulus_length == 0 || modulus_length > (size_t)(sequence_end - cursor)) {
        ml_mem_free(der);
        return false;
    }
    *block_size = modulus_length - (modulus_length > 1 && cursor[0] == 0 ? 1 : 0);
    ml_mem_free(der);
    ML_LOG_TRACE(L"asset-hooks", L"RSA parse complete: modulus=%zu block=%zu", modulus_length, *block_size);
    return *block_size != 0;
}

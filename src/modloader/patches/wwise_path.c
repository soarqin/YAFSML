/*
 * Copyright (C) 2026, Soar Qin<soarchin@gmail.com>
 *
 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "wwise_path.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdint.h>
#include <wchar.h>

size_t wwise_find_open_call(const uint8_t *text, size_t size) {
    if (text == NULL) return SIZE_MAX;
    for (size_t i = 0; i + 18 <= size; i++) {
        size_t offset;
        if (text[i] != 0xe8 || text[i + 5] != 0x83 || text[i + 6] != 0xf8 || text[i + 7] != 0x01) continue;
        if (text[i + 8] == 0x74) offset = i + 10;
        else if (i + 14 <= size && text[i + 8] == 0x0f && text[i + 9] == 0x84) offset = i + 14;
        else continue;
        if (offset + 8 > size || text[offset] < 0x48 || text[offset] > 0x4f ||
            text[offset + 1] != 0x83 || text[offset + 2] < 0xc0 || text[offset + 2] > 0xc7 ||
            text[offset + 3] != 0x38 || text[offset + 4] < 0x48 || text[offset + 4] > 0x4f ||
            text[offset + 5] != 0x83) continue;
        if (offset + 9 <= size && text[offset + 6] == 0x7d && text[offset + 8] == 0x08) return i;
        if (offset + 12 <= size && text[offset + 6] == 0xbd && text[offset + 11] == 0x08) return i;
    }
    return SIZE_MAX;
}

const wchar_t *wwise_strip_prefixes(const wchar_t *path) {
    if (path == NULL) return NULL;

    for (;;) {
        if (wcsncmp(path, L"sd:/", 4) == 0) {
            path += 4;
        } else if (wcsncmp(path, L"sd_dlc02:/", 10) == 0) {
            path += 10;
        } else {
            return path;
        }
    }
}

/* Concatenate `prefix + middle + path` (`middle` may be NULL) the way snprintf
 * reports: the returned length excludes the terminating NUL, and `buffer` is only
 * written when the result fits. Returns SIZE_MAX on a NULL prefix/path or on
 * length overflow. Lets the resolver hot path build candidates in a stack buffer
 * instead of allocating one heap block per candidate. */
size_t wwise_join3(wchar_t *buffer, size_t capacity, const wchar_t *prefix,
                   const wchar_t *middle, const wchar_t *path) {
    size_t lengths[3];
    const wchar_t *parts[3];
    size_t total = 0;
    size_t offset = 0;
    if (prefix == NULL || path == NULL) return SIZE_MAX;
    parts[0] = prefix;
    parts[1] = middle == NULL ? L"" : middle;
    parts[2] = path;
    for (size_t i = 0; i < 3; i++) {
        lengths[i] = wcslen(parts[i]);
        if (lengths[i] > SIZE_MAX - 1 - total) return SIZE_MAX;
        total += lengths[i];
    }
    if (buffer == NULL || total + 1 > capacity) return total;
    for (size_t i = 0; i < 3; i++) {
        memcpy(buffer + offset, parts[i], lengths[i] * sizeof(*buffer));
        offset += lengths[i];
    }
    buffer[offset] = L'\0';
    return total;
}

/* Wwise `.wem` requests are looked up under `wem/<name>` and under the
 * two-digit bucket `wem/<first two chars>/<name>`. */
void wwise_wem_bucket(const wchar_t *path, wchar_t bucket[WWISE_WEM_BUCKET_SIZE]) {
    static const wchar_t placeholder[WWISE_WEM_BUCKET_SIZE] = L"wem/00/";
    memcpy(bucket, placeholder, sizeof(placeholder));
    if (path == NULL || path[0] == L'\0' || path[1] == L'\0') return;
    bucket[4] = path[0];
    bucket[5] = path[1];
}

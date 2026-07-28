/*
 * Copyright (C) 2024,2025, Soar Qin<soarchin@gmail.com>

 * Use of this source code is governed by an MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT.
 */

#include "scanner.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#if defined(_MSC_VER)
#include <BaseTsd.h>
#define ssize_t SSIZE_T
#endif

/* Boyer Moore Matching Algorithm */
#define BM_DATA_MAX_LEN 256

// A utility function to get maximum of two integers
#if defined(max)
#undef max
#endif
static ssize_t max(ssize_t a, ssize_t b) { return a > b ? a : b; }

// The preprocessing function for Boyer Moore's
// bad character heuristic
void bad_data_heuristic(const uint8_t *data, const uint8_t *mask, size_t size,
                        ssize_t baddata[BM_DATA_MAX_LEN]) {
    ssize_t i;

    // Initialize all occurrences as -1
    for (i = BM_DATA_MAX_LEN - 1; i >= 0; i--)
        baddata[i] = -1;

    // Fill the actual value of last occurrence
    // of a character
    if (mask == NULL) {
        for (i = 0; i < size; i++)
            baddata[data[i]] = i;
    } else {
        for (i = 0; i < size; i++)
            if (mask[i] == 0xFF)
                baddata[data[i]] = i;
            else {
                for (ssize_t j = 0; j < 256; j++)
                    if ((data[i] & mask[i]) == (j & mask[i]))
                        baddata[j] = i;
            }
    }
}

/* A pattern searching function that uses Bad
   Character Heuristic of Boyer Moore Algorithm */
uint8_t *sig_scan_without_mask(void *base, size_t data_size, const uint8_t *pat, size_t pat_size) {
    ssize_t badchar[BM_DATA_MAX_LEN];
    uint8_t *data = base;

    /* Guard against unsigned underflow in the `data_size - pat_size` loop
       bound below: when the remaining data is smaller than the pattern the
       subtraction wraps to a huge value and the scan reads out of bounds. */
    if (data == NULL || pat_size == 0 || data_size < pat_size) return NULL;

    /* Fill the bad character array by calling
       the preprocessing function badCharHeuristic()
       for given pattern */
    bad_data_heuristic(pat, NULL, pat_size, badchar);

    ssize_t s = 0; // s is shift of the pattern with
    // respect to text
    while (s <= data_size - pat_size) {
        ssize_t j = (ssize_t)pat_size - 1;

        /* Keep reducing index j of pattern while
           characters of pattern and text are
           matching at this shift s */
        while (j >= 0 && pat[j] == data[s + j]) {
            j--;
        }

        /* If the pattern is present at current
           shift, then index j will become -1 after
           the above loop */
        if (j < 0) {
            return data + s;

            /* Shift the pattern so that the next
               character in text aligns with the last
               occurrence of it in pattern.
               The condition s+m < n is necessary for
               the case when pattern occurs at the end
               of text */
            /* s += s + pat_size < data_size ? pat_size - badchar[data[s + pat_size]] : 1; */
        } else
        /* Shift the pattern so that the bad character
           in text aligns with the last occurrence of
           it in pattern. The max function is used to
           make sure that we get a positive shift.
           We may get a negative shift if the last
           occurrence  of bad character in pattern
           is on the right side of the current
           character. */
            s += max(1, j - badchar[data[s + j]]);
    }
    return NULL;
}

uint8_t *sig_scan_with_mask(void *base, size_t data_size, const uint8_t *pat, const uint8_t *mask, size_t pat_size) {
    ssize_t badchar[BM_DATA_MAX_LEN];
    uint8_t *data = base;

    /* Guard against unsigned underflow in the `data_size - pat_size` loop
       bound below (see sig_scan_without_mask). */
    if (data == NULL || pat_size == 0 || data_size < pat_size) return NULL;

    /* Fill the bad character array by calling
       the preprocessing function badCharHeuristic()
       for given pattern */
    bad_data_heuristic(pat, mask, pat_size, badchar);

    ssize_t s = 0; // s is shift of the pattern with
    // respect to text
    while (s <= data_size - pat_size) {
        ssize_t j = (ssize_t)pat_size - 1;

        /* Keep reducing index j of pattern while
           characters of pattern and text are
           matching at this shift s */
        while (j >= 0) {
            switch (mask[j]) {
                case 0:
                    break;
                case 0xFF:
                    if (pat[j] != data[s + j])
                        goto next;
                    break;
                default:
                    if ((pat[j] & mask[j]) != (data[s + j] & mask[j]))
                        goto next;
                    break;
            }
            j--;
        }
    next:

        /* If the pattern is present at current
           shift, then index j will become -1 after
           the above loop */
        if (j < 0) {
            return data + s;

            /* Shift the pattern so that the next
               character in text aligns with the last
               occurrence of it in pattern.
               The condition s+m < n is necessary for
               the case when pattern occurs at the end
               of text */
            /* s += s + pat_size < data_size ? pat_size - badchar[data[s + pat_size]] : 1; */
        } else
        /* Shift the pattern so that the bad character
           in text aligns with the last occurrence of
           it in pattern. The max function is used to
           make sure that we get a positive shift.
           We may get a negative shift if the last
           occurrence  of bad character in pattern
           is on the right side of the current
           character. */
            s += max(1, j - badchar[data[s + j]]);
    }
    return NULL;
}

/*
uint8_t *sig_scan_without_mask(void *base, size_t size, const uint8_t *pattern, size_t pattern_size) {
    size_t end = size - pattern_size;
    uint8_t *u8_base = base;
    for (size_t i = 0; i < end; i++) {
        if (memcmp(pattern, u8_base + i, pattern_size) != 0) continue;
        return u8_base + i;
    }
    return NULL;
}

uint8_t *sig_scan_with_mask(void *base, size_t size, const uint8_t *pattern, const uint8_t *mask, size_t pattern_size) {
    size_t end = size - pattern_size;
    uint8_t *u8_curr = base;
    uint8_t *u8_end = u8_curr + end;
    while (u8_curr < u8_end) {
        for (size_t i = 0; i < pattern_size; i++) {
            switch (mask[i]) {
                case 0:
                    continue;
                case 0xFF:
                    if (pattern[i] != u8_curr[i])
                        goto next;
                    break;
                default:
                    if ((pattern[i] & mask[i]) != (u8_curr[i] & mask[i]))
                        goto next;
                    break;
            }
        }
        return u8_curr;
    next:
        u8_curr++;
    }
    return NULL;
}
*/

size_t sig_build_pattern_with_mask(const char *pattern, uint8_t *out_pattern, uint8_t *out_mask, size_t out_size) {
    static const int8_t char_table[] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -2, -2, -2, -1, -2, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
         0,  1,  2,  3,  4,  5,  6,  7,  8,  9, -1, -1, -1, -1, -1, -3,
        -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    };
    size_t pattern_size = 0;
    bool second_char = false;
    while (*pattern) {
        switch (char_table[(unsigned char)*pattern]) {
            case -1:
                return (size_t)-2;
            case -2:
                if (second_char) {
                    if (out_mask[pattern_size] == 0xF)
                        out_mask[pattern_size] = 0xFF;
                    pattern_size++;
                    second_char = false;
                }
                pattern++;
                break;
            case -3:
                if (second_char) {
                    out_mask[pattern_size] <<= 4;
                    out_pattern[pattern_size] <<= 4;
                    pattern_size++;
                    second_char = false;
                } else {
                    if (pattern_size >= out_size) {
                        return (size_t)-1;
                    }
                    out_mask[pattern_size] = 0x00;
                    out_pattern[pattern_size] = 0x00;
                    second_char = true;
                }
                pattern++;
                break;
            default:
                if (second_char) {
                    out_mask[pattern_size] <<= 4;
                    out_pattern[pattern_size] <<= 4;
                    out_mask[pattern_size] |= 0xF;
                    out_pattern[pattern_size] |= (uint8_t)char_table[*pattern];
                    pattern_size++;
                    second_char = false;
                } else {
                    if (pattern_size >= out_size) {
                        return (size_t)-1;
                    }
                    out_mask[pattern_size] = 0xF;
                    out_pattern[pattern_size] = (uint8_t)char_table[*pattern];
                    second_char = true;
                }
                pattern++;
                break;
        }
    }
    if (second_char) {
        if (out_mask[pattern_size] == 0xF)
            out_mask[pattern_size] = 0xFF;
        pattern_size++;
    }
    return pattern_size;
}

uint8_t *sig_scan(void *base, size_t size, const char *pattern) {
    size_t out_size = 64;
    uint8_t *out_pattern;
    uint8_t *out_mask;
    size_t pattern_size;
    for (;;) {
        out_pattern = LocalAlloc(0, out_size);
        out_mask = LocalAlloc(0, out_size);
        if (out_pattern == NULL || out_mask == NULL) {
            LocalFree(out_pattern);
            LocalFree(out_mask);
            return NULL;
        }
        pattern_size = sig_build_pattern_with_mask(pattern, out_pattern, out_mask, out_size);
        if (pattern_size == (size_t)-2) {
            LocalFree(out_pattern);
            LocalFree(out_mask);
            return NULL;
        }
        if (pattern_size == (size_t)-1) {
            LocalFree(out_pattern);
            LocalFree(out_mask);
            out_size <<= 1;
            continue;
        }
        break;
    }
    bool any_mask_not_0xff = false;
    for (size_t i = 0; i < pattern_size; i++) {
        if (out_mask[i] != 0xFF) {
            any_mask_not_0xff = true;
            break;
        }
    }
    uint8_t *res = any_mask_not_0xff ? sig_scan_with_mask(base, size, out_pattern, out_mask, pattern_size) : sig_scan_without_mask(base, size, out_pattern, pattern_size);
    LocalFree(out_pattern);
    LocalFree(out_mask);
    return res;
}

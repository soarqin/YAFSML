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
static void bad_data_heuristic(const uint8_t *data, const uint8_t *mask, size_t size,
                               ssize_t baddata[BM_DATA_MAX_LEN]) {
    // Initialize all occurrences as -1
    for (size_t c = 0; c < BM_DATA_MAX_LEN; c++)
        baddata[c] = -1;

    // Fill the actual value of last occurrence
    // of a character; a partially masked byte
    // occurs as every value it matches
    for (size_t i = 0; i < size; i++) {
        if (mask[i] == 0xFF) {
            baddata[data[i]] = (ssize_t)i;
            continue;
        }
        for (size_t c = 0; c < BM_DATA_MAX_LEN; c++)
            if ((data[i] & mask[i]) == (c & mask[i]))
                baddata[c] = (ssize_t)i;
    }
}

/* The parser leaves the wildcard bits of the pattern zero, so a masked
   compare is (data & mask) == pattern. */
static bool masked_equal(const uint8_t *data, const uint8_t *pat, const uint8_t *mask, size_t size) {
    for (size_t i = 0; i < size; i++)
        if ((data[i] & mask[i]) != pat[i])
            return false;
    return true;
}

/* A pattern searching function that uses Bad Character Heuristic of Boyer
   Moore Algorithm. Only pat[window, window + window_size) drives the shifts;
   every shift where that window matches is checked against the whole
   pattern, so the result is still the first full match. The caller
   guarantees data_size >= pat_size. */
static uint8_t *sig_scan_window(uint8_t *data, size_t data_size, const uint8_t *pat, const uint8_t *mask,
                                size_t pat_size, size_t window, size_t window_size) {
    ssize_t badchar[BM_DATA_MAX_LEN];
    const uint8_t *window_pat = pat + window;
    const uint8_t *window_mask = mask + window;
    size_t s = 0; // s is shift of the pattern with respect to text

    bad_data_heuristic(window_pat, window_mask, window_size, badchar);
    while (s <= data_size - pat_size) {
        const uint8_t *text = data + s + window;
        ssize_t j = (ssize_t)window_size - 1;

        /* Keep reducing index j of the window while
           it matches the text at this shift s */
        while (j >= 0 && (text[j] & window_mask[j]) == window_pat[j])
            j--;

        if (j >= 0)
            /* Shift the pattern so that the bad character
               in text aligns with the last occurrence of
               it in the window. The max function is used to
               make sure that we get a positive shift.
               We may get a negative shift if the last
               occurrence of bad character in the window
               is on the right side of the current
               character. */
            s += (size_t)max(1, j - badchar[text[j]]);
        else if (masked_equal(data + s, pat, mask, pat_size))
            return data + s;
        else
            s++;
    }
    return NULL;
}

/* Longest run of fully masked bytes; ties keep the later run. */
static size_t longest_fixed_run(const uint8_t *mask, size_t size, size_t *out_start) {
    size_t best = 0;
    size_t run = 0;

    *out_start = 0;
    for (size_t i = 0; i < size; i++) {
        run = mask[i] == 0xFF ? run + 1 : 0;
        if (run != 0 && run >= best) {
            best = run;
            *out_start = i + 1 - run;
        }
    }
    return best;
}

static uint8_t *sig_scan_pattern(uint8_t *data, size_t size, const uint8_t *pat, const uint8_t *mask,
                                 size_t pattern_size) {
    size_t fixed = pattern_size;
    size_t window;
    size_t window_size;

    /* Guard against unsigned underflow in the `size - pattern_size` bounds
       below: when the data is smaller than the pattern the subtraction wraps
       to a huge value and the scan reads out of bounds. */
    if (data == NULL || pattern_size == 0 || size < pattern_size) return NULL;

    /* Trailing wildcards only need room after a match, so scan the fixed
       prefix over a range shortened by their count: the first match is the
       same, but they no longer collapse every shift to one byte. */
    while (fixed != 0 && mask[fixed - 1] == 0) fixed--;
    if (fixed == 0) return data;

    /* A wildcard byte matches every character, so the bad character shift
       never moves past it; drive the scan with the longest fully masked run
       instead, or with the whole pattern when it has none. */
    window_size = longest_fixed_run(mask, fixed, &window);
    if (window_size == 0) {
        window = 0;
        window_size = fixed;
    }
    return sig_scan_window(data, size - (pattern_size - fixed), pat, mask, fixed, window, window_size);
}

static size_t sig_build_pattern_with_mask(const char *pattern, uint8_t *out_pattern, uint8_t *out_mask, size_t out_size) {
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
        const unsigned char c = (unsigned char)*pattern;
        int8_t value;

        /* char_table only covers ASCII; anything else is a parse error. */
        if (c >= sizeof(char_table) / sizeof(char_table[0]))
            return (size_t)-2;
        value = char_table[c];
        switch (value) {
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
                    out_pattern[pattern_size] |= (uint8_t)value;
                    pattern_size++;
                    second_char = false;
                } else {
                    if (pattern_size >= out_size) {
                        return (size_t)-1;
                    }
                    out_mask[pattern_size] = 0xF;
                    out_pattern[pattern_size] = (uint8_t)value;
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
    uint8_t *res;
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
    res = sig_scan_pattern(base, size, out_pattern, out_mask, pattern_size);
    LocalFree(out_pattern);
    LocalFree(out_mask);
    return res;
}

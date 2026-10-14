#include <stdint.h>
#include <stdio.h>

#include "test_common.h"
#include "scanner.h"

/* Naive reference: the first shift where every byte matches under its mask. */
static uint8_t *reference_scan(uint8_t *data, size_t size, const uint8_t *pat, const uint8_t *mask,
                               size_t pat_size) {
    if (pat_size == 0 || size < pat_size) return NULL;
    for (size_t s = 0; s <= size - pat_size; s++) {
        size_t i = 0;
        while (i < pat_size && (data[s + i] & mask[i]) == (pat[i] & mask[i])) i++;
        if (i == pat_size) return data + s;
    }
    return NULL;
}

static uint32_t random_state = 0x2545F491u;

static uint32_t next_random(void) {
    /* xorshift32: deterministic, so a failure reproduces. */
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

/* Random masked patterns against the naive scan. The small alphabet keeps
   partial matches, nibble aliases and repeated candidates frequent. */
static int test_matches_reference(void) {
    static const uint8_t alphabet[] = { 0x00, 0x05, 0x48, 0x4B, 0x8B, 0xC3, 0xE8 };
    static const char hex[] = "0123456789ABCDEF";
    static const uint8_t masks[] = { 0x00, 0xF0, 0x0F, 0xFF, 0xFF, 0xFF };
    uint8_t data[1024];

    for (size_t i = 0; i < sizeof(data); i++) data[i] = alphabet[next_random() % sizeof(alphabet)];

    for (int round = 0; round < 20000; round++) {
        uint8_t pat[16];
        uint8_t mask[16];
        char text[sizeof(pat) * 3];
        const size_t pat_size = 1 + next_random() % sizeof(pat);
        const uint8_t *source = data + next_random() % (sizeof(data) - sizeof(pat));
        const size_t offset = next_random() % sizeof(data);
        const size_t size = next_random() % (sizeof(data) - offset + 1);
        size_t len = 0;

        for (size_t i = 0; i < pat_size; i++) {
            /* Mostly copy the data so that full matches exist. */
            const uint8_t value = next_random() % 4 == 0 ? alphabet[next_random() % sizeof(alphabet)] : source[i];
            mask[i] = masks[next_random() % sizeof(masks)];
            pat[i] = value & mask[i];
            text[len++] = (mask[i] & 0xF0) != 0 ? hex[value >> 4] : '?';
            text[len++] = (mask[i] & 0x0F) != 0 ? hex[value & 0x0F] : '?';
            text[len++] = ' ';
        }
        text[len - 1] = '\0';
        if (sig_scan(data + offset, size, text) != reference_scan(data + offset, size, pat, mask, pat_size)) {
            fprintf(stderr, "smoke_scanner: \"%s\" differs from the reference at offset %zu size %zu\n",
                    text, offset, size);
            return 1;
        }
    }
    return 0;
}

int main(void) {
    /* data1:  48 8B 05 11 22 33 44 C3 */
    uint8_t data[] = { 0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x44, 0xC3 };
    const size_t data_size = sizeof(data);

    /* exact match (no wildcards), hit at offset 0 */
    EXPECT_EQ(sig_scan(data, data_size, "48 8B 05"), data);

    /* wildcard match, spans the whole buffer */
    EXPECT_EQ(sig_scan(data, data_size, "48 8B 05 ?? ?? ?? ?? C3"), data);

    /* single byte at the very end (boundary: s == data_size - pat_size) */
    EXPECT_EQ(sig_scan(data, data_size, "C3"), data + 7);

    /* interior match */
    EXPECT_EQ(sig_scan(data, data_size, "33 44"), data + 5);

    /* no match returns NULL (with and without wildcards) */
    EXPECT_NULL(sig_scan(data, data_size, "99 99 99"));
    EXPECT_NULL(sig_scan(data, data_size, "48 ?? 99"));

    /* regression: pattern longer than data must not underflow / read OOB.
       Before the guard, `data_size - pat_size` wrapped to a huge value. */
    {
        uint8_t tiny[] = { 0x48, 0x8B };
        EXPECT_NULL(sig_scan(tiny, sizeof(tiny), "48 8B 05 11"));    /* no wildcards */
        EXPECT_NULL(sig_scan(tiny, sizeof(tiny), "48 ?? 05 11"));    /* wildcards */
        EXPECT_NULL(sig_scan(tiny, 0, "48"));                        /* empty data */
        EXPECT_NULL(sig_scan(tiny, 1, "48 8B"));                     /* off-by-one */
    }

    /* The first full match wins over earlier partial and later full ones. */
    {
        uint8_t many[] = {
            0x48, 0x8B, 0x05, 0x01, 0x02, 0x03, 0x04, 0x90,
            0x48, 0x8B, 0x05, 0x05, 0x06, 0x07, 0x08, 0xC3,
            0x48, 0x8B, 0x05, 0x09, 0x0A, 0x0B, 0x0C, 0xC3,
        };
        EXPECT_EQ(sig_scan(many, sizeof(many), "48 8B 05 ?? ?? ?? ?? C3"), many + 8);
        EXPECT_EQ(sig_scan(many + 9, sizeof(many) - 9, "48 8B 05 ?? ?? ?? ?? C3"), many + 16);
        /* The longest fixed run leads the pattern; then a tie between runs. */
        EXPECT_EQ(sig_scan(many, sizeof(many), "48 8B 05 ?? ?? 07 ?? C3"), many + 8);
        EXPECT_EQ(sig_scan(many, sizeof(many), "8B 05 ?? ?? 07 ?? C3 48"), many + 9);
        EXPECT_EQ(sig_scan(many, sizeof(many), "48 8B 05 ?? ?? ?? ?? C3 48 ?? 05"), many + 8);
        EXPECT_NULL(sig_scan(many, sizeof(many), "48 8B 05 ?? ?? ?? ?? C3 48 ?? 06"));
    }

    /* Trailing wildcards still need room after the fixed prefix. */
    {
        uint8_t tail[] = { 0x00, 0x00, 0x48, 0x8B, 0x05 };
        EXPECT_EQ(sig_scan(tail, sizeof(tail), "48 8B 05"), tail + 2);
        EXPECT_NULL(sig_scan(tail, sizeof(tail), "48 8B 05 ??"));
        EXPECT_NULL(sig_scan(tail, sizeof(tail), "48 8B ?? ??"));
        EXPECT_EQ(sig_scan(tail, sizeof(tail), "00 ?? 48 ?? ??"), tail);
        EXPECT_EQ(sig_scan(data, data_size, "33 44 ??"), data + 5);
        EXPECT_NULL(sig_scan(data, data_size, "44 C3 ??"));
        EXPECT_EQ(sig_scan(data, data_size, "48 ?? ?? ?? ?? ?? ?? ??"), data);
        EXPECT_NULL(sig_scan(data, data_size, "48 ?? ?? ?? ?? ?? ?? ?? ??"));
        EXPECT_EQ(sig_scan(data, data_size, "?? 11 ?? 33"), data + 2);
    }

    /* All-wildcard patterns match at the start when they fit. */
    EXPECT_EQ(sig_scan(data, data_size, "?? ??"), data);
    EXPECT_EQ(sig_scan(data, data_size, "?? ?? ?? ?? ?? ?? ?? ??"), data);
    EXPECT_NULL(sig_scan(data, data_size, "?? ?? ?? ?? ?? ?? ?? ?? ??"));
    EXPECT_NULL(sig_scan(NULL, data_size, "??"));
    EXPECT_NULL(sig_scan(data, data_size, ""));

    /* Nibble masks: `4?` fixes the high nibble, `?4` the low one. */
    {
        uint8_t nibbles[] = { 0x12, 0x45, 0x34, 0x47, 0x14 };
        uint8_t single[] = { 0x50, 0x05 };
        EXPECT_EQ(sig_scan(nibbles, sizeof(nibbles), "4?"), nibbles + 1);
        EXPECT_EQ(sig_scan(nibbles, sizeof(nibbles), "?4"), nibbles + 2);
        EXPECT_EQ(sig_scan(nibbles, sizeof(nibbles), "4? ?4"), nibbles + 1);
        EXPECT_EQ(sig_scan(nibbles, sizeof(nibbles), "?4 4?"), nibbles + 2);
        EXPECT_EQ(sig_scan(nibbles, sizeof(nibbles), "?4 ?7 1?"), nibbles + 2);
        EXPECT_EQ(sig_scan(nibbles, sizeof(nibbles), "45 ?? 4?"), nibbles + 1);
        EXPECT_NULL(sig_scan(nibbles, sizeof(nibbles), "5?"));
        EXPECT_NULL(sig_scan(nibbles, sizeof(nibbles), "?4 ?4"));
        /* A lone digit is a whole byte: "5" is "05". */
        EXPECT_EQ(sig_scan(single, sizeof(single), "5"), single + 1);
    }

    /* Whitespace separates bytes; anything else outside hex digits and `?`,
       including non-ASCII bytes, is a parse error. */
    EXPECT_EQ(sig_scan(data, data_size, "48\t8B\r\n05"), data);
    EXPECT_NULL(sig_scan(data, data_size, "48 8G"));
    EXPECT_NULL(sig_scan(data, data_size, "48 \xC3\xA9"));
    EXPECT_NULL(sig_scan(data, data_size, "4\xB8"));
    EXPECT_NULL(sig_scan(data, data_size, "\xFF"));

    /* Patterns longer than the initial 64-byte parse buffer. */
    {
        uint8_t zeros[80] = { 0 };
        char text[80 * 3];
        for (size_t i = 0; i < sizeof(zeros); i++) {
            text[i * 3] = '0';
            text[i * 3 + 1] = i == 70 ? '?' : '0';
            text[i * 3 + 2] = ' ';
        }
        text[sizeof(text) - 1] = '\0';
        EXPECT_EQ(sig_scan(zeros, sizeof(zeros), text), zeros);
        zeros[70] = 0x0F;
        EXPECT_EQ(sig_scan(zeros, sizeof(zeros), text), zeros);
        zeros[70] = 0x10;
        EXPECT_NULL(sig_scan(zeros, sizeof(zeros), text));
    }

    EXPECT_EQ(test_matches_reference(), 0);

    printf("smoke_scanner: all tests passed\n");
    return 0;
}

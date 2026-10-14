#include "regulation_sig.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool sprj_writer_signature_matches(const uint8_t *target,
                                          size_t available) {
    if (target == NULL || available < 18) return false;
    if (memcmp(target, "\x48\x8b\xd1\x48\x8b\x0d", 6) != 0) return false;
    if (memcmp(target + 10, "\x48\x85\xc9", 3) != 0) return false;
    if (target[13] == 0x75) {
        return target[15] == 0x32 && target[16] == 0xc0 &&
               target[17] == 0xc3;
    }
    return available >= 22 && target[13] == 0x0f && target[14] == 0x85 &&
           target[19] == 0x32 && target[20] == 0xc0 &&
           target[21] == 0xc3;
}

static bool writer_already_added(uint8_t **writers, size_t count,
                                 uint8_t *target) {
    if (writers == NULL) return false;
    for (size_t i = 0; i < count; i++) {
        if (writers[i] == target) return true;
    }
    return false;
}

size_t ml_regulation_sprj_find_writers(uint8_t *text, size_t text_size,
                                       uint8_t **writers,
                                       size_t writer_capacity) {
    size_t count = 0;

    if (text == NULL) return 0;
    /*
     * Like me3: a direct call with a stack object in RCX, followed by
     * `test al, al; je`. The same call and wrapper shapes also reach the save
     * regulation reader (Sekiro 1.06 and Dark Souls III consume its bool with
     * `movzx ebp, al`) and Dark Souls III stats requests, which must keep
     * running.
     */
    for (size_t offset = 0; offset + 14 <= text_size; offset++) {
        int32_t displacement;
        int64_t target_offset;
        uint8_t *target;

        if (writers != NULL && count == writer_capacity) break;
        if (memcmp(text + offset, "\x48\x8d\x4c\x24", 4) != 0 ||
            text[offset + 5] != 0xe8 ||
            memcmp(text + offset + 10, "\x84\xc0\x74", 3) != 0) {
            continue;
        }
        memcpy(&displacement, text + offset + 6, sizeof(displacement));
        target_offset = (int64_t)offset + 10 + displacement;
        if (target_offset < 0 || (uint64_t)target_offset >= text_size) {
            continue;
        }
        target = text + (size_t)target_offset;
        if (!sprj_writer_signature_matches(
                target, text_size - (size_t)target_offset)) {
            continue;
        }
        if (writer_already_added(writers, count, target)) continue;
        if (writers != NULL) writers[count] = target;
        count++;
    }
    return count;
}

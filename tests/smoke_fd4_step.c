#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"
#include "process/fd4_step.h"
#include "process/fd4_step_static.h"

static void *seen_this;
static fd4_time_t *seen_time;

static void __cdecl original_step(void *this_ptr, fd4_time_t *time) {
    seen_this = this_ptr;
    seen_time = time;
}

static void __cdecl trampoline(void *this_ptr, fd4_time_t *time) {
    original_step(this_ptr, time);
}

static void write_rip_relative(uint8_t *instruction, uint8_t opcode2,
                               uintptr_t instruction_va, uintptr_t target_va) {
    intptr_t displacement = (intptr_t)(target_va - (instruction_va + 7));
    int32_t displacement32 = (int32_t)displacement;

    instruction[0] = 0x48;
    instruction[1] = opcode2;
    instruction[2] = 0x05;
    memcpy(instruction + 3, &displacement32, sizeof(displacement32));
}

static int test_static_initializer_lookup(void) {
    static const wchar_t step_name[] = L"TitleStep::STEP_InitMenu";
    uint8_t text[64] = { 0 };
    uint8_t rdata[128] = { 0 };
    const uintptr_t text_va = 0x140001000;
    const uintptr_t data_va = 0x140002000;
    const uintptr_t rdata_va = 0x140003000;
    const uintptr_t step_va = text_va + 32;
    fd4_step_static_sections_t sections = {
        text, sizeof(text), text_va,
        data_va, 16,
        rdata, sizeof(rdata), rdata_va,
    };
    fd4_step_static_result_t result;

    memcpy(rdata + sizeof(wchar_t), step_name, sizeof(step_name));
    write_rip_relative(text, 0x8d, text_va, step_va);
    write_rip_relative(text + 7, 0x89, text_va + 7, data_va);
    write_rip_relative(text + 14, 0x8d, text_va + 14, rdata_va + sizeof(wchar_t));
    write_rip_relative(text + 21, 0x89, text_va + 21, data_va + sizeof(void *));

    EXPECT_TRUE(fd4_step_static_find(&sections, step_name, &result));
    EXPECT_EQ(result.step, (void *)step_va);
    EXPECT_EQ(result.slot, (void **)data_va);
    EXPECT_TRUE(!fd4_step_static_find(&sections, L"TitleStep::STEP_Missing", &result));
    text[21] = 0x90;
    EXPECT_TRUE(!fd4_step_static_find(&sections, step_name, &result));
    return 0;
}

int main(void) {
    fd4_time_t time = { 0, 1.0f };
    int object = 0;

    trampoline(&object, &time);
    EXPECT_EQ(seen_this, &object);
    EXPECT_EQ(seen_time, &time);
    EXPECT_EQ(test_static_initializer_lookup(), 0);
    printf("smoke_fd4_step: all tests passed\n");
    return 0;
}

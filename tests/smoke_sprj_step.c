#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"
#include "process/sprj_step.h"

/* Mirrors the Dark Souls III `NS_SPRJ::Step<ParamStep>` constructor tail:
 *
 *   code+0   lea  rax, [rip+base_vtable]
 *   code+7   mov  [rbx], rax
 *   code+10  xor  ecx, ecx
 *   code+12  mov  [rbx+0x10], rcx
 *   code+16  lea  rax, [rip+step_table]
 *   code+23  mov  [rbx+0x20], rax
 *   code+27  lea  rax, [rip+derived_vtable]
 *   code+34  mov  [rbx], rax
 */
#define MODRM_BASE_RBX 0x03
#define MODRM_REG_RAX 0
#define MODRM_REG_RCX 1
#define STEP_INDEX_OFFSET 0x10
#define STEP_TABLE_OFFSET 0x20
#define CODE_OFFSET 16
#define INDEX_STORE_OFFSET (CODE_OFFSET + 12)
#define DERIVED_STORE_OFFSET (CODE_OFFSET + 34)

static uint8_t text[128];
static void *step_table[4];
static uint8_t base_vtable[8];
static uint8_t derived_vtable[8];
static uint8_t unrelated_data[32];

static int write_lea_rax(uint8_t *instruction, const void *target) {
    int64_t displacement = (int64_t)((const uint8_t *)target - (instruction + 7));
    int32_t relative;
    EXPECT_TRUE(displacement >= INT32_MIN && displacement <= INT32_MAX);
    relative = (int32_t)displacement;
    instruction[0] = 0x48;
    instruction[1] = 0x8d;
    instruction[2] = 0x05;
    memcpy(instruction + 3, &relative, sizeof(relative));
    return 0;
}

static void write_mov_base(uint8_t *instruction) {
    instruction[0] = 0x48;
    instruction[1] = 0x89;
    instruction[2] = MODRM_BASE_RBX;
}

static void write_mov_base_disp8(uint8_t *instruction, uint8_t reg, uint8_t displacement) {
    instruction[0] = 0x48;
    instruction[1] = 0x89;
    instruction[2] = (uint8_t)(0x40 | (reg << 3) | MODRM_BASE_RBX);
    instruction[3] = displacement;
}

static int build_constructor(void) {
    uint8_t *code = text + CODE_OFFSET;

    memset(text, 0x90, sizeof(text));
    step_table[0] = text + 4;
    step_table[1] = text + 8;
    step_table[2] = NULL;
    step_table[3] = NULL;

    EXPECT_EQ(write_lea_rax(code, base_vtable), 0);
    write_mov_base(code + 7);
    code[10] = 0x33;
    code[11] = 0xc9;
    write_mov_base_disp8(code + 12, MODRM_REG_RCX, STEP_INDEX_OFFSET);
    EXPECT_EQ(write_lea_rax(code + 16, step_table), 0);
    write_mov_base_disp8(code + 23, MODRM_REG_RAX, STEP_TABLE_OFFSET);
    EXPECT_EQ(write_lea_rax(code + 27, derived_vtable), 0);
    write_mov_base(code + 34);
    return 0;
}

static int test_resolves_wait_step(void) {
    sprj_step_table_t found;

    EXPECT_EQ(build_constructor(), 0);
    EXPECT_TRUE(sprj_step_find_from_vtable(text, sizeof(text), derived_vtable,
                                           (const uint8_t *)step_table, sizeof(step_table),
                                           &found));
    EXPECT_EQ(found.table, step_table);
    EXPECT_EQ(found.first_step, step_table[0]);
    EXPECT_EQ(found.second_step, step_table[1]);
    EXPECT_EQ(found.index_offset, STEP_INDEX_OFFSET);
    EXPECT_EQ(found.table_offset, STEP_TABLE_OFFSET);
    /* The base vtable is stored without a step table of its own. */
    EXPECT_TRUE(!sprj_step_find_from_vtable(text, sizeof(text), base_vtable,
                                            (const uint8_t *)step_table, sizeof(step_table),
                                            &found));
    return 0;
}

static int test_rejects_broken_constructor(void) {
    sprj_step_table_t found;

    /* `mov [rip+disp32], rax` names no base register, so the store that anchors
       the match is no longer a candidate. */
    EXPECT_EQ(build_constructor(), 0);
    text[DERIVED_STORE_OFFSET + 2] = 0x05;
    EXPECT_TRUE(!sprj_step_find_from_vtable(text, sizeof(text), derived_vtable,
                                            (const uint8_t *)step_table, sizeof(step_table),
                                            &found));

    /* Without the step-index store the object layout stays unverified. */
    EXPECT_EQ(build_constructor(), 0);
    memset(text + INDEX_STORE_OFFSET, 0x90, 4);
    EXPECT_TRUE(!sprj_step_find_from_vtable(text, sizeof(text), derived_vtable,
                                            (const uint8_t *)step_table, sizeof(step_table),
                                            &found));

    /* The step index must live before the table pointer in the object. */
    EXPECT_EQ(build_constructor(), 0);
    write_mov_base_disp8(text + INDEX_STORE_OFFSET, MODRM_REG_RCX, STEP_TABLE_OFFSET + 8);
    EXPECT_TRUE(!sprj_step_find_from_vtable(text, sizeof(text), derived_vtable,
                                            (const uint8_t *)step_table, sizeof(step_table),
                                            &found));

    /* A table outside the data section is rejected. */
    EXPECT_EQ(build_constructor(), 0);
    EXPECT_TRUE(!sprj_step_find_from_vtable(text, sizeof(text), derived_vtable,
                                            unrelated_data, sizeof(unrelated_data), &found));

    /* Step entries must point into the text section. */
    EXPECT_EQ(build_constructor(), 0);
    step_table[1] = derived_vtable;
    EXPECT_TRUE(!sprj_step_find_from_vtable(text, sizeof(text), derived_vtable,
                                            (const uint8_t *)step_table, sizeof(step_table),
                                            &found));

    EXPECT_EQ(build_constructor(), 0);
    EXPECT_TRUE(!sprj_step_find_from_vtable(NULL, sizeof(text), derived_vtable,
                                            (const uint8_t *)step_table, sizeof(step_table),
                                            &found));
    EXPECT_TRUE(!sprj_step_find_from_vtable(text, sizeof(text), NULL,
                                            (const uint8_t *)step_table, sizeof(step_table),
                                            &found));
    return 0;
}

int main(void) {
    EXPECT_EQ(test_resolves_wait_step(), 0);
    EXPECT_EQ(test_rejects_broken_constructor(), 0);
    printf("smoke_sprj_step: all tests passed\n");
    return 0;
}

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <Zydis/Zydis.h>

#include "test_common.h"
#include "dearxan/vm.h"

typedef struct vm_step_record {
    ZydisMnemonic mnemonic;
    bool ecx_known;
    bool edx_known;
    uint64_t ecx;
    uint64_t edx;
} vm_step_record_t;

static bool decode(const unsigned char *bytes, size_t size,
                   ZydisDecodedInstruction *instruction,
                   ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT]) {
    ZydisDecoder decoder;
    return ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64,
                                         ZYDIS_STACK_WIDTH_64)) &&
           ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes, size,
                                               instruction, operands));
}

static int test_registers(void) {
    dearxan_vm_t vm;
    uint64_t value;

    dearxan_vm_init(&vm, NULL, 0x1234, 0x10000);
    EXPECT_TRUE(dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RSP, &value));
    EXPECT_EQ(value, 0x10000);
    EXPECT_TRUE(!dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RAX, &value));

    dearxan_vm_write_register(&vm, ZYDIS_REGISTER_RAX, true,
                              UINT64_C(0x1122334455667788));
    EXPECT_TRUE(dearxan_vm_read_register(&vm, ZYDIS_REGISTER_EAX, &value));
    EXPECT_EQ(value, 0x55667788);
    dearxan_vm_write_register(&vm, ZYDIS_REGISTER_AX, true, 0xAABBu);
    EXPECT_TRUE(dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RAX, &value));
    EXPECT_EQ(value, UINT64_C(0x112233445566AABB));
    dearxan_vm_write_register(&vm, ZYDIS_REGISTER_AL, true, 0xCCu);
    EXPECT_TRUE(dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RAX, &value));
    EXPECT_EQ(value, UINT64_C(0x112233445566AACC));
    dearxan_vm_write_register(&vm, ZYDIS_REGISTER_EAX, true, 0xDEADBEEFu);
    EXPECT_TRUE(dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RAX, &value));
    EXPECT_EQ(value, 0xDEADBEEFu);
    dearxan_vm_write_register(&vm, ZYDIS_REGISTER_RAX, false, 0);
    EXPECT_TRUE(!dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RAX, &value));
    dearxan_vm_write_register(&vm, ZYDIS_REGISTER_AX, true, 0x1234);
    EXPECT_TRUE(!dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RAX, &value));
    dearxan_vm_uninit(&vm);
    return 0;
}

static int test_memory(void) {
    unsigned char image_bytes[128];
    dearxan_image_t image = { image_bytes, sizeof(image_bytes), 0, 0, 0 };
    dearxan_vm_t vm;
    dearxan_vm_t clone;
    unsigned char source[] = { 0xde, 0xad, 0xbe, 0xef, 0x42, 0x69 };
    unsigned char output[sizeof(source)] = { 0 };
    unsigned char changed = 0x11;

    for (size_t i = 0; i < sizeof(image_bytes); i++) image_bytes[i] = (unsigned char)i;
    dearxan_vm_init(&vm, &image, 0, 0x10000);
    EXPECT_TRUE(dearxan_vm_read_memory(&vm, 62, output, sizeof(output)));
    EXPECT_EQ(output[0], 62);
    EXPECT_EQ(output[5], 67);
    EXPECT_TRUE(dearxan_vm_write_memory(&vm, 62, source, sizeof(source)));
    memset(output, 0, sizeof(output));
    EXPECT_TRUE(dearxan_vm_read_memory(&vm, 62, output, sizeof(output)));
    EXPECT_TRUE(memcmp(output, source, sizeof(source)) == 0);
    dearxan_vm_invalidate_memory(&vm, 64, 2);
    EXPECT_TRUE(!dearxan_vm_read_memory(&vm, 62, output, sizeof(output)));
    EXPECT_TRUE(dearxan_vm_read_memory(&vm, 62, output, 2));

    EXPECT_TRUE(dearxan_vm_clone(&clone, &vm));
    EXPECT_TRUE(dearxan_vm_write_memory(&clone, 62, &changed, 1));
    EXPECT_TRUE(dearxan_vm_read_memory(&vm, 62, output, 1));
    EXPECT_EQ(output[0], source[0]);
    EXPECT_TRUE(dearxan_vm_read_memory(&clone, 62, output, 1));
    EXPECT_EQ(output[0], changed);
    dearxan_vm_uninit(&clone);
    dearxan_vm_uninit(&vm);
    return 0;
}

/* The block table grows and rehashes; every block written before a growth must
 * still read back afterwards, and a clone taken after growth must be
 * independent. Block index 0 is also exercised because an empty slot is encoded
 * as index+1 == 0. */
static int test_memory_table_growth(void) {
    dearxan_vm_t vm;
    dearxan_vm_t clone;
    uint64_t value;
    const uint64_t count = 300;

    dearxan_vm_init(&vm, NULL, 0, 0x10000);
    for (uint64_t i = 0; i < count; i++) {
        uint64_t address = i * 64;
        value = i + 1;
        EXPECT_TRUE(dearxan_vm_write_memory(&vm, address, &value, sizeof(value)));
    }
    for (uint64_t i = 0; i < count; i++) {
        value = 0;
        EXPECT_TRUE(dearxan_vm_read_memory(&vm, i * 64, &value, sizeof(value)));
        EXPECT_EQ(value, i + 1);
    }
    /* Unwritten blocks stay absent: with no image behind them the read fails. */
    EXPECT_TRUE(!dearxan_vm_read_memory(&vm, count * 64, &value, sizeof(value)));

    EXPECT_TRUE(dearxan_vm_clone(&clone, &vm));
    value = UINT64_C(0xdeadbeef);
    EXPECT_TRUE(dearxan_vm_write_memory(&clone, 0, &value, sizeof(value)));
    value = 0;
    EXPECT_TRUE(dearxan_vm_read_memory(&vm, 0, &value, sizeof(value)));
    EXPECT_EQ(value, 1);
    value = 0;
    EXPECT_TRUE(dearxan_vm_read_memory(&clone, 0, &value, sizeof(value)));
    EXPECT_EQ(value, UINT64_C(0xdeadbeef));
    /* Writing new blocks into the clone must not disturb the source. */
    for (uint64_t i = count; i < count + 200; i++) {
        value = i;
        EXPECT_TRUE(dearxan_vm_write_memory(&clone, i * 64, &value, sizeof(value)));
    }
    EXPECT_TRUE(!dearxan_vm_read_memory(&vm, count * 64, &value, sizeof(value)));
    for (uint64_t i = 1; i < count; i++) {
        value = 0;
        EXPECT_TRUE(dearxan_vm_read_memory(&clone, i * 64, &value, sizeof(value)));
        EXPECT_EQ(value, i + 1);
    }
    dearxan_vm_uninit(&clone);
    dearxan_vm_uninit(&vm);

    /* A clone of a VM that never touched memory has no table at all. */
    dearxan_vm_init(&vm, NULL, 0, 0x10000);
    EXPECT_TRUE(dearxan_vm_clone(&clone, &vm));
    EXPECT_TRUE(!dearxan_vm_read_memory(&clone, 0, &value, sizeof(value)));
    value = 7;
    EXPECT_TRUE(dearxan_vm_write_memory(&clone, 0, &value, sizeof(value)));
    value = 0;
    EXPECT_TRUE(dearxan_vm_read_memory(&clone, 0, &value, sizeof(value)));
    EXPECT_EQ(value, 7);
    dearxan_vm_uninit(&clone);
    dearxan_vm_uninit(&vm);
    return 0;
}

static int test_xchg_aliasing(void) {
    static const unsigned char code[] = { 0x48, 0x87, 0x00 };
    dearxan_vm_t vm;
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    dearxan_vm_t forked;
    bool has_fork;
    uint64_t old_address = 0x20000;
    uint64_t new_address = 0x30000;
    uint64_t value;

    dearxan_vm_init(&vm, NULL, 0, 0x10000);
    dearxan_vm_write_register(&vm, ZYDIS_REGISTER_RAX, true, old_address);
    EXPECT_TRUE(dearxan_vm_write_memory(&vm, old_address, &new_address,
                                        sizeof(new_address)));
    value = UINT64_C(0xaaaaaaaaaaaaaaaa);
    EXPECT_TRUE(dearxan_vm_write_memory(&vm, new_address, &value,
                                        sizeof(value)));
    EXPECT_TRUE(decode(code, sizeof(code), &instruction, operands));
    EXPECT_TRUE(dearxan_vm_step(&vm, &instruction, operands, &forked,
                                &has_fork));
    EXPECT_TRUE(!has_fork);
    EXPECT_TRUE(dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RAX, &value));
    EXPECT_EQ(value, new_address);
    EXPECT_TRUE(dearxan_vm_read_memory(&vm, old_address, &value, sizeof(value)));
    EXPECT_EQ(value, old_address);
    EXPECT_TRUE(dearxan_vm_read_memory(&vm, new_address, &value, sizeof(value)));
    EXPECT_EQ(value, UINT64_C(0xaaaaaaaaaaaaaaaa));
    dearxan_vm_uninit(&vm);
    return 0;
}

static int step_one(dearxan_vm_t *vm, const unsigned char *code, size_t size,
                    dearxan_vm_t *forked, bool *has_fork) {
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    EXPECT_TRUE(decode(code, size, &instruction, operands));
    EXPECT_TRUE(dearxan_vm_step(vm, &instruction, operands, forked, has_fork));
    return 0;
}

/* PUSHFQ must invalidate the slot it writes at the *new* RSP, not the pre-push
 * RSP. A stale value just below the stack must not survive, while the value at
 * the old RSP is untouched. POPFQ then only advances RSP. */
static int test_pushfq_popfq_stale_slot(void) {
    static const unsigned char pushfq[] = { 0x9c };
    static const unsigned char popfq[] = { 0x9d };
    dearxan_vm_t vm;
    dearxan_vm_t forked;
    bool has_fork = true;
    uint64_t stale = UINT64_C(0xAAAAAAAAAAAAAAAA);
    uint64_t below = UINT64_C(0xBBBBBBBBBBBBBBBB);
    uint64_t value;

    dearxan_vm_init(&vm, NULL, 0x2000, 0x10000);
    EXPECT_TRUE(dearxan_vm_write_memory(&vm, 0xFFF8, &stale, sizeof(stale)));
    EXPECT_TRUE(dearxan_vm_write_memory(&vm, 0x10000, &below, sizeof(below)));
    if (step_one(&vm, pushfq, sizeof(pushfq), &forked, &has_fork) != 0) return 1;
    EXPECT_TRUE(!has_fork);
    EXPECT_TRUE(dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RSP, &value));
    EXPECT_EQ(value, 0xFFF8);
    EXPECT_TRUE(!dearxan_vm_read_memory(&vm, 0xFFF8, &value, sizeof(value)));
    EXPECT_TRUE(dearxan_vm_read_memory(&vm, 0x10000, &value, sizeof(value)));
    EXPECT_EQ(value, below);

    if (step_one(&vm, popfq, sizeof(popfq), &forked, &has_fork) != 0) return 1;
    EXPECT_TRUE(!has_fork);
    EXPECT_TRUE(dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RSP, &value));
    EXPECT_EQ(value, 0x10000);
    EXPECT_TRUE(!dearxan_vm_read_memory(&vm, 0xFFF8, &value, sizeof(value)));
    dearxan_vm_uninit(&vm);
    return 0;
}

/* Interrupt and undefined instructions end the current execution path. */
static int test_trap_ends_path(void) {
    static const unsigned char int3[] = { 0xcc };
    static const unsigned char ud2[] = { 0x0f, 0x0b };
    dearxan_vm_t vm;
    dearxan_vm_t forked;
    bool has_fork = true;

    dearxan_vm_init(&vm, NULL, 0x3000, 0x10000);
    if (step_one(&vm, int3, sizeof(int3), &forked, &has_fork) != 0) return 1;
    EXPECT_TRUE(!has_fork);
    EXPECT_TRUE(!vm.rip_known);
    dearxan_vm_uninit(&vm);

    dearxan_vm_init(&vm, NULL, 0x3000, 0x10000);
    if (step_one(&vm, ud2, sizeof(ud2), &forked, &has_fork) != 0) return 1;
    EXPECT_TRUE(!has_fork);
    EXPECT_TRUE(!vm.rip_known);
    dearxan_vm_uninit(&vm);
    return 0;
}

/* The VM steps a SYSCALL without forking or branching, advancing to the next
 * instruction and clobbering the volatile registers the instruction writes.
 * This fall-through behaviour is what lets the analyzer treat it as a call. */
static int test_syscall(void) {
    static const unsigned char syscall_bytes[] = { 0x0f, 0x05 };
    dearxan_vm_t vm;
    dearxan_vm_t forked;
    bool has_fork = true;
    uint64_t value;

    dearxan_vm_init(&vm, NULL, 0x4000, 0x10000);
    dearxan_vm_write_register(&vm, ZYDIS_REGISTER_RCX, true, 0x1234);
    if (step_one(&vm, syscall_bytes, sizeof(syscall_bytes), &forked, &has_fork) != 0) return 1;
    EXPECT_TRUE(!has_fork);
    EXPECT_TRUE(vm.rip_known);
    EXPECT_EQ(vm.rip, 0x4002);
    EXPECT_TRUE(dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RSP, &value));
    EXPECT_EQ(value, 0x10000);
    EXPECT_TRUE(!dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RCX, &value));
    dearxan_vm_uninit(&vm);
    return 0;
}

static int test_push_pop_call_ret(void) {
    static const unsigned char push_rax[] = { 0x50 };
    static const unsigned char pop_rcx[] = { 0x59 };
    static const unsigned char call_rel[] = { 0xe8, 0x05, 0x00, 0x00, 0x00 };
    static const unsigned char ret_near[] = { 0xc3 };
    static const unsigned char ret_imm16[] = { 0xc2, 0x10, 0x00 };
    dearxan_vm_t vm;
    dearxan_vm_t forked;
    bool has_fork = true;
    uint64_t value;

    /* PUSH writes the register below the new RSP; POP reads it back. */
    dearxan_vm_init(&vm, NULL, 0x5000, 0x10000);
    dearxan_vm_write_register(&vm, ZYDIS_REGISTER_RAX, true, UINT64_C(0xCAFEF00D));
    if (step_one(&vm, push_rax, sizeof(push_rax), &forked, &has_fork) != 0) return 1;
    EXPECT_TRUE(dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RSP, &value));
    EXPECT_EQ(value, 0xFFF8);
    EXPECT_TRUE(dearxan_vm_read_memory(&vm, 0xFFF8, &value, sizeof(value)));
    EXPECT_EQ(value, UINT64_C(0xCAFEF00D));
    if (step_one(&vm, pop_rcx, sizeof(pop_rcx), &forked, &has_fork) != 0) return 1;
    EXPECT_TRUE(dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RCX, &value));
    EXPECT_EQ(value, UINT64_C(0xCAFEF00D));
    EXPECT_TRUE(dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RSP, &value));
    EXPECT_EQ(value, 0x10000);
    dearxan_vm_uninit(&vm);

    /* CALL pushes the return address and transfers control to the target. */
    dearxan_vm_init(&vm, NULL, 0x6000, 0x10000);
    if (step_one(&vm, call_rel, sizeof(call_rel), &forked, &has_fork) != 0) return 1;
    EXPECT_TRUE(!has_fork);
    EXPECT_EQ(vm.rip, 0x600A);
    EXPECT_TRUE(dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RSP, &value));
    EXPECT_EQ(value, 0xFFF8);
    EXPECT_TRUE(dearxan_vm_read_memory(&vm, 0xFFF8, &value, sizeof(value)));
    EXPECT_EQ(value, 0x6005);
    /* RET pops it. */
    if (step_one(&vm, ret_near, sizeof(ret_near), &forked, &has_fork) != 0) return 1;
    EXPECT_EQ(vm.rip, 0x6005);
    EXPECT_TRUE(dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RSP, &value));
    EXPECT_EQ(value, 0x10000);
    dearxan_vm_uninit(&vm);

    /* RET imm16 additionally releases imm16 bytes of arguments. */
    dearxan_vm_init(&vm, NULL, 0x7000, 0x10000);
    value = 0x7777;
    EXPECT_TRUE(dearxan_vm_write_memory(&vm, 0x10000, &value, sizeof(value)));
    if (step_one(&vm, ret_imm16, sizeof(ret_imm16), &forked, &has_fork) != 0) return 1;
    EXPECT_EQ(vm.rip, 0x7777);
    EXPECT_TRUE(dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RSP, &value));
    EXPECT_EQ(value, 0x10000 + 8 + 0x10);
    dearxan_vm_uninit(&vm);
    return 0;
}

static int test_movsx(void) {
    static const unsigned char movsx_eax_cl[] = { 0x0f, 0xbe, 0xc1 };
    static const unsigned char movsxd_rax_ecx[] = { 0x48, 0x63, 0xc1 };
    dearxan_vm_t vm;
    dearxan_vm_t forked;
    bool has_fork = true;
    uint64_t value;

    /* movsx eax, cl sign-extends an 8-bit 0x80 to 0xFFFFFF80. */
    dearxan_vm_init(&vm, NULL, 0x8000, 0x10000);
    dearxan_vm_write_register(&vm, ZYDIS_REGISTER_RCX, true, 0x80);
    if (step_one(&vm, movsx_eax_cl, sizeof(movsx_eax_cl), &forked, &has_fork) != 0) return 1;
    EXPECT_TRUE(dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RAX, &value));
    EXPECT_EQ(value, 0xFFFFFF80u);
    dearxan_vm_uninit(&vm);

    /* movsxd rax, ecx sign-extends a 32-bit value into all 64 bits. */
    dearxan_vm_init(&vm, NULL, 0x8000, 0x10000);
    dearxan_vm_write_register(&vm, ZYDIS_REGISTER_RCX, true, 0x80000000u);
    if (step_one(&vm, movsxd_rax_ecx, sizeof(movsxd_rax_ecx), &forked, &has_fork) != 0) return 1;
    EXPECT_TRUE(dearxan_vm_read_register(&vm, ZYDIS_REGISTER_RAX, &value));
    EXPECT_EQ(value, UINT64_C(0xFFFFFFFF80000000));
    dearxan_vm_uninit(&vm);
    return 0;
}

/* A conditional branch forks: the fork takes the branch target while the
 * original continues to the fall-through instruction. */
static int test_conditional_branch_fork(void) {
    static const unsigned char jz_rel8[] = { 0x74, 0x05 };
    dearxan_vm_t vm;
    dearxan_vm_t forked;
    bool has_fork = false;

    dearxan_vm_init(&vm, NULL, 0x9000, 0x10000);
    if (step_one(&vm, jz_rel8, sizeof(jz_rel8), &forked, &has_fork) != 0) return 1;
    EXPECT_TRUE(has_fork);
    EXPECT_TRUE(vm.rip_known);
    EXPECT_EQ(vm.rip, 0x9002);
    EXPECT_TRUE(forked.rip_known);
    EXPECT_EQ(forked.rip, 0x9007);
    dearxan_vm_uninit(&forked);
    dearxan_vm_uninit(&vm);
    return 0;
}

static int append_state(vm_step_record_t *records, size_t capacity, size_t *count,
                        ZydisMnemonic mnemonic, const dearxan_vm_t *vm) {
    vm_step_record_t *record;
    EXPECT_TRUE(*count < capacity);
    record = &records[(*count)++];
    record->mnemonic = mnemonic;
    record->ecx_known = dearxan_vm_read_register(vm, ZYDIS_REGISTER_ECX, &record->ecx);
    record->edx_known = dearxan_vm_read_register(vm, ZYDIS_REGISTER_EDX, &record->edx);
    return 0;
}

static int test_cmov_upstream_golden(void) {
    static const unsigned char code[] = {
        0xb9, 0x42, 0x00, 0x00, 0x00,
        0xba, 0x69, 0x00, 0x00, 0x00,
        0x0f, 0x44, 0xca,
        0x83, 0xc1, 0x01,
        0x0f, 0x44, 0xd1,
        0x83, 0xea, 0x04,
        0xc3
    };
    static const vm_step_record_t expected[] = {
        { ZYDIS_MNEMONIC_MOV, false, false, 0, 0 },
        { ZYDIS_MNEMONIC_MOV, true, false, 0x42, 0 },
        { ZYDIS_MNEMONIC_CMOVZ, true, true, 0x42, 0x69 },
        { ZYDIS_MNEMONIC_ADD, true, true, 0x69, 0x69 },
        { ZYDIS_MNEMONIC_CMOVZ, true, true, 0x6a, 0x69 },
        { ZYDIS_MNEMONIC_SUB, true, true, 0x6a, 0x6a },
        { ZYDIS_MNEMONIC_RET, true, true, 0x6a, 0x66 },
        { ZYDIS_MNEMONIC_SUB, true, true, 0x6a, 0x69 },
        { ZYDIS_MNEMONIC_RET, true, true, 0x6a, 0x65 },
        { ZYDIS_MNEMONIC_ADD, true, true, 0x42, 0x69 },
        { ZYDIS_MNEMONIC_CMOVZ, true, true, 0x43, 0x69 },
        { ZYDIS_MNEMONIC_SUB, true, true, 0x43, 0x43 },
        { ZYDIS_MNEMONIC_RET, true, true, 0x43, 0x3f },
        { ZYDIS_MNEMONIC_SUB, true, true, 0x43, 0x69 },
        { ZYDIS_MNEMONIC_RET, true, true, 0x43, 0x65 }
    };
    dearxan_image_t image = { code, sizeof(code), 0, 0, 0 };
    dearxan_vm_t work[8];
    size_t work_count = 1;
    vm_step_record_t actual[sizeof(expected) / sizeof(expected[0])];
    size_t actual_count = 0;

    dearxan_vm_init(&work[0], &image, 0, 0x10000);
    while (work_count != 0) {
        dearxan_vm_t *vm = &work[work_count - 1];
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        dearxan_vm_t forked;
        bool has_fork;

        if (!vm->rip_known || vm->rip >= sizeof(code)) {
            dearxan_vm_uninit(vm);
            work_count--;
            continue;
        }
        EXPECT_TRUE(decode(code + vm->rip, sizeof(code) - (size_t)vm->rip,
                           &instruction, operands));
        if (append_state(actual, sizeof(actual) / sizeof(actual[0]),
                         &actual_count, instruction.mnemonic, vm) != 0) return 1;
        EXPECT_TRUE(dearxan_vm_step(vm, &instruction, operands, &forked, &has_fork));
        if (has_fork) {
            EXPECT_TRUE(work_count < sizeof(work) / sizeof(work[0]));
            work[work_count++] = forked;
        }
    }
    EXPECT_EQ(actual_count, sizeof(expected) / sizeof(expected[0]));
    for (size_t i = 0; i < actual_count; i++) {
        EXPECT_EQ(actual[i].mnemonic, expected[i].mnemonic);
        EXPECT_EQ(actual[i].ecx_known, expected[i].ecx_known);
        EXPECT_EQ(actual[i].edx_known, expected[i].edx_known);
        if (actual[i].ecx_known) EXPECT_EQ(actual[i].ecx, expected[i].ecx);
        if (actual[i].edx_known) EXPECT_EQ(actual[i].edx, expected[i].edx);
    }
    return 0;
}

int main(void) {
    if (test_registers() != 0 || test_memory() != 0 ||
        test_memory_table_growth() != 0 ||
        test_xchg_aliasing() != 0 ||
        test_pushfq_popfq_stale_slot() != 0 ||
        test_trap_ends_path() != 0 ||
        test_syscall() != 0 ||
        test_push_pop_call_ret() != 0 ||
        test_movsx() != 0 ||
        test_conditional_branch_fork() != 0 ||
        test_cmov_upstream_golden() != 0) return 1;
    printf("smoke_dearxan_vm: all tests passed\n");
    return 0;
}

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "test_common.h"
#include "process/util.h"

static int test_mask_selection(void) {
    uint64_t masks[256] = { 0 };
    uint64_t selected = 0;

    masks[0] = UINT64_C(0x0f);
    masks[1] = UINT64_C(0xf0);
    masks[3] = UINT64_C(0x0f00);
    EXPECT_TRUE(select_process_cpu_affinity_mask(1, masks, UINT64_C(0xffff),
                                                 &selected));
    EXPECT_EQ(selected, UINT64_C(0x0ffe));
    EXPECT_TRUE(select_process_cpu_affinity_mask(2, masks, UINT64_C(0xffff),
                                                 &selected));
    EXPECT_EQ(selected, UINT64_C(0x0f));
    EXPECT_TRUE(select_process_cpu_affinity_mask(3, masks, UINT64_C(0xffff),
                                                 &selected));
    EXPECT_EQ(selected, UINT64_C(0x0f00));
    EXPECT_TRUE(select_process_cpu_affinity_mask(4, masks, UINT64_C(0xffff),
                                                 &selected));
    EXPECT_EQ(selected, UINT64_C(0x0e00));
    EXPECT_TRUE(select_process_cpu_affinity_mask(1, masks, UINT64_C(0x000f),
                                                 &selected));
    EXPECT_EQ(selected, UINT64_C(0x000e));
    EXPECT_TRUE(!select_process_cpu_affinity_mask(1, masks, 0, &selected));
    EXPECT_TRUE(!select_process_cpu_affinity_mask(0, masks, UINT64_MAX,
                                                  &selected));
    EXPECT_TRUE(!select_process_cpu_affinity_mask(5, masks, UINT64_MAX,
                                                  &selected));
    memset(masks, 0, sizeof(masks));
    masks[3] = UINT64_C(1);
    EXPECT_TRUE(!select_process_cpu_affinity_mask(4, masks, UINT64_MAX,
                                                  &selected));
    return 0;
}

static int test_win32_application_path(void) {
    DWORD_PTR original_process_mask;
    DWORD_PTR system_mask;
    uint64_t applied_mask = 0;
    uint32_t error_code = 0;

    EXPECT_TRUE(GetProcessAffinityMask(GetCurrentProcess(),
                                       &original_process_mask, &system_mask));
    if (original_process_mask != 0 && (system_mask & ~((DWORD_PTR)1)) != 0) {
        bool applied = set_process_cpu_affinity_strategy(
            1, &applied_mask, &error_code);
        if (applied) {
            DWORD_PTR current_process_mask;
            DWORD_PTR current_system_mask;
            EXPECT_TRUE(GetProcessAffinityMask(GetCurrentProcess(),
                                               &current_process_mask,
                                               &current_system_mask));
            EXPECT_EQ(applied_mask, (uint64_t)current_process_mask);
            EXPECT_TRUE(SetProcessAffinityMask(GetCurrentProcess(),
                                               original_process_mask));
        } else {
            printf("smoke_cpu_affinity: application path skipped error=%lu\n",
                   (unsigned long)error_code);
        }
    }
    return 0;
}

int main(void) {
    if (test_mask_selection() != 0 || test_win32_application_path() != 0) return 1;
    printf("smoke_cpu_affinity: all tests passed\n");
    return 0;
}

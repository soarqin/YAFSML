#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "test_common.h"
#include "process/cpu_sets.h"
#include "process/util.h"

static int expect_ids(int strategy,
                      const process_cpu_set_info_t *sets,
                      size_t set_count,
                      const process_cpu_set_id_t *expected,
                      size_t expected_count) {
    process_cpu_set_id_t ids[32] = {0};
    const size_t count = select_process_cpu_set_ids(strategy, sets, set_count,
                                                    ids, 32);
    EXPECT_EQ(count, expected_count);
    for (size_t i = 0; i < expected_count; i++) {
        EXPECT_EQ(ids[i], expected[i]);
    }
    return 0;
}

static int test_core_ultra_layout(void) {
    const process_cpu_set_info_t sets[] = {
        {256, 0, 0, 1, false, false},
        {257, 0, 1, 1, false, false},
        {258, 0, 2, 0, false, false},
        {259, 0, 3, 0, false, false},
        {260, 0, 4, 0, false, false},
        {261, 0, 5, 0, false, false},
        {262, 0, 6, 1, false, false},
        {263, 0, 7, 1, false, false},
        {264, 0, 8, 1, false, false},
        {265, 0, 9, 1, false, false},
        {266, 0, 10, 0, false, false},
        {267, 0, 11, 0, false, false},
        {268, 0, 12, 0, false, false},
        {269, 0, 13, 0, false, false},
        {270, 0, 14, 0, false, false},
        {271, 0, 15, 0, false, false},
        {272, 0, 16, 0, false, false},
        {273, 0, 17, 0, false, false},
        {274, 0, 18, 1, false, false},
        {275, 0, 19, 1, false, false},
    };
    const process_cpu_set_id_t strategy1[] = {
        257, 258, 259, 260, 261, 262, 263, 264, 265, 266,
        267, 268, 269, 270, 271, 272, 273, 274, 275
    };
    const process_cpu_set_id_t strategy2[] = {
        258, 259, 260, 261, 266, 267, 268, 269, 270, 271, 272, 273
    };
    const process_cpu_set_id_t strategy3[] = {
        256, 257, 262, 263, 264, 265, 274, 275
    };
    const process_cpu_set_id_t strategy4[] = {
        257, 262, 263, 264, 265, 274, 275
    };
    EXPECT_EQ(expect_ids(1, sets, 20, strategy1, 19), 0);
    EXPECT_EQ(expect_ids(2, sets, 20, strategy2, 12), 0);
    EXPECT_EQ(expect_ids(3, sets, 20, strategy3, 8), 0);
    EXPECT_EQ(expect_ids(4, sets, 20, strategy4, 7), 0);
    return 0;
}

static int test_multiple_processor_groups(void) {
    const process_cpu_set_info_t sets[] = {
        {100, 0, 0, 0, false, false},
        {101, 0, 1, 1, false, false},
        {200, 1, 0, 0, false, false},
        {201, 1, 1, 1, false, false},
        {300, 2, 0, 0, false, false},
        {301, 2, 1, 1, false, false},
    };
    const process_cpu_set_id_t strategy1[] = {101, 200, 201, 300, 301};
    const process_cpu_set_id_t strategy2[] = {100, 200, 300};
    const process_cpu_set_id_t strategy3[] = {101, 201, 301};
    const process_cpu_set_id_t strategy4[] = {201, 301};
    EXPECT_EQ(expect_ids(1, sets, 6, strategy1, 5), 0);
    EXPECT_EQ(expect_ids(2, sets, 6, strategy2, 3), 0);
    EXPECT_EQ(expect_ids(3, sets, 6, strategy3, 3), 0);
    EXPECT_EQ(expect_ids(4, sets, 6, strategy4, 2), 0);
    return 0;
}

static int test_first_cpu_set_selection_is_topology_based(void) {
    const process_cpu_set_info_t sets[] = {
        {275, 0, 19, 1, false, false},
        {258, 0, 2, 0, false, false},
        {256, 0, 0, 1, false, false},
        {261, 0, 5, 0, false, false},
        {257, 0, 1, 1, false, false},
    };
    const process_cpu_set_id_t strategy1[] = {275, 258, 261, 257};
    const process_cpu_set_id_t strategy4[] = {275, 257};
    EXPECT_EQ(expect_ids(1, sets, 5, strategy1, 4), 0);
    EXPECT_EQ(expect_ids(4, sets, 5, strategy4, 2), 0);
    return 0;
}

static int test_allocated_cpu_sets(void) {
    const process_cpu_set_info_t sets[] = {
        {10, 0, 0, 0, false, false},
        {11, 0, 1, 1, true, false},
        {12, 0, 2, 1, true, true},
    };
    const process_cpu_set_id_t strategy3[] = {12};
    EXPECT_EQ(expect_ids(3, sets, 3, strategy3, 1), 0);
    return 0;
}

static int test_highest_efficiency_class_is_selected(void) {
    const process_cpu_set_info_t sets[] = {
        {100, 0, 0, 0, false, false},
        {101, 0, 1, 1, false, false},
        {200, 0, 2, 2, false, false},
        {201, 0, 3, 2, false, false},
    };
    const process_cpu_set_id_t strategy2[] = {100};
    const process_cpu_set_id_t strategy3[] = {200, 201};
    const process_cpu_set_id_t strategy4[] = {201};
    EXPECT_EQ(expect_ids(2, sets, 4, strategy2, 1), 0);
    EXPECT_EQ(expect_ids(3, sets, 4, strategy3, 2), 0);
    EXPECT_EQ(expect_ids(4, sets, 4, strategy4, 1), 0);
    return 0;
}

static int test_strategy_one_excludes_first_logical_cpu_set(void) {
    const process_cpu_set_info_t sets[] = {
        {100, 0, 0, 0, false, false},
        {101, 0, 1, 1, false, false},
        {200, 1, 0, 1, false, false},
    };
    const process_cpu_set_id_t expected[] = {101, 200};
    EXPECT_EQ(expect_ids(1, sets, 3, expected, 2), 0);
    return 0;
}

static bool query_default_cpu_sets(process_cpu_set_id_t **ids_out,
                                    DWORD *count_out,
                                    DWORD *error_out) {
    DWORD required = 0;
    SetLastError(ERROR_SUCCESS);
    if (GetProcessDefaultCpuSets(GetCurrentProcess(), NULL, 0, &required)) {
        *ids_out = NULL;
        *count_out = 0;
        *error_out = ERROR_SUCCESS;
        return true;
    }
    DWORD error = GetLastError();
    if (error != ERROR_INSUFFICIENT_BUFFER || required == 0) {
        *error_out = error == ERROR_SUCCESS ? ERROR_NOT_SUPPORTED : error;
        return false;
    }

    process_cpu_set_id_t *ids = (process_cpu_set_id_t *)malloc(
        required * sizeof(process_cpu_set_id_t));
    if (ids == NULL) {
        *error_out = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    if (!GetProcessDefaultCpuSets(GetCurrentProcess(), (PULONG)ids, required,
                                  &required)) {
        error = GetLastError();
        free(ids);
        *error_out = error;
        return false;
    }
    *ids_out = ids;
    *count_out = required;
    *error_out = ERROR_SUCCESS;
    return true;
}

static int test_win32_application_path(void) {
    process_cpu_set_id_t *original_ids = NULL;
    DWORD original_count = 0;
    DWORD error_code = ERROR_SUCCESS;
    if (!query_default_cpu_sets(&original_ids, &original_count, &error_code)) {
        printf("smoke_cpu_affinity: application path skipped error=%lu\n",
               (unsigned long)error_code);
        return 0;
    }

    uint32_t applied_count = 0;
    bool applied = set_process_cpu_affinity_strategy(1, &applied_count,
                                                     &error_code);
    if (!applied) {
        printf("smoke_cpu_affinity: application path skipped error=%lu\n",
               (unsigned long)error_code);
        free(original_ids);
        return 0;
    }
    EXPECT_TRUE(applied_count > 0);

    process_cpu_set_id_t *current_ids = NULL;
    DWORD current_count = 0;
    EXPECT_TRUE(query_default_cpu_sets(&current_ids, &current_count,
                                       &error_code));
    EXPECT_EQ(current_count, applied_count);
    free(current_ids);

    EXPECT_TRUE(SetProcessDefaultCpuSets(GetCurrentProcess(), original_ids,
                                         original_count));
    free(original_ids);
    return 0;
}

int main(void) {
    if (test_core_ultra_layout() != 0 ||
        test_multiple_processor_groups() != 0 ||
        test_first_cpu_set_selection_is_topology_based() != 0 ||
        test_allocated_cpu_sets() != 0 ||
        test_highest_efficiency_class_is_selected() != 0 ||
        test_strategy_one_excludes_first_logical_cpu_set() != 0 ||
        test_win32_application_path() != 0) {
        return 1;
    }
    printf("smoke_cpu_affinity: all tests passed\n");
    return 0;
}

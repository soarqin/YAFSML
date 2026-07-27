#include <stddef.h>
#include <stdio.h>

#include "test_common.h"
#include "dearxan/dearxan.h"

int main(void) {
    EXPECT_EQ(DearxanInvalid, 0);
    EXPECT_EQ(DearxanSuccess, 1);
    EXPECT_EQ(DearxanError, 2);
    EXPECT_EQ(DearxanPanic, 3);
    EXPECT_EQ(offsetof(DearxanResult, result_size), 0);
    EXPECT_EQ(offsetof(DearxanResult, status), 8);
    EXPECT_EQ(offsetof(DearxanResult, error_msg), 16);
    EXPECT_EQ(offsetof(DearxanResult, error_msg_size), 24);
    EXPECT_EQ(offsetof(DearxanResult, is_arxan_detected), 32);
    EXPECT_EQ(offsetof(DearxanResult, is_executing_entrypoint), 33);
    EXPECT_EQ(DEARXAN_RESULT_SIZE, 34);
    printf("smoke_dearxan_abi: all tests passed\n");
    return 0;
}

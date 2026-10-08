#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>

#include <ini.h>

#include "test_common.h"

static bool fail_allocation;
static bool fail_growth;
static unsigned active_allocations;
static unsigned handler_calls;

void *ini_malloc(size_t size) {
    void *allocation;
    if (fail_allocation) return NULL;
    allocation = malloc(size);
    if (allocation != NULL) active_allocations++;
    return allocation;
}

void *ini_realloc(void *allocation, size_t size) {
    if (fail_growth) return NULL;
    return realloc(allocation, size);
}

void ini_free(void *allocation) {
    if (allocation != NULL) active_allocations--;
    free(allocation);
}

static int count_value(void *user, const char *section, const char *name,
                        const char *value) {
    (void)user;
    (void)section;
    (void)name;
    (void)value;
    handler_calls++;
    return 1;
}

int main(void) {
    char contents[1024];
    memset(contents, 'x', sizeof(contents) - 1);
    memcpy(contents, "path=", strlen("path="));
    contents[sizeof(contents) - 1] = '\0';
    fail_allocation = true;
    EXPECT_EQ(ini_parse_string("game=nightreign\n", count_value, NULL), -2);
    EXPECT_EQ(active_allocations, 0);
    EXPECT_EQ(handler_calls, 0);
    fail_allocation = false;
    fail_growth = true;
    EXPECT_EQ(ini_parse_string(contents, count_value, NULL), -2);
    EXPECT_EQ(active_allocations, 0);
    EXPECT_EQ(handler_calls, 0);
    fail_growth = false;
    EXPECT_EQ(ini_parse_string(contents, count_value, NULL), 0);
    EXPECT_EQ(active_allocations, 0);
    EXPECT_EQ(handler_calls, 1);
    printf("smoke_ini_allocation: all tests passed\n");
    return 0;
}

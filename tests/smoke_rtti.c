#include <stdint.h>
#include <stdio.h>

#include "test_common.h"
#include "process/rtti.h"

static int expect_order(void *map, const char *class_name, void *const *expected, size_t count) {
    EXPECT_EQ(rtti_test_vtable_count(map, class_name), count);
    for (size_t i = 0; i < count; i++) {
        EXPECT_EQ(rtti_test_find_vtable_at(map, class_name, i), expected[i]);
    }
    EXPECT_NULL(rtti_test_find_vtable_at(map, class_name, count));
    return 0;
}

int main(void) {
    void *map = rtti_test_map_create();
    int vtables[5];

    EXPECT_NOT_NULL(map);

    /* Out-of-order offsets come back sorted; a vtable is only kept once. */
    rtti_test_insert(map, "CS::Sorted", &vtables[1], 8);
    rtti_test_insert(map, "CS::Sorted", &vtables[0], 0);
    rtti_test_insert(map, "CS::Sorted", &vtables[2], 16);
    rtti_test_insert(map, "CS::Sorted", &vtables[0], 0);
    rtti_test_insert(map, "CS::Sorted", &vtables[2], 4);
    {
        void *const expected[] = { &vtables[0], &vtables[1], &vtables[2] };
        EXPECT_EQ(expect_order(map, "CS::Sorted", expected, 3), 0);
    }

    /* A lower offset arriving after the list has grown takes over the head
       and keeps the old head behind it (regression: it was dropped). */
    rtti_test_insert(map, "CS::Late", &vtables[3], 16);
    rtti_test_insert(map, "CS::Late", &vtables[4], 24);
    rtti_test_insert(map, "CS::Late", &vtables[2], 8);
    rtti_test_insert(map, "CS::Late", &vtables[1], 0);
    {
        void *const expected[] = { &vtables[1], &vtables[2], &vtables[3], &vtables[4] };
        EXPECT_EQ(expect_order(map, "CS::Late", expected, 4), 0);
    }

    /* Equal offsets keep their insertion order. */
    rtti_test_insert(map, "CS::Equal", &vtables[0], 8);
    rtti_test_insert(map, "CS::Equal", &vtables[1], 8);
    rtti_test_insert(map, "CS::Equal", &vtables[2], 0);
    rtti_test_insert(map, "CS::Equal", &vtables[3], 8);
    {
        void *const expected[] = { &vtables[2], &vtables[0], &vtables[1], &vtables[3] };
        EXPECT_EQ(expect_order(map, "CS::Equal", expected, 4), 0);
    }

    /* Classes are independent, and invalid inserts are ignored. */
    {
        void *const expected[] = { &vtables[0], &vtables[1], &vtables[2] };
        EXPECT_EQ(expect_order(map, "CS::Sorted", expected, 3), 0);
    }
    rtti_test_insert(map, "CS::Null", NULL, 0);
    rtti_test_insert(map, NULL, &vtables[0], 0);
    EXPECT_EQ(rtti_test_vtable_count(map, "CS::Null"), 0);
    EXPECT_EQ(rtti_test_vtable_count(map, "CS::Missing"), 0);
    EXPECT_NULL(rtti_test_find_vtable_at(map, "CS::Missing", 0));
    EXPECT_EQ(rtti_test_vtable_count(map, NULL), 0);

    rtti_test_map_destroy(map);
    printf("smoke_rtti: all tests passed\n");
    return 0;
}

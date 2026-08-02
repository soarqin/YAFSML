#include <stdbool.h>
#include <stdio.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "test_common.h"
#include "dearxan/dearxan.h"

typedef struct callback_context {
    HANDLE complete;
    LONG count;
    LONG next_index;
    int order[4];
    bool detected[4];
    bool executing_entrypoint[4];
} callback_context_t;

typedef struct callback_item {
    callback_context_t *context;
    int id;
} callback_item_t;

static callback_item_t *reentrant_item;

static void record_callback(bool detected, bool executing_entrypoint, void *opaque) {
    callback_item_t *item = opaque;
    callback_context_t *context = item->context;
    LONG index = InterlockedIncrement(&context->next_index) - 1;

    context->order[index] = item->id;
    context->detected[index] = detected;
    context->executing_entrypoint[index] = executing_entrypoint;
    if (item->id == 0 && reentrant_item != NULL) {
        callback_item_t *next = reentrant_item;
        reentrant_item = NULL;
        dearxan_schedule_after_arxan(record_callback, next);
    }
    if (InterlockedIncrement(&context->count) == 4) SetEvent(context->complete);
}

int main(void) {
    callback_context_t context = { 0 };
    callback_item_t items[4];

    context.complete = CreateEventW(NULL, TRUE, FALSE, NULL);
    EXPECT_NOT_NULL(context.complete);
    reentrant_item = &items[3];
    items[3].context = &context;
    items[3].id = 3;
    for (int i = 0; i < 3; i++) {
        items[i].context = &context;
        items[i].id = i;
        dearxan_schedule_after_arxan(record_callback, &items[i]);
    }
    EXPECT_EQ(WaitForSingleObject(context.complete, 5000), WAIT_OBJECT_0);
    Sleep(50);
    EXPECT_EQ(context.count, 4);
    {
        int positions[4] = { -1, -1, -1, -1 };
        for (int i = 0; i < 4; i++) {
            EXPECT_TRUE(context.order[i] >= 0 && context.order[i] < 4);
            EXPECT_EQ(positions[context.order[i]], -1);
            positions[context.order[i]] = i;
            EXPECT_TRUE(!context.detected[i]);
            EXPECT_TRUE(!context.executing_entrypoint[i]);
        }
        EXPECT_TRUE(positions[0] < positions[1]);
        EXPECT_TRUE(positions[1] < positions[2]);
        EXPECT_TRUE(positions[0] < positions[3]);
    }
    CloseHandle(context.complete);
    printf("smoke_dearxan_scheduler: all tests passed\n");
    return 0;
}

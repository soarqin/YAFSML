#include <stdio.h>

#include "test_common.h"
#include "modloader/lifecycle.h"

static int callback_count;
static ml_lifecycle_phase_t callback_phases[6];
static int callback_tags[6];

static void on_phase(ml_lifecycle_phase_t phase, void *userp) {
    int *tag = userp;
    callback_tags[callback_count] = tag == NULL ? -1 : *tag;
    callback_phases[callback_count++] = phase;
    if (tag != NULL) (*tag)++;
}

int main(void) {
    int immediate_count = 0;
    int first_runtime = 10;
    int second_runtime = 20;

    ml_lifecycle_init();
    EXPECT_EQ(ml_lifecycle_current(), ML_LIFECYCLE_PHASE_UNKNOWN);
    EXPECT_TRUE(ml_lifecycle_on_phase(ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT, on_phase, &first_runtime));
    EXPECT_TRUE(ml_lifecycle_on_phase(ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT, on_phase, &second_runtime));
    EXPECT_TRUE(ml_lifecycle_on_phase(ML_LIFECYCLE_PHASE_AFTER_PROPERTIES_READY, on_phase, NULL));
    EXPECT_TRUE(ml_lifecycle_on_phase(ML_LIFECYCLE_PHASE_AFTER_GAME_DATA_READY, on_phase, NULL));
    EXPECT_TRUE(!ml_lifecycle_on_phase(ML_LIFECYCLE_PHASE_UNKNOWN, on_phase, NULL));
    EXPECT_TRUE(!ml_lifecycle_on_phase(ML_LIFECYCLE_PHASE_BEFORE_MAIN, NULL, NULL));

    EXPECT_TRUE(ml_lifecycle_advance(ML_LIFECYCLE_PHASE_PRE_ENTRY_SAFE));
    EXPECT_EQ(callback_count, 0);
    EXPECT_TRUE(ml_lifecycle_advance(ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT));
    EXPECT_EQ(callback_count, 2);
    EXPECT_EQ(callback_phases[0], ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT);
    EXPECT_EQ(callback_phases[1], ML_LIFECYCLE_PHASE_AFTER_RUNTIME_INIT);
    EXPECT_EQ(callback_tags[0], 10);
    EXPECT_EQ(callback_tags[1], 20);
    EXPECT_TRUE(ml_lifecycle_on_phase(ML_LIFECYCLE_PHASE_BEFORE_MAIN, on_phase, &immediate_count));
    EXPECT_EQ(immediate_count, 0);
    EXPECT_EQ(callback_count, 2);
    EXPECT_TRUE(ml_lifecycle_advance(ML_LIFECYCLE_PHASE_BEFORE_MAIN));
    EXPECT_EQ(immediate_count, 1);
    EXPECT_EQ(callback_count, 3);
    EXPECT_EQ(callback_phases[2], ML_LIFECYCLE_PHASE_BEFORE_MAIN);
    EXPECT_TRUE(!ml_lifecycle_on_phase(ML_LIFECYCLE_PHASE_BEFORE_MAIN, on_phase, &immediate_count));
    EXPECT_TRUE(ml_lifecycle_advance(ML_LIFECYCLE_PHASE_BEFORE_MAIN));
    EXPECT_TRUE(ml_lifecycle_advance(ML_LIFECYCLE_PHASE_AFTER_PROPERTIES_READY));
    EXPECT_EQ(callback_count, 4);
    EXPECT_EQ(callback_phases[3], ML_LIFECYCLE_PHASE_AFTER_PROPERTIES_READY);
    EXPECT_TRUE(ml_lifecycle_advance(ML_LIFECYCLE_PHASE_AFTER_GAME_DATA_READY));
    EXPECT_EQ(callback_count, 5);
    EXPECT_EQ(callback_phases[4], ML_LIFECYCLE_PHASE_AFTER_GAME_DATA_READY);
    EXPECT_EQ(ml_lifecycle_current(), ML_LIFECYCLE_PHASE_AFTER_GAME_DATA_READY);
    EXPECT_TRUE(ml_lifecycle_advance(ML_LIFECYCLE_PHASE_AFTER_GAME_DATA_READY));
    EXPECT_EQ(callback_count, 5);

    ml_lifecycle_uninit();
    printf("smoke_lifecycle: all tests passed\n");
    return 0;
}

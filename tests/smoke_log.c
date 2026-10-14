#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "test_common.h"
#include "log.h"

static int sink_calls;
static ml_log_level_t sink_level;
static wchar_t sink_component[32];
static wchar_t sink_message[4096];
static int evaluated;

static void test_sink(ml_log_level_t level, const wchar_t *component, const wchar_t *message) {
    sink_calls++;
    sink_level = level;
    wcsncpy_s(sink_component, 32, component == NULL ? L"" : component, _TRUNCATE);
    wcsncpy_s(sink_message, 4096, message, _TRUNCATE);
}

static int count_evaluation(void) {
    return ++evaluated;
}

int main(void) {
    ml_log_level_t level;
    wchar_t long_value[2048];

    EXPECT_TRUE(ml_log_level_parse("trace", &level));
    EXPECT_EQ(level, ML_LOG_LEVEL_TRACE);
    EXPECT_TRUE(ml_log_level_parse("DEBUG", &level));
    EXPECT_EQ(level, ML_LOG_LEVEL_DEBUG);
    EXPECT_TRUE(ml_log_level_parse("warning", &level) == false);
    EXPECT_TRUE(ml_log_level_parse(NULL, &level) == false);

    ml_log_set_level(ML_LOG_LEVEL_WARN);
    EXPECT_EQ(ml_log_get_level(), ML_LOG_LEVEL_WARN);
    ml_log_set_level(ML_LOG_LEVEL_OFF);
    EXPECT_EQ(ml_log_get_level(), ML_LOG_LEVEL_OFF);

    /* Filtered messages neither reach the sink nor evaluate their arguments. */
    ml_log_set_sink(test_sink);
    ml_log_set_level(ML_LOG_LEVEL_WARN);
    ML_LOG_INFO(L"test", L"value %d", count_evaluation());
    EXPECT_EQ(evaluated, 0);
    EXPECT_EQ(sink_calls, 0);

    ML_LOG_ERROR(L"test", L"value %d %ls", count_evaluation(), L"中文");
    EXPECT_EQ(evaluated, 1);
    EXPECT_EQ(sink_calls, 1);
    EXPECT_EQ(sink_level, ML_LOG_LEVEL_ERROR);
    EXPECT_STREQ_W(sink_component, L"test");
    EXPECT_STREQ_W(sink_message, L"value 1 中文");

    /* Messages longer than the stack buffer are formatted in full. */
    for (size_t i = 0; i + 1 < sizeof(long_value) / sizeof(long_value[0]); i++) long_value[i] = L'a' + (wchar_t)(i % 26);
    long_value[sizeof(long_value) / sizeof(long_value[0]) - 1] = L'\0';
    ml_log_write(ML_LOG_LEVEL_WARN, NULL, L"%ls!", long_value);
    EXPECT_EQ(sink_calls, 2);
    EXPECT_EQ(wcslen(sink_message), wcslen(long_value) + 1);
    EXPECT_EQ(sink_message[wcslen(long_value)], L'!');

    ml_log_set_sink(NULL);
    printf("smoke_log: all tests passed\n");
    return 0;
}

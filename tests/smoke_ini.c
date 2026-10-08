#include <stdio.h>
#include <stdlib.h>

#include <ini.h>

#include "test_common.h"

typedef struct {
    const char *value;
    unsigned calls;
} ini_test_state_t;

static int check_value(void *user, const char *section, const char *name,
                        const char *value) {
    ini_test_state_t *state = user;
    state->calls++;
    return strcmp(section, "mod") == 0 && strcmp(name, "path") == 0 &&
           strcmp(value, state->value) == 0;
}

static int test_line_length(size_t line_length, const char *ending) {
    static const char prefix[] = "[mod]\npath=";
    size_t value_length = line_length - strlen("path=") - strlen(ending);
    size_t contents_length = sizeof(prefix) - 1 + value_length + strlen(ending);
    char *contents = malloc(contents_length + 1);
    char *value = malloc(value_length + 1);
    ini_test_state_t state = { value, 0 };
    FILE *file;
    EXPECT_NOT_NULL(contents);
    EXPECT_NOT_NULL(value);
    memset(value, 'x', value_length);
    value[value_length] = '\0';
    memcpy(contents, prefix, sizeof(prefix) - 1);
    memcpy(contents + sizeof(prefix) - 1, value, value_length);
    memcpy(contents + sizeof(prefix) - 1 + value_length, ending, strlen(ending) + 1);
    EXPECT_EQ(ini_parse_string(contents, check_value, &state), 0);
    EXPECT_EQ(state.calls, 1);
    file = tmpfile();
    EXPECT_NOT_NULL(file);
    EXPECT_EQ(fwrite(contents, 1, contents_length, file), contents_length);
    rewind(file);
    state.calls = 0;
    EXPECT_EQ(ini_parse_file(file, check_value, &state), 0);
    EXPECT_EQ(state.calls, 1);
    EXPECT_EQ(fclose(file), 0);
    free(contents);
    free(value);
    return 0;
}

int main(void) {
    static const size_t lengths[] = { 198, 199, 200, 399, 400, 4096, 65536 };
    static const char *endings[] = { "", "\n", "\r\n" };
    ini_test_state_t state = { "x", 0 };
    FILE *file;
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++)
        for (size_t j = 0; j < sizeof(endings) / sizeof(endings[0]); j++)
            EXPECT_EQ(test_line_length(lengths[i], endings[j]), 0);
    EXPECT_EQ(ini_parse_string("; comment\n[broken\n", check_value, &state), 2);
    EXPECT_EQ(state.calls, 0);
    file = fopen("NUL", "w");
    EXPECT_NOT_NULL(file);
    /* Reading a write-only stream must report I/O failure rather than an empty INI. */
    EXPECT_EQ(ini_parse_file(file, check_value, &state), -1);
    EXPECT_EQ(state.calls, 0);
    EXPECT_EQ(fclose(file), 0);
    printf("smoke_ini: all tests passed\n");
    return 0;
}

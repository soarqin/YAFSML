#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>

#include "test_common.h"
#include "log.h"
#include "modloader/extdll.h"

static wchar_t fixture_path[MAX_PATH];
static const wchar_t *expected_path = L"extdll_fixture.dll";
static int initializer_errors;

wchar_t *config_full_path_alloc(const wchar_t *filename) {
    if (filename != NULL && lstrcmpW(filename, expected_path) == 0) {
        return ml_mem_strdup_w(fixture_path);
    }
    return ml_mem_strdup_w(L".");
}

static int expect_order(const char *first, const char *second, const char *third) {
    if (first != NULL && strcmp(extdlls_test_name_at(0), first) != 0) return 1;
    if (second != NULL && strcmp(extdlls_test_name_at(1), second) != 0) return 1;
    if (third != NULL && strcmp(extdlls_test_name_at(2), third) != 0) return 1;
    return 0;
}

static void count_initializer_errors(ml_log_level_t level, const wchar_t *component,
                                     const wchar_t *message) {
    (void)message;
    if (level == ML_LOG_LEVEL_ERROR && wcscmp(component, L"extdll") == 0) initializer_errors++;
}

static int expect_fixture_initialization(LONG compatibility, LONG custom, LONG failed, int errors) {
    static const char *const counters[] = {
        "extdll_fixture_init_count", "extdll_fixture_connector_count",
        "extdll_fixture_custom_init_count", "extdll_fixture_failed_init_count",
    };
    const LONG expected[] = { compatibility, compatibility, custom, failed };
    typedef LONG (*fixture_count_t)(void);
    initializer_errors = 0;
    /* Neither a successful call nor a failed or missing initializer is retried. */
    for (int attempt = 0; attempt < 2; attempt++) {
        HMODULE fixture;
        extdlls_test_load_at(0);
        fixture = GetModuleHandleW(L"extdll_fixture.dll");
        EXPECT_NOT_NULL(fixture);
        for (size_t i = 0; i < sizeof(counters) / sizeof(counters[0]); i++) {
            fixture_count_t count = (fixture_count_t)GetProcAddress(fixture, counters[i]);
            EXPECT_NOT_NULL(count);
            EXPECT_EQ(count(), expected[i]);
        }
        EXPECT_EQ(initializer_errors, errors);
    }
    extdlls_unload_all();
    EXPECT_NULL(GetModuleHandleW(L"extdll_fixture.dll"));
    return 0;
}

static int test_initializers(void) {
    static const struct { const char *spec; const wchar_t *path; } paths[] = {
        { "extdll_fixture.dll", L"extdll_fixture.dll" },
        { "C:\\mods\\extdll_fixture.dll", L"C:\\mods\\extdll_fixture.dll" },
        { "C:/mods/extdll_fixture.dll", L"C:/mods/extdll_fixture.dll" },
        { "\\\\?\\C:\\mods\\extdll_fixture.dll", L"\\\\?\\C:\\mods\\extdll_fixture.dll" },
        { "\\\\.\\C:\\mods\\extdll_fixture.dll", L"\\\\.\\C:\\mods\\extdll_fixture.dll" },
        { "//?/C:/mods/extdll_fixture.dll", L"//?/C:/mods/extdll_fixture.dll" },
        { "\\\\server\\share\\extdll_fixture.dll", L"\\\\server\\share\\extdll_fixture.dll" },
        { "\\\\?\\UNC\\server\\share\\extdll_fixture.dll", L"\\\\?\\UNC\\server\\share\\extdll_fixture.dll" },
        { "模组/扩展.dll", L"模组/扩展.dll" },
    };
    static const char *const suffixes[] = {
        "", ":extdll_fixture_init", ":extdll_fixture_init|delay,25|after,dependency",
    };
    char spec[512];
    ml_log_set_sink(count_initializer_errors);
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        expected_path = paths[i].path;
        for (size_t j = 0; j < sizeof(suffixes) / sizeof(suffixes[0]); j++) {
            snprintf(spec, sizeof(spec), "%s%s", paths[i].spec, suffixes[j]);
            extdlls_add_spec("fixture", spec);
            EXPECT_EQ(extdlls_count(), 1);
            if (j == 2) {
                EXPECT_EQ(extdlls_test_delay_at(0), 25);
                EXPECT_EQ(extdlls_test_after_count(0), 1);
                EXPECT_STREQ_A(extdlls_test_after_at(0, 0), "dependency");
            }
            EXPECT_EQ(expect_fixture_initialization(j == 0, j != 0, 0, 0), 0);
        }
    }
    expected_path = L"extdll_fixture.dll";
    /* These DLLs also export modengine_ext_init, which must stay uncalled. */
    extdlls_add_spec("failed", "extdll_fixture.dll:extdll_fixture_fail_init|early");
    EXPECT_TRUE(extdlls_test_is_early_at(0));
    EXPECT_EQ(expect_fixture_initialization(0, 0, 1, 1), 0);
    extdlls_add_spec("missing", "extdll_fixture.dll:extdll_fixture_missing_init|data_ready");
    EXPECT_TRUE(extdlls_test_is_data_ready_at(0));
    EXPECT_EQ(expect_fixture_initialization(0, 0, 0, 1), 0);

    extdlls_add_spec("empty", "extdll_fixture.dll:");
    extdlls_add_spec("empty_with_options", "extdll_fixture.dll:|early");
    extdlls_add_spec("empty_path", ":extdll_fixture_init");
    EXPECT_EQ(extdlls_count(), 0);
    EXPECT_NULL(GetModuleHandleW(L"extdll_fixture.dll"));
    extdlls_unload_all();
    ml_log_set_sink(NULL);
    return 0;
}

int main(void) {
    typedef LONG (*fixture_count_t)(void);
    HMODULE fixture;
    fixture_count_t init_count;
    fixture_count_t connector_count;

    EXPECT_TRUE(GetModuleFileNameW(NULL, fixture_path, MAX_PATH) != 0);
    PathRemoveFileSpecW(fixture_path);
    PathAppendW(fixture_path, L"extdll_fixture.dll");
    extdlls_add_spec("fixture", "extdll_fixture.dll|early");
    extdlls_test_load_at(0);
    fixture = GetModuleHandleW(L"extdll_fixture.dll");
    EXPECT_NOT_NULL(fixture);
    init_count = (fixture_count_t)GetProcAddress(fixture, "extdll_fixture_init_count");
    connector_count = (fixture_count_t)GetProcAddress(fixture, "extdll_fixture_connector_count");
    EXPECT_NOT_NULL(init_count);
    EXPECT_NOT_NULL(connector_count);
    EXPECT_EQ(init_count(), 1);
    EXPECT_EQ(connector_count(), 1);
    extdlls_test_load_at(0);
    EXPECT_EQ(init_count(), 1);
    extdlls_unload_all();
    EXPECT_NULL(GetModuleHandleW(L"extdll_fixture.dll"));

    EXPECT_EQ(test_initializers(), 0);

    extdlls_add_spec("plain", "plain.dll");
    extdlls_add_spec("early", "early.dll|early");
    extdlls_add_spec("delayed", "delayed.dll|delay,500|after,plain");
    extdlls_add_spec("dependent", "dependent.dll|after,delayed");
    EXPECT_EQ(extdlls_count(), 4);
    EXPECT_TRUE(!extdlls_test_is_early_at(0));
    EXPECT_TRUE(extdlls_test_is_early_at(1));
    EXPECT_EQ(extdlls_test_delay_at(2), 500);
    EXPECT_EQ(extdlls_test_after_count(2), 1);
    EXPECT_TRUE(strcmp(extdlls_test_after_at(2, 0), "plain") == 0);
    extdlls_prepare();
    EXPECT_TRUE(strcmp(extdlls_test_name_at(0), "plain") == 0);
    EXPECT_TRUE(strcmp(extdlls_test_name_at(1), "early") == 0);
    EXPECT_TRUE(strcmp(extdlls_test_name_at(2), "delayed") == 0);
    EXPECT_TRUE(strcmp(extdlls_test_name_at(3), "dependent") == 0);
    EXPECT_TRUE(!extdlls_test_is_deferred_at(0));
    EXPECT_TRUE(!extdlls_test_is_deferred_at(1));
    EXPECT_TRUE(!extdlls_test_is_deferred_at(2));
    EXPECT_TRUE(extdlls_test_is_deferred_at(3));
    extdlls_unload_all();

    extdlls_add_spec("delayed", "delayed.dll|delay,5000");
    extdlls_add_spec("immediate", "immediate.dll");
    extdlls_add_spec("dependent", "dependent.dll|after,delayed");
    extdlls_prepare();
    EXPECT_TRUE(!extdlls_test_is_deferred_at(0));
    EXPECT_TRUE(!extdlls_test_is_deferred_at(1));
    EXPECT_TRUE(extdlls_test_is_deferred_at(2));
    EXPECT_TRUE(extdlls_test_has_delayed());
    extdlls_unload_all();

    extdlls_add_spec("plain", "plain.dll");
    extdlls_add_spec("delayed", "delayed.dll|delay,25|after,plain");
    EXPECT_EQ(extdlls_test_delay_at(1), 25);
    extdlls_unload_all();

    extdlls_add_spec("negative", "negative.dll|delay,-1");
    extdlls_add_spec("overflow", "overflow.dll|delay,4294967296");
    EXPECT_EQ(extdlls_test_delay_at(0), 0);
    EXPECT_EQ(extdlls_test_delay_at(1), 0);
    extdlls_unload_all();

    extdlls_add_spec("early_then_delay", "early.dll|early|delay,25");
    extdlls_add_spec("delay_then_early", "delay.dll|delay,25|early");
    EXPECT_TRUE(extdlls_test_is_early_at(0));
    EXPECT_EQ(extdlls_test_delay_at(0), 0);
    EXPECT_TRUE(!extdlls_test_is_early_at(1));
    EXPECT_EQ(extdlls_test_delay_at(1), 25);
    extdlls_unload_all();

    extdlls_add_spec("early", "early.dll|early|after,dependency");
    extdlls_add_spec("dependency", "dependency.dll");
    extdlls_prepare();
    EXPECT_TRUE(extdlls_test_is_early_at(0));
    EXPECT_TRUE(extdlls_test_is_early_at(1));
    extdlls_unload_all();

    /* data_ready is a stage of its own and cannot be combined with the others. */
    extdlls_add_spec("data_ready", "data_ready.dll|data_ready");
    extdlls_add_spec("data_ready_then_early", "both.dll|data_ready|early");
    extdlls_add_spec("early_then_data_ready", "both2.dll|early|data_ready");
    extdlls_add_spec("data_ready_then_delay", "both3.dll|data_ready|delay,25");
    extdlls_add_spec("delay_then_data_ready", "both4.dll|delay,25|data_ready");
    EXPECT_TRUE(extdlls_test_is_data_ready_at(0));
    EXPECT_TRUE(extdlls_test_is_data_ready_at(1));
    EXPECT_TRUE(!extdlls_test_is_early_at(1));
    EXPECT_TRUE(extdlls_test_is_early_at(2));
    EXPECT_TRUE(!extdlls_test_is_data_ready_at(2));
    EXPECT_TRUE(extdlls_test_is_data_ready_at(3));
    EXPECT_EQ(extdlls_test_delay_at(3), 0);
    EXPECT_TRUE(!extdlls_test_is_data_ready_at(4));
    EXPECT_EQ(extdlls_test_delay_at(4), 25);
    extdlls_unload_all();

    /* A dependent never loads before its dependency, so it moves to the later
       stage; an early dependent of a data-ready DLL is demoted with an error. */
    extdlls_add_spec("normal_after_data_ready", "normal.dll|after,ready");
    extdlls_add_spec("early_after_data_ready", "early.dll|early|after,ready");
    extdlls_add_spec("ready", "ready.dll|data_ready");
    extdlls_prepare();
    EXPECT_EQ(expect_order("ready", "normal_after_data_ready", "early_after_data_ready"), 0);
    EXPECT_TRUE(extdlls_test_is_data_ready_at(0));
    EXPECT_TRUE(extdlls_test_is_data_ready_at(1));
    EXPECT_TRUE(extdlls_test_is_data_ready_at(2));
    EXPECT_TRUE(!extdlls_test_is_early_at(2));
    extdlls_unload_all();

    /* Both deferred stages share one worker that walks them in array order, so a
       dependency spanning the two stages keeps each entry's own stage: ordering
       comes from the sort, not from moving entries between stages. */
    extdlls_add_spec("ready_after_delay", "ready.dll|data_ready|after,slow");
    extdlls_add_spec("slow", "slow.dll|delay,50");
    extdlls_prepare();
    EXPECT_EQ(expect_order("slow", "ready_after_delay", NULL), 0);
    EXPECT_EQ(extdlls_test_delay_at(0), 50);
    EXPECT_TRUE(extdlls_test_is_data_ready_at(1));
    EXPECT_TRUE(!extdlls_test_is_deferred_at(1));
    extdlls_unload_all();

    extdlls_add_spec("delay_after_ready", "slow.dll|delay,50|after,ready");
    extdlls_add_spec("ready", "ready.dll|data_ready");
    extdlls_prepare();
    EXPECT_EQ(expect_order("ready", "delay_after_ready", NULL), 0);
    EXPECT_TRUE(extdlls_test_is_data_ready_at(0));
    EXPECT_TRUE(!extdlls_test_is_data_ready_at(1));
    EXPECT_EQ(extdlls_test_delay_at(1), 50);
    extdlls_unload_all();

    /* Depending on entries in both deferred stages is deterministic: the sort
       puts both prerequisites first and the dependent joins the same worker. */
    extdlls_add_spec("both", "both.dll|after,slow|after,ready");
    extdlls_add_spec("slow", "slow.dll|delay,50");
    extdlls_add_spec("ready", "ready.dll|data_ready");
    extdlls_prepare();
    EXPECT_EQ(expect_order("slow", "ready", "both"), 0);
    EXPECT_EQ(extdlls_test_delay_at(0), 50);
    EXPECT_TRUE(extdlls_test_is_data_ready_at(1));
    EXPECT_TRUE(extdlls_test_is_deferred_at(2));
    extdlls_unload_all();

    extdlls_add_spec("both", "both.dll|after,ready|after,slow");
    extdlls_add_spec("ready", "ready.dll|data_ready");
    extdlls_add_spec("slow", "slow.dll|delay,50");
    extdlls_prepare();
    EXPECT_EQ(expect_order("ready", "slow", "both"), 0);
    EXPECT_TRUE(extdlls_test_is_data_ready_at(0));
    EXPECT_EQ(extdlls_test_delay_at(1), 50);
    EXPECT_TRUE(extdlls_test_is_deferred_at(2));
    extdlls_unload_all();

    extdlls_add_spec("first", "first.dll");
    extdlls_add_spec("dependent", "dependent.dll|after,later");
    extdlls_add_spec("later", "later.dll");
    extdlls_prepare();
    EXPECT_EQ(expect_order("first", "later", "dependent"), 0);
    extdlls_unload_all();

    extdlls_add_spec("target", "target.dll|after,last|after,first");
    extdlls_add_spec("first", "first.dll");
    extdlls_add_spec("last", "last.dll");
    extdlls_prepare();
    EXPECT_EQ(expect_order("first", "last", "target"), 0);
    extdlls_unload_all();

    extdlls_add_spec("dependent", "dependent.dll|after,later");
    extdlls_add_spec("tail", "tail.dll");
    extdlls_add_spec("later", "later.dll");
    extdlls_prepare();
    EXPECT_EQ(expect_order("later", "dependent", "tail"), 0);
    extdlls_unload_all();

    extdlls_add_spec("first", "first.dll");
    extdlls_add_spec("dependent", "dependent.dll|after,first|after,first");
    extdlls_add_spec("tail", "tail.dll");
    extdlls_prepare();
    EXPECT_EQ(expect_order("first", "dependent", "tail"), 0);
    extdlls_unload_all();

    extdlls_add_spec("first", "first.dll");
    extdlls_add_spec("dependent", "dependent.dll|after,missing");
    extdlls_add_spec("tail", "tail.dll");
    extdlls_prepare();
    EXPECT_EQ(expect_order("first", "dependent", "tail"), 0);
    extdlls_unload_all();

    extdlls_add_spec("left", "left.dll|after,right");
    extdlls_add_spec("right", "right.dll|after,left");
    extdlls_prepare();
    EXPECT_EQ(expect_order("left", "right", NULL), 0);
    extdlls_unload_all();

    /* A cycle keeps the configured order without disabling the stage rules for
       unrelated entries. */
    extdlls_add_spec("left", "left.dll|after,right");
    extdlls_add_spec("right", "right.dll|after,left");
    extdlls_add_spec("waiting", "waiting.dll|after,ready");
    extdlls_add_spec("ready", "ready.dll|data_ready");
    extdlls_add_spec("prerequisite", "prerequisite.dll");
    extdlls_add_spec("early", "early.dll|early|after,prerequisite");
    extdlls_prepare();
    EXPECT_TRUE(strcmp(extdlls_test_name_at(0), "left") == 0);
    EXPECT_TRUE(strcmp(extdlls_test_name_at(2), "waiting") == 0);
    EXPECT_TRUE(extdlls_test_is_data_ready_at(2));
    EXPECT_TRUE(extdlls_test_is_early_at(4));
    extdlls_unload_all();

    printf("smoke_extdll_config: all tests passed\n");
    return 0;
}

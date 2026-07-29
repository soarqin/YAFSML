#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlwapi.h>

#include "test_common.h"
#include "modloader/extdll.h"

static wchar_t fixture_path[MAX_PATH];

void config_full_path(wchar_t *path, const wchar_t *filename) {
    if (filename != NULL && lstrcmpW(filename, L"extdll_fixture.dll") == 0) {
        lstrcpyW(path, fixture_path);
    } else {
        lstrcpyW(path, L".");
    }
}

static int expect_order(const char *first, const char *second, const char *third) {
    if (first != NULL && strcmp(extdlls_test_name_at(0), first) != 0) return 1;
    if (second != NULL && strcmp(extdlls_test_name_at(1), second) != 0) return 1;
    if (third != NULL && strcmp(extdlls_test_name_at(2), third) != 0) return 1;
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
    EXPECT_TRUE(!extdlls_test_has_delayed_at(true));
    EXPECT_TRUE(extdlls_test_has_delayed_at(false));
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
    EXPECT_TRUE(extdlls_test_is_effective_early_at(0));
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

    printf("smoke_extdll_config: all tests passed\n");
    return 0;
}

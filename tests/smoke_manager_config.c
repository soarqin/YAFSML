#include <windows.h>
#include <shlwapi.h>
#include <stdio.h>
#include "config_model.h"
#include "test_common.h"

int main(void) {
    static manager_config_t config;
    static manager_config_t round_trip;
    static manager_config_t defaults;
    wchar_t source[MAX_PATH], temp[MAX_PATH];
    wchar_t error[256];
    manager_config_defaults(&defaults);
    EXPECT_STREQ_W(defaults.game, L"eldenring");
    EXPECT_TRUE(defaults.skip_intro);
    EXPECT_TRUE(defaults.prevent_regulation_save_write);
    EXPECT_TRUE(defaults.patch_mem);
    EXPECT_TRUE(defaults.boot_boost);
    EXPECT_EQ(defaults.cpu_affinity, 0);
    EXPECT_STREQ_W(defaults.log_level, L"warn");
    MultiByteToWideChar(CP_UTF8, 0, ML_TEST_SOURCE_ROOT, -1, source, MAX_PATH);
    PathAppendW(source, L"src\\YAFSML.ini");
    EXPECT_TRUE(manager_config_load(&config, source));
    EXPECT_TRUE(manager_config_validate(&config, error, sizeof(error) / sizeof(error[0])));
    EXPECT_EQ(config.dll_count, (size_t)0);
    EXPECT_EQ(config.mod_count, (size_t)0);
    config.dll_count = 2;
    lstrcpyW(config.dlls[0].name, L"first"); lstrcpyW(config.dlls[0].path, L"one.dll");
    lstrcpyW(config.dlls[1].name, L"second"); lstrcpyW(config.dlls[1].path, L"two.dll");
    lstrcpyW(config.dlls[1].condition, L"after,first");
    EXPECT_TRUE(manager_config_validate(&config, error, sizeof(error) / sizeof(error[0])));
    EXPECT_TRUE(GetTempPathW(MAX_PATH, temp) != 0);
    EXPECT_TRUE(GetTempFileNameW(temp, L"yfc", 0, temp) != 0);
    EXPECT_TRUE(manager_config_save(&config, temp));
    EXPECT_TRUE(manager_config_load(&round_trip, temp));
    EXPECT_EQ(round_trip.dll_count, (size_t)2);
    EXPECT_STREQ_W(round_trip.dlls[1].condition, L"after,first");
    DeleteFileW(temp);
    return 0;
}

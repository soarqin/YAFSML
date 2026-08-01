#include <stdint.h>
#include <stdio.h>

#include "test_common.h"
#include "modloader/config.h"
#include "modloader/patches/regulation.h"

config_t config;
static const wchar_t *regulation_override;
static const wchar_t *last_search_path;

const wchar_t *mods_file_search(const wchar_t *path) {
    last_search_path = path;
    return regulation_override;
}

typedef struct test_dl_vector_ptr_msvc2015_s {
    void *allocator;
    void **first;
    void **last;
    void **end;
} test_dl_vector_ptr_msvc2015_t;

typedef struct test_cs_regulation_manager_s {
    void *vtable;
    void *regulation_step;
    test_dl_vector_ptr_msvc2015_t param_res_caps;
    uint8_t *raw_regulation;
    size_t raw_regulation_len;
} test_cs_regulation_manager_t;

int main(void) {
    uint8_t raw[32] = { 0x52, 0x45, 0x47, 0x20 };
    test_cs_regulation_manager_t manager = { 0 };
    uint8_t *save_job_raw;
    size_t save_job_len;

    EXPECT_TRUE(!ml_regulation_override_present());
    EXPECT_STREQ_W(last_search_path, L"regulation.bin");
    EXPECT_TRUE(!ml_regulation_requested());
    config.prevent_regulation_save_write = true;
    EXPECT_TRUE(!ml_regulation_requested());
    regulation_override = L"C:\\mods\\regulation.bin";
    EXPECT_TRUE(ml_regulation_override_present());
    EXPECT_STREQ_W(last_search_path, L"regulation.bin");
    EXPECT_TRUE(ml_regulation_requested());
    config.prevent_regulation_save_write = false;
    EXPECT_TRUE(!ml_regulation_requested());

    manager.raw_regulation = raw;
    manager.raw_regulation_len = sizeof(raw);
    save_job_raw = manager.raw_regulation;
    save_job_len = manager.raw_regulation_len;

    ml_regulation_test_suppress_fd4_save(&manager);
    EXPECT_EQ(manager.raw_regulation, raw);
    EXPECT_EQ(manager.raw_regulation_len, 0);
    EXPECT_EQ(save_job_raw, raw);
    EXPECT_EQ(save_job_len, sizeof(raw));
    EXPECT_EQ(save_job_raw[0], 0x52);

    EXPECT_TRUE(ml_regulation_test_skip_write(&manager));
    EXPECT_EQ(manager.raw_regulation, raw);
    EXPECT_EQ(manager.raw_regulation_len, 0);

    ml_regulation_test_suppress_fd4_save(NULL);
    printf("smoke_regulation: all tests passed\n");
    return 0;
}

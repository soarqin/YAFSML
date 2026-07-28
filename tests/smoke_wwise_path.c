#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "test_common.h"
#include "modloader/patches/wwise_path.h"

int main(void) {
    static const uint8_t wwise_call[] = {
        0xe8, 1, 2, 3, 4, 0x83, 0xf8, 0x01, 0x74, 0x05,
        0x48, 0x83, 0xc3, 0x38, 0x48, 0x83, 0x7d, 0x10, 0x08,
    };
    wchar_t buffer[64];
    wchar_t bucket[WWISE_WEM_BUCKET_SIZE];
    wchar_t long_path[600];

    EXPECT_EQ(wwise_find_open_call(wwise_call, sizeof(wwise_call)), 0);
    EXPECT_EQ(wwise_find_open_call(wwise_call, sizeof(wwise_call) - 1), SIZE_MAX);

    EXPECT_STREQ_W(wwise_strip_prefixes(L"sd:/sd_dlc02:/init.bnk"), L"init.bnk");
    EXPECT_STREQ_W(wwise_strip_prefixes(L"sd_dlc02:/1000519763.wem"), L"1000519763.wem");

    /* Two-part join (no middle) and three-part joins matching the resolver's
     * `wem/` and `wem/<bucket>/` candidate shapes. */
    EXPECT_EQ(wwise_join3(buffer, 64, L"sd/", NULL, L"init.bnk"), 11);
    EXPECT_STREQ_W(buffer, L"sd/init.bnk");
    EXPECT_EQ(wwise_join3(buffer, 64, L"sd/enus/", L"wem/", L"1000519763.wem"), 26);
    EXPECT_STREQ_W(buffer, L"sd/enus/wem/1000519763.wem");
    wwise_wem_bucket(L"1000519763.wem", bucket);
    EXPECT_STREQ_W(bucket, L"wem/10/");
    EXPECT_EQ(wwise_join3(buffer, 64, L"sd/", bucket, L"1000519763.wem"), 24);
    EXPECT_STREQ_W(buffer, L"sd/wem/10/1000519763.wem");

    /* A short name keeps the placeholder bucket; the resolver skips it. */
    wwise_wem_bucket(L"1", bucket);
    EXPECT_STREQ_W(bucket, L"wem/00/");

    /* Reports the needed length without writing when the buffer is too small,
     * and rejects NULL prefix/path. */
    buffer[0] = L'\0';
    EXPECT_EQ(wwise_join3(buffer, 4, L"sd/", NULL, L"init.bnk"), 11);
    EXPECT_STREQ_W(buffer, L"");
    EXPECT_EQ(wwise_join3(NULL, 0, L"sd/", NULL, L"init.bnk"), 11);
    EXPECT_EQ(wwise_join3(buffer, 64, NULL, NULL, L"init.bnk"), SIZE_MAX);
    EXPECT_EQ(wwise_join3(buffer, 64, L"sd/", NULL, NULL), SIZE_MAX);

    long_path[0] = L'1';
    long_path[1] = L'0';
    for (size_t i = 2; i < 594; i++) long_path[i] = L'a';
    memcpy(long_path + 594, L".wem", 5 * sizeof(*long_path));
    wwise_wem_bucket(long_path, bucket);
    EXPECT_STREQ_W(bucket, L"wem/10/");
    EXPECT_EQ(wwise_join3(buffer, 64, L"sd/", bucket, long_path), wcslen(long_path) + 10);

    printf("smoke_wwise_path: all tests passed\n");
    return 0;
}

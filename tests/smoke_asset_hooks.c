#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"
#include "game/game.h"
#include "modloader/patches/asset_hooks.h"

int main(void) {
    typedef struct test_bhd5_holder_s {
        uint8_t *header;
        uint32_t bucket_count;
        uint32_t *buckets;
    } test_bhd5_holder_t;
    static const char rsa_key[] =
        "-----BEGIN RSA PUBLIC KEY-----\n"
        "MAsCCQCQAQIDBAUGBw==\n"
        "-----END RSA PUBLIC KEY-----\n";
    static const uint8_t mount_pattern[] = {
        0x48, 0x8B, 0x45, 0x10, 0x48, 0x89, 0x44, 0x24, 0x28, 0x4C, 0x89, 0x4C, 0x24, 0x20,
        0x4C, 0x8B, 0x0D, 1, 2, 3, 4, 0x49, 0x8B, 0xD2, 0x48, 0x8B, 0xCB, 0xE8, 5, 6, 7, 8,
        0x0F, 0xB6, 0xD8, 0x48, 0x83, 0x7D, 0x18, 0x08, 0x72, 0x09,
    };
    static const uint8_t alt_mount_pattern[] = {
        0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x44, 0x24, 0x70, 0x4D, 0x8B, 0xD1,
        0x4C, 0x8B, 0x4C, 0x24, 0x60, 0x4D, 0x8B, 0xD8, 0x48, 0x89, 0x44, 0x24, 0x28,
        0x48, 0x8B, 0xCA, 0x48, 0x8B, 0x44, 0x24, 0x68, 0x4D, 0x8B, 0xC2, 0x49, 0x8B,
        0xD3, 0x48, 0x89, 0x44, 0x24, 0x20, 0xE8, 1, 2, 3, 4, 0x48, 0x83, 0xC4, 0x30,
        0x5B, 0xC3,
    };
    static const uint8_t rsp_mount_pattern[] = {
        0x48, 0x8B, 0x45, 0x10, 0x48, 0x89, 0x44, 0x24, 0x28, 0x4C, 0x89, 0x4C, 0x24, 0x20,
        0x4C, 0x8B, 0x0D, 1, 2, 3, 4, 0x49, 0x8B, 0xD2, 0x48, 0x8B, 0xCB, 0xE8, 5, 6, 7, 8,
        0x0F, 0xB6, 0xD8, 0x48, 0x83, 0x7C, 0x24, 0x18, 0x08, 0x72, 0x09,
    };
    uint8_t invalid[sizeof(mount_pattern)];
    size_t displacement_offset;
    size_t instruction_end_offset;
    size_t block_size;
    uint8_t previous[24] = { 0 };
    uint8_t cached[64] = { 0 };
    test_bhd5_holder_t holder = { previous, 0, NULL };
    const ml_game_descriptor_t *ds3 = ml_game_by_id(ML_GAME_DARK_SOULS_3);
    const ml_game_descriptor_t *nightreign = ml_game_by_id(ML_GAME_NIGHTREIGN);

    EXPECT_EQ(sizeof(mount_pattern), 42);
    EXPECT_TRUE(ml_asset_hooks_test_match_mount_ebl(mount_pattern, sizeof(mount_pattern),
                                                     &displacement_offset, &instruction_end_offset));
    EXPECT_EQ(displacement_offset, 28);
    EXPECT_EQ(instruction_end_offset, 32);
    EXPECT_TRUE(ml_asset_hooks_test_match_mount_ebl(alt_mount_pattern, sizeof(alt_mount_pattern),
                                                     &displacement_offset, &instruction_end_offset));
    EXPECT_EQ(displacement_offset, 46);
    EXPECT_EQ(instruction_end_offset, 50);
    EXPECT_TRUE(ml_asset_hooks_test_match_mount_ebl(rsp_mount_pattern, sizeof(rsp_mount_pattern),
                                                     &displacement_offset, &instruction_end_offset));
    EXPECT_EQ(displacement_offset, 28);
    EXPECT_EQ(instruction_end_offset, 32);
    EXPECT_TRUE(!ml_asset_hooks_test_match_mount_ebl(rsp_mount_pattern, sizeof(rsp_mount_pattern) - 1,
                                                      NULL, NULL));
    EXPECT_TRUE(ml_asset_hooks_test_rsa_public_key_block_size(rsa_key, sizeof(rsa_key) - 1, &block_size));
    EXPECT_EQ(block_size, 8);
    EXPECT_TRUE(ml_asset_hooks_test_assign_bhd_contents(&holder, cached, 2, 32));
    EXPECT_EQ(holder.header, cached);
    EXPECT_EQ(holder.bucket_count, 2);
    EXPECT_EQ(holder.buckets, (uint32_t *)(cached + 32));
    EXPECT_EQ(holder.header != previous, 1);
    memcpy(invalid, mount_pattern, sizeof(invalid));
    invalid[40] = 0x73;
    EXPECT_TRUE(!ml_asset_hooks_test_match_mount_ebl(invalid, sizeof(invalid), NULL, NULL));
    EXPECT_NOT_NULL(ds3);
    EXPECT_NOT_NULL(nightreign);
    EXPECT_EQ(nightreign->ebl_bhd_holder_offset, 0xB0);
    EXPECT_TRUE(!ml_asset_hooks_loose_params_present(ml_game_by_id(ML_GAME_SEKIRO)));
    EXPECT_TRUE(!ml_asset_hooks_loose_params_present(ds3));
    EXPECT_TRUE(ml_asset_hooks_is_loose_param_path(ds3,
        L"data1:/param/gameparam/gameparam.parambnd.dcx"));
    EXPECT_TRUE(ml_asset_hooks_is_loose_param_path(ds3,
        L"data1:/param/gameparam/gameparam_dlc1.parambnd.dcx"));
    EXPECT_TRUE(ml_asset_hooks_is_loose_param_path(ds3,
        L"data1:/param/gameparam/gameparam_dlc2.parambnd.dcx"));
    EXPECT_TRUE(!ml_asset_hooks_is_loose_param_path(ds3, L"data0:/regulation.bin"));
    EXPECT_TRUE(!ml_asset_hooks_is_loose_param_path(ml_game_by_id(ML_GAME_SEKIRO),
        L"data1:/param/gameparam/gameparam.parambnd.dcx"));
    printf("smoke_asset_hooks: all tests passed\n");
    return 0;
}

#include <stdint.h>
#include <string.h>
#include <xxhash.h>

#include "test_common.h"
#include "modloader/patches/dl_device.h"

int main(void) {
    struct test_image_s {
        IMAGE_DOS_HEADER dos;
        IMAGE_NT_HEADERS64 nt;
    } image = { 0 }, relocated;
    wchar_t temporary[MAX_PATH];
    HANDLE file;
    FILETIME write_time = { 1000000000, 30000000 };
    FILETIME later_time = { 2000000000, 30000000 };
    LARGE_INTEGER end = { 0 };
    uint64_t game_key[2], next_game_key[2];
    uint64_t key[2], next_key[2];
    uint8_t bhd[64] = { 0 }, output[64], stub[24];
    const uint32_t file_size = sizeof(bhd), bucket_count = 2, bucket_offset = 32;

    image.dos.e_magic = IMAGE_DOS_SIGNATURE;
    image.dos.e_lfanew = offsetof(struct test_image_s, nt);
    image.nt.Signature = IMAGE_NT_SIGNATURE;
    image.nt.FileHeader.TimeDateStamp = 100;
    image.nt.OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    image.nt.OptionalHeader.SizeOfImage = 4096;
    EXPECT_TRUE(GetTempPathW(MAX_PATH, temporary) != 0);
    EXPECT_TRUE(GetTempFileNameW(temporary, L"bbv", 0, temporary) != 0);
    file = CreateFileW(temporary, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    EXPECT_TRUE(file != INVALID_HANDLE_VALUE);
    EXPECT_TRUE(SetFileTime(file, NULL, NULL, &write_time));
    EXPECT_TRUE(ml_boot_boost_game_key(&image, file, game_key));
    relocated = image;
    EXPECT_TRUE(ml_boot_boost_game_key(&relocated, file, next_game_key));
    EXPECT_EQ(memcmp(game_key, next_game_key, sizeof(game_key)), 0);

    /* An EXE-only update must invalidate caches even when every BHD is unchanged. */
    image.nt.FileHeader.TimeDateStamp++;
    EXPECT_TRUE(ml_boot_boost_game_key(&image, file, next_game_key));
    EXPECT_TRUE(memcmp(game_key, next_game_key, sizeof(game_key)) != 0);
    image.nt.FileHeader.TimeDateStamp--;
    image.nt.OptionalHeader.SizeOfImage++;
    EXPECT_TRUE(ml_boot_boost_game_key(&image, file, next_game_key));
    EXPECT_TRUE(memcmp(game_key, next_game_key, sizeof(game_key)) != 0);
    image.nt.OptionalHeader.SizeOfImage--;
    image.nt.OptionalHeader.CheckSum++;
    EXPECT_TRUE(ml_boot_boost_game_key(&image, file, next_game_key));
    EXPECT_TRUE(memcmp(game_key, next_game_key, sizeof(game_key)) != 0);
    image.nt.OptionalHeader.CheckSum--;

    /* Access times must not cause a rebuild. Last-write time and size must,
       including builds with unchanged PE timestamps and zero PE checksums. */
    EXPECT_TRUE(SetFileTime(file, NULL, &later_time, NULL));
    EXPECT_TRUE(ml_boot_boost_game_key(&image, file, next_game_key));
    EXPECT_EQ(memcmp(game_key, next_game_key, sizeof(game_key)), 0);
    EXPECT_TRUE(SetFileTime(file, NULL, NULL, &later_time));
    EXPECT_TRUE(ml_boot_boost_game_key(&image, file, next_game_key));
    EXPECT_TRUE(memcmp(game_key, next_game_key, sizeof(game_key)) != 0);
    end.QuadPart = 1;
    EXPECT_TRUE(SetFilePointerEx(file, end, NULL, FILE_BEGIN));
    EXPECT_TRUE(SetEndOfFile(file));
    EXPECT_TRUE(SetFileTime(file, NULL, NULL, &write_time));
    EXPECT_TRUE(ml_boot_boost_game_key(&image, file, next_game_key));
    EXPECT_TRUE(memcmp(game_key, next_game_key, sizeof(game_key)) != 0);
    EXPECT_TRUE(!ml_boot_boost_game_key(&image, INVALID_HANDLE_VALUE, next_game_key));
    EXPECT_TRUE(!ml_boot_boost_game_key(NULL, file, next_game_key));
    image.dos.e_magic = 0;
    EXPECT_TRUE(!ml_boot_boost_game_key(&image, file, next_game_key));
    CloseHandle(file);
    DeleteFileW(temporary);

    memcpy(bhd, "BHD5", 4);
    memcpy(bhd + 12, &file_size, sizeof(file_size));
    memcpy(bhd + 16, &bucket_count, sizeof(bucket_count));
    memcpy(bhd + 20, &bucket_offset, sizeof(bucket_offset));
    memcpy(stub, bhd, sizeof(stub));
    EXPECT_TRUE(ml_boot_boost_cache_key(bhd, sizeof(bhd), game_key, "RSA key", key));
    {
        XXH128_hash_t legacy = XXH3_128bits_withSeed(bhd, sizeof(bhd), 1);
        EXPECT_TRUE(key[0] != legacy.low64 || key[1] != legacy.high64);
    }
    EXPECT_TRUE(ml_boot_boost_cache_key(bhd, sizeof(bhd), game_key, "RSA key", next_key));
    EXPECT_EQ(memcmp(key, next_key, sizeof(key)), 0);
    EXPECT_TRUE(ml_boot_boost_cache_store(temporary, key, bhd, sizeof(bhd)));
    EXPECT_TRUE(ml_boot_boost_cache_load(temporary, key, stub, output, sizeof(output)));
    EXPECT_EQ(memcmp(output, bhd, sizeof(bhd)), 0);

    /* Same-length source changes outside the first block remain detectable. */
    bhd[sizeof(bhd) - 1] ^= 1;
    EXPECT_TRUE(ml_boot_boost_cache_key(bhd, sizeof(bhd), game_key, "RSA key", next_key));
    EXPECT_TRUE(memcmp(key, next_key, sizeof(key)) != 0);
    bhd[sizeof(bhd) - 1] ^= 1;
    EXPECT_TRUE(ml_boot_boost_cache_key(bhd, sizeof(bhd), game_key, "new RSA key", next_key));
    EXPECT_TRUE(memcmp(key, next_key, sizeof(key)) != 0);
    EXPECT_TRUE(ml_boot_boost_cache_key(bhd, sizeof(bhd), next_game_key, "RSA key", next_key));
    EXPECT_TRUE(memcmp(key, next_key, sizeof(key)) != 0);
    EXPECT_TRUE(!ml_boot_boost_cache_load(temporary, next_key, stub, output, sizeof(output)));
    EXPECT_EQ(GetFileAttributesW(temporary), INVALID_FILE_ATTRIBUTES);
    EXPECT_TRUE(ml_boot_boost_cache_store(temporary, next_key, bhd, sizeof(bhd)));
    EXPECT_TRUE(ml_boot_boost_cache_load(temporary, next_key, stub, output, sizeof(output)));
    DeleteFileW(temporary);

    /* A structurally valid payload with a correct checksum can still disagree
       with the live stub. Reject it before installing it in the game device. */
    for (size_t i = 4; i < sizeof(stub); i += 12) {
        stub[i] ^= 1;
        EXPECT_TRUE(ml_boot_boost_cache_store(temporary, key, bhd, sizeof(bhd)));
        EXPECT_TRUE(!ml_boot_boost_cache_load(temporary, key, stub, output, sizeof(output)));
        EXPECT_EQ(GetFileAttributesW(temporary), INVALID_FILE_ATTRIBUTES);
        stub[i] ^= 1;
    }
    EXPECT_TRUE(ml_boot_boost_cache_store(temporary, key, bhd, sizeof(bhd)));
    EXPECT_TRUE(ml_boot_boost_cache_load(temporary, key, stub, output, sizeof(output)));
    DeleteFileW(temporary);
    printf("smoke_boot_boost: all tests passed\n");
    return 0;
}

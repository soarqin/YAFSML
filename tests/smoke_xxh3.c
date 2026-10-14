#include <stdint.h>
#include <stdio.h>

#include "test_common.h"
#include "xxhash.h"

typedef struct xxh3_vector_s {
    size_t size;
    uint64_t low64;
    uint64_t high64;
} xxh3_vector_t;

static const xxh3_vector_t vectors[] = {
    { 0,    UINT64_C(0x6131b78f753823cd), UINT64_C(0xd9265cc53bb2b9ae) },
    { 1,    UINT64_C(0xa9e1b81ea26b3e09), UINT64_C(0x586db770bdaabf6d) },
    { 2,    UINT64_C(0x9d7e7f3c588c6cd6), UINT64_C(0xc0063d86bc43393f) },
    { 3,    UINT64_C(0xb97aaeb03d3b58de), UINT64_C(0x54376f38a65aedad) },
    { 4,    UINT64_C(0x66446314bc0e93a9), UINT64_C(0x5b39fd4a011d3907) },
    { 8,    UINT64_C(0xc756934c2c6a457e), UINT64_C(0xce5c053054ca18bd) },
    { 9,    UINT64_C(0xc9fa4febb4ec4823), UINT64_C(0x4266fc81e9b5a8e3) },
    { 16,   UINT64_C(0x3f532077195cb629), UINT64_C(0x28b649754abb2b49) },
    { 17,   UINT64_C(0xb30d278795f32d8d), UINT64_C(0x4d7651b3200abc75) },
    { 32,   UINT64_C(0x650cf1bcbca3314d), UINT64_C(0xb62d23652fdfc56d) },
    { 128,  UINT64_C(0x47fa5be2bae82f13), UINT64_C(0x9720abb2a3d48a76) },
    { 129,  UINT64_C(0x3ba1fe144266606e), UINT64_C(0x46130d4f500cc336) },
    { 240,  UINT64_C(0xf9e08cf73c6edcb0), UINT64_C(0xc6c0993921e56e8b) },
    { 241,  UINT64_C(0xe7cb060ffa6e803b), UINT64_C(0x7c5d8b9023bddabc) },
    { 482,  UINT64_C(0x8f98fa3b2c0e268f), UINT64_C(0xab165d6aa19f4c22) },
    { 964,  UINT64_C(0xc0c7dbb31900ed96), UINT64_C(0x3ed427705fdd8046) },
    { 1928, UINT64_C(0xb227c4960b72be9f), UINT64_C(0x338cf6a8af457924) },
    { 3856, UINT64_C(0x1ebe457f15a08127), UINT64_C(0xdd74d58a7e650371) },
};

int main(void) {
    uint8_t data[4096];

    for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i * 37 + 11);
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        XXH128_hash_t actual = XXH3_128bits_withSeed(data, vectors[i].size, 1);
        if (actual.low64 != vectors[i].low64 || actual.high64 != vectors[i].high64) {
            fprintf(stderr, "size=%zu actual=%016llx/%016llx expected=%016llx/%016llx\n",
                    vectors[i].size, (unsigned long long)actual.low64,
                    (unsigned long long)actual.high64,
                    (unsigned long long)vectors[i].low64,
                    (unsigned long long)vectors[i].high64);
            return 1;
        }
    }
    printf("smoke_xxh3: all tests passed\n");
    return 0;
}

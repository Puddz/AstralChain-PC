// Precomputed BC7 textures (setup step: runtime/bc7_convert.c writes them; bc7_gpu.c reads them).
// cache/bc7/index.bin: "BC7C", version, count, then `count` records sorted by hash. cache/bc7/data.bin: each
// texture's blocks, layer by layer and level by level within a layer, 16 bytes per 4x4 texels.
// hash: textures.c's hash_full of the texture's guest bytes (tegra_total_size of its layout), as the game loads them.
#pragma once
#include <stdint.h>

#define BC7_CACHE_MAGIC 0x43374342u  // "BC7C"
#define BC7_CACHE_VERSION 1u

typedef struct {
    uint64_t hash, total, offset, size;
    uint32_t format, width, height, levels, layers, pad;
} Bc7Record;
_Static_assert(sizeof(Bc7Record) == 56, "BC7 cache record");

// textures.c's hash_full (same constants: the cache is looked up with the value upload_texture computes)
static inline uint64_t bc7_hash_full(const uint8_t *p, uint64_t n) {
    uint64_t h0 = 0x9E3779B97F4A7C15ull ^ n, h1 = 0xC2B2AE3D27D4EB4Full, h2 = 0x165667B19E3779F9ull, h3 = 0x27D4EB2F165667C5ull;
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t v[4];
        __builtin_memcpy(v, p + i, 32);
        h0 = (h0 ^ v[0]) * 0x100000001B3ull; h0 ^= h0 >> 29;
        h1 = (h1 ^ v[1]) * 0x100000001B3ull; h1 ^= h1 >> 29;
        h2 = (h2 ^ v[2]) * 0x100000001B3ull; h2 ^= h2 >> 29;
        h3 = (h3 ^ v[3]) * 0x100000001B3ull; h3 ^= h3 >> 29;
    }
    uint64_t h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull) ^ (h2 * 0xC2B2AE3D27D4EB4Full) ^ (h3 * 0x165667B19E3779F9ull);
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 31);
}

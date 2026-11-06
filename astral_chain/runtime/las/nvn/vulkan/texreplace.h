// Texture replacement (texreplace.c): DDS files named after a texture's data hash, used in place of the game's.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct {
    void *file;           // the whole file (freed by texreplace_free)
    const uint8_t *data;  // per layer, per level, tightly packed
    VkFormat format;      // in the original texture's colour space
    uint32_t width, height, levels, layers;
    uint8_t block, bpb;   // block size in texels (1 or 4), bytes per block
    const char *path;
} TexReplacement;

bool texreplace_has(uint64_t hash);                     // AC_TEXTURE_REPLACE=1 and a file for this hash
bool texreplace_load(uint64_t hash, bool srgb, uint32_t want_layers, TexReplacement *out);
void texreplace_free(TexReplacement *r);
bool texreplace_is_srgb(VkFormat f);

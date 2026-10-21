// BC7 precomputation for the optional setup step (astral_chain/bc7_cache.py): one game texture (its XT1 header
// fields and data, as stored in the game's WTP files and loaded as is into texture memory) -> BC7 blocks of every
// layer and level, in bc7_cache.h's order, encoded on the CPU with bc7enc (third_party/bc7enc: modes 1, 5, 6 and 7).
// Built as a DLL; Python calls it from several threads at once (every call is independent).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "las/nvn/tegra.h"
#include "las/nvn/astc.h"
#include "las/nvn/vulkan/bc7_cache.h"
#include "bc7enc.h"

#define EXPORT __declspec(dllexport)

EXPORT void bc7c_init(void) { bc7enc_compress_block_init(); }

// The layout the game's texture gets (nvn objects.c builder_layout, packaged block height from the XT1 header).
static int layout_of(TexLayout *t, uint32_t format, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, uint32_t packaged) {
    memset(t, 0, sizeof *t);
    t->fmt = nvn_format_info(format);
    if (!t->fmt || t->fmt->bpb != 16 || t->fmt->bw < 4 || t->fmt->bh < 4 || format < 0x79 || format > 0x94) return 0;  // ASTC only
    t->width = w;
    t->height = h;
    t->depth = 1;
    t->layers = layers ? layers : 1;
    t->levels = levels ? levels : 1;
    tegra_layout_init(t);
    if (packaged && (packaged & 0xFF) <= 5) t->block_height_log2 = packaged & 0xFF;
    return 1;
}

// Bytes of BC7 output for the texture (0: not an ASTC texture we convert).
EXPORT uint64_t bc7c_size(uint32_t format, uint32_t w, uint32_t h, uint32_t levels, uint32_t layers, uint32_t packaged) {
    TexLayout t;
    if (!layout_of(&t, format, w, h, levels, layers, packaged)) return 0;
    uint64_t n = 0;
    for (uint32_t lv = 0; lv < t.levels; lv++) {
        uint32_t lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1;
        n += (uint64_t)((lw + 3) / 4) * ((lh + 3) / 4) * 16;
    }
    return n * t.layers;
}

// Hash and total guest size as the runtime computes them (needs data_size >= total). Returns 0 if not possible.
EXPORT int bc7c_key(const uint8_t *data, uint64_t data_size, uint32_t format, uint32_t w, uint32_t h, uint32_t levels,
                    uint32_t layers, uint32_t packaged, uint64_t *hash, uint64_t *total) {
    TexLayout t;
    if (!layout_of(&t, format, w, h, levels, layers, packaged)) return 0;
    *total = tegra_total_size(&t);
    if (*total > data_size) return 0;
    *hash = bc7_hash_full(data, *total);
    return 1;
}

// Encodes every layer and level into out (bc7c_size bytes). quality: bc7enc's uber level (0-4).
EXPORT int bc7c_convert(const uint8_t *data, uint64_t data_size, uint32_t format, uint32_t w, uint32_t h, uint32_t levels,
                        uint32_t layers, uint32_t packaged, uint32_t quality, uint8_t *out) {
    TexLayout t;
    if (!layout_of(&t, format, w, h, levels, layers, packaged) || tegra_total_size(&t) > data_size) return 0;
    bool srgb = format >= 0x87;
    bc7enc_compress_block_params p;
    p.m_max_partitions_mode = BC7ENC_MAX_PARTITIONS1;
    p.m_try_least_squares = BC7ENC_TRUE;
    p.m_mode_partition_estimation_filterbank = BC7ENC_TRUE;
    p.m_uber_level = quality > BC7ENC_MAX_UBER_LEVEL ? BC7ENC_MAX_UBER_LEVEL : quality;
    p.m_use_mode5_for_alpha = BC7ENC_TRUE;
    p.m_use_mode7_for_alpha = BC7ENC_TRUE;
    // sRGB textures are colour (perceptual weights); UNORM ones are often normal maps and masks (every channel alike)
    p.m_perceptual = srgb ? BC7ENC_TRUE : BC7ENC_FALSE;
    static const uint32_t pw[4] = {128, 64, 16, 32}, lw4[4] = {1, 1, 1, 1};
    memcpy(p.m_weights, srgb ? pw : lw4, sizeof p.m_weights);
    uint64_t layer_size = tegra_layer_size(&t);
    uint8_t *blocks = NULL, *rgba = NULL;
    size_t cap_b = 0, cap_r = 0;
    for (uint32_t ly = 0; ly < t.layers; ly++)
        for (uint32_t lv = 0; lv < t.levels; lv++) {
            LevelLayout l = tegra_level(&t, lv);
            size_t nb = (size_t)l.wb * l.hb * 16, nr = (size_t)l.w * l.h * 4;
            if (nb > cap_b) { free(blocks); blocks = malloc(cap_b = nb); }
            if (nr > cap_r) { free(rgba); rgba = malloc(cap_r = nr); }
            if (!blocks || !rgba) { free(blocks); free(rgba); return 0; }
            tegra_deswizzle(data + ly * layer_size + l.offset, blocks, l.wb, l.hb, 1, 16, l.block_height_log2, l.block_depth_log2);
            astc_decode_ldr(blocks, l.w, l.h, t.fmt->bw, t.fmt->bh, srgb, rgba);
            uint32_t cx = (l.w + 3) / 4, cy = (l.h + 3) / 4;
            for (uint32_t by = 0; by < cy; by++)
                for (uint32_t bx = 0; bx < cx; bx++) {
                    uint8_t px[16 * 4];
                    for (uint32_t i = 0; i < 16; i++) {  // (edge texels repeated past the level's edge)
                        uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                        if (x >= l.w) x = l.w - 1;
                        if (y >= l.h) y = l.h - 1;
                        memcpy(px + i * 4, rgba + ((size_t)y * l.w + x) * 4, 4);
                    }
                    bc7enc_compress_block(out, px, &p);
                    out += 16;
                }
        }
    free(blocks);
    free(rgba);
    return 1;
}

// (quality measurements) Level 0 of layer 0 as linear ASTC blocks into out (wb * hb * 16 bytes).
EXPORT int bc7c_level0_blocks(const uint8_t *data, uint64_t data_size, uint32_t format, uint32_t w, uint32_t h, uint32_t levels,
                              uint32_t layers, uint32_t packaged, uint8_t *out) {
    TexLayout t;
    if (!layout_of(&t, format, w, h, levels, layers, packaged) || tegra_total_size(&t) > data_size) return 0;
    LevelLayout l = tegra_level(&t, 0);
    tegra_deswizzle(data + l.offset, out, l.wb, l.hb, 1, 16, l.block_height_log2, l.block_depth_log2);
    return 1;
}

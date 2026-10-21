// ASTC textures recompressed to BC7 on the GPU (Astral Chain, optional: LARECOMP_BC7=1, the launcher's "Compress
// textures in video memory"). Desktop GPUs cannot sample ASTC, so its textures are decoded into RGBA8 (astc_gpu.c):
// 4 bytes per texel, 16x ASTC 8x8 (a 4096x4096 texture with its mipmaps: ~90 MB). BC7, which every desktop GPU
// samples, takes 1 byte per texel: a quarter of that video memory.
//
// Per level: the ASTC decoder (astc_gpu.c, writing a buffer instead of the image) decodes into a scratch buffer, the
// encoder below turns every 4x4 texels into one BC7 block (mode 6: one subset, RGBA endpoints of 7 bits and a p-bit,
// 4-bit indices; endpoints from the principal axis, then refined by least squares), and the blocks are copied into
// the BC7 image. Done when the texture's data arrives, like the plain GPU decode; no setup step, nothing on disk.
// tools: runtime/bc7_gpu_test.c (python -m astral_chain.bc7_gpu_test) measures the encoder's quality.
#ifndef BC7_GPU_SOURCE_ONLY
#include "vk.h"
#endif

#include <stdlib.h>
#include <string.h>

static const char *BC7_GLSL =
"#version 460\n"
"layout(local_size_x = 64) in;\n"
"layout(std430, set = 0, binding = 0) readonly buffer Src { uint texels[]; };\n"
"layout(std430, set = 0, binding = 1) writeonly buffer Dst { uvec4 blocks[]; };\n"
"layout(push_constant) uniform PC { uint w, h, bx, by; };\n"
"const int W4[16] = int[](0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64);\n"
"vec4 px[16];\n"
"uint idx[16];\n"
// the endpoint as stored: 7 bits per channel and a p-bit shared by the channels (whichever p-bit is closer)
"ivec4 quantize(vec4 e, out uint p) {\n"
"  ivec4 best = ivec4(0); float err = 1e30; p = 0u;\n"
"  for (int pb = 0; pb < 2; pb++) {\n"
"    ivec4 c = clamp(ivec4(floor((e - float(pb)) * 0.5 + 0.5)), ivec4(0), ivec4(127));\n"
"    vec4 d = vec4(c * 2 + pb) - e;\n"
"    float de = dot(d, d);\n"
"    if (de < err) { err = de; best = c; p = uint(pb); }\n"
"  }\n"
"  return best;\n"
"}\n"
// indices for endpoints q0, q1 (8-bit values); the total squared error
"float fit(ivec4 q0, ivec4 q1) {\n"
"  vec4 pal[16];\n"
"  for (int k = 0; k < 16; k++) pal[k] = vec4((q0 * (64 - W4[k]) + q1 * W4[k] + 32) >> 6);\n"
"  float total = 0.0;\n"
"  for (int i = 0; i < 16; i++) {\n"
"    float best = 1e30; uint bk = 0u;\n"
"    for (int k = 0; k < 16; k++) { vec4 d = px[i] - pal[k]; float e = dot(d, d); if (e < best) { best = e; bk = uint(k); } }\n"
"    idx[i] = bk; total += best;\n"
"  }\n"
"  return total;\n"
"}\n"
"void putbits(inout uvec4 o, inout int pos, uint v, int n) {\n"
"  int wi = pos >> 5, sh = pos & 31;\n"
"  o[wi] |= v << uint(sh);\n"
"  if (sh + n > 32) o[wi + 1] |= v >> uint(32 - sh);\n"
"  pos += n;\n"
"}\n"
"void main() {\n"
"  uint id = gl_GlobalInvocationID.x;\n"
"  if (id >= bx * by) return;\n"
"  uint x0 = (id % bx) * 4u, y0 = (id / bx) * 4u;\n"
"  vec4 mean = vec4(0.0), lo = vec4(255.0), hi = vec4(0.0);\n"
"  for (uint i = 0u; i < 16u; i++) {\n"
"    uint x = min(x0 + (i & 3u), w - 1u), y = min(y0 + (i >> 2), h - 1u);\n"
"    uint c = texels[y * w + x];\n"
"    px[i] = vec4(float(c & 0xFFu), float((c >> 8) & 0xFFu), float((c >> 16) & 0xFFu), float(c >> 24));\n"
"    mean += px[i]; lo = min(lo, px[i]); hi = max(hi, px[i]);\n"
"  }\n"
"  mean *= 1.0 / 16.0;\n"
// principal axis of the texels (power iteration on their covariance, from the bounding box's diagonal)
"  mat4 cov = mat4(0.0);\n"
"  for (int i = 0; i < 16; i++) { vec4 d = px[i] - mean; cov += outerProduct(d, d); }\n"
"  vec4 axis = hi - lo;\n"
"  for (int it = 0; it < 6; it++) { vec4 v = cov * axis; float l = length(v); if (l < 1e-6) break; axis = v / l; }\n"
"  if (length(axis) < 1e-6) axis = vec4(0.5); else axis = normalize(axis);\n"
"  float tmin = 1e30, tmax = -1e30;\n"
"  for (int i = 0; i < 16; i++) { float t = dot(px[i] - mean, axis); tmin = min(tmin, t); tmax = max(tmax, t); }\n"
"  vec4 e0 = clamp(mean + axis * tmin, 0.0, 255.0), e1 = clamp(mean + axis * tmax, 0.0, 255.0);\n"
"  uint p0, p1, bp0 = 0u, bp1 = 0u;\n"
"  ivec4 c0 = quantize(e0, p0), c1 = quantize(e1, p1);\n"
"  float best = fit(c0 * 2 + int(p0), c1 * 2 + int(p1));\n"
"  ivec4 b0 = c0, b1 = c1; bp0 = p0; bp1 = p1;\n"
"  uint bidx[16];\n"
"  for (int i = 0; i < 16; i++) bidx[i] = idx[i];\n"
// least squares endpoints for the chosen indices, twice
"  for (int round_ = 0; round_ < 2 && best > 0.0; round_++) {\n"
"    float A = 0.0, B = 0.0, C = 0.0; vec4 X = vec4(0.0), Y = vec4(0.0);\n"
"    for (int i = 0; i < 16; i++) {\n"
"      float t = float(W4[bidx[i]]) / 64.0, s = 1.0 - t;\n"
"      A += s * s; B += s * t; C += t * t; X += s * px[i]; Y += t * px[i];\n"
"    }\n"
"    float det = A * C - B * B;\n"
"    if (abs(det) < 1e-4) break;\n"
"    e0 = clamp((C * X - B * Y) / det, 0.0, 255.0); e1 = clamp((A * Y - B * X) / det, 0.0, 255.0);\n"
"    c0 = quantize(e0, p0); c1 = quantize(e1, p1);\n"
"    float err = fit(c0 * 2 + int(p0), c1 * 2 + int(p1));\n"
"    if (err >= best) break;\n"
"    best = err; b0 = c0; b1 = c1; bp0 = p0; bp1 = p1;\n"
"    for (int i = 0; i < 16; i++) bidx[i] = idx[i];\n"
"  }\n"
// the first index's top bit is implied 0: otherwise swap the endpoints
"  if (bidx[0] >= 8u) {\n"
"    ivec4 tc = b0; b0 = b1; b1 = tc; uint tp = bp0; bp0 = bp1; bp1 = tp;\n"
"    for (int i = 0; i < 16; i++) bidx[i] = 15u - bidx[i];\n"
"  }\n"
"  uvec4 o = uvec4(0u); int pos = 0;\n"
"  putbits(o, pos, 64u, 7);\n"  // mode 6
"  for (int c = 0; c < 4; c++) { putbits(o, pos, uint(b0[c]), 7); putbits(o, pos, uint(b1[c]), 7); }\n"
"  putbits(o, pos, bp0, 1); putbits(o, pos, bp1, 1);\n"
"  putbits(o, pos, bidx[0], 3);\n"
"  for (int i = 1; i < 16; i++) putbits(o, pos, bidx[i], 4);\n"
"  blocks[id] = o;\n"
"}\n";

const char *vk_bc7_glsl(void) { return BC7_GLSL; }

// The ASTC decoder's source (astc_gpu.c) writing packed RGBA8 to a buffer (binding 1, row stride w) instead of the image.
static char *bc7_replace(const char *s, const char *from, const char *to) {
    const char *at = strstr(s, from);
    if (!at) return NULL;
    size_t n = strlen(s) - strlen(from) + strlen(to) + 1;
    char *out = malloc(n);
    memcpy(out, s, (size_t)(at - s));
    strcpy(out + (at - s), to);
    strcat(out, at + strlen(from));
    return out;
}
char *vk_astc_buffer_glsl(void) {
    extern const char *vk_astc_glsl(void);
    char *a = bc7_replace(vk_astc_glsl(), "layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;",
                          "layout(std430, set = 0, binding = 1) writeonly buffer Out { uint outp[]; };");
    if (!a) return NULL;
    char *b = bc7_replace(a, "imageStore(dst, ivec2(int(x0 + x), int(y0 + y)), vec4(c) / 255.0);",
                          "outp[(y0 + y) * w + x0 + x] = c.r | c.g << 8 | c.b << 16 | c.a << 24;");
    free(a);
    return b;
}

#ifndef BC7_GPU_SOURCE_ONLY
// Scratch: the decoded texels of one level (RGBA8) and its BC7 blocks. Levels over BC7_MAX_TEXELS stay RGBA8.
#define BC7_MAX_TEXELS (4096u * 4096u)
static VkPipeline dec_pipe, enc_pipe;
static VkPipelineLayout layout;
static VkDescriptorSetLayout set_layout;
static VkBuffer texels, blocks;
static VmaAllocation texels_mem, blocks_mem;
static int state;  // 0 not tried, 1 ready, 2 unavailable

static VkPipeline bc7_pipeline(const char *src, const char *name) {
    VkShaderModule m = src ? vk_compile_glsl(src, STAGE_COMPUTE, name) : VK_NULL_HANDLE;
    if (!m) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = layout};
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &p);
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    return r == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

static VkBuffer bc7_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocation *mem) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer b = VK_NULL_HANDLE;
    if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &b, mem, NULL) != VK_SUCCESS) return VK_NULL_HANDLE;
    return b;
}

static bool bc7_format_ok(VkFormat f) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
                                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    return (p.optimalTilingFeatures & need) == need;
}

// LARECOMP_BC7=1 and everything needed is there (the GPU ASTC decoder included).
bool vk_bc7_ready(void) {
    if (state) return state == 1;
    state = 2;
    const char *e = getenv("LARECOMP_BC7");
    if (!e || !atoi(e)) return false;
    if (!vk_astc_gpu_ready()) { LOG("[vk] BC7: needs the GPU ASTC decoder, off\n"); return false; }
    if (!bc7_format_ok(VK_FORMAT_BC7_UNORM_BLOCK) || !bc7_format_ok(VK_FORMAT_BC7_SRGB_BLOCK)) {
        LOG("[vk] BC7: not supported by this GPU, off\n");
        return false;
    }
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &set_layout));
    VkPushConstantRange r = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pi, NULL, &layout));
    char *dec_src = vk_astc_buffer_glsl();
    dec_pipe = bc7_pipeline(dec_src, "astc-buffer");
    free(dec_src);
    enc_pipe = bc7_pipeline(BC7_GLSL, "bc7");
    if (!dec_pipe || !enc_pipe) { LOG("[vk] BC7: shaders failed, off\n"); return false; }
    texels = bc7_buffer((VkDeviceSize)BC7_MAX_TEXELS * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &texels_mem);
    blocks = bc7_buffer((VkDeviceSize)BC7_MAX_TEXELS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &blocks_mem);
    if (!texels || !blocks) { LOG("[vk] BC7: no memory for the scratch buffers, off\n"); return false; }
    LOG("[vk] BC7: ASTC textures recompressed to BC7 on the GPU\n");
    state = 1;
    return true;
}

// Whether a w x h (level 0) ASTC texture can be stored as BC7.
bool vk_bc7_fits(uint32_t w, uint32_t h) {
    uint32_t bw = (w + 3) / 4 * 4, bh = (h + 3) / 4 * 4;
    return (uint64_t)bw * bh <= BC7_MAX_TEXELS && vk_bc7_ready();
}

static void bc7_dispatch(VkCommandBuffer cb, VkPipeline p, VkBuffer in, uint64_t in_off, uint64_t in_size, VkBuffer out,
                         uint64_t out_size, const uint32_t pc[8], uint32_t groups) {
    VkDescriptorSet ds = vk_alloc_set(set_layout);
    VkDescriptorBufferInfo b0 = {in, in_off, in_size}, b1 = {out, 0, out_size};
    VkWriteDescriptorSet wr[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b0},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b1}};
    vkUpdateDescriptorSets(g_vk.dev, 2, wr, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &ds, 0, NULL);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, pc);
    vkCmdDispatch(cb, groups, 1, 1);
}

// One level/layer: the ASTC blocks (bx x by of them, at word `first` of buf, bound at off/size) decoded at w x h,
// encoded, and copied into level lv (lw x lh), layer `layer` of the BC7 image.
void vk_bc7_level(VkCommandBuffer cb, VkBuffer buf, uint64_t off, uint64_t size, uint32_t first, VkImage image, uint32_t lv,
                  uint32_t layer, uint32_t bx, uint32_t by, uint32_t w, uint32_t h, uint32_t lw, uint32_t lh, uint32_t bw,
                  uint32_t bh, bool srgb) {
    uint32_t cx = (w + 3) / 4, cy = (h + 3) / 4;
    if ((uint64_t)cx * cy * 16 > BC7_MAX_TEXELS) return;
    vk_full_barrier(cb);  // (the scratch buffers: the previous level's copy is done with them)
    uint32_t dpc[8] = {first, bx, by, w, h, bw, bh, srgb ? 1u : 0u};
    bc7_dispatch(cb, dec_pipe, buf, off, size, texels, (uint64_t)w * h * 4, dpc, (bx * by + 63) / 64);
    vk_full_barrier(cb);
    uint32_t epc[8] = {w, h, cx, cy, 0, 0, 0, 0};
    bc7_dispatch(cb, enc_pipe, texels, 0, (uint64_t)w * h * 4, blocks, (uint64_t)cx * cy * 16, epc, (cx * cy + 63) / 64);
    vk_full_barrier(cb);
    VkBufferImageCopy rg = {.bufferOffset = 0, .bufferRowLength = cx * 4, .bufferImageHeight = cy * 4,
                            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, layer, 1},
                            // (whole blocks, or up to the level's edge)
                            .imageExtent = {cx * 4 < lw ? cx * 4 : lw, cy * 4 < lh ? cy * 4 : lh, 1}};
    vkCmdCopyBufferToImage(cb, blocks, image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
}
#endif

#ifndef BC7_GPU_SOURCE_ONLY
// ---- precomputed BC7 (setup step, bc7_cache.h): a texture found there is uploaded as stored, no GPU conversion
#include "bc7_cache.h"
#include <stdio.h>

static Bc7Record *cache_index;
static uint32_t cache_count;
static FILE *cache_data;
static uint64_t cache_hits, cache_misses;

static void cache_open(void) {
    static int tried;
    if (tried) return;
    tried = 1;
    const char *e = getenv("LARECOMP_BC7_CACHE");
    if (e && !atoi(e)) return;
    FILE *f = fopen("cache/bc7/index.bin", "rb");
    if (!f) return;
    uint32_t head[3];
    if (fread(head, 4, 3, f) == 3 && head[0] == BC7_CACHE_MAGIC && head[1] == BC7_CACHE_VERSION && head[2]) {
        cache_index = malloc((size_t)head[2] * sizeof *cache_index);
        if (cache_index && fread(cache_index, sizeof *cache_index, head[2], f) == head[2]) cache_count = head[2];
    }
    fclose(f);
    if (cache_count) cache_data = fopen("cache/bc7/data.bin", "rb");
    if (!cache_data) cache_count = 0;
    LOG("[vk] BC7: %u precomputed textures%s\n", cache_count, cache_count ? "" : " (none: converted on the GPU)");
}

// The texture with guest bytes hashing to `hash` (total bytes, layout w x h, levels, layers, NVN format), from the
// cache: its first `levels` levels uploaded into image. False: not there (convert on the GPU instead).
bool vk_bc7_cache_upload(VkCommandBuffer cb, VkImage image, uint64_t hash, uint64_t total, uint32_t format, uint32_t w,
                         uint32_t h, uint32_t layout_levels, uint32_t layers, uint32_t levels) {
    cache_open();
    static int log = -1;
    if (log < 0) { const char *e = getenv("LARECOMP_BC7_LOG"); log = e && atoi(e); }
    const Bc7Record *r = NULL;
    for (uint32_t lo = 0, hi = cache_count; lo < hi;) {
        uint32_t mid = (lo + hi) / 2;
        if (cache_index[mid].hash < hash) lo = mid + 1;
        else { if (cache_index[mid].hash == hash) r = &cache_index[mid]; hi = mid; }
    }
    // (equal hashes are adjacent: the first with the same layout)
    while (r && r < cache_index + cache_count && r->hash == hash &&
           (r->total != total || r->format != format || r->width != w || r->height != h || r->levels != layout_levels ||
            r->layers != layers))
        r++;
    if (r && (r >= cache_index + cache_count || r->hash != hash)) r = NULL;
    if (log) LOG("[bc7key] %016llx total %llu fmt %#x %ux%u levels %u layers %u: %s\n", (unsigned long long)hash,
                 (unsigned long long)total, format, w, h, layout_levels, layers, r ? "hit" : "miss");
    if (!r) { cache_misses++; return false; }
    VkSpan s = vk_transient(r->size, 16);
    if (_fseeki64(cache_data, (long long)r->offset, SEEK_SET) || fread(s.ptr, 1, r->size, cache_data) != r->size) {
        cache_misses++;
        return false;
    }
    uint64_t off = 0;
    for (uint32_t ly = 0; ly < layers; ly++)
        for (uint32_t lv = 0; lv < layout_levels; lv++) {
            uint32_t lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1, cx = (lw + 3) / 4, cy = (lh + 3) / 4;
            if (lv < levels) {
                VkBufferImageCopy rg = {.bufferOffset = s.off + off, .bufferRowLength = cx * 4, .bufferImageHeight = cy * 4,
                                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1}, .imageExtent = {lw, lh, 1}};
                vkCmdCopyBufferToImage(cb, s.buf, image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            }
            off += (uint64_t)cx * cy * 16;
        }
    cache_hits++;
    return true;
}

void vk_bc7_cache_stats(uint64_t *hits, uint64_t *misses) { *hits = cache_hits; *misses = cache_misses; }
#endif

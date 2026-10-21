// Measures the GPU BC7 encoder (las/nvn/vulkan/bc7_gpu.c): encodes test images and ASTC texture dumps on the GPU,
// decodes the blocks here and prints the error. Development tool, not part of the game:
// python -m astral_chain.bc7_gpu_test [astc dumps...]
//
//   bc7_gpu_test <glslang.exe> <work folder> [astc_NN_WxH_BWxBH_srgb|unorm.bin ...]
//
// Every block must be a valid mode 6 block; smooth test images must stay above 40 dB PSNR. No game data is needed;
// the dumps (LARECOMP_DUMP_ASTC) are decoded with the CPU ASTC decoder first.
#define main astc_test_main
#include "astc_gpu_test.c"
#undef main
#define BC7_GPU_SOURCE_ONLY
#include "las/nvn/vulkan/bc7_gpu.c"

#include <math.h>

static VkPipeline enc;

static void init_encoder(const char *glslang, const char *work) {
    char path[MAX_PATH], spv[MAX_PATH], cmd[3 * MAX_PATH];
    snprintf(path, sizeof path, "%s\\bc7_test.comp", work);
    snprintf(spv, sizeof spv, "%s\\bc7_test.spv", work);
    FILE *f = fopen(path, "wb");
    fputs(vk_bc7_glsl(), f);
    fclose(f);
    snprintf(cmd, sizeof cmd, "\"\"%s\" -V --target-env vulkan1.3 \"%s\" -o \"%s\" > nul\"", glslang, path, spv);
    if (system(cmd)) { fprintf(stderr, "glslang failed on %s\n", path); exit(2); }
    f = fopen(spv, "rb");
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc((size_t)size);
    fread(code, 1, (size_t)size, f);
    fclose(f);
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)size, .pCode = code};
    VkShaderModule m;
    CHECK(vkCreateShaderModule(dev, &mi, NULL, &m));
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = layout};
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &enc));
}

// BC7 mode 6 decoder (the only mode the encoder writes); false for any other mode.
static bool bc7_decode6(const uint8_t blk[16], uint8_t out[16][4]) {
    if ((blk[0] & 0x7F) != 0x40) return false;
    int pos = 7;
#define BITS(n) (pos += (n), (int)((lo_hi >> (pos - (n))) & ((1u << (n)) - 1)))
    uint32_t e[2][4], p[2];
    uint8_t ix[16];
    int bit = 7;
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 2; k++) {
            uint32_t v = 0;
            for (int i = 0; i < 7; i++, bit++) v |= (uint32_t)((blk[bit >> 3] >> (bit & 7)) & 1) << i;
            e[k][c] = v;
        }
    for (int k = 0; k < 2; k++, bit++) p[k] = (blk[bit >> 3] >> (bit & 7)) & 1;
    for (int t = 0; t < 16; t++) {
        int n = t == 0 ? 3 : 4;
        uint32_t v = 0;
        for (int i = 0; i < n; i++, bit++) v |= (uint32_t)((blk[bit >> 3] >> (bit & 7)) & 1) << i;
        ix[t] = (uint8_t)v;
    }
    (void)pos;
#undef BITS
    static const int w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    for (int t = 0; t < 16; t++)
        for (int c = 0; c < 4; c++) {
            int a = (int)(e[0][c] << 1 | p[0]), b = (int)(e[1][c] << 1 | p[1]);
            out[t][c] = (uint8_t)((a * (64 - w4[ix[t]]) + b * w4[ix[t]] + 32) >> 6);
        }
    return true;
}

// Encodes w x h RGBA8 texels on the GPU; returns the PSNR (all four channels), or -1 when a block is not mode 6.
static double encode(const uint8_t *rgba, uint32_t w, uint32_t h, double *ms) {
    uint32_t cx = (w + 3) / 4, cy = (h + 3) / 4;
    HostBuf in = host_buffer((uint64_t)w * h * 4), out = host_buffer((uint64_t)cx * cy * 16);
    memcpy(in.ptr, rgba, (size_t)w * h * 4);
    CHECK(vkResetDescriptorPool(dev, pool, 0));
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = pool,
                                      .descriptorSetCount = 1, .pSetLayouts = &dsl};
    VkDescriptorSet ds;
    CHECK(vkAllocateDescriptorSets(dev, &ai, &ds));
    VkDescriptorBufferInfo b0 = {in.buf, 0, VK_WHOLE_SIZE}, b1 = {out.buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr[2] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b0},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &b1}};
    vkUpdateDescriptorSets(dev, 2, wr, 0, NULL);
    CHECK(vkResetCommandBuffer(cb, 0));
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(cb, &bi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, enc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &ds, 0, NULL);
    uint32_t pc[8] = {w, h, cx, cy, 0, 0, 0, 0};
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (cx * cy + 63) / 64, 1, 1);
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    LARGE_INTEGER f0, f1, fq;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&f0);
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    QueryPerformanceCounter(&f1);
    *ms = (f1.QuadPart - f0.QuadPart) * 1000.0 / fq.QuadPart;
    double se = 0;
    bool ok = true;
    uint8_t *dump = getenv("BC7_TEST_DUMP") ? calloc((size_t)w * h, 4) : NULL;  // (decoded result, for a look at it)
    for (uint32_t by = 0; by < cy && ok; by++)
        for (uint32_t bx = 0; bx < cx; bx++) {
            uint8_t px[16][4];
            if (!bc7_decode6((uint8_t *)out.ptr + 16 * ((uint64_t)by * cx + bx), px)) { ok = false; break; }
            for (int t = 0; t < 16; t++) {
                uint32_t x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                if (x >= w || y >= h) continue;
                for (int c = 0; c < 4; c++) { double d = (double)px[t][c] - rgba[((uint64_t)y * w + x) * 4 + c]; se += d * d; }
                if (dump) memcpy(dump + ((uint64_t)y * w + x) * 4, px[t], 4);
            }
        }
    free_buffer(&in);
    free_buffer(&out);
    if (dump) {
        FILE *f = fopen(getenv("BC7_TEST_DUMP"), "wb");
        if (f) { fwrite(dump, 4, (size_t)w * h, f); fclose(f); }
        free(dump);
    }
    if (!ok) return -1;
    double mse = se / ((double)w * h * 4);
    return mse == 0 ? 99.0 : 10 * log10(255.0 * 255.0 / mse);
}

static int failures;
static void check(const char *name, const uint8_t *rgba, uint32_t w, uint32_t h, double min_psnr) {
    double ms, psnr = encode(rgba, w, h, &ms);
    bool bad = psnr < 0 || psnr < min_psnr;
    if (psnr < 0) printf("%-40s %ux%u: NOT MODE 6\n", name, w, h);
    else printf("%-40s %ux%u: %.2f dB%s (%.1f ms)\n", name, w, h, psnr, bad ? " TOO LOW" : "", ms);
    failures += bad;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: bc7_gpu_test <glslang.exe> <work folder> [dumps...]\n"); return 2; }
    init(argv[1], argv[2]);
    init_encoder(argv[1], argv[2]);
    uint32_t w = 1023, h = 517;  // (partial blocks at the right and bottom edges)
    uint8_t *img = malloc((size_t)w * h * 4);
    for (int kind = 0; kind < 5; kind++) {
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint8_t *p = img + ((uint64_t)y * w + x) * 4;
                uint32_t r = (uint32_t)next();
                switch (kind) {
                case 0: p[0] = p[1] = p[2] = 90; p[3] = 255; break;  // solid
                case 1: p[0] = (uint8_t)(x * 255 / w); p[1] = (uint8_t)(y * 255 / h); p[2] = (uint8_t)((x + y) * 255 / (w + h)); p[3] = (uint8_t)(255 - x * 255 / w); break;
                case 2: p[0] = (uint8_t)(128 + 100 * sin(x * 0.05)); p[1] = (uint8_t)(128 + 100 * cos(y * 0.07)); p[2] = (uint8_t)(128 + 90 * sin((x + y) * 0.03)); p[3] = 255; break;
                case 3: { bool on = ((x / 7) ^ (y / 5)) & 1; p[0] = on ? 230 : 20; p[1] = on ? 40 : 200; p[2] = on ? 60 : 90; p[3] = on ? 255 : 0; break; }
                default: p[0] = (uint8_t)r; p[1] = (uint8_t)(r >> 8); p[2] = (uint8_t)(r >> 16); p[3] = (uint8_t)(r >> 24); break;
                }
            }
        static const char *names[5] = {"solid", "gradients (RGBA)", "waves", "two-colour edges", "noise"};
        static const double mins[5] = {50, 40, 40, 30, 0};
        check(names[kind], img, w, h, mins[kind]);
    }
    free(img);
    for (int a = 3; a < argc; a++) {  // astc_NN_WxH_BWxBH_srgb.bin
        unsigned n, dw, dh, bw, bh;
        char kind[8] = {0};
        const char *name = strrchr(argv[a], '\\') ? strrchr(argv[a], '\\') + 1 : argv[a];
        if (sscanf(name, "astc_%u_%ux%u_%ux%u_%5[a-z]", &n, &dw, &dh, &bw, &bh, kind) != 6) { printf("%s: name not understood\n", name); continue; }
        FILE *f = fopen(argv[a], "rb");
        size_t size = (size_t)((dw + bw - 1) / bw) * ((dh + bh - 1) / bh) * 16;
        uint8_t *blocks = malloc(size), *rgba = malloc((size_t)dw * dh * 4);
        size_t got = f ? fread(blocks, 1, size, f) : 0;
        if (f) fclose(f);
        if (got != size) { printf("%s: short file\n", name); free(blocks); free(rgba); continue; }
        astc_decode_ldr(blocks, dw, dh, bw, bh, !strcmp(kind, "srgb"), rgba);
        check(name, rgba, dw, dh, 30);
        free(blocks);
        free(rgba);
    }
    printf(failures ? "FAILED: %d cases\n" : "all good\n", failures);
    return failures ? 1 : 0;
}

// Checks the GPU ASTC decoder (las/nvn/vulkan/astc_gpu.c) against the CPU one (las/nvn/astc.c), byte for byte.
// Development tool, not part of the game: python -m astral_chain.astc_gpu_test [dump files...]
//
//   astc_gpu_test <glslang.exe> <work folder> [astc_NN_WxH_BWxBH_srgb|unorm.bin ...]
//
// Decodes random blocks for every 2D footprint, sRGB and not (most random blocks are illegal or use rare modes, so
// they reach every path, error blocks included), plus the given dumps of real game textures (LARECOMP_DUMP_ASTC).
// The shader runs as in the game except that it writes a buffer instead of an image. No game data is needed.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASTC_GPU_SOURCE_ONLY
#include "las/nvn/vulkan/astc_gpu.c"

#define FN(name) static PFN_##name name;
FN(vkCreateInstance) FN(vkEnumeratePhysicalDevices) FN(vkGetPhysicalDeviceQueueFamilyProperties) FN(vkCreateDevice)
FN(vkGetDeviceQueue) FN(vkCreateBuffer) FN(vkGetBufferMemoryRequirements) FN(vkGetPhysicalDeviceMemoryProperties)
FN(vkAllocateMemory) FN(vkBindBufferMemory) FN(vkMapMemory) FN(vkCreateShaderModule) FN(vkCreateDescriptorSetLayout)
FN(vkCreatePipelineLayout) FN(vkCreateComputePipelines) FN(vkCreateDescriptorPool) FN(vkAllocateDescriptorSets)
FN(vkUpdateDescriptorSets) FN(vkCreateCommandPool) FN(vkAllocateCommandBuffers) FN(vkBeginCommandBuffer)
FN(vkCmdBindPipeline) FN(vkCmdBindDescriptorSets) FN(vkCmdPushConstants) FN(vkCmdDispatch) FN(vkEndCommandBuffer)
FN(vkQueueSubmit) FN(vkQueueWaitIdle) FN(vkResetCommandBuffer) FN(vkResetDescriptorPool) FN(vkGetPhysicalDeviceProperties)
FN(vkDestroyBuffer) FN(vkFreeMemory)
static PFN_vkGetInstanceProcAddr gipa;

static VkDevice dev;
static VkPhysicalDevice phys;
static VkQueue queue;
static VkPipeline pipe;
static VkPipelineLayout layout;
static VkDescriptorSetLayout dsl;
static VkDescriptorPool pool;
static VkCommandBuffer cb;

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d\n", #x, r_); exit(2); } } while (0)

typedef struct { VkBuffer buf; VkDeviceMemory mem; void *ptr; } HostBuf;
static HostBuf host_buffer(VkDeviceSize size) {
    HostBuf b;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    CHECK(vkCreateBuffer(dev, &bi, NULL, &b.buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, b.buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, type = 0;
    while (type < mp.memoryTypeCount && !((mr.memoryTypeBits >> type & 1) && (mp.memoryTypes[type].propertyFlags & want) == want)) type++;
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = type};
    CHECK(vkAllocateMemory(dev, &ai, NULL, &b.mem));
    CHECK(vkBindBufferMemory(dev, b.buf, b.mem, 0));
    CHECK(vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.ptr));
    return b;
}
static void free_buffer(HostBuf *b) { vkDestroyBuffer(dev, b->buf, NULL); vkFreeMemory(dev, b->mem, NULL); }

static char *replace(const char *s, const char *from, const char *to) {
    const char *at = strstr(s, from);
    if (!at) { fprintf(stderr, "shader text not found: %s\n", from); exit(2); }
    size_t n = strlen(s) - strlen(from) + strlen(to) + 1;
    char *out = malloc(n);
    memcpy(out, s, (size_t)(at - s));
    strcpy(out + (at - s), to);
    strcat(out, at + strlen(from));
    return out;
}

static void init(const char *glslang, const char *work) {
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) { fprintf(stderr, "no vulkan-1.dll\n"); exit(2); }
    gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(lib, "vkGetInstanceProcAddr");
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance inst;
    CHECK(vkCreateInstance(&ii, NULL, &inst));
#define LOADI(name) name = (PFN_##name)gipa(inst, #name);
    LOADI(vkEnumeratePhysicalDevices) LOADI(vkGetPhysicalDeviceQueueFamilyProperties) LOADI(vkCreateDevice)
    LOADI(vkGetDeviceQueue) LOADI(vkCreateBuffer) LOADI(vkGetBufferMemoryRequirements) LOADI(vkGetPhysicalDeviceMemoryProperties)
    LOADI(vkAllocateMemory) LOADI(vkBindBufferMemory) LOADI(vkMapMemory) LOADI(vkCreateShaderModule) LOADI(vkCreateDescriptorSetLayout)
    LOADI(vkCreatePipelineLayout) LOADI(vkCreateComputePipelines) LOADI(vkCreateDescriptorPool) LOADI(vkAllocateDescriptorSets)
    LOADI(vkUpdateDescriptorSets) LOADI(vkCreateCommandPool) LOADI(vkAllocateCommandBuffers) LOADI(vkBeginCommandBuffer)
    LOADI(vkCmdBindPipeline) LOADI(vkCmdBindDescriptorSets) LOADI(vkCmdPushConstants) LOADI(vkCmdDispatch) LOADI(vkEndCommandBuffer)
    LOADI(vkQueueSubmit) LOADI(vkQueueWaitIdle) LOADI(vkResetCommandBuffer) LOADI(vkResetDescriptorPool)
    LOADI(vkGetPhysicalDeviceProperties) LOADI(vkDestroyBuffer) LOADI(vkFreeMemory)
    uint32_t n = 8;
    VkPhysicalDevice pd[8];
    CHECK(vkEnumeratePhysicalDevices(inst, &n, pd));
    phys = pd[0];
    for (uint32_t i = 0; i < n; i++) {  // (a discrete GPU, as the game picks)
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { phys = pd[i]; break; }
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("GPU: %s\n", props.deviceName);
    uint32_t qn = 16, qf = 0;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qp);
    while (qf < qn && !(qp[qf].queueFlags & VK_QUEUE_COMPUTE_BIT)) qf++;
    float prio = 1;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qf, .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi};
    CHECK(vkCreateDevice(phys, &di, NULL, &dev));
    vkGetDeviceQueue(dev, qf, 0, &queue);

    // the game's shader, writing packed RGBA8 to binding 1 instead of the image
    char *src = replace(vk_astc_glsl(), "layout(set = 0, binding = 1, rgba8) uniform writeonly image2D dst;",
                        "layout(std430, set = 0, binding = 1) writeonly buffer Out { uint outp[]; };");
    src = replace(src, "imageStore(dst, ivec2(int(x0 + x), int(y0 + y)), vec4(c) / 255.0);",
                  "outp[(y0 + y) * w + x0 + x] = c.r | c.g << 8 | c.b << 16 | c.a << 24;");
    char path[MAX_PATH], spv[MAX_PATH], cmd[3 * MAX_PATH];
    snprintf(path, sizeof path, "%s\\astc_test.comp", work);
    snprintf(spv, sizeof spv, "%s\\astc_test.spv", work);
    FILE *f = fopen(path, "wb");
    fputs(src, f);
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
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                         {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = b};
    CHECK(vkCreateDescriptorSetLayout(dev, &li, NULL, &dsl));
    VkPushConstantRange r = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
    CHECK(vkCreatePipelineLayout(dev, &pi, NULL, &layout));
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = layout};
    CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe));
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
    VkDescriptorPoolCreateInfo poi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    CHECK(vkCreateDescriptorPool(dev, &poi, NULL, &pool));
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qf};
    VkCommandPool cp;
    CHECK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
    VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
}

// Decodes blocks (bx x by of them) at w x h on both sides; returns the number of differing texels.
static long compare(const uint8_t *blocks, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, bool srgb) {
    uint32_t bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
    uint64_t nblocks = (uint64_t)bx * by;
    HostBuf in = host_buffer(1088 + nblocks * 16), out = host_buffer((uint64_t)w * h * 4);
    vk_astc_tables(in.ptr);
    memcpy((uint8_t *)in.ptr + 1088, blocks, nblocks * 16);
    memset(out.ptr, 0xCD, (size_t)w * h * 4);
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
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &ds, 0, NULL);
    uint32_t pc[8] = {1088 / 4, bx, by, w, h, bw, bh, srgb};
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (uint32_t)((nblocks + 63) / 64), 1, 1);
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
    uint8_t *cpu = malloc((size_t)w * h * 4);
    astc_decode_ldr(blocks, w, h, bw, bh, srgb, cpu);
    long bad = 0, shown = 0;
    for (uint64_t i = 0; i < (uint64_t)w * h; i++) {
        if (memcmp(cpu + 4 * i, (uint8_t *)out.ptr + 4 * i, 4)) {
            if (shown++ < 5) {
                const uint8_t *g = (uint8_t *)out.ptr + 4 * i, *c = cpu + 4 * i;
                uint32_t x = (uint32_t)(i % w), y = (uint32_t)(i / w);
                const uint8_t *blk = blocks + 16 * ((y / bh) * bx + x / bw);
                printf("    texel %u,%u: cpu %02x%02x%02x%02x gpu %02x%02x%02x%02x, block", x, y, c[0], c[1], c[2], c[3], g[0], g[1], g[2], g[3]);
                for (int k = 0; k < 16; k++) printf(" %02x", blk[k]);
                printf("\n");
            }
            bad++;
        }
    }
    free(cpu);
    free_buffer(&in);
    free_buffer(&out);
    return bad;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: astc_gpu_test <glslang.exe> <work folder> [dumps...]\n"); return 2; }
    init(argv[1], argv[2]);
    static const uint8_t fp[14][2] = {{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
    long failures = 0;
    for (int f = 0; f < 14; f++) {
        uint32_t bw = fp[f][0], bh = fp[f][1], bx = 256, by = 64;
        uint32_t w = bx * bw - bw / 2, h = by * bh - 1;  // (partial blocks at the right and bottom edges)
        uint8_t *blocks = malloc((size_t)bx * by * 16);
        for (int pass = 0; pass < 4; pass++) {
            for (uint32_t i = 0; i < bx * by * 2; i++) ((uint64_t *)blocks)[i] = next();
            if (pass >= 2)  // (legal-looking blocks more often: single partition, low mode bits of real encoders)
                for (uint32_t i = 0; i < bx * by; i++) { blocks[16 * i] &= 0xF3; blocks[16 * i + 1] &= 0xE7; }
            bool srgb = pass & 1;
            uint32_t legal = 0;
            uint8_t tmp[12 * 12 * 4];
            for (uint32_t i = 0; i < bx * by; i++) legal += astc_decode_block(blocks + 16 * i, bw, bh, srgb, tmp);
            long bad = compare(blocks, w, h, bw, bh, srgb);
            printf("%2ux%-2u %s pass %d: %s (%ld texels differ; %u of %u blocks legal)\n", bw, bh, srgb ? "srgb " : "unorm", pass,
                   bad ? "DIFFERENT" : "same", bad, legal, bx * by);
            failures += bad != 0;
        }
        free(blocks);
    }
    for (int a = 3; a < argc; a++) {  // astc_NN_WxH_BWxBH_srgb.bin: blocks, then the CPU decode
        unsigned n, w, h, bw, bh;
        char kind[8] = {0};
        const char *name = strrchr(argv[a], '\\') ? strrchr(argv[a], '\\') + 1 : argv[a];
        if (sscanf(name, "astc_%u_%ux%u_%ux%u_%5[a-z]", &n, &w, &h, &bw, &bh, kind) != 6) { printf("%s: name not understood\n", name); continue; }
        FILE *f = fopen(argv[a], "rb");
        size_t size = (size_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * 16;
        uint8_t *blocks = malloc(size);
        size_t got = f ? fread(blocks, 1, size, f) : 0;
        if (f) fclose(f);
        if (got != size) { printf("%s: short file\n", name); free(blocks); continue; }
        long bad = compare(blocks, w, h, bw, bh, !strcmp(kind, "srgb"));
        printf("%s: %s (%ld texels differ)\n", name, bad ? "DIFFERENT" : "same", bad);
        failures += bad != 0;
        free(blocks);
    }
    printf(failures ? "FAILED: %ld cases differ\n" : "all same\n", failures);
    return failures ? 1 : 0;
}

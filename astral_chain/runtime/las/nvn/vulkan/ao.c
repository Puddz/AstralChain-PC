// Screen-space ambient occlusion (AC_AO = off, low or high; issue #46). Astral Chain has none of its own: every surface
// takes the same ambient light however enclosed it is. This darkens the scene's light where the depth buffer shows
// nearby geometry hiding part of the sky above a surface: creases, corners, the ground under characters and objects.
//
// Ported from captainj-sparrow's Astral Chain PC recomp (https://github.com/captainj-sparrow/astral-chain-pc-recomp,
// runtime/vk_ao.c and runtime/shaders/ao*.comp, ao_apply.frag; MIT License, Copyright (c) 2026 Valiantsin Dzerakh, the
// licence text in LICENSE-astral-chain-pc-recomp). Their shaders are used as they are (ao_shaders.h); this file is
// their vk_ao.c rewritten for this renderer, without their gi level (radiance cascades, vk_rc.c), which is not ported.
// Their notes on how dark it gets and why (AO_OPEN, AO_POWER, AO_MULTIBOUNCE, AO_THIN, the fog and the ambient light
// kept) are in their vk_ao.c and docs/architecture.md; the values are theirs.
//
// Where it runs (their findings, the same game): the frame draws into its light target (R11G11B10F, RGBA16F in a few
// places) with the scene depth, turns that depth into an R32F linear depth in a pass of its own with no depth target,
// and some passes later draws the opaque world into the light target with the same depth. The first light-target pass
// with that depth after the linear depth pass that writes colour is the opaque lighting (every other frame a pass with
// its colour writes masked off comes first). When the next pass with other targets begins, or a dispatch, a copy or
// the submission comes first, the steps run: ao_prep.comp takes the depth to a half-resolution view z, ao_src.comp the
// light target (without the fog) to the light the surfaces send out, ao.comp searches the horizons, ao_blur.comp
// averages the pattern out and makes the factor on the light, and ao_apply.frag multiplies the light target by it
// through a blend. Transparent surfaces, bloom, tonemap and HUD come after, untouched.
//
// From the opaque lighting's draws: the camera (vertex cbuf 3: view, projection, view-projection, inverse view, as
// temporal.c reads it on the dlss-fsr branch), the fog's parameters (fragment cbuf 5, rows 2 and 4) with its cube map
// (fragment texture slot 12), and the ambient light (row 1 .w of the same block, the ambient cube map in fragment
// texture slot 5). Without fog parameters the whole pixel takes the occlusion; without the ambient light the whole
// surface does, and what glows is let go (ao_meter.comp).
// AC_AO_DEBUG=1 shows the factor alone, 2 the part of the scene it acts on, 3 multiplies the fog too (no menu row).
#include "vk.h"
#include "ao_shaders.h"
#include "../../rt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t nvn_depth_mode(void);
extern volatile int ac_ao;  // (live_settings.h) -1 not read, 0 off, 1 low, 2 high

#define Z_FORMAT VK_FORMAT_R32_SFLOAT
#define K_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT
#define AO_RADIUS 1.5f
#define AO_SCALE 2u
#define AO_REACH 4.0f
#define AO_GLOW_LO 6.0f
#define AO_GLOW_HI 24.0f
#define AO_OPEN 0.90f
#define AO_POWER 1.5f
#define AO_MULTIBOUNCE 0.3f
#define AO_THIN 4.0f
#define AO_FADE 120.0f
#define AO_CREDIT_LO 1.0f
#define AO_CREDIT_HI 3.0f
#define CAM_FLOATS 64
#define FOG_FLOATS 20     // rows 0-4 of fragment cbuf 5
#define AMB_DOWN 1u       // the ambient cube map's level read, counted from its smallest
#define SRC_LEVELS 6u
#define N_IM 5            // view z, the occlusion, the factor, the 1x1 brightness, the stand-in cube map
#define MAX_APPLY 4

typedef struct { const float *addr; uint32_t n; float block[CAM_FLOATS]; } Cam;
typedef struct { const float *addr; NvnTexture *cube; uint32_t n; float block[FOG_FLOATS]; } Fog;
typedef struct { NvnTexture *t; uint32_t n; } Amb;

static struct {
    int level, debug, logged_level;
    int tried, ok;
    VkSampler nearest, linear, trilinear;
    VkDescriptorSetLayout dsl_c, dsl_b, dsl_a;
    VkPipelineLayout lay_c, lay_b, lay_a;
    VkPipeline prep, meter, src, gather[2], blur[2];
    struct { VkFormat fmt; int debug; VkPipeline p; } apply[MAX_APPLY];
    int napply;
    struct { VkImage img; VmaAllocation mem; VkImageView view; } im[N_IM];
    uint32_t iw, ih;
    VkImage srcimg;
    VmaAllocation src_mem;
    VkImageView src_view, src_view0;
    uint32_t src_levels;
    // this frame
    NvnTexture *light, *depth;
    int have, lin, armed, done, wrote;
    Cam cams[4];
    Fog fogs[4];
    Amb ambs[4];
    int logs, fog_state, light_state;
} A;

static const VkComponentMapping ID = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                      VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};

static void read_settings(void) {
    if (ac_ao < 0) {
        const char *e = getenv("AC_AO");
        ac_ao = e && !strcmp(e, "high") ? 2 : e && !strcmp(e, "low") ? 1 : 0;
    }
    static int dbg = -1;
    if (dbg < 0) { const char *e = getenv("AC_AO_DEBUG"); dbg = e && *e && strcmp(e, "0") ? (!strcmp(e, "2") ? 2 : !strcmp(e, "3") ? 3 : 1) : 0; }
    A.level = ac_ao, A.debug = dbg;
    if (A.level != A.logged_level) {
        A.logged_level = A.level;
        LOG("[ao] ambient occlusion %s%s\n", A.level == 2 ? "high" : A.level ? "low" : "off",
            A.debug == 3 ? " (the fog multiplied too)" : A.debug == 2 ? " (debug view: the surface's share)" : A.debug ? " (debug view)" : "");
    }
}

// ---------------------------------------------------------------- pipelines
static VkDescriptorSetLayout set_layout(const VkDescriptorType *t, uint32_t n, VkShaderStageFlags stage) {
    VkDescriptorSetLayoutBinding b[6];
    for (uint32_t i = 0; i < n; i++) b[i] = (VkDescriptorSetLayoutBinding){i, t[i], 1, stage, NULL};
    VkDescriptorSetLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = b};
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    return vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &l) == VK_SUCCESS ? l : VK_NULL_HANDLE;
}

static VkPipelineLayout pipe_layout(VkDescriptorSetLayout *dsl, VkShaderStageFlags stage, uint32_t pc) {
    VkPushConstantRange r = {stage, 0, pc};
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
    VkPipelineLayout l = VK_NULL_HANDLE;
    return vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l) == VK_SUCCESS ? l : VK_NULL_HANDLE;
}

static VkPipeline comp_pipeline(const char *src, const char *name, VkPipelineLayout lay, int lit) {
    VkShaderModule m = vk_compile_glsl(src, STAGE_COMPUTE, name);
    if (!m) return VK_NULL_HANDLE;
    VkBool32 v = lit ? VK_TRUE : VK_FALSE;
    VkSpecializationMapEntry me = {0, 0, sizeof v};
    VkSpecializationInfo sp = {1, &me, sizeof v, &v};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                .module = m, .pName = "main", .pSpecializationInfo = &sp},
                                      .layout = lay};
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &p);
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    return r == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

static int init(void) {
    A.tried = 1;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .maxLod = 0};
    VkSamplerCreateInfo sl = si;
    sl.magFilter = sl.minFilter = VK_FILTER_LINEAR;
    VkSamplerCreateInfo st = sl;
    st.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR, st.maxLod = VK_LOD_CLAMP_NONE;
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const VkDescriptorType tc[2] = {S, I}, tb[6] = {S, I, S, S, S, S}, ta[2] = {S, S};
    if (vkCreateSampler(g_vk.dev, &si, NULL, &A.nearest) != VK_SUCCESS || vkCreateSampler(g_vk.dev, &sl, NULL, &A.linear) != VK_SUCCESS ||
        vkCreateSampler(g_vk.dev, &st, NULL, &A.trilinear) != VK_SUCCESS || !(A.dsl_c = set_layout(tc, 2, VK_SHADER_STAGE_COMPUTE_BIT)) ||
        !(A.dsl_b = set_layout(tb, 6, VK_SHADER_STAGE_COMPUTE_BIT)) || !(A.dsl_a = set_layout(ta, 2, VK_SHADER_STAGE_FRAGMENT_BIT)) ||
        !(A.lay_c = pipe_layout(&A.dsl_c, VK_SHADER_STAGE_COMPUTE_BIT, 64)) || !(A.lay_b = pipe_layout(&A.dsl_b, VK_SHADER_STAGE_COMPUTE_BIT, 128)) ||
        !(A.lay_a = pipe_layout(&A.dsl_a, VK_SHADER_STAGE_FRAGMENT_BIT, 64))) {
        LOG("[ao] off: its layouts could not be made\n");
        return 0;
    }
    A.prep = comp_pipeline(AO_PREP_GLSL, "ao_prep", A.lay_c, 0);
    A.meter = comp_pipeline(AO_METER_GLSL, "ao_meter", A.lay_c, 0);
    A.src = comp_pipeline(AO_SRC_GLSL, "ao_src", A.lay_b, 0);
    for (int i = 0; i < 2; i++) {
        A.gather[i] = comp_pipeline(AO_GATHER_GLSL, "ao", A.lay_b, i);
        A.blur[i] = comp_pipeline(AO_BLUR_GLSL, "ao_blur", A.lay_b, i);
    }
    if (!A.prep || !A.meter || !A.src || !A.gather[0] || !A.gather[1] || !A.blur[0] || !A.blur[1]) {
        LOG("[ao] off: its compute pipelines could not be made\n");
        return 0;
    }
    return A.ok = 1;
}

// The apply for a light target format: target = target x output, or no blend in the debug view.
static VkPipeline apply_pipeline(VkFormat fmt, int debug) {
    for (int i = 0; i < A.napply; i++)
        if (A.apply[i].fmt == fmt && A.apply[i].debug == debug) return A.apply[i].p;
    if (A.napply == MAX_APPLY) return VK_NULL_HANDLE;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, fmt, &fp);
    if (!debug && !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) {
        if (A.logs++ < 8) LOG("[ao] cannot blend into format %d: skipped\n", (int)fmt);
        return VK_NULL_HANDLE;
    }
    VkShaderModule vs = vk_compile_glsl(AO_FULLSCREEN_GLSL, STAGE_VERTEX, "ao_fullscreen"),
                   fs = vk_compile_glsl(AO_APPLY_GLSL, STAGE_FRAGMENT, "ao_apply");
    VkPipelineShaderStageCreateInfo stg[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {.blendEnable = debug ? VK_FALSE : VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_ZERO,
                                              .dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR, .colorBlendOp = VK_BLEND_OP_ADD,
                                              .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
                                              .alphaBlendOp = VK_BLEND_OP_ADD,
                                              .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = stg,
                                       .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = A.lay_a};
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vs && fs ? vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &gi, NULL, &p) : VK_ERROR_UNKNOWN;
    if (vs) vkDestroyShaderModule(g_vk.dev, vs, NULL);
    if (fs) vkDestroyShaderModule(g_vk.dev, fs, NULL);
    if (r != VK_SUCCESS) {
        if (A.logs++ < 8) LOG("[ao] no apply pipeline for format %d: skipped\n", (int)fmt);
        return VK_NULL_HANDLE;
    }
    A.apply[A.napply].fmt = fmt, A.apply[A.napply].debug = debug, A.apply[A.napply++].p = p;
    return p;
}

// ---------------------------------------------------------------- images
static void free_images(void) {
    for (int i = 0; i < N_IM; i++) {
        if (A.im[i].view) vk_defer_view(A.im[i].view);
        if (A.im[i].img) vk_defer_image(A.im[i].img, A.im[i].mem);
        A.im[i].img = VK_NULL_HANDLE, A.im[i].view = VK_NULL_HANDLE, A.im[i].mem = NULL;
    }
    if (A.src_view0 && A.src_view0 != A.src_view) vk_defer_view(A.src_view0);
    if (A.src_view) vk_defer_view(A.src_view);
    if (A.srcimg) vk_defer_image(A.srcimg, A.src_mem);
    A.srcimg = VK_NULL_HANDLE, A.src_view = A.src_view0 = VK_NULL_HANDLE, A.src_mem = NULL;
    A.iw = A.ih = 0;
}

static int make_image(VkCommandBuffer cb, VkFormat f, uint32_t w, uint32_t h, uint32_t levels, int cube, VkImageUsageFlags usage,
                      VkImage *img, VmaAllocation *mem, VkImageView *view) {
    uint32_t layers = cube ? 6 : 1;
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = {w, h, 1}, .mipLevels = levels, .arrayLayers = layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ii, &ai, img, mem, NULL) != VK_SUCCESS) { *img = VK_NULL_HANDLE; return 0; }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
                                .viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D, .format = f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers}};
    if (vkCreateImageView(g_vk.dev, &vi, NULL, view) != VK_SUCCESS) { *view = VK_NULL_HANDLE; return 0; }
    vk_image_init_layout(cb, *img, VK_IMAGE_ASPECT_COLOR_BIT);
    return 1;
}

// The occlusion's images at w x h, and the light sent out at half that with its levels; a new size retires the old.
static int ensure_images(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (A.im[0].img && A.iw == w && A.ih == h) return 1;
    if (A.im[0].img || A.srcimg) free_images();
    const VkImageUsageFlags SU = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    for (int i = 0; i < N_IM; i++) {
        int one = i >= 3, cube = i == 4;
        VkFormat f = cube ? VK_FORMAT_R8G8B8A8_UNORM : i == 0 || i == 3 ? Z_FORMAT : K_FORMAT;
        if (!make_image(cb, f, one ? 1 : w, one ? 1 : h, 1, cube, cube ? VK_IMAGE_USAGE_SAMPLED_BIT : SU, &A.im[i].img, &A.im[i].mem,
                        &A.im[i].view)) { free_images(); return 0; }
    }
    uint32_t sw = (w + 1) / 2, sh = (h + 1) / 2, lv = 1;
    while (lv < SRC_LEVELS && (sw >> lv) >= 4 && (sh >> lv) >= 4) lv++;
    A.src_levels = lv;
    if (!make_image(cb, K_FORMAT, sw, sh, lv, 0, SU | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &A.srcimg,
                    &A.src_mem, &A.src_view)) { free_images(); return 0; }
    A.src_view0 = A.src_view;
    if (lv > 1) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = A.srcimg, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = K_FORMAT, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(g_vk.dev, &vi, NULL, &A.src_view0) != VK_SUCCESS) { A.src_view0 = A.src_view; free_images(); return 0; }
    }
    A.iw = w, A.ih = h;
    return 1;
}

static void src_levels(VkCommandBuffer cb) {
    uint32_t w = (A.iw + 1) / 2, h = (A.ih + 1) / 2;
    for (uint32_t i = 1; i < A.src_levels; i++) {
        int32_t sw = (int32_t)(w >> (i - 1)), sh = (int32_t)(h >> (i - 1)), dw = (int32_t)(w >> i), dh = (int32_t)(h >> i);
        VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1},
                         .srcOffsets = {{0, 0, 0}, {sw > 0 ? sw : 1, sh > 0 ? sh : 1, 1}},
                         .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1},
                         .dstOffsets = {{0, 0, 0}, {dw > 0 ? dw : 1, dh > 0 ? dh : 1, 1}}};
        vkCmdBlitImage(cb, A.srcimg, VK_IMAGE_LAYOUT_GENERAL, A.srcimg, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
        vk_full_barrier(cb);
    }
}

// ---------------------------------------------------------------- the frame's inputs
static int inv4(const double m[16], double o[16]) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 8; k++) a[r][k] = k < 4 ? m[r * 4 + k] : (k - 4 == r);
    for (int c = 0; c < 4; c++) {
        int piv = c;
        for (int r = c + 1; r < 4; r++) if (fabs(a[r][c]) > fabs(a[piv][c])) piv = r;
        if (fabs(a[piv][c]) < 1e-300) return 0;
        for (int k = 0; k < 8; k++) { double t = a[c][k]; a[c][k] = a[piv][k]; a[piv][k] = t; }
        double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) { double f = a[r][c]; for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k]; }
    }
    for (int r = 0; r < 4; r++) for (int k = 0; k < 4; k++) o[r * 4 + k] = a[r][k + 4];
    return 1;
}

// A perspective projection (row-major) as ao.comp needs it: clip w a multiple of view z, clip x and y from their own
// coordinate and z alone.
static int plain_perspective(const double p[16]) {
    double ws = p[14];
    for (int i = 0; i < 16; i++) if (!isfinite(p[i])) return 0;
    return fabs(p[0]) > 1e-6 && fabs(p[5]) > 1e-6 && fabs(ws) > 1e-9 && fabs(p[12]) <= 1e-6 * fabs(ws) && fabs(p[13]) <= 1e-6 * fabs(ws) &&
           fabs(p[15]) <= 1e-6 * fabs(ws) && fabs(p[1]) <= 1e-6 * fabs(p[0]) && fabs(p[3]) <= 1e-6 && fabs(p[4]) <= 1e-6 * fabs(p[5]) &&
           fabs(p[7]) <= 1e-6;
}

static const float *frame_camera(void) {
    const Cam *best = NULL;
    for (int i = 0; i < 4; i++) if (A.cams[i].addr && (!best || A.cams[i].n > best->n)) best = &A.cams[i];
    return best ? best->block : NULL;
}

static const Fog *frame_c5(void) {
    const Fog *best = NULL;
    for (int i = 0; i < 4; i++) if (A.fogs[i].addr && (!best || A.fogs[i].n > best->n)) best = &A.fogs[i];
    return best;
}

// A cube map of the game's that a plain view of one level reads as its shaders do (no channel remap). NULL: none.
static VkImageView plain_cube(NvnTexture *t, uint32_t down, uint32_t *size, uint32_t *level, uint32_t *levels) {
    TexBackend *b = t ? t->backend : NULL;
    if (!b || !b->image || b->base_type != VK_IMAGE_VIEW_TYPE_CUBE || b->layers < 6 || b->samples > 1 ||
        !(b->aspect & VK_IMAGE_ASPECT_COLOR_BIT) || vk_format_alpha_one(t->format))
        return VK_NULL_HANDLE;
    const uint8_t *s = t->swizzle;
    if ((s[0] | s[1] | s[2] | s[3]) && !(s[0] == 2 && s[1] == 3 && s[2] == 4 && s[3] == 5)) return VK_NULL_HANDLE;
    uint32_t lv = down == ~0u ? 0 : b->levels > down ? b->levels - 1 - down : 0;
    if (size) *size = b->width, *level = lv, *levels = b->levels;
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_CUBE, b->format, lv, 1, 0, 6, ID, VK_IMAGE_ASPECT_COLOR_BIT);
}

// ---------------------------------------------------------------- the pass
typedef struct { float glow[4], curve[4], ndc_to_view[4], size[4], world[3][4], fog[4]; } BlurPush;

static void dispatch(VkCommandBuffer cb, VkPipeline pipe, VkPipelineLayout lay, VkDescriptorSetLayout dsl, const VkDescriptorImageInfo *img,
                     const VkDescriptorType *types, uint32_t n, const void *pc, uint32_t pc_size, uint32_t gx, uint32_t gy) {
    VkDescriptorSet set = vk_alloc_set(dsl);
    VkWriteDescriptorSet w[6];
    for (uint32_t i = 0; i < n; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i, .descriptorCount = 1,
                                      .descriptorType = types[i], .pImageInfo = &img[i]};
    vkUpdateDescriptorSets(g_vk.dev, n, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, lay, 0, 1, &set, 0, NULL);
    if (pc_size) vkCmdPushConstants(cb, lay, VK_SHADER_STAGE_COMPUTE_BIT, 0, pc_size, pc);
    vkCmdDispatch(cb, gx, gy, 1);
    vk_full_barrier(cb);
}

static void run(void) {
    A.armed = 0, A.done = 1;
    if (!A.tried && !init()) return;
    if (!A.ok) return;
    const float *cam = frame_camera();
    double pj[16], inv[16];
    for (int i = 0; i < 16; i++) pj[i] = cam ? cam[16 + i] : 0;
    if (!cam || !plain_perspective(pj) || !inv4(pj, inv)) {
        if (A.logs++ < 8) LOG("[ao] %s this frame: skipped\n", cam ? "no perspective projection" : "no camera block");
        return;
    }
    TexBackend *t = A.light->backend, *z = A.depth->backend;
    if (!t || !z || !t->image || !z->image) return;
    uint32_t w = t->width, h = t->height, sc = AO_SCALE;
    uint32_t hw = (w + sc - 1) / sc, hh = (h + sc - 1) / sc, gx = (hw + 7) / 8, gy = (hh + 7) / 8;
    VkPipeline ap = apply_pipeline(t->format, A.debug == 1);
    if (!ap) return;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // what drew the scene is done
    if (!ensure_images(cb, hw, hh)) {
        if (A.logs++ < 8) LOG("[ao] its %ux%u images could not be made\n", hw, hh);
        return;
    }
    VkImageView zv = vk_image_view(z, VK_IMAGE_VIEW_TYPE_2D, z->format, 0, 1, 0, 1, ID, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageView cv = vk_image_view(t, VK_IMAGE_VIEW_TYPE_2D, t->format, 0, 1, 0, 1, ID, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!zv || !cv) return;
    static int said;
    if (!said) {
        said = 1;
        LOG("[ao] on a %ux%u scene (format %d, depth format %d), %ux%u occlusion, projection y scale %.3f\n", w, h, (int)t->format,
            (int)z->format, hw, hh, pj[5]);
    }
    // the game's fog (c5 rows 2 and 4, cube map in slot 12) and ambient light (c5 row 1 .w, cube map in slot 5)
    const Fog *c5 = frame_c5();
    const Fog *fog = NULL;
    VkImageView fog_view = VK_NULL_HANDLE;
    if (c5 && A.debug != 3) {
        const float *r2 = c5->block + 8, *r4 = c5->block + 16;
        if (isfinite(r2[0]) && r2[1] > 0 && isfinite(r2[1]) && r2[2] > 0 && r2[2] < 64 && r2[3] > 0 && isfinite(r2[3]) && isfinite(r4[1]) &&
            isfinite(r4[2]) && (fog_view = plain_cube(c5->cube, ~0u, NULL, NULL, NULL)))
            fog = c5;
    }
    float amb_factor = 0;
    VkImageView amb_view = VK_NULL_HANDLE;
    uint32_t amb_size = 0, amb_level = 0, amb_levels = 0;
    const char *why = "no c5 block";
    if (c5) {
        float fac = c5->block[7];
        const Amb *best = NULL;
        for (int i = 0; i < 4; i++) if (A.ambs[i].n && (!best || A.ambs[i].n > best->n)) best = &A.ambs[i];
        if (!(isfinite(fac) && fac > 0 && fac < 1e4f)) why = "row 1 of the c5 block does not read as an ambient factor";
        else if (!best) why = "no draw with an ambient cube map";
        else if (!(amb_view = plain_cube(best->t, AMB_DOWN, &amb_size, &amb_level, &amb_levels))) why = "the ambient cube map cannot be read as it is";
        else amb_factor = fac / 3.14159265f;
    }
    int lit = amb_view != VK_NULL_HANDLE;
    if (lit + 1 != A.light_state && A.logs < 24) {
        A.logs++;
        if (lit) LOG("[ao] ambient factor %.3f, ambient cube map %u wide read at level %u of %u\n", amb_factor, amb_size, amb_level, amb_levels);
        else LOG("[ao] no ambient light this frame (%s): the whole surface takes it\n", why);
    }
    A.light_state = lit + 1;
    if ((fog ? 1 : 2) != A.fog_state && A.logs < 24) {
        A.logs++;
        if (fog) LOG("[ao] the fog keeps its light (start %.1f, range %.0f, exponent %.2f, scale %.0f)\n", fog->block[8], 1.0 / fog->block[9],
                     fog->block[10], fog->block[11]);
        else LOG("[ao] no fog parameters this frame: the whole pixel is multiplied\n");
    }
    A.fog_state = fog ? 1 : 2;
    float depth_mode = nvn_depth_mode() == 0 ? 1.0f : 0.0f;  // clip z -1..1
    struct { float inv2[4], inv3[4], size[4]; } pz = {.size = {(float)w, (float)h, depth_mode, (float)sc}};
    for (int i = 0; i < 4; i++) pz.inv2[i] = (float)inv[8 + i], pz.inv3[i] = (float)inv[12 + i];
    double ws = pj[14];
    struct { float ndc_to_view[4], size[4], radius[4], steps[4], lit[4], world[3][4]; } pa = {
        {(float)(ws / pj[0]), (float)(-pj[2] / pj[0]), (float)(ws / pj[5]), (float)(-pj[6] / pj[5])},
        {(float)hw, (float)hh, (float)(fabs(pj[5]) * hh / (fabs(pj[0]) * hw)), 0},
        {AO_RADIUS, (float)(fabs(pj[5]) * 0.5 * hh / fabs(ws)), (float)hh / AO_REACH, AO_FADE},
        {A.level >= 2 ? 3.0f : 2.0f, A.level >= 2 ? 5.0f : 4.0f, AO_THIN, 0},
        {amb_factor, (float)(A.src_levels - 1), AO_CREDIT_LO, 1.0f / (AO_CREDIT_HI - AO_CREDIT_LO)}};
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) pa.world[r][k] = cam[48 + 4 * r + k];
    BlurPush pb = {.glow = {(float)sc, AO_GLOW_LO, AO_GLOW_HI, fog ? 1.0f : 0.0f},
                   .curve = {AO_OPEN, AO_POWER, A.debug == 2 ? 2.0f : AO_MULTIBOUNCE, amb_factor},
                   .size = {(float)w, (float)h, fog ? fog->block[17] : 0, fog ? fog->block[18] : 1.0f}};
    memcpy(pb.ndc_to_view, pa.ndc_to_view, sizeof pb.ndc_to_view);
    memcpy(pb.world, pa.world, sizeof pb.world);
    if (fog) memcpy(pb.fog, fog->block + 8, sizeof pb.fog);
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo depth_in = {A.nearest, zv, G}, z_in = {A.nearest, A.im[0].view, G}, light_in = {A.linear, cv, G},
                          mean_in = {A.nearest, A.im[3].view, G}, cube_in = {A.linear, A.im[4].view, G},
                          fog_in = {A.linear, fog ? fog_view : A.im[4].view, G}, amb_in = {A.linear, lit ? amb_view : A.im[4].view, G},
                          src_out = {VK_NULL_HANDLE, A.src_view0, G}, src_in = {A.trilinear, A.src_view, G},
                          occ_out = {VK_NULL_HANDLE, A.im[1].view, G}, occ_in = {A.nearest, A.im[1].view, G},
                          k_out = {VK_NULL_HANDLE, A.im[2].view, G}, k_in = {A.nearest, A.im[2].view, G},
                          z_out = {VK_NULL_HANDLE, A.im[0].view, G}, mean_out = {VK_NULL_HANDLE, A.im[3].view, G};
    const VkDescriptorType tc[2] = {S, I}, tb[6] = {S, I, S, S, S, S};
    (void)cube_in;
    // 1: view z at the occlusion's resolution
    VkDescriptorImageInfo i1[2] = {depth_in, z_out};
    dispatch(cb, A.prep, A.lay_c, A.dsl_c, i1, tc, 2, &pz, sizeof pz, gx, gy);
    // the light each surface sends out, and its coarser levels (only with the ambient light)
    if (lit) {
        VkDescriptorImageInfo i5[6] = {z_in, src_out, light_in, mean_in, fog_in, amb_in};
        dispatch(cb, A.src, A.lay_b, A.dsl_b, i5, tb, 6, &pb, sizeof pb, ((hw + 1) / 2 + 7) / 8, ((hh + 1) / 2 + 7) / 8);
        src_levels(cb);
    }
    // 2: the occlusion
    VkDescriptorImageInfo i2[6] = {z_in, occ_out, lit ? src_in : z_in, amb_in, fog_in, amb_in};
    dispatch(cb, A.gather[lit], A.lay_b, A.dsl_b, i2, tb, 6, &pa, sizeof pa, gx, gy);
    // 3: without the ambient light, the light's typical brightness; then the blur and the factor on the light
    if (!lit) {
        VkDescriptorImageInfo i3[2] = {light_in, mean_out};
        dispatch(cb, A.meter, A.lay_c, A.dsl_c, i3, tc, 2, NULL, 0, 1, 1);
    }
    VkDescriptorImageInfo i4[6] = {occ_in, k_out, light_in, mean_in, fog_in, amb_in};
    dispatch(cb, A.blur[lit], A.lay_b, A.dsl_b, i4, tb, 6, &pb, sizeof pb, gx, gy);
    // 4: scaled up and onto the light target
    struct { float inv2[4], inv3[4], size[4], mode[4]; } pf;
    memcpy(&pf, &pz, sizeof pz);
    pf.size[3] = 0;
    pf.mode[0] = A.debug == 1 ? 1.0f : 0.0f, pf.mode[1] = 0, pf.mode[2] = (float)sc, pf.mode[3] = 0;
    VkDescriptorSet set = vk_alloc_set(A.dsl_a);
    VkDescriptorImageInfo ia[2] = {k_in, depth_in};
    VkWriteDescriptorSet wa[2];
    for (int i = 0; i < 2; i++)
        wa[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = (uint32_t)i,
                                       .descriptorCount = 1, .descriptorType = S, .pImageInfo = &ia[i]};
    vkUpdateDescriptorSets(g_vk.dev, 2, wa, 0, NULL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = cv, .imageLayout = G,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, ap);
    VkViewport v = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D s = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &s);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, A.lay_a, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, A.lay_a, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pf, &pf);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);  // what follows sees the darkened light
}

// ---------------------------------------------------------------- hooks (exec.c, device.c)
static int scene_color(VkFormat f) { return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT; }

static int scene_depth(TexBackend *z) { return z && z->image && z->width >= 640 && (z->aspect & VK_IMAGE_ASPECT_DEPTH_BIT); }

bool vk_ao_armed(void) { return A.level && A.armed && A.wrote && !A.done; }

void vk_ao_run(void) {
    if (vk_ao_armed()) run();
}

// A pass begins (outside rendering): color 0 at level lv0, the depth.
void vk_ao_pass(int ncolor, NvnTexture *c0, uint32_t lv0, NvnTexture *depth) {
    if (!A.level || A.done) return;
    TexBackend *c = c0 ? c0->backend : NULL, *z = depth ? depth->backend : NULL;
    int light = ncolor == 1 && c && c->image && scene_color(c->format) && scene_depth(z) && !lv0 && c->width == z->width &&
                c->height == z->height && c->samples <= 1 && c->base_type != VK_IMAGE_VIEW_TYPE_3D;
    if (A.armed) {
        if (light && c0 == A.light && depth == A.depth) return;  // the opaque pass goes on
        if (A.wrote) { run(); return; }
        A.armed = 0;  // that pass drew no colour: not the opaque lighting, which this one may be
    }
    if (light) {
        if (A.have && A.lin && depth == A.depth) {  // the opaque lighting, after the linear depth
            A.light = c0, A.armed = 1, A.wrote = 0;
            return;
        }
        if (!A.have || depth != A.depth) memset(A.cams, 0, sizeof A.cams);  // another scene's camera
        A.light = c0, A.depth = depth, A.have = 1, A.lin = 0;
        return;
    }
    // the linear depth: one R32F target of the scene's size and no depth target
    TexBackend *d = A.have && A.depth ? A.depth->backend : NULL;
    if (d && ncolor == 1 && c && c->format == VK_FORMAT_R32_SFLOAT && !depth && c->width == d->width && c->height == d->height) A.lin = 1;
}

// A draw about to be recorded into c0 / depth: whether it writes colour, its vertex cbuf 3 (the camera), its fragment
// cbuf 5 (fog and ambient factor) and fragment texture slots 5 (ambient cube map) and 12 (fog cube map).
void vk_ao_draw(bool writes, NvnTexture *c0, NvnTexture *depth, const float *cam, uint64_t cam_size, const float *c5, uint64_t c5_size,
                NvnTexture *amb, NvnTexture *fogcube) {
    if (!A.level || A.done || !A.have) return;
    if (A.armed && writes && c0 == A.light && depth == A.depth) {
        A.wrote = 1;
        if (amb) {
            int i = 0, low = 0;
            for (; i < 4 && A.ambs[i].n && A.ambs[i].t != amb; i++) if (A.ambs[i].n < A.ambs[low].n) low = i;
            if (i == 4) i = low, A.ambs[i].n = 0;
            A.ambs[i].t = amb, A.ambs[i].n++;
        }
        if (c5 && c5_size >= FOG_FLOATS * 4 && fogcube) {
            int i = 0, low = 0;
            for (; i < 4 && A.fogs[i].addr && (A.fogs[i].addr != c5 || A.fogs[i].cube != fogcube); i++) if (A.fogs[i].n < A.fogs[low].n) low = i;
            if (i == 4) i = low, A.fogs[i].addr = NULL;
            Fog *f = &A.fogs[i];
            if (!f->addr) { memcpy(f->block, c5, sizeof f->block); f->addr = c5, f->cube = fogcube, f->n = 0; }
            f->n++;
        }
    }
    if (!cam || cam_size < CAM_FLOATS * 4 || depth != A.depth) return;
    int i = 0, low = 0;
    for (; i < 4 && A.cams[i].addr && A.cams[i].addr != cam; i++) if (A.cams[i].n < A.cams[low].n) low = i;
    if (i == 4) i = low, A.cams[i].addr = NULL;
    Cam *cm = &A.cams[i];
    if (cm->addr != cam) { memcpy(cm->block, cam, sizeof cm->block); cm->addr = cam, cm->n = 0; }
    cm->n++;
}

// Each present: the frame state starts over, the setting is read again (the in-game menu changes it at once).
void vk_ao_frame(void) {
    read_settings();
    A.have = A.lin = A.armed = A.done = A.wrote = 0;
    A.light = A.depth = NULL;
    memset(A.cams, 0, sizeof A.cams);
    memset(A.fogs, 0, sizeof A.fogs);
    memset(A.ambs, 0, sizeof A.ambs);
    if (!A.level && A.im[0].img) free_images();  // (turned off: its images go)
}

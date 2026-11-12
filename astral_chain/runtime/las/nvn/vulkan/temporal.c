// Temporal upscaling (Astral Chain, optional; issue #31): AMD FSR 4 / 3.1 through AMD's FSR API DLLs, or NVIDIA DLSS
// (DLAA at the native ratio) through the driver's NGX library.
//
// The game draws its 3D scene at a "level" size (1440x810 under its 1600x900 picture; dynamic resolution) and its
// last scene pass, filtersmaablend, scales that into the display-size picture the HUD is then drawn on. With
// AC_UPSCALER=fsr that pass is replaced (exec.c draw_): the tonemapped scene, its depth and the game's own velocity
// map go to the upscaler, whose output lands in the display-size picture. The level size is the display size divided
// by the quality mode's ratio (host_runtime.c.in, svc #0xB3), and the scene's geometry is drawn with a sub-pixel
// offset that changes every frame (the viewport is moved, exec.c), which is what lets the upscaler rebuild detail.
//
// Inputs prepared here (one compute pass):
//  - depth: the scene's D24S8 / D32S8 as R32F;
//  - motion vectors, in pixels, from this frame's position to the last frame's: for a pixel a model's velocity draw
//    covered (exec.c draws those again into a float target), the shader's (ndc now - ndc before - 0.001) * 10 + 0.5;
//    for the others, the pixel's depth taken back through this frame's view-projection and forward through the last
//    one (the camera's motion). NDC y points up, pixel y down.
//
// FSR (AC_UPSCALER=fsr): the DLLs (tools\fsr, AC_FSR_DIR) are amd_fidelityfx_loader_vk.dll and
// amd_fidelityfx_upscaler_vk.dll, from AMD's FSR SDK (signedbin). Without them, or if the context cannot be made,
// the game's own pass runs as before.
//
// DLSS (AC_UPSCALER=dlss; NVIDIA RTX GPUs): NGX is the driver's _nvngx.dll (found through the registry, as NVIDIA's
// SDK library finds it), called directly: the SDK's library is built for MSVC and cannot be linked here, and its
// headers may not be put in an open repository, so the few declarations needed are written out below. The functions
// are the DLL's exports; NVSDK_NGX_Parameter is a C++ object whose methods are called through its (MSVC) vtable:
// Set(void*) 0, Set(int) 3, Set(unsigned) 4, Set(double) 5, Set(float) 6, Set(ull) 7, Get(void**) 8, Get(int*) 11,
// Get(unsigned*) 12. The DLSS model is nvngx_dlss.dll in tools\dlss (AC_DLSS_DIR), or the one NVIDIA's app keeps
// up to date on the PC. The device needs VK_NVX_binary_import, VK_NVX_image_view_handle, VK_KHR_push_descriptor and
// buffer device addresses (device.c enables them when AC_UPSCALER=dlss).
#include "vk.h"
#include "api/include/vk/ffx_api_vk.h"
#include "upscalers/include/ffx_upscale.h"

#include <SDL3/SDL_timer.h>
#include <math.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>

__declspec(dllimport) void *LoadLibraryA(const char *name);
__declspec(dllimport) void *LoadLibraryW(const wchar_t *name);
__declspec(dllimport) void *GetProcAddress(void *module, const char *name);
__declspec(dllimport) int MultiByteToWideChar(unsigned cp, unsigned long flags, const char *s, int n, wchar_t *w, int wn);

typedef struct {
    float near_z, fov_y;
    float reproject[16];  // this frame's clip space -> the last frame's (row-major)
    bool cut;             // the camera jumped: drop the history
    bool have;            // reproject is valid
    bool minus_one;       // clip z is -1..1 (depth = z / 2 + 0.5), else 0..1
} TuCamera;

static struct {
    int mode;        // -1 not read, 0 off, 1 FSR, 2 DLSS
    int state;       // 0 not tried, 1 ready, 2 unavailable
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxConfigure configure;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    ffxContext ctx;
    uint32_t rw, rh, ow, oh;  // the context's render and output sizes
    uint32_t frame;           // jitter index
    float jx, jy;             // this frame's jitter, in render pixels
    uint64_t last_ns;
    uint64_t ok_frames;
    TexBackend depth, mv, out;
    VkPipeline prep;
    VkPipelineLayout prep_layout;
    VkDescriptorSetLayout prep_set;
} tu = {.mode = -1};

// AC_UPSCALER=fsr: AMD FSR (4 where the GPU and driver have it, else 3.1); =dlss: NVIDIA DLSS.
int vk_tu_mode(void) {
    if (tu.mode < 0) {
        const char *u = getenv("AC_UPSCALER");
        tu.mode = u && !strcmp(u, "fsr") ? 1 : u && !strcmp(u, "dlss") ? 2 : 0;
    }
    return tu.state == 2 ? 0 : tu.mode;
}

static void tu_message(uint32_t type, const wchar_t *msg) {
    char b[512];
    int n = 0;
    for (; msg && msg[n] && n < 511; n++) b[n] = msg[n] < 128 ? (char)msg[n] : '?';
    b[n] = 0;
    LOG("[fsr] %s: %s\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", b);
}

static bool tu_load(void) {
    if (tu.state) return tu.state == 1;
    tu.state = 2;
    const char *dir = getenv("AC_FSR_DIR");
    char path[600];
    snprintf(path, sizeof path, "%s%samd_fidelityfx_loader_vk.dll", dir ? dir : "", dir && *dir ? "\\" : "");
    void *m = LoadLibraryA(path);
    if (!m) { LOG("[fsr] %s not found: the game's own scaling is used\n", path); return false; }
    tu.create = (PfnFfxCreateContext)GetProcAddress(m, "ffxCreateContext");
    tu.destroy = (PfnFfxDestroyContext)GetProcAddress(m, "ffxDestroyContext");
    tu.configure = (PfnFfxConfigure)GetProcAddress(m, "ffxConfigure");
    tu.query = (PfnFfxQuery)GetProcAddress(m, "ffxQuery");
    tu.dispatch = (PfnFfxDispatch)GetProcAddress(m, "ffxDispatch");
    if (!tu.create || !tu.destroy || !tu.query || !tu.dispatch) { LOG("[fsr] %s: not an FSR API DLL\n", path); return false; }
    tu.state = 1;
    return true;
}

// An image of our own (kept until the size changes), always in the GENERAL layout like the backend's others.
static TexBackend *tu_image(TexBackend *d, VkFormat f, uint32_t w, uint32_t h) {
    if (d->image && (d->width != w || d->height != h)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D; d->usage = ci.usage;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

static VkImageView tu_view(TexBackend *d, VkImageAspectFlags aspect) {
    static const VkComponentMapping id = {0};
    return vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, aspect);
}

static const char *PREP_GLSL =
"#version 450\n"
"layout(local_size_x = 8, local_size_y = 8) in;\n"
"layout(set = 0, binding = 0) uniform sampler2D depth_t;\n"
"layout(set = 0, binding = 1) uniform sampler2D vel_t;\n"
"layout(set = 0, binding = 2, r32f) uniform writeonly image2D depth_o;\n"
"layout(set = 0, binding = 3, rg16f) uniform writeonly image2D mv_o;\n"
// m: clip now -> clip before (rows). a: size, jitter (pixels). b.x: model velocity bound, b.y: clip z -1..1,
// b.z: m valid, b.w: debug (1: no camera motion, 2: no model motion)
"layout(push_constant) uniform PC { vec4 m[4]; vec4 a; vec4 b; } pc;\n"
"void main() {\n"
"  ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
"  if (any(greaterThanEqual(p, imageSize(depth_o)))) return;\n"
"  float d = texelFetch(depth_t, p, 0).r;\n"
"  imageStore(depth_o, p, vec4(d));\n"
"  vec2 mv = vec2(0.0);\n"
"  vec4 v = pc.b.x > 0.5 ? texelFetch(vel_t, p, 0) : vec4(0.0, 0.0, 0.0, 1.0);\n"
"  if (v.a < 0.5) {\n"  // a model: its own motion (the camera's included)
"    vec2 n = (v.rg - 0.5) * 0.1 + 0.001;\n"
"    if (pc.b.w != 2.0) mv = vec2(-n.x, n.y) * 0.5 * pc.a.xy;\n"
"  } else if (pc.b.z > 0.5 && pc.b.w != 1.0) {\n"
     // (the geometry was drawn pc.a.zw pixels off: this pixel's centre shows the point at centre - jitter)
"    vec2 uv = (vec2(p) + 0.5 - pc.a.zw) / pc.a.xy;\n"
"    vec4 c = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, pc.b.y > 0.5 ? d * 2.0 - 1.0 : d, 1.0);\n"
"    vec3 q = vec3(dot(pc.m[0], c), dot(pc.m[1], c), dot(pc.m[3], c));\n"
"    if (q.z > 1e-6) mv = (q.xy / q.z - c.xy) * vec2(0.5, -0.5) * pc.a.xy;\n"
"  }\n"
"  imageStore(mv_o, p, vec4(mv, 0.0, 0.0));\n"
"}\n";

static bool tu_prep_ready(void) {
    if (tu.prep) return true;
    VkDescriptorSetLayoutBinding b[4] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = b};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &tu.prep_set));
    VkPushConstantRange r = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 96};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &tu.prep_set, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pi, NULL, &tu.prep_layout));
    VkShaderModule m = vk_compile_glsl(PREP_GLSL, STAGE_COMPUTE, "temporal-prep");
    if (!m) return false;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = tu.prep_layout};
    VkResult res = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &tu.prep);
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    return res == VK_SUCCESS;
}

static void tu_destroy(void) {
    if (!tu.ctx) return;
    vkDeviceWaitIdle(g_vk.dev);  // (its resources may be in use by submitted work)
    tu.destroy(&tu.ctx, NULL);
    tu.ctx = NULL;
}

static bool tu_context(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    if (tu.ctx && tu.rw == rw && tu.rh == rh && tu.ow == ow && tu.oh == oh) return true;
    tu_destroy();
    struct ffxCreateBackendVKDesc be = {0};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.deviceContext.vkDevice = g_vk.dev;
    be.deviceContext.vkPhysicalDevice = g_vk.phys;
    be.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;
    struct ffxCreateContextDescUpscaleVersion ver = {0};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.version = FFX_UPSCALER_VERSION;
    ver.header.pNext = &be.header;
    struct ffxCreateContextDescUpscale up = {0};
    up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    up.header.pNext = &ver.header;
    // (the tonemapped picture; depth is 0 near .. 1 at infinity)
    up.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE | FFX_UPSCALE_ENABLE_DEPTH_INFINITE;
    if (getenv("AC_FSR_DEBUG")) up.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING | FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION;
    up.maxRenderSize.width = rw; up.maxRenderSize.height = rh;
    up.maxUpscaleSize.width = ow; up.maxUpscaleSize.height = oh;
    up.fpMessage = tu_message;
    uint64_t t0 = SDL_GetTicksNS();
    ffxReturnCode_t rc = tu.create(&tu.ctx, &up.header, NULL);
    if (rc != FFX_API_RETURN_OK || !tu.ctx) {
        LOG("[fsr] context %ux%u -> %ux%u failed (%u): the game's own scaling is used\n", rw, rh, ow, oh, rc);
        tu.ctx = NULL;
        tu.state = 2;
        return false;
    }
    struct ffxQueryGetProviderVersion pv = {0};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    tu.query(&tu.ctx, &pv.header);
    LOG("[fsr] %s: %ux%u -> %ux%u (%.0f ms)\n", pv.versionName ? pv.versionName : "FSR", rw, rh, ow, oh,
        (SDL_GetTicksNS() - t0) / 1e6);
    tu.rw = rw; tu.rh = rh; tu.ow = ow; tu.oh = oh;
    tu.frame = 0;
    return true;
}

// ================================================================== DLSS (NGX)
typedef struct NgxParam NgxParam;
typedef struct NgxHandle NgxHandle;
#define NGX_OK 1u
typedef struct {  // NVSDK_NGX_FeatureCommonInfo
    struct { const wchar_t *const *path; unsigned length; } paths;
    void *internal;
    struct { void (*callback)(const char *msg, int level, int feature); int min_level; bool only; } log;
} NgxCommon;
typedef struct {  // NVSDK_NGX_Resource_VK
    struct { VkImageView view; VkImage image; VkImageSubresourceRange range; VkFormat format; unsigned width, height; } img;
    int type;  // 0: image view
    bool read_write;
} NgxImage;
static struct {
    void *core;
    unsigned (*init)(const char *project, int engine, const char *engine_version, const wchar_t *data_path, VkInstance inst,
                     VkPhysicalDevice pd, VkDevice dev, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa, int sdk_version,
                     const NgxCommon *common);
    unsigned (*capability)(NgxParam **out);
    unsigned (*allocate)(NgxParam **out);
    unsigned (*destroy_params)(NgxParam *p);
    unsigned (*create)(VkDevice dev, VkCommandBuffer cb, int feature, NgxParam *p, NgxHandle **out);
    unsigned (*evaluate)(VkCommandBuffer cb, const NgxHandle *h, const NgxParam *p, void *progress);
    unsigned (*release)(NgxHandle *h);
    NgxParam *params;
    NgxHandle *feature;
    uint32_t rw, rh, ow, oh;
    bool inited;
} ngx;
#define NGX_VT(p, i) ((*(void ***)(p))[i])
static void ngx_set_ptr(NgxParam *p, const char *n, void *v) { ((void (*)(NgxParam *, const char *, void *))NGX_VT(p, 0))(p, n, v); }
static void ngx_set_i(NgxParam *p, const char *n, int v) { ((void (*)(NgxParam *, const char *, int))NGX_VT(p, 3))(p, n, v); }
static void ngx_set_ui(NgxParam *p, const char *n, unsigned v) { ((void (*)(NgxParam *, const char *, unsigned))NGX_VT(p, 4))(p, n, v); }
static void ngx_set_f(NgxParam *p, const char *n, float v) { ((void (*)(NgxParam *, const char *, float))NGX_VT(p, 6))(p, n, v); }
static unsigned ngx_get_i(NgxParam *p, const char *n, int *v) { return ((unsigned (*)(NgxParam *, const char *, int *))NGX_VT(p, 11))(p, n, v); }
static unsigned ngx_get_ui(NgxParam *p, const char *n, unsigned *v) { return ((unsigned (*)(NgxParam *, const char *, unsigned *))NGX_VT(p, 12))(p, n, v); }

static void ngx_log(const char *msg, int level, int feature) {
    (void)level; (void)feature;
    size_t n = msg ? strlen(msg) : 0;
    LOG("[dlss] %s%s", msg ? msg : "", n && msg[n - 1] == '\n' ? "" : "\n");
}

// The driver's NGX core: <driver store>\_nvngx.dll, its folder in the registry (HKLM, either key).
static void *ngx_core(void) {
    void *adv = LoadLibraryA("advapi32.dll");
    if (!adv) return NULL;
    long (*get)(void *key, const wchar_t *sub, const wchar_t *value, unsigned long flags, unsigned long *type, void *data, unsigned long *size) =
        (long (*)(void *, const wchar_t *, const wchar_t *, unsigned long, unsigned long *, void *, unsigned long *))GetProcAddress(adv, "RegGetValueW");
    if (!get) return NULL;
    static const wchar_t *const keys[2][2] = {
        {L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", L"NGXPath"},
        {L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath"}};
    for (int i = 0; i < 2; i++) {
        wchar_t path[600];
        unsigned long size = 520 * sizeof(wchar_t);
        if (get((void *)(uintptr_t)0x80000002u /* HKEY_LOCAL_MACHINE */, keys[i][0], keys[i][1], 2 /* RRF_RT_REG_SZ */, NULL, path, &size)) continue;
        size_t n = wcslen(path);
        static const wchar_t name[] = L"\\_nvngx.dll";
        memcpy(path + n, name, sizeof name);
        void *m = LoadLibraryW(path);
        if (m) return m;
    }
    return NULL;
}

static bool ngx_load(void) {
    if (tu.state) return tu.state == 1;
    tu.state = 2;
    ngx.core = ngx_core();
    if (!ngx.core) { LOG("[dlss] NVIDIA's NGX library was not found (no RTX driver?): the game's own scaling is used\n"); return false; }
    ngx.init = (void *)GetProcAddress(ngx.core, "NVSDK_NGX_VULKAN_Init_ProjectID_Ext");
    ngx.capability = (void *)GetProcAddress(ngx.core, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
    ngx.allocate = (void *)GetProcAddress(ngx.core, "NVSDK_NGX_VULKAN_AllocateParameters");
    ngx.destroy_params = (void *)GetProcAddress(ngx.core, "NVSDK_NGX_VULKAN_DestroyParameters");
    ngx.create = (void *)GetProcAddress(ngx.core, "NVSDK_NGX_VULKAN_CreateFeature1");
    ngx.evaluate = (void *)GetProcAddress(ngx.core, "NVSDK_NGX_VULKAN_EvaluateFeature");
    ngx.release = (void *)GetProcAddress(ngx.core, "NVSDK_NGX_VULKAN_ReleaseFeature");
    if (!ngx.init || !ngx.capability || !ngx.allocate || !ngx.create || !ngx.evaluate || !ngx.release) {
        LOG("[dlss] the driver's NGX library is too old for this (update the NVIDIA driver): the game's own scaling is used\n");
        return false;
    }
    // where nvngx_dlss.dll may be: tools\dlss, then the game's folder (NGX also knows the copy NVIDIA's app updates)
    static wchar_t dir[600], here[] = L".";
    const char *d = getenv("AC_DLSS_DIR");
    static const wchar_t *list[2];
    unsigned nlist = 0;
    if (d && *d && MultiByteToWideChar(65001, 0, d, -1, dir, 599) > 0) list[nlist++] = dir;
    list[nlist++] = here;
    NgxCommon common = {.paths = {list, nlist}, .log = {ngx_log, getenv("AC_DLSS_LOG") ? 1 : 0, true}};
    if (!getenv("AC_DLSS_LOG")) common.log.callback = NULL, common.log.only = false;
    // a project id of our own (a GUID), engine type "custom"
    unsigned rc = ngx.init("5d4c1a0e-7b6f-4c57-9d3e-ac31dc2026b1", 0, "1.0", here, g_vk.inst, g_vk.phys, g_vk.dev, vkGetInstanceProcAddr,
                           vkGetDeviceProcAddr, 0x15, &common);
    if (rc != NGX_OK) { LOG("[dlss] NGX did not start (%#x): the game's own scaling is used\n", rc); return false; }
    ngx.inited = true;
    NgxParam *cap = NULL;
    int available = 0, need_driver = 0;
    unsigned major = 0, minor = 0;
    if (ngx.capability(&cap) != NGX_OK || !cap) { LOG("[dlss] no capability parameters: the game's own scaling is used\n"); return false; }
    ngx_get_i(cap, "SuperSampling.Available", &available);
    ngx_get_i(cap, "SuperSampling.NeedsUpdatedDriver", &need_driver);
    if (!available) {
        int why = 0;
        ngx_get_i(cap, "SuperSampling.FeatureInitResult", &why);
        ngx_get_ui(cap, "SuperSampling.MinDriverVersionMajor", &major);
        ngx_get_ui(cap, "SuperSampling.MinDriverVersionMinor", &minor);
        if (need_driver) LOG("[dlss] DLSS needs NVIDIA driver %u.%u or newer: the game's own scaling is used\n", major, minor);
        else LOG("[dlss] DLSS is not available here (%#x; an RTX GPU and nvngx_dlss.dll in tools\\dlss are needed): the game's own "
                 "scaling is used\n", (unsigned)why);
        return false;
    }
    if (ngx.allocate(&ngx.params) != NGX_OK || !ngx.params) { LOG("[dlss] no parameters: the game's own scaling is used\n"); return false; }
    tu.state = 1;
    return true;
}

static bool ngx_feature(VkCommandBuffer cb, uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    if (ngx.feature && ngx.rw == rw && ngx.rh == rh && ngx.ow == ow && ngx.oh == oh) return true;
    if (ngx.feature) { vkDeviceWaitIdle(g_vk.dev); ngx.release(ngx.feature); ngx.feature = NULL; }
    NgxParam *p = ngx.params;
    // the quality level NGX is told: by the ratio (0 performance, 1 balanced, 2 quality, 3 ultra performance, 5 DLAA)
    float ratio = (float)ow / (float)rw;
    int quality = ratio < 1.05f ? 5 : ratio < 1.6f ? 2 : ratio < 1.85f ? 1 : ratio < 2.5f ? 0 : 3;
    // the presets NVIDIA's SDK library fills in when the application names none (its version 0x15)
    static const struct { const char *name; unsigned preset; } presets[] = {
        {"DLSS.Hint.Render.Preset.DLAA", 0x8000000b}, {"DLSS.Hint.Render.Preset.Quality", 0x8000000b},
        {"DLSS.Hint.Render.Preset.Balanced", 0x8000000b}, {"DLSS.Hint.Render.Preset.Performance", 0x8000000d},
        {"DLSS.Hint.Render.Preset.UltraQuality", 0x8000000b}, {"DLSS.Hint.Render.Preset.UltraPerformance", 0x8000000c}};
    for (unsigned i = 0; i < sizeof presets / sizeof *presets; i++) {
        unsigned v = 0;
        if (ngx_get_ui(p, presets[i].name, &v) != NGX_OK || !v) ngx_set_ui(p, presets[i].name, presets[i].preset);
    }
    ngx_set_ui(p, "CreationNodeMask", 1);
    ngx_set_ui(p, "VisibilityNodeMask", 1);
    ngx_set_ui(p, "Width", rw);
    ngx_set_ui(p, "Height", rh);
    ngx_set_ui(p, "OutWidth", ow);
    ngx_set_ui(p, "OutHeight", oh);
    ngx_set_i(p, "PerfQualityValue", quality);
    ngx_set_i(p, "DLSS.Feature.Create.Flags", 1 << 1);  // motion vectors at the render size; LDR colour, depth 0 near
    ngx_set_i(p, "DLSS.Enable.Output.Subrects", 0);
    uint64_t t0 = SDL_GetTicksNS();
    unsigned rc = ngx.create(g_vk.dev, cb, 1 /* SuperSampling */, p, &ngx.feature);
    if (rc != NGX_OK || !ngx.feature) {
        LOG("[dlss] feature %ux%u -> %ux%u failed (%#x): the game's own scaling is used\n", rw, rh, ow, oh, rc);
        ngx.feature = NULL;
        tu.state = 2;
        return false;
    }
    LOG("[dlss] %s: %ux%u -> %ux%u (%.0f ms)\n", quality == 5 ? "DLAA" : "DLSS", rw, rh, ow, oh, (SDL_GetTicksNS() - t0) / 1e6);
    ngx.rw = rw; ngx.rh = rh; ngx.ow = ow; ngx.oh = oh;
    tu.rw = rw; tu.rh = rh; tu.ow = ow; tu.oh = oh;
    tu.frame = 0;
    return true;
}

static void ngx_layout(VkCommandBuffer cb, TexBackend *b, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 m = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = b->image,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &m};
    vkCmdPipelineBarrier2(cb, &di);
}

static NgxImage ngx_image(TexBackend *b, VkImageView view, bool write) {
    NgxImage r = {.img = {view, b->image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, b->format, b->width, b->height}, .type = 0, .read_write = write};
    return r;
}

static float halton(uint32_t i, uint32_t base) {
    float f = 1, r = 0;
    for (; i; i /= base) { f /= base; r += f * (float)(i % base); }
    return r;
}

// Per presented frame: the next jitter (the sequence FSR documents: Halton 2, 3 over 8 * (output / render)^2 frames).
void vk_tu_frame(void) {
    if ((!tu.ctx && !ngx.feature) || !tu.rw) { tu.jx = tu.jy = 0; return; }
    float ratio = (float)tu.ow / (float)tu.rw;
    uint32_t phases = (uint32_t)(8.0f * ratio * ratio + 0.5f);
    if (phases < 8) phases = 8;
    tu.frame++;
    uint32_t i = tu.frame % phases + 1;
    tu.jx = halton(i, 2) - 0.5f;
    tu.jy = halton(i, 3) - 0.5f;
    static int off = -1;
    if (off < 0) off = getenv("AC_UPSCALE_NO_JITTER") != NULL;
    if (off) tu.jx = tu.jy = 0;
}

// The sub-pixel offset for a scene draw into a w x h target. False: that is not the upscaler's render size.
bool vk_tu_jitter(uint32_t w, uint32_t h, float *jx, float *jy) {
    if ((!tu.ctx && !ngx.feature) || w != tu.rw || h != tu.rh) return false;
    *jx = tu.jx; *jy = tu.jy;
    return true;
}

// Textures are sampled as if the scene were drawn at the output size (their mip levels would otherwise be chosen for
// the smaller render size and look soft once upscaled): the samplers' level-of-detail bias, log2(render / output).
float vk_tu_lod_bias(void) {
    static float bias = 1;
    if (bias > 0) {
        bias = 0;
        const char *q = getenv("AC_UPSCALE_QUALITY"), *e = getenv("AC_UPSCALE_LOD_BIAS");
        if (vk_tu_mode()) {
            float ratio = !q ? 1.5f : !strcmp(q, "native") ? 1.0f : !strcmp(q, "balanced") ? 1.7f : !strcmp(q, "performance") ? 2.0f :
                          !strcmp(q, "ultra") ? 3.0f : 1.5f;
            bias = e ? (float)atof(e) : -log2f(ratio);
            if (bias > 0) bias = 0;
            if (bias < -3) bias = -3;
        }
    }
    return bias;
}

bool vk_tu_running(void) { return (tu.ctx != NULL || ngx.feature != NULL) && tu.ok_frames > 0 && tu.state == 1; }

static struct FfxApiResource tu_resource(TexBackend *b, uint32_t format, uint32_t usage, uint32_t state) {
    struct FfxApiResource r = {0};
    r.resource = (void *)b->image;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = format;
    r.description.width = b->width;
    r.description.height = b->height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = usage;
    r.state = state;
    return r;
}

// color (tonemapped scene), depth and vel (the models' float velocity, or NULL) at the render size -> dst (display size).
// Outside any pass. False: nothing was done (the caller draws the game's own pass).
bool vk_tu_upscale(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, TexBackend *vel, TexBackend *dst, const TuCamera *cam) {
    int mode = vk_tu_mode();
    if (!mode || !(mode == 2 ? ngx_load() : tu_load())) return false;
    if (!color || !depth || !dst || !color->image || !depth->image || !dst->image) return false;
    if (color->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32 || !(depth->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return false;
    if (depth->width != color->width || depth->height != color->height) return false;
    if (dst->width < color->width || dst->height < color->height) return false;
    if (dst->format != VK_FORMAT_R8G8B8A8_UNORM && dst->format != VK_FORMAT_R8G8B8A8_SRGB) return false;
    if (vel && (vel->width != color->width || vel->height != color->height || !vel->image)) vel = NULL;
    uint32_t rw = color->width, rh = color->height, ow = dst->width, oh = dst->height;
    if (!tu_prep_ready()) { tu.state = 2; return false; }
    bool fresh = (mode == 2 ? !ngx.feature : !tu.ctx) || tu.rw != rw || tu.rh != rh || tu.ow != ow || tu.oh != oh;
    if (mode == 2 ? !ngx_feature(vk_cmd(), rw, rh, ow, oh) : !tu_context(rw, rh, ow, oh)) return false;

    TexBackend *d = tu_image(&tu.depth, VK_FORMAT_R32_SFLOAT, rw, rh);
    TexBackend *mv = tu_image(&tu.mv, VK_FORMAT_R16G16_SFLOAT, rw, rh);
    TexBackend *out = tu_image(&tu.out, VK_FORMAT_R8G8B8A8_UNORM, ow, oh);
    cb = vk_cmd();
    vk_full_barrier(cb);
    {   // depth and motion vectors
        VkDescriptorSet ds = vk_alloc_set(tu.prep_set);
        VkDescriptorImageInfo ii[4] = {
            {vk_default_sampler(false), tu_view(depth, VK_IMAGE_ASPECT_DEPTH_BIT), VK_IMAGE_LAYOUT_GENERAL},
            {vk_default_sampler(false), vel ? tu_view(vel, VK_IMAGE_ASPECT_COLOR_BIT) : tu_view(mv, VK_IMAGE_ASPECT_COLOR_BIT), VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, tu_view(d, VK_IMAGE_ASPECT_COLOR_BIT), VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, tu_view(mv, VK_IMAGE_ASPECT_COLOR_BIT), VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet w[4];
        for (uint32_t i = 0; i < 4; i++)
            w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = i,
                                          .descriptorCount = 1,
                                          .descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                          .pImageInfo = &ii[i]};
        vkUpdateDescriptorSets(g_vk.dev, 4, w, 0, NULL);
        static float dbg = -1;
        if (dbg < 0) { const char *e = getenv("AC_UPSCALE_MV"); dbg = e ? (float)atof(e) : 0; }  // (testing: 1 models only, 2 camera only)
        float pc[24] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (cam && cam->have) memcpy(pc, cam->reproject, 16 * sizeof(float));
        pc[16] = (float)rw; pc[17] = (float)rh; pc[18] = tu.jx; pc[19] = tu.jy;
        pc[20] = vel ? 1.0f : 0.0f; pc[21] = cam && cam->minus_one ? 1.0f : 0.0f; pc[22] = cam && cam->have ? 1.0f : 0.0f; pc[23] = dbg;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, tu.prep);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, tu.prep_layout, 0, 1, &ds, 0, NULL);
        vkCmdPushConstants(cb, tu.prep_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 96, pc);
        vkCmdDispatch(cb, (rw + 7) / 8, (rh + 7) / 8, 1);
        vk_full_barrier(cb);
    }
    uint64_t now = SDL_GetTicksNS();
    float dt = tu.last_ns ? (float)((now - tu.last_ns) / 1e6) : 16.7f;
    tu.last_ns = now;
    if (dt < 1) dt = 1;
    if (dt > 100) dt = 100;

    static float jsx = 0, jsy;
    if (jsx == 0) {  // (AC_UPSCALE_JITTER=<x sign>,<y sign>: testing)
        jsx = 1; jsy = 1;
        const char *e = getenv("AC_UPSCALE_JITTER");
        if (e) sscanf(e, "%f,%f", &jsx, &jsy);
    }
    if (mode == 2) {
        // (NGX reads its inputs as shader-read images and writes the output as a general one)
        ngx_layout(cb, color, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        ngx_layout(cb, d, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        ngx_layout(cb, mv, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        NgxImage ic = ngx_image(color, tu_view(color, VK_IMAGE_ASPECT_COLOR_BIT), false);
        NgxImage id = ngx_image(d, tu_view(d, VK_IMAGE_ASPECT_COLOR_BIT), false);
        NgxImage im = ngx_image(mv, tu_view(mv, VK_IMAGE_ASPECT_COLOR_BIT), false);
        NgxImage io = ngx_image(out, tu_view(out, VK_IMAGE_ASPECT_COLOR_BIT), true);
        NgxParam *p = ngx.params;
        ngx_set_ptr(p, "Color", &ic);
        ngx_set_ptr(p, "Output", &io);
        ngx_set_ptr(p, "Depth", &id);
        ngx_set_ptr(p, "MotionVectors", &im);
        ngx_set_f(p, "Jitter.Offset.X", jsx * tu.jx);
        ngx_set_f(p, "Jitter.Offset.Y", jsy * tu.jy);
        ngx_set_f(p, "Sharpness", 0);
        ngx_set_i(p, "Reset", fresh || (cam && cam->cut));
        ngx_set_f(p, "MV.Scale.X", 1);
        ngx_set_f(p, "MV.Scale.Y", 1);
        ngx_set_ptr(p, "TransparencyMask", NULL);
        ngx_set_ptr(p, "ExposureTexture", NULL);
        ngx_set_ptr(p, "DLSS.Input.Bias.Current.Color.Mask", NULL);
        ngx_set_f(p, "FrameTimeDeltaInMsec", dt);
        ngx_set_ui(p, "DLSS.Render.Subrect.Dimensions.Width", rw);
        ngx_set_ui(p, "DLSS.Render.Subrect.Dimensions.Height", rh);
        ngx_set_f(p, "DLSS.Pre.Exposure", 1);
        ngx_set_f(p, "DLSS.Exposure.Scale", 1);
        unsigned rc = ngx.evaluate(cb, ngx.feature, p, NULL);
        ngx_layout(cb, color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        ngx_layout(cb, d, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        ngx_layout(cb, mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        if (rc != NGX_OK) {
            LOG("[dlss] evaluate failed (%#x): the game's own scaling is used\n", rc);
            tu.state = 2;
            return false;
        }
        goto copy_out;
    }
    struct ffxDispatchDescUpscale dd = {0};
    dd.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dd.commandList = (void *)cb;
    // (every image is in the GENERAL layout, which is what FSR's "unordered access" state means on Vulkan; it moves
    // them to the layouts it reads and writes with and back)
    dd.color = tu_resource(color, FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dd.depth = tu_resource(d, FFX_API_SURFACE_FORMAT_R32_FLOAT, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dd.motionVectors = tu_resource(mv, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dd.output = tu_resource(out, FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    dd.jitterOffset.x = jsx * tu.jx;
    dd.jitterOffset.y = jsy * tu.jy;
    dd.motionVectorScale.x = 1;
    dd.motionVectorScale.y = 1;
    dd.renderSize.width = rw; dd.renderSize.height = rh;
    dd.upscaleSize.width = ow; dd.upscaleSize.height = oh;
    extern volatile float ac_upscale_sharpness;
    float sharp = ac_upscale_sharpness;
    dd.enableSharpening = sharp > 0.001f;
    dd.sharpness = sharp > 1 ? 1 : sharp < 0 ? 0 : sharp;
    dd.frameTimeDelta = dt;
    dd.preExposure = 1;
    dd.reset = fresh || (cam && cam->cut);
    dd.cameraNear = cam && cam->near_z > 0 ? cam->near_z : 0.1f;
    dd.cameraFar = 3.4e38f;  // (infinite: FFX_UPSCALE_ENABLE_DEPTH_INFINITE)
    dd.cameraFovAngleVertical = cam && cam->fov_y > 0.01f ? cam->fov_y : 0.8f;
    dd.viewSpaceToMetersFactor = 1;
    dd.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    if (getenv("AC_FSR_DEBUG") && atoi(getenv("AC_FSR_DEBUG")) > 1) dd.flags |= FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW;
    ffxReturnCode_t rc = tu.dispatch(&tu.ctx, &dd.header);
    if (rc != FFX_API_RETURN_OK) {
        LOG("[fsr] dispatch failed (%u): the game's own scaling is used\n", rc);
        tu.state = 2;
        return false;
    }
copy_out:
    vk_full_barrier(cb);
    VkImageCopy c = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .extent = {ow, oh, 1}};
    vkCmdCopyImage(cb, out->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    vk_full_barrier(cb);
    tu.ok_frames++;
    return true;
}

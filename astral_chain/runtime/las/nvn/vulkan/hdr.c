// HDR output (AC_HDR=1; issue #49): an HDR10 swapchain (Rec. 2020 primaries, PQ) and a game picture with its highlights
// above the SDR white. Settings: AC_HDR (read at start), AC_HDR_PEAK_NITS (the display's peak), AC_HDR_GAME_NITS (the
// picture's white, "paper white"), AC_HDR_UI_NITS (the HUD's and the menus' white) and AC_HDR_HUE (how far highlights
// keep the SDR picture's hue); the last four live (live_settings.h, the in-game menu). Without AC_HDR=1 nothing here runs.
//
// Ported from captainj-sparrow's Astral Chain PC recomp (https://github.com/captainj-sparrow/astral-chain-pc-recomp,
// runtime/vk_hdr.c and runtime/shaders/hdr_tonemap.frag, hdr_encode.comp, hdr_ui.frag; MIT License, Copyright (c) 2026
// Valiantsin Dzerakh, the licence text in LICENSE-astral-chain-pc-recomp). Their findings and their shaders' maths are
// used as they are; the code around them is rewritten for this renderer (its translator writes GLSL, so the tonemap is
// replaced as GLSL, and its present draws the window texture inside the window image's rendering). Their tonemap's
// approach follows ShortFuse's RenoDX (github.com/clshortfuse/renodx, MIT): grade in SDR, then restore the highlights by
// a luminance ratio.
//
// The game's picture (their measurements, the same game): it tonemaps its scene (Hable's curve or Uchimura's, a
// vignette, the sRGB encode, a 16x16x16 colour LUT) into an R11G11B10F target at the output's size, sRGB-encoded; its
// SMAA then reads that target and writes the 8-bit window texture it presents, and the HUD and menus draw into the
// window texture afterwards. So:
// 1. vk_hdr_tonemap_glsl: the tonemap's translated fragment shader (filtershadertonemap; recognised by its constants and
//    its four textures, not by a hash) gets a main of our own that computes the game's SDR result and, while the HDR
//    swapchain shows the scene, writes the picture with its highlights (above 1, up to the peak over the game's white)
//    into the float target, still sRGB-encoded. Its parameters come in the tint block (binding 19, vk_hdr_tint).
// 2. vk_hdr_reads: a draw into a presented window texture that samples a float texture of the window's size (the SMAA
//    blend) marks that texture as the frame's scene. A frame without one (movies, loading screens) has none: it is
//    shown at the HUD's white, and the tonemap writes the game's SDR result until a present has the scene again, so
//    the window texture never holds a clipped HDR picture on its own.
// 3. vk_hdr_encode (the present, exec.c post_process): the window texture to PQ code values in an RGBA16F image, which
//    FSR 1 or the present draw then scale into the swapchain. A window pixel that still equals the clamped scene pixel,
//    or its blend with one axis neighbour (SMAA), is the scene, shown at the game's white with its highlights from the
//    scene; any other pixel (the HUD, the game's menus, its dimming behind a pop-up) at the HUD's white.
// 4. vk_hdr_ui_*: the in-game menu (las/menu.cpp: settings, frame rate, credit) draws into an 8-bit image, which is
//    blended onto the swapchain at the HUD's white.
// Test: AC_HDR_FORCE=1 runs all of it on an SDR swapchain (wrong colours: PQ codes shown as they are);
// AC_HDR_DUMP_AT=<frame>: the encoded image of that frame to hdr_<frame>.bin (RGBA16F, its size first, two uint32).
#include "vk.h"
#include "../../rt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern volatile int ac_hdr_peak, ac_hdr_game, ac_hdr_ui, ac_hdr_hue, ac_hdr_state;  // (live_settings.h)

// ---------------------------------------------------------------- settings
static int env_int(const char *name, int def, int lo, int hi) {
    const char *e = getenv(name);
    int v = e && *e ? atoi(e) : def;
    return v < lo ? lo : v > hi ? hi : v;
}

int vk_hdr_wanted(void) {
    static int w = -1;
    if (w < 0) { const char *e = getenv("AC_HDR"); w = e && !strcmp(e, "1"); }
    return w;
}

static int forced(void) {
    static int f = -1;
    if (f < 0) { const char *e = getenv("AC_HDR_FORCE"); f = e && !strcmp(e, "1"); }
    return f;
}

typedef struct { float peak, game, ui, hue; } Nits;
static Nits nits(void) {
    if (ac_hdr_peak < 0) ac_hdr_peak = env_int("AC_HDR_PEAK_NITS", 1000, 400, 10000);
    if (ac_hdr_game < 0) ac_hdr_game = env_int("AC_HDR_GAME_NITS", 203, 80, 500);
    if (ac_hdr_ui < 0) ac_hdr_ui = env_int("AC_HDR_UI_NITS", 203, 80, 500);
    if (ac_hdr_hue < 0) ac_hdr_hue = env_int("AC_HDR_HUE", 80, 0, 100);
    Nits n = {(float)ac_hdr_peak, (float)ac_hdr_game, (float)ac_hdr_ui, (float)ac_hdr_hue / 100.0f};
    if (n.peak < 80) n.peak = 80;
    if (n.game < 80) n.game = 80;
    if (n.ui < 80) n.ui = 80;
    return n;
}

static int g_active;  // the swapchain is HDR10 (vk_hdr_swapchain), or AC_HDR_FORCE
int vk_hdr_active(void) { return g_active; }

// ---------------------------------------------------------------- swapchain
// With AC_HDR=1: the HDR10 format among the surface's (10-bit UNORM, ST 2084), 1 with *out set; else 0, the reason
// logged once.
int vk_hdr_surface_format(const VkSurfaceFormatKHR *f, uint32_t n, VkSurfaceFormatKHR *out) {
    if (!vk_hdr_wanted()) return 0;
    static int logged;
    if (!logged) {
        char b[1024];
        int o = 0;
        for (uint32_t i = 0; i < n && o < (int)sizeof b - 32; i++) o += snprintf(b + o, sizeof b - (size_t)o, " %d/%d", (int)f[i].format, (int)f[i].colorSpace);
        LOG("[hdr] surface formats (VkFormat/VkColorSpaceKHR):%s\n", b);
    }
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < n; i++)
            if (f[i].colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
                f[i].format == (pass ? VK_FORMAT_A2R10G10B10_UNORM_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32)) {
                *out = f[i];
                if (!logged) LOG("[hdr] HDR10 swapchain (format %d)\n", (int)f[i].format);
                logged = 1;
                return 1;
            }
    if (!logged)
        LOG("[hdr] the display offers no HDR10 format, so the picture stays SDR (turn on \"Use HDR\" in Windows' display "
            "settings for this screen)\n");
    logged = 1;
    return 0;
}

// The swapchain was made (device.c create_swapchain): hdr10 1 for an HDR10 one.
void vk_hdr_swapchain(int hdr10) {
    g_active = hdr10 || (vk_hdr_wanted() && forced());
    ac_hdr_state = !vk_hdr_wanted() ? 0 : hdr10 ? 1 : forced() ? 3 : 2;
    static int logged = -1;
    if (vk_hdr_wanted() && forced() && logged != g_active) LOG("[hdr] AC_HDR_FORCE: HDR output on an SDR swapchain (test)\n");
    logged = g_active;
}

// The display's peak and the picture's white for the swapchain, again when they change (each present).
void vk_hdr_metadata(int new_swapchain) {
    static int sent_peak = -1, sent_game = -1;
    if (!g_vk.hdr_metadata || ac_hdr_state != 1) return;
    Nits n = nits();
    if (!new_swapchain && sent_peak == (int)n.peak && sent_game == (int)n.game) return;
    sent_peak = (int)n.peak, sent_game = (int)n.game;
    static PFN_vkSetHdrMetadataEXT set;
    if (!set) set = (PFN_vkSetHdrMetadataEXT)vkGetDeviceProcAddr(g_vk.dev, "vkSetHdrMetadataEXT");
    if (!set) return;
    VkHdrMetadataEXT m = {.sType = VK_STRUCTURE_TYPE_HDR_METADATA_EXT, .displayPrimaryRed = {0.708f, 0.292f},
                          .displayPrimaryGreen = {0.170f, 0.797f}, .displayPrimaryBlue = {0.131f, 0.046f},
                          .whitePoint = {0.3127f, 0.3290f}, .maxLuminance = n.peak, .minLuminance = 0.0f,
                          .maxContentLightLevel = n.peak, .maxFrameAverageLightLevel = n.game};
    set(g_vk.dev, 1, &g_vk.swapchain, &m);
}

// ---------------------------------------------------------------- 1. the tonemap
// The game's tonemap as our translator writes it: cb0 (driver constants: row 86 holds the address of the auto
// exposure's average, read through the guest memory buffer), cb1 (its curve constants, Hable's), cb4 (its parameters),
// t0 the lit scene, t1 the bloom, t2 the adaptation, t3 the colour LUT (256x16). The capture variant (photos) has a
// fifth texture and is left alone.
static const char TONEMAP_MAIN[] =
    "layout(std140, set = 1, binding = 19) uniform HdrTint { vec4 hdr_p[2]; };\n"  // y: peak / game white (1: SDR), z: hue
    "#define C4(i) uintBitsToFloat(cb4[i])\n"
    "#define C1(i) uintBitsToFloat(cb1[i])\n"
    "float hdr_sat(float x) { return isnan(x) ? 0.0 : clamp(x, 0.0, 1.0); }\n"
    "float hdr_lum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }\n"
    "float hdr_maxc(vec3 c) { return max(c.r, max(c.g, c.b)); }\n"
    // the sRGB encode as the game writes it (c1[1].z: the linear part's end, c1[1].w: 1.055), and continued above 1
    "float hdr_encode1(float c, float lim, float k) {\n"
    "    float e = hdr_sat(c * 12.92);\n"
    "    if (!(c <= lim)) e = hdr_sat(exp2(log2(abs(c)) * 0.416666657) * k - 0.055);\n"
    "    return e;\n"
    "}\n"
    "vec3 hdr_encode_ext(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, greaterThan(c, vec3(0.0031308))); }\n"
    "vec3 hdr_decode(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045))); }\n"
    // Hable's curve with the game's constants, white-scaled; the bloom is added after it
    "vec3 hdr_hable(vec3 x, vec3 bloom) {\n"
    "    vec4 a = C1(0), b = C1(1);\n"
    "    vec3 num = x * (x * a.y + 0.05) + a.w, den = x * (x * a.y + 0.5) + a.z;\n"
    "    return (num / den + b.x) * b.y + bloom;\n"
    "}\n"
    // Uchimura's curve: toe, linear part, shoulder rising to the ceiling ceil_ (the game's c4[4].x in SDR)
    "vec3 hdr_uchimura(vec3 x, float ceil_) {\n"
    "    vec4 p4 = C4(4), p5 = C4(5);\n"
    "    float inv_m = 1.0 / p4.z, room = p5.y + (ceil_ - p4.x);\n"
    "    vec3 t = clamp(x * inv_m, 0.0, 1.0);\n"
    "    vec3 w0 = (t * -2.0 + 3.0) * t * t;\n"
    "    vec3 toe = exp2(log2(abs(x * inv_m)) * p4.w) * p5.z + p5.w;\n"
    "    vec3 w2 = vec3(greaterThan(x, vec3(p5.x)));\n"
    "    vec3 sh = exp2((p5.x - x) * p4.y * (1.0 / room) * 1.44269502) * -room + ceil_;\n"
    "    vec3 lin = (x - p4.z) * p4.y + p5.z;\n"
    "    return w2 * sh + lin * (w0 - w2) + (w0 * -toe + toe);\n"
    "}\n"
    "void main() {\n"
    "    vec2 at = v0.xy;\n"
    "    vec3 scene = texture(t0, at).rgb, bloom = texture(t1, at).rgb;\n"
    "    float ratio = hdr_p[0].y;\n"
    "    bool hdr = ratio > 1.0;\n"
    "    vec4 c4_0 = C4(0), c4_1 = C4(1), c4_2 = C4(2), c4_6 = C4(6);\n"
    "    vec3 v = scene, t = scene, th = scene;\n"  // th: the extended picture
    "    if (c4_6.w == 0.0) {\n"
    "        if (c4_1.w >= 0.0) {\n"  // the auto exposure's average, in guest memory
    "            uint64_t ga = (uint64_t(cb0[86][1]) << 32 | uint64_t(cb0[86][0])) - 0x7200000000ul;\n"
    "            float avg = ga < 0x100000000ul ? uintBitsToFloat(gmem_.w[uint(ga >> 2)]) : 0.0;\n"
    "            vec3 s = scene * (1.0 / avg * c4_1.w);\n"
    "            v = s * s * C1(0).x + s;\n"
    "        }\n"
    "        t = hdr_hable(v * exp2(c4_2.x), bloom);\n"
    "        th = t;\n"
    "        if (hdr) {\n"  // a roll-off to the peak where the game clamps
    "            float top = hdr_maxc(th), knee = max(1.0, 0.5 * ratio);\n"
    "            if (top > knee) th *= (knee + (ratio - knee) * (1.0 - exp(-(top - knee) / (ratio - knee)))) / top;\n"
    "        }\n"
    "    }\n"
    "    if (0.0 < c4_6.z) {\n"
    "        float adapt = texture(t2, vec2(0.5)).g;\n"
    "        t = adapt * ((bloom * c4_2.z + t) * c4_0.y);\n"
    "        th = adapt * ((bloom * c4_2.z + th) * c4_0.y);\n"
    "    }\n"
    "    if (0.0 < c4_6.w) {\n"
    "        float ceil_ = C4(4).x;\n"
    "        vec3 x = t;\n"
    "        t = hdr_uchimura(x, ceil_);\n"
    "        th = hdr ? hdr_uchimura(x, ceil_ * ratio) : t;\n"
    "    }\n"
    "    vec4 c4_3 = C4(3);\n"
    "    if (0.0 < c4_3.x) {\n"
    "        float inv = 1.0 / c4_3.x;\n"
    "        vec2 d = 0.5 - at;\n"
    "        float vig = exp2(log2(abs(hdr_sat(inv * -sqrt(d.y * d.y + d.x * d.x) + inv))) * c4_3.y);\n"
    "        t *= vig;\n"
    "        th *= vig;\n"
    "    }\n"
    // the game's SDR grade
    "    vec3 c = vec3(hdr_sat(t.r), hdr_sat(t.g), hdr_sat(t.b));\n"
    "    float lim = C1(1).z, k = C1(1).w;\n"
    "    vec3 e = vec3(hdr_encode1(c.r, lim, k), hdr_encode1(c.g, lim, k), hdr_encode1(c.b, lim, k));\n"
    "    float slice = floor(e.b * 14.9998999);\n"  // blue picks the slice, red runs along it, green down
    "    float u = slice * 0.0625 + e.r * 0.05859375, w = e.g * 0.9375 + 0.03125;\n"
    "    vec3 hi = texture(t3, vec2(u + 0.064453125, w)).rgb, lo = texture(t3, vec2(u + 0.001953125, w)).rgb;\n"
    "    vec3 g = (hi - lo) * (e.b * 15.0 - slice) + lo;\n"
    "    g = clamp(exp2(log2(abs(g)) * c4_1.x), 0.0, 1.0);\n"
    "    vec3 fade = vec3(0.0, 0.0780000016, 0.156800002);\n"
    "    vec3 sdr = (g - fade) * c4_0.w + fade;\n"
    "    if (hdr) {\n"  // the highlights the clamp took, as a luminance ratio on the graded colour
    "        float ys = hdr_lum(c), yh = hdr_lum(max(th, vec3(0.0)));\n"
    "        float gain = ys > 1e-5 ? yh / ys : 1.0;\n"
    "        if (gain > 1.0001) {\n"
    "            vec3 lin = hdr_decode(max(sdr, vec3(0.0))) * gain;\n"  // the SDR hue at the HDR luminance
    "            float yl = hdr_lum(lin), yt = hdr_lum(max(th, vec3(0.0)));\n"
    "            float sat = (1.0 - clamp(hdr_p[0].z, 0.0, 1.0)) * smoothstep(0.5, 1.5, hdr_maxc(th));\n"
    "            if (sat > 0.0 && yt > 1e-5) lin = mix(lin, max(th, vec3(0.0)) * (yl / yt), sat);\n"
    "            sdr = hdr_encode_ext(lin);\n"
    "        }\n"
    "    }\n"
    "    c0 = vec4(sdr, 0.0);\n"
    "}\n";

// src: a translated fragment shader. The tonemap gets our main (a new string, malloc'd); anything else: NULL.
char *vk_hdr_tonemap_glsl(const char *src) {
    if (!vk_hdr_wanted() || !src) return NULL;
    // the LUT's slice step and offsets, the slice index scale, the fade's green and blue, the sRGB encode's 1/2.4
    static const char *const want[] = {"0x3d700000u", "0x3d840000u", "0x3b000000u", "0x416fff97u", "0x3d9fbe77u",
                                       "0x3e20902eu", "0x3ed55555u", "uniform sampler2D t3;", "uniform CB4 { uvec4 cb4[16]; }",
                                       "uniform CB1 { uvec4 cb1[16]; }", "uniform CB0 { uvec4 cb0[96]; }", "} gmem_;",
                                       "cb0[86][0]", "- 0x7200000000ul;", "layout(location = 0) in vec4 v0;",
                                       "layout(location = 0) out vec4 c0;"};
    for (size_t i = 0; i < sizeof want / sizeof want[0]; i++)
        if (!strstr(src, want[i])) return NULL;
    if (strstr(src, "uniform sampler2D t4;") || strstr(src, "binding = 19)") || strstr(src, "out vec4 c1;")) return NULL;
    const char *m = strstr(src, "\nvoid main() {");
    if (!m) return NULL;
    size_t head = (size_t)(m - src) + 1;
    char *out = malloc(head + sizeof TONEMAP_MAIN);
    if (!out) return NULL;
    memcpy(out, src, head);
    memcpy(out + head, TONEMAP_MAIN, sizeof TONEMAP_MAIN);
    static int swaps;
    LOG("[hdr] the game's tonemap shader runs as ours (%d so far)\n", ++swaps);
    return out;
}

static int g_scene_seen;  // the last present had the scene (vk_hdr_present_scene)

// The tonemap's tint block: y the peak over the picture's white (1 on an SDR swapchain or while the present gets no
// scene), z the hue correction.
void vk_hdr_tint(float tint[8]) {
    Nits n = nits();
    memset(tint, 0, 8 * sizeof *tint);
    tint[1] = g_active && g_scene_seen && n.peak > n.game ? n.peak / n.game : 1.0f;
    tint[2] = n.hue;
}

// ---------------------------------------------------------------- 2. the frame's scene
// Per presented window texture: the float texture a draw into it sampled since its last present.
static struct { TexBackend *win, *scene; int fresh; } recs[4];
static int nrecs;

int vk_hdr_tracking(void) { return g_active; }

static int is_float(VkFormat f) {
    return f == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_R32G32B32A32_SFLOAT;
}

// A draw into the presented window texture win sampled src.
void vk_hdr_reads(TexBackend *win, TexBackend *src) {
    if (!g_active || !win || !src || !src->image || !is_float(src->format) || src->samples > 1) return;
    // the window's size, or smaller with its shape where that draw also scales
    if (src->width > win->width || src->height > win->height || src->width * 4 < win->width ||
        llabs((long long)src->width * win->height - (long long)src->height * win->width) * 50 > (long long)win->width * win->height)
        return;
    int r = -1;
    for (int i = 0; i < nrecs; i++) if (recs[i].win == win) r = i;
    if (r < 0) {
        if (nrecs == 4) memmove(recs, recs + 1, 3 * sizeof recs[0]), nrecs = 3;
        r = nrecs++;
        recs[r].win = win;
    }
    recs[r].scene = src, recs[r].fresh = 1;
}

// The present of win: the scene drawn into it since its last present, or NULL.
static TexBackend *present_scene(TexBackend *win) {
    TexBackend *s = NULL;
    for (int i = 0; i < nrecs; i++)
        if (recs[i].win == win) {
            if (recs[i].fresh && recs[i].scene && recs[i].scene->image) s = recs[i].scene;
            recs[i].fresh = 0;
        }
    g_scene_seen = s != NULL;
    static int logged;
    if (!logged && s) logged = 1, LOG("[hdr] the present reads the game's scene beside its window texture\n");
    return s;
}

void vk_hdr_texture_gone(TexBackend *b) {  // (textures.c: a game texture is destroyed)
    for (int i = 0; i < nrecs; i++) {
        if (recs[i].scene == b) recs[i].scene = NULL, recs[i].fresh = 0;
        if (recs[i].win == b) recs[i] = recs[--nrecs], i--;
    }
}

// ---------------------------------------------------------------- 3. the present's encode
static const char ENCODE_GLSL[] =
    "#version 450\n"
    // the window texture (src: 8-bit, p.z = 1: it holds sRGB codes) and the scene (p.w = 1; float, the window's size
    // or smaller, sRGB-encoded and continued above 1) into PQ code values in Rec. 2020. p.x: the HUD's white in nits,
    // p.y: the display's peak, q.x: the picture's white. PQ: SMPTE ST 2084; Rec. 709 to Rec. 2020: ITU-R BT.2087.
    "layout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(binding = 0) uniform sampler2D src;\n"
    "layout(binding = 1, rgba16f) uniform writeonly image2D dst;\n"
    "layout(binding = 2) uniform sampler2D scene;\n"
    "layout(push_constant) uniform P { ivec2 offset; ivec2 size; vec4 p; vec4 q; } pc;\n"
    "vec3 srgb_decode(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045))); }\n"
    "vec3 srgb_encode(vec3 c) { return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, greaterThan(c, vec3(0.0031308))); }\n"
    "vec3 pq(vec3 nits) {\n"
    "    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;\n"
    "    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));\n"
    "    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));\n"
    "}\n"
    "void main() {\n"
    "    ivec2 at = ivec2(gl_GlobalInvocationID.xy);\n"
    "    if (any(greaterThanEqual(at, pc.size))) return;\n"
    "    ivec2 px = at + pc.offset;\n"
    "    vec3 c = max(texelFetch(src, px, 0).rgb, vec3(0.0));\n"
    "    vec3 code = pc.p.z > 0.5 ? clamp(c, 0.0, 1.0) : srgb_encode(clamp(c, 0.0, 1.0));\n"  // the window pixel as stored
    "    vec3 lin = pc.p.z > 0.5 ? srgb_decode(code) : c;\n"
    "    vec3 nits = lin * pc.p.x;\n"
    "    if (pc.p.w > 0.5) {\n"
    "        vec2 texel = 1.0 / vec2(textureSize(src, 0)), uv = (vec2(px) + 0.5) * texel;\n"
    "        vec3 s = max(textureLod(scene, uv, 0.0).rgb, vec3(0.0)), sc = min(s, vec3(1.0));\n"
    "        const float tol = 1.5 / 255.0;\n"  // the window texture's rounding
    // the scene where the window pixel is the clamped scene pixel, or a blend of it with at most half of one axis
    // neighbour (SMAA's neighbourhood blend); a HUD or menu pixel is neither
    "        bool is_scene = all(lessThanEqual(abs(code - sc), vec3(tol)));\n"
    "        const vec2 axis[4] = vec2[4](vec2(1, 0), vec2(-1, 0), vec2(0, 1), vec2(0, -1));\n"
    "        for (int i = 0; i < 4 && !is_scene; i++) {\n"
    "            vec3 d = min(textureLod(scene, uv + axis[i] * texel, 0.0).rgb, vec3(1.0)) - sc;\n"
    "            float dd = dot(d, d);\n"
    "            if (dd < tol * tol) continue;\n"
    "            float t = clamp(dot(code - sc, d) / dd, 0.0, 0.5);\n"
    "            is_scene = all(lessThanEqual(abs(code - (sc + t * d)), vec3(tol)));\n"
    "        }\n"
    "        if (is_scene) nits = (any(greaterThan(s, vec3(1.0))) ? srgb_decode(s) : lin) * pc.q.x;\n"
    "    }\n"
    "    const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);\n"
    "    nits = min(max(to2020 * nits, vec3(0.0)), vec3(pc.p.y));\n"
    "    imageStore(dst, at, vec4(pq(nits), 1.0));\n"
    "}\n";

static const char FULLSCREEN_GLSL[] =
    "#version 450\n"
    "layout(location = 0) out vec2 uv;\n"
    "void main() {\n"
    "    uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
    "    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

// The menu image (premultiplied sRGB codes over a clear 0, as ImGui blends) onto the swapchain: p.x the HUD's white
// in nits; p.y = 1 for PQ, 0 for an SDR swapchain (the codes as they are).
static const char UI_GLSL[] =
    "#version 450\n"
    "layout(location = 0) in vec2 uv;\n"
    "layout(location = 0) out vec4 color;\n"
    "layout(binding = 0) uniform sampler2D ui;\n"
    "layout(push_constant) uniform P { vec4 p; } pc;\n"
    "vec3 srgb_decode(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045))); }\n"
    "vec3 pq(vec3 nits) {\n"
    "    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;\n"
    "    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));\n"
    "    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));\n"
    "}\n"
    "void main() {\n"
    "    vec4 u = texture(ui, uv);\n"
    "    if (u.a <= 0.0) discard;\n"
    "    vec3 c = clamp(u.rgb / u.a, 0.0, 1.0);\n"
    "    if (pc.p.y > 0.5) {\n"
    "        const mat3 to2020 = mat3(0.6274, 0.0691, 0.0164, 0.3293, 0.9195, 0.0880, 0.0433, 0.0114, 0.8956);\n"
    "        c = pq(max(to2020 * srgb_decode(c), vec3(0.0)) * pc.p.x);\n"
    "    }\n"
    "    color = vec4(c, u.a);\n"
    "}\n";

typedef struct { VkImage img; VmaAllocation mem; VkImageView view; uint32_t w, h; } Img;

static struct {
    int tried, ok;
    VkSampler nearest, linear;
    VkDescriptorSetLayout dsl_enc, dsl_ui;
    VkPipelineLayout lay_enc, lay_ui;
    VkPipeline enc, ui;
    VkFormat ui_fmt;
    Img out, menu;
    TexBackend out_tb;  // (out as a TexBackend, for the post passes' views)
} H;

static void img_free(Img *m) {
    if (m->view) vk_defer_view(m->view);
    if (m->img) vk_defer_image(m->img, m->mem);
    *m = (Img){0};
}

static int img_ensure(VkCommandBuffer cb, Img *m, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage) {
    if (m->img && m->w == w && m->h == h) return 1;
    img_free(m);
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(g_vk.vma, &ii, &ai, &m->img, &m->mem, NULL) != VK_SUCCESS) { m->img = VK_NULL_HANDLE; return 0; }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = m->img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = f, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &m->view) != VK_SUCCESS) { m->view = VK_NULL_HANDLE; img_free(m); return 0; }
    vk_image_init_layout(cb, m->img, VK_IMAGE_ASPECT_COLOR_BIT);
    m->w = w, m->h = h;
    return 1;
}

static VkDescriptorSetLayout set_layout(const VkDescriptorType *t, uint32_t n, VkShaderStageFlags stage) {
    VkDescriptorSetLayoutBinding b[3];
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

static int init(void) {
    H.tried = 1;
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .maxLod = 0};
    VkSamplerCreateInfo sl = si;
    sl.magFilter = sl.minFilter = VK_FILTER_LINEAR;
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, I = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const VkDescriptorType te[3] = {S, I, S};
    if (vkCreateSampler(g_vk.dev, &si, NULL, &H.nearest) != VK_SUCCESS || vkCreateSampler(g_vk.dev, &sl, NULL, &H.linear) != VK_SUCCESS ||
        !(H.dsl_enc = set_layout(te, 3, VK_SHADER_STAGE_COMPUTE_BIT)) || !(H.dsl_ui = set_layout(&S, 1, VK_SHADER_STAGE_FRAGMENT_BIT)) ||
        !(H.lay_enc = pipe_layout(&H.dsl_enc, VK_SHADER_STAGE_COMPUTE_BIT, 48)) ||
        !(H.lay_ui = pipe_layout(&H.dsl_ui, VK_SHADER_STAGE_FRAGMENT_BIT, 16))) {
        LOG("[hdr] the present passes could not be made: the picture goes out unconverted\n");
        return 0;
    }
    VkShaderModule m = vk_compile_glsl(ENCODE_GLSL, STAGE_COMPUTE, "hdr_encode");
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = H.lay_enc};
    VkResult r = m ? vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &H.enc) : VK_ERROR_UNKNOWN;
    if (m) vkDestroyShaderModule(g_vk.dev, m, NULL);
    if (r != VK_SUCCESS) {
        LOG("[hdr] the encode pipeline could not be made: the picture goes out unconverted\n");
        return 0;
    }
    return H.ok = 1;
}

static struct { VkSpan s; uint64_t seq; uint32_t w, h; long frame; int pending; } D;

static void dump(VkCommandBuffer cb, const Img *m) {  // AC_HDR_DUMP_AT: copied now, written after the present
    static long at = -1;
    if (at < 0) { const char *e = getenv("AC_HDR_DUMP_AT"); at = e ? atol(e) : 0; }
    if (!at || D.pending || (long)vk_frame_counter() != at) return;
    D.s = vk_readback((uint64_t)m->w * m->h * 8);
    VkBufferImageCopy rg = {.bufferOffset = D.s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {m->w, m->h, 1}};
    vkCmdCopyImageToBuffer(cb, m->img, VK_IMAGE_LAYOUT_GENERAL, D.s.buf, 1, &rg);
    vk_full_barrier(cb);
    D.seq = vk_pending_seq(), D.w = m->w, D.h = m->h, D.frame = at, D.pending = 1;
}

void vk_hdr_after_present(void) {  // (device.c, after the present was submitted)
    if (!D.pending) return;
    D.pending = 0;
    vk_wait_seq(D.seq);
    char path[64];
    snprintf(path, sizeof path, "hdr_%ld.bin", D.frame);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t wh[2] = {D.w, D.h};
    fwrite(wh, 4, 2, f);
    fwrite(D.s.ptr, 1, (size_t)D.w * D.h * 8, f);
    fclose(f);
    LOG("[hdr] frame %ld: the encoded picture written to %s\n", D.frame, path);
}

// Outside any rendering: the window texture win (its view raw: sRGB codes as UNORM) into PQ code values; the encoded
// image as a TexBackend of win's size (RGBA16F), or NULL (the picture goes out as it is).
TexBackend *vk_hdr_encode(VkCommandBuffer cb, TexBackend *win, VkImageView raw) {
    if (!g_active || !win || !raw) return NULL;
    if (!H.tried) init();
    if (!H.ok || !img_ensure(cb, &H.out, win->width, win->height, VK_FORMAT_R16G16B16A16_SFLOAT,
                             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return NULL;
    TexBackend *scene = present_scene(win);
    static const VkComponentMapping id = {0};
    VkImageView sv = scene ? vk_image_view(scene, VK_IMAGE_VIEW_TYPE_2D, scene->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT) : VK_NULL_HANDLE;
    if (!sv) scene = NULL;
    Nits n = nits();
    VkFormat f = win->format;
    int decode = f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_SRGB ||
                 f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_UNORM_PACK32 || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    vk_full_barrier(cb);
    VkDescriptorSet set = vk_alloc_set(H.dsl_enc);
    VkDescriptorImageInfo ii[3] = {{H.nearest, raw, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, H.out.view, VK_IMAGE_LAYOUT_GENERAL},
                                   {H.linear, sv ? sv : raw, VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorType ty[3] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER};
    VkWriteDescriptorSet w[3];
    for (uint32_t i = 0; i < 3; i++)
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = i, .descriptorCount = 1,
                                      .descriptorType = ty[i], .pImageInfo = &ii[i]};
    vkUpdateDescriptorSets(g_vk.dev, 3, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.enc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, H.lay_enc, 0, 1, &set, 0, NULL);
    struct { int32_t offset[2], size[2]; float p[4], q[4]; } pc = {
        {0, 0}, {(int32_t)win->width, (int32_t)win->height}, {n.ui, n.peak, decode ? 1.0f : 0.0f, scene ? 1.0f : 0.0f}, {n.game, 0, 0, 0}};
    vkCmdPushConstants(cb, H.lay_enc, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (win->width + 7) / 8, (win->height + 7) / 8, 1);
    vk_full_barrier(cb);
    dump(cb, &H.out);
    TexBackend *o = &H.out_tb;
    if (o->image != H.out.img) {
        if (o->image) vk_views_forget(o);
        memset(o, 0, sizeof *o);
        o->image = H.out.img, o->format = VK_FORMAT_R16G16B16A16_SFLOAT, o->width = H.out.w, o->height = H.out.h;
        o->depth = o->layers = o->levels = o->samples = 1, o->aspect = VK_IMAGE_ASPECT_COLOR_BIT, o->base_type = VK_IMAGE_VIEW_TYPE_2D;
    }
    return o;
}

// ---------------------------------------------------------------- 4. the in-game menu
// The menu goes through an image of its own: on an HDR10 swapchain, or with AC_HDR=1 on an SDR swapchain of another
// format than the menu's (vk_hdr_ui_format).
int vk_hdr_ui_needed(void) { return vk_hdr_wanted() && (g_active || g_vk.sc_format != VK_FORMAT_B8G8R8A8_UNORM); }
VkFormat vk_hdr_ui_format(void) { return vk_hdr_wanted() ? VK_FORMAT_B8G8R8A8_UNORM : g_vk.sc_format; }

static int ui_pipeline(VkFormat fmt) {
    if (H.ui && H.ui_fmt == fmt) return 1;
    if (H.ui) { vkDeviceWaitIdle(g_vk.dev); vkDestroyPipeline(g_vk.dev, H.ui, NULL); H.ui = VK_NULL_HANDLE; }
    VkShaderModule vs = vk_compile_glsl(FULLSCREEN_GLSL, STAGE_VERTEX, "hdr_fullscreen"), fs = vk_compile_glsl(UI_GLSL, STAGE_FRAGMENT, "hdr_ui");
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
    VkPipelineColorBlendAttachmentState ba = {.blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
                                              .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
                                              .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                              .alphaBlendOp = VK_BLEND_OP_ADD, .colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkGraphicsPipelineCreateInfo gi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2, .pStages = stg,
                                       .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                       .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = H.lay_ui};
    VkResult r = vs && fs ? vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &gi, NULL, &H.ui) : VK_ERROR_UNKNOWN;
    if (vs) vkDestroyShaderModule(g_vk.dev, vs, NULL);
    if (fs) vkDestroyShaderModule(g_vk.dev, fs, NULL);
    if (r != VK_SUCCESS) { H.ui = VK_NULL_HANDLE; LOG("[hdr] the menu pass could not be made: the menu is not shown\n"); return 0; }
    H.ui_fmt = fmt;
    return 1;
}

// Outside any rendering: begins rendering into the menu's image (w x h, cleared to 0); 0: no menu this frame.
int vk_hdr_ui_begin(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (!H.tried) init();
    if (!H.ok || !ui_pipeline(g_vk.sc_format) ||  // (made before any rendering begins: see exec.c post_pass)
        !img_ensure(cb, &H.menu, w, h, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        return 0;
    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = H.menu.view,
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    return 1;
}

// Inside the window image's rendering again: the menu's image blended onto it.
void vk_hdr_ui_compose(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (!H.ui || !H.menu.img) return;
    VkDescriptorSet set = vk_alloc_set(H.dsl_ui);
    VkDescriptorImageInfo ii = {H.nearest, H.menu.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, H.ui);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, H.lay_ui, 0, 1, &set, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pc[4] = {nits().ui, g_active ? 1.0f : 0.0f, 0, 0};
    vkCmdPushConstants(cb, H.lay_ui, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// ---------------------------------------------------------------- screenshots
// 10-bit PQ pixels (A2B10G10R10, or A2R10G10B10 with a2r) to sRGB bytes at the HUD's white (LARECOMP_WINDOW_SHOT).
static float pq_nits(float e) {
    const float m1 = 0.1593017578125f, m2 = 78.84375f, c1 = 0.8359375f, c2 = 18.8515625f, c3 = 18.6875f;
    float p = powf(e, 1.0f / m2);
    return 10000.0f * powf(fmaxf(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
}

void vk_hdr_to_srgb8(uint8_t *px, size_t n, int a2r) {
    float ui = nits().ui;
    static const float m[9] = {1.6605f, -0.5876f, -0.0728f, -0.1246f, 1.1329f, -0.0083f, -0.0182f, -0.1006f, 1.1187f};  // 2020 to 709
    for (size_t i = 0; i < n; i++) {
        uint32_t v;
        memcpy(&v, px + 4 * i, 4);
        float c[3] = {(float)(v & 1023), (float)(v >> 10 & 1023), (float)(v >> 20 & 1023)};
        if (a2r) { float t = c[0]; c[0] = c[2]; c[2] = t; }
        for (int k = 0; k < 3; k++) c[k] = pq_nits(c[k] / 1023.0f) / ui;
        for (int k = 0; k < 3; k++) {
            float l = fminf(fmaxf(m[3 * k] * c[0] + m[3 * k + 1] * c[1] + m[3 * k + 2] * c[2], 0.0f), 1.0f);
            float s = l <= 0.0031308f ? 12.92f * l : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
            px[4 * i + k] = (uint8_t)(s * 255.0f + 0.5f);
        }
        px[4 * i + 3] = 255;
    }
}

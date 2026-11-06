// Texture replacement for modders (AC_TEXTURE_REPLACE=1): DDS files named after a texture's hash (as texture dumping
// names them, texdump.c) are used in place of the game's texture with that data (textures.c upload_texture).
//
// - <dir>: AC_TEXTURE_REPLACE_DIR, else userdata\textures\replace of the install. Searched with its subfolders once,
//   at the first texture upload: a file counts when its name starts with the 16 hex digits of a hash (anything may
//   follow: "<hash>_1024x1024.dds", "<hash> sword.dds"). The first file found for a hash is used.
// - Any size (larger than the original for HD textures), any number of mip levels (fewer: the image has fewer). Layers
//   and cube faces must match the original's.
// - Formats: BC1-BC7, RGBA8, BGRA8, R8, RG8, RGBA16F, RGBA32F; legacy DDS headers (DXT1/DXT3/DXT5, ATI1/ATI2,
//   BC4U/BC5U, 32-bit RGBA/BGRA masks) and DX10 headers. The colour space follows the original texture: an sRGB
//   texture's replacement is read as sRGB even when its file says UNORM (editors often drop the flag), and the other
//   way round.
// - Files are read when a texture with that hash is uploaded (a short pause for large files), and again when the game
//   loads it again.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "texreplace.h"

typedef struct { uint64_t hash; char *path; } Ent;
static Ent *tab;
static uint64_t tab_cap, tab_n;
static char repl_dir[MAX_PATH];
static int enabled = -1;

static void tab_put(uint64_t h, const char *path) {
    if (!h) return;
    if ((tab_n + 1) * 2 > tab_cap) {
        uint64_t nc = tab_cap ? tab_cap * 2 : 1024;
        Ent *nt = calloc(nc, sizeof *nt);
        if (!nt) return;
        for (uint64_t i = 0; i < tab_cap; i++)
            if (tab[i].hash) { uint64_t j = tab[i].hash & (nc - 1); while (nt[j].hash) j = (j + 1) & (nc - 1); nt[j] = tab[i]; }
        free(tab); tab = nt; tab_cap = nc;
    }
    uint64_t j = h & (tab_cap - 1);
    while (tab[j].hash) { if (tab[j].hash == h) return; j = (j + 1) & (tab_cap - 1); }
    tab[j].hash = h; tab[j].path = _strdup(path); tab_n++;
}

static const char *tab_get(uint64_t h) {
    if (!tab_cap || !h) return NULL;
    uint64_t j = h & (tab_cap - 1);
    while (tab[j].hash) { if (tab[j].hash == h) return tab[j].path; j = (j + 1) & (tab_cap - 1); }
    return NULL;
}

static int hexv(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void scan(const char *dir, int depth) {
    char pat[MAX_PATH + 4];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        char path[MAX_PATH];
        if (snprintf(path, sizeof path, "%s\\%s", dir, fd.cFileName) >= (int)sizeof path) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 8) scan(path, depth + 1); continue; }
        size_t n = strlen(fd.cFileName);
        if (n < 20 || _stricmp(fd.cFileName + n - 4, ".dds")) continue;
        uint64_t h = 0;
        int i = 0;
        for (; i < 16; i++) { int v = hexv(fd.cFileName[i]); if (v < 0) break; h = h << 4 | (uint64_t)v; }
        if (i == 16 && hexv(fd.cFileName[16]) < 0) tab_put(h, path);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
}

static bool init(void) {
    const char *e = getenv("AC_TEXTURE_REPLACE");
    if (!e || atoi(e) == 0) return false;
    const char *d = getenv("AC_TEXTURE_REPLACE_DIR");
    if (d && *d) {
        snprintf(repl_dir, sizeof repl_dir, "%s", d);
    } else {  // build\game\round-00\trace.exe -> userdata\textures\replace
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
        if (!n || n >= sizeof exe) return false;
        for (int i = 0; i < 4; i++) { char *k = strrchr(exe, '\\'); if (!k) return false; *k = 0; }
        snprintf(repl_dir, sizeof repl_dir, "%s\\userdata\\textures\\replace", exe);
    }
    size_t l = strlen(repl_dir);
    while (l > 3 && (repl_dir[l - 1] == '\\' || repl_dir[l - 1] == '/')) repl_dir[--l] = 0;
    scan(repl_dir, 0);
    fprintf(stderr, "[texreplace] %llu replacement textures in %s\n", (unsigned long long)tab_n, repl_dir);
    fflush(stderr);
    return tab_n > 0;
}

bool texreplace_has(uint64_t hash) {
    if (enabled < 0) enabled = init();
    return enabled && tab_get(hash);
}

// DXGI format -> Vulkan format, block size, bytes per block; srgb: the format's sRGB-ness
typedef struct { VkFormat vf; uint8_t bw, bpb; } Fmt;
static Fmt fmt_dxgi(uint32_t d) {
    switch (d) {
    case 2: return (Fmt){VK_FORMAT_R32G32B32A32_SFLOAT, 1, 16};
    case 10: return (Fmt){VK_FORMAT_R16G16B16A16_SFLOAT, 1, 8};
    case 28: return (Fmt){VK_FORMAT_R8G8B8A8_UNORM, 1, 4};
    case 29: return (Fmt){VK_FORMAT_R8G8B8A8_SRGB, 1, 4};
    case 49: return (Fmt){VK_FORMAT_R8G8_UNORM, 1, 2};
    case 61: return (Fmt){VK_FORMAT_R8_UNORM, 1, 1};
    case 71: return (Fmt){VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 8};
    case 72: return (Fmt){VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 4, 8};
    case 74: return (Fmt){VK_FORMAT_BC2_UNORM_BLOCK, 4, 16};
    case 75: return (Fmt){VK_FORMAT_BC2_SRGB_BLOCK, 4, 16};
    case 77: return (Fmt){VK_FORMAT_BC3_UNORM_BLOCK, 4, 16};
    case 78: return (Fmt){VK_FORMAT_BC3_SRGB_BLOCK, 4, 16};
    case 80: return (Fmt){VK_FORMAT_BC4_UNORM_BLOCK, 4, 8};
    case 81: return (Fmt){VK_FORMAT_BC4_SNORM_BLOCK, 4, 8};
    case 83: return (Fmt){VK_FORMAT_BC5_UNORM_BLOCK, 4, 16};
    case 84: return (Fmt){VK_FORMAT_BC5_SNORM_BLOCK, 4, 16};
    case 95: return (Fmt){VK_FORMAT_BC6H_UFLOAT_BLOCK, 4, 16};
    case 96: return (Fmt){VK_FORMAT_BC6H_SFLOAT_BLOCK, 4, 16};
    case 98: return (Fmt){VK_FORMAT_BC7_UNORM_BLOCK, 4, 16};
    case 99: return (Fmt){VK_FORMAT_BC7_SRGB_BLOCK, 4, 16};
    case 87: case 88: return (Fmt){VK_FORMAT_B8G8R8A8_UNORM, 1, 4};
    case 91: case 93: return (Fmt){VK_FORMAT_B8G8R8A8_SRGB, 1, 4};
    default: return (Fmt){VK_FORMAT_UNDEFINED, 0, 0};
    }
}

// the sRGB / UNORM twin of a format, or itself
static VkFormat colour_space(VkFormat f, bool srgb) {
    static const VkFormat pairs[][2] = {
        {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB}, {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB},
        {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK}, {VK_FORMAT_BC2_UNORM_BLOCK, VK_FORMAT_BC2_SRGB_BLOCK},
        {VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC3_SRGB_BLOCK}, {VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_BC7_SRGB_BLOCK},
    };
    for (size_t i = 0; i < sizeof pairs / sizeof *pairs; i++)
        if (f == pairs[i][0] || f == pairs[i][1]) return pairs[i][srgb];
    return f;
}

bool texreplace_is_srgb(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK:
        return true;
    default:
        return f >= VK_FORMAT_ASTC_4x4_SRGB_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK && (f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) % 2 == 1;
    }
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static void bad(const char *path, const char *why) {
    static int n;
    if (n++ < 50) { fprintf(stderr, "[texreplace] %s not used: %s\n", path, why); fflush(stderr); }
}

bool texreplace_load(uint64_t hash, bool srgb, uint32_t want_layers, TexReplacement *out) {
    memset(out, 0, sizeof *out);
    const char *path = tab_get(hash);
    if (!path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) { bad(path, "cannot open"); return false; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = size > 128 ? malloc((size_t)size) : NULL;
    bool ok = file && fread(file, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (!ok || memcmp(file, "DDS ", 4) || rd32(file + 4) != 124) { free(file); bad(path, "not a DDS file"); return false; }
    uint32_t h = rd32(file + 12), w = rd32(file + 16), mips = rd32(file + 28), pf_flags = rd32(file + 80), fourcc = rd32(file + 84);
    uint32_t caps2 = rd32(file + 112);
    uint32_t bits = rd32(file + 88), rmask = rd32(file + 92), gmask = rd32(file + 96), bmask = rd32(file + 100);
    if (!mips) mips = 1;
    uint64_t off = 128;
    uint32_t layers = 1;
    bool cube = (caps2 & 0x200) != 0;
    Fmt fm = {0};
    if ((pf_flags & 0x4) && fourcc == 0x30315844) {  // "DX10"
        if (size < 148) { free(file); bad(path, "short DX10 header"); return false; }
        fm = fmt_dxgi(rd32(file + 128));
        if (rd32(file + 132) != 3) { free(file); bad(path, "not a 2D texture"); return false; }
        cube = (rd32(file + 136) & 0x4) != 0;
        layers = rd32(file + 140) ? rd32(file + 140) : 1;
        off = 148;
    } else if (pf_flags & 0x4) {
        switch (fourcc) {
        case 0x31545844: fm = fmt_dxgi(71); break;  // DXT1
        case 0x32545844: case 0x33545844: fm = fmt_dxgi(74); break;  // DXT2/3
        case 0x34545844: case 0x35545844: fm = fmt_dxgi(77); break;  // DXT4/5
        case 0x31495441: case 0x55344342: fm = fmt_dxgi(80); break;  // ATI1, BC4U
        case 0x32495441: case 0x55354342: fm = fmt_dxgi(83); break;  // ATI2, BC5U
        default: break;
        }
    } else if ((pf_flags & 0x40) && bits == 32) {  // RGB(A) masks
        if (rmask == 0xff && gmask == 0xff00 && bmask == 0xff0000) fm = fmt_dxgi(28);
        else if (rmask == 0xff0000 && gmask == 0xff00 && bmask == 0xff) fm = fmt_dxgi(87);
    }
    if (!fm.vf) { free(file); bad(path, "unsupported format"); return false; }
    if (cube) layers *= 6;
    if (layers != want_layers) { free(file); bad(path, "layer / cube face count differs from the game's texture"); return false; }
    if (!w || !h || w > 16384 || h > 16384) { free(file); bad(path, "bad size"); return false; }
    uint32_t maxm = 1;
    for (uint32_t s = w > h ? w : h; s > 1; s >>= 1) maxm++;
    if (mips > maxm) mips = maxm;
    uint64_t need = 0;
    for (uint32_t lv = 0; lv < mips; lv++) {
        uint32_t lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1;
        need += (uint64_t)((lw + fm.bw - 1) / fm.bw) * ((lh + fm.bw - 1) / fm.bw) * fm.bpb;
    }
    need *= layers;
    if (off + need > (uint64_t)size) { free(file); bad(path, "file shorter than its header says"); return false; }
    out->file = file;
    out->data = file + off;
    out->format = colour_space(fm.vf, srgb);
    out->width = w; out->height = h; out->levels = mips; out->layers = layers;
    out->block = fm.bw; out->bpb = fm.bpb;
    out->path = path;
    return true;
}

void texreplace_free(TexReplacement *r) { free(r->file); r->file = NULL; r->data = NULL; }

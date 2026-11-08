// Texture dumping for modders (AC_TEXTURE_DUMP=1): every texture the game uploads from its own data is written once
// as a DDS file, named by the hash of its guest data (the same hash textures.c uses to detect changes and the BC7
// cache uses as its key, so it is the same in every run), with all mip levels and array layers / cube faces.
//
// - ASTC (most of the game's textures; no PC format) is decoded to RGBA8 (R8G8B8A8_UNORM, or _SRGB for sRGB ASTC).
//   BCn, RGBA8, BGRA8, float and the other common formats are written as they are. RGB8 is expanded to RGBA8.
// - Not dumped: render targets and images the game draws or computes into, video frames, linear (CPU-written) and
//   changing textures, depth / stencil, 3D textures, multisampled ones and formats DDS has no match for (16-bit
//   packed RGB). Textures whose data is all zeros (targets not drawn yet) are skipped.
// - Files: <dir>\<hash>_<width>x<height>.dds, and <dir>\textures.csv with one line per file (hash, size, levels,
//   layers, format, and the game's debug label where it set one). A file already there (an earlier run) is kept.
// - <dir>: AC_TEXTURE_DUMP_DIR, else userdata\textures\dump of the install (next to launcher.ini).
//
// The render thread only copies the texture's blocks out of guest memory (deswizzled, compressed size); decoding and
// writing happen on a background thread. Up to 1 GB may wait there; past that the render thread waits (a hitch while
// dumping, not a lost texture).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../astc.h"
#include "../nvn.h"
#include "../tegra.h"

// DXGI formats (dxgiformat.h) for NVN formats (textures.c fmt_of); 0: not dumped.
static uint32_t dxgi_of(uint32_t f, bool *astc) {
    *astc = f >= 0x79 && f <= 0x94;
    if (*astc) return f >= 0x87 ? 29 : 28;  // decoded: R8G8B8A8_UNORM_SRGB / _UNORM
    switch (f) {
    case 0x01: return 61;  // R8_UNORM
    case 0x02: return 63;  // R8_SNORM
    case 0x05: return 54;  // R16_FLOAT
    case 0x06: return 56;  // R16_UNORM
    case 0x0A: return 41;  // R32_FLOAT
    case 0x0D: return 49;  // R8G8_UNORM
    case 0x0E: return 51;  // R8G8_SNORM
    case 0x11: return 34;  // R16G16_FLOAT
    case 0x12: return 35;  // R16G16_UNORM
    case 0x16: return 16;  // R32G32_FLOAT
    case 0x19: case 0x25: case 0x6D: return 28;  // R8G8B8A8_UNORM (RGB8 expanded; RGBX8)
    case 0x26: return 31;  // R8G8B8A8_SNORM
    case 0x29: case 0x71: return 10;  // R16G16B16A16_FLOAT
    case 0x2A: return 11;  // R16G16B16A16_UNORM
    case 0x2E: case 0x76: return 2;  // R32G32B32A32_FLOAT
    case 0x37: case 0x38: return 29;  // R8G8B8A8_UNORM_SRGB
    case 0x3D: return 24;  // R10G10B10A2_UNORM
    case 0x3F: return 26;  // R11G11B10_FLOAT
    case 0x40: return 67;  // R9G9B9E5_SHAREDEXP
    case 0x41: case 0x42: return 71;  // BC1_UNORM
    case 0x43: return 74;  // BC2_UNORM
    case 0x44: return 77;  // BC3_UNORM
    case 0x45: case 0x46: return 72;  // BC1_UNORM_SRGB
    case 0x47: return 75;  // BC2_UNORM_SRGB
    case 0x48: return 78;  // BC3_UNORM_SRGB
    case 0x49: return 80;  // BC4_UNORM
    case 0x4A: return 81;  // BC4_SNORM
    case 0x4B: return 83;  // BC5_UNORM
    case 0x4C: return 84;  // BC5_SNORM
    case 0x4D: return 98;  // BC7_UNORM
    case 0x4E: return 99;  // BC7_UNORM_SRGB
    case 0x4F: return 96;  // BC6H_SF16
    case 0x50: return 95;  // BC6H_UF16
    case 0x98: case 0x99: return 87;  // B8G8R8A8_UNORM
    case 0x9A: case 0x9B: return 91;  // B8G8R8A8_UNORM_SRGB
    default: return 0;
    }
}

static const char *dxgi_name(uint32_t d) {
    switch (d) {
    case 2: return "R32G32B32A32_FLOAT"; case 10: return "R16G16B16A16_FLOAT"; case 11: return "R16G16B16A16_UNORM";
    case 16: return "R32G32_FLOAT"; case 24: return "R10G10B10A2_UNORM"; case 26: return "R11G11B10_FLOAT";
    case 28: return "R8G8B8A8_UNORM"; case 29: return "R8G8B8A8_UNORM_SRGB"; case 31: return "R8G8B8A8_SNORM";
    case 34: return "R16G16_FLOAT"; case 35: return "R16G16_UNORM"; case 41: return "R32_FLOAT"; case 49: return "R8G8_UNORM";
    case 51: return "R8G8_SNORM"; case 54: return "R16_FLOAT"; case 56: return "R16_UNORM"; case 61: return "R8_UNORM";
    case 63: return "R8_SNORM"; case 67: return "R9G9B9E5_SHAREDEXP"; case 71: return "BC1_UNORM"; case 72: return "BC1_UNORM_SRGB";
    case 74: return "BC2_UNORM"; case 75: return "BC2_UNORM_SRGB"; case 77: return "BC3_UNORM"; case 78: return "BC3_UNORM_SRGB";
    case 80: return "BC4_UNORM"; case 81: return "BC4_SNORM"; case 83: return "BC5_UNORM"; case 84: return "BC5_SNORM";
    case 87: return "B8G8R8A8_UNORM"; case 91: return "B8G8R8A8_UNORM_SRGB"; case 95: return "BC6H_UF16"; case 96: return "BC6H_SF16";
    case 98: return "BC7_UNORM"; case 99: return "BC7_UNORM_SRGB";
    default: return "?";
    }
}

typedef struct Job {
    struct Job *next;
    uint64_t hash;
    uint32_t nvn_format, dxgi, w, h, levels, layers;
    bool cube, astc, expand3;
    uint8_t bw, bh, bpb;
    char label[64];
    uint8_t *data;  // every layer's levels, deswizzled (block rows), in DDS order
    uint64_t size;
} Job;

static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER, q_room = PTHREAD_COND_INITIALIZER;
static Job *q_head, *q_tail;
static uint64_t q_bytes;
#define Q_MAX (1ull << 30)
static char dump_dir[MAX_PATH];
static uint64_t n_written, n_kept, bytes_written;

// Seen hashes (render thread only): open addressing, 0 = empty
static uint64_t *seen;
static uint64_t seen_cap, seen_n;
static bool seen_add(uint64_t h) {
    if (!h) h = 1;
    if ((seen_n + 1) * 2 > seen_cap) {
        uint64_t nc = seen_cap ? seen_cap * 2 : 4096, *ns = calloc(nc, 8);
        if (!ns) return false;
        for (uint64_t i = 0; i < seen_cap; i++)
            if (seen[i]) { uint64_t j = seen[i] & (nc - 1); while (ns[j]) j = (j + 1) & (nc - 1); ns[j] = seen[i]; }
        free(seen); seen = ns; seen_cap = nc;
    }
    uint64_t j = h & (seen_cap - 1);
    while (seen[j]) { if (seen[j] == h) return false; j = (j + 1) & (seen_cap - 1); }
    seen[j] = h; seen_n++;
    return true;
}

static void mkdirs(const char *path) {
    char p[MAX_PATH];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 3; *c; c++)
        if (*c == '\\' || *c == '/') { char k = *c; *c = 0; _mkdir(p); *c = k; }
    _mkdir(p);
}

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }

static void write_job(Job *j) {
    char path[MAX_PATH + 64];
    snprintf(path, sizeof path, "%s\\%016llx_%ux%u.dds", dump_dir, (unsigned long long)j->hash, j->w, j->h);
    FILE *f = fopen(path, "rb");
    if (f) {  // (an earlier run's)
        fclose(f);
        if (++n_kept % 500 == 0) { fprintf(stderr, "[texdump] %llu textures already in %s from earlier runs\n", (unsigned long long)n_kept, dump_dir); fflush(stderr); }
        return;
    }
    char tmp[MAX_PATH + 72];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    f = fopen(tmp, "wb");
    if (!f) return;
    // DDS header + DX10 header
    bool compressed = !j->astc && j->bw > 1;
    uint32_t top = compressed ? ((j->w + j->bw - 1) / j->bw) * ((j->h + j->bh - 1) / j->bh) * j->bpb
                              : j->w * (j->astc || j->expand3 ? 4u : j->bpb);
    fwrite("DDS ", 1, 4, f);
    put32(f, 124);
    put32(f, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | (compressed ? 0x80000 : 0x8));  // CAPS HEIGHT WIDTH PIXELFORMAT MIPMAPCOUNT LINEARSIZE/PITCH
    put32(f, j->h); put32(f, j->w);
    put32(f, top);  // (linear size of level 0, or its row pitch)
    put32(f, 0); put32(f, j->levels);
    for (int i = 0; i < 11; i++) put32(f, 0);
    put32(f, 32); put32(f, 0x4); fwrite("DX10", 1, 4, f);  // pixel format: FOURCC DX10
    for (int i = 0; i < 5; i++) put32(f, 0);
    put32(f, 0x1000 | (j->levels > 1 ? 0x400008 : 0) | (j->cube ? 0x8 : 0));  // TEXTURE, MIPMAP|COMPLEX, COMPLEX
    put32(f, j->cube ? 0xFE00 : 0);  // all six cube faces
    put32(f, 0); put32(f, 0); put32(f, 0);
    put32(f, j->dxgi); put32(f, 3);  // TEXTURE2D
    put32(f, j->cube ? 0x4 : 0); put32(f, j->cube ? j->layers / 6 : j->layers); put32(f, 0);
    // data: per layer, per level (as queued); ASTC decoded, RGB8 expanded
    // (a level that cannot be written, or a failed close, rejects the whole file: a short DDS would be kept forever)
    const uint8_t *p = j->data;
    bool failed = false;
    for (uint32_t layer = 0; layer < j->layers && !failed; layer++)
        for (uint32_t lv = 0; lv < j->levels && !failed; lv++) {
            uint32_t w = j->w >> lv ? j->w >> lv : 1, h = j->h >> lv ? j->h >> lv : 1;
            uint32_t wb = (w + j->bw - 1) / j->bw, hb = (h + j->bh - 1) / j->bh;
            uint64_t n = (uint64_t)wb * hb * j->bpb;
            if (j->astc) {
                uint8_t *rgba = malloc((uint64_t)w * h * 4);
                if (!rgba) { failed = true; break; }
                astc_decode_ldr(p, w, h, j->bw, j->bh, j->nvn_format >= 0x87, rgba); fwrite(rgba, 4, (size_t)w * h, f); free(rgba);
            } else if (j->expand3) {
                for (uint64_t i = 0; i < (uint64_t)wb * hb; i++) { fwrite(p + i * 3, 1, 3, f); fputc(255, f); }
            } else {
                fwrite(p, 1, n, f);
            }
            p += n;
        }
    long sz = ftell(f);
    bool ok = !failed && !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileA(tmp); return; }
    n_written++; bytes_written += sz > 0 ? (uint64_t)sz : 0;
    char csv[MAX_PATH + 16];
    snprintf(csv, sizeof csv, "%s\\textures.csv", dump_dir);
    FILE *c = fopen(csv, "rb");
    bool fresh = !c;
    if (c) fclose(c);
    c = fopen(csv, "ab");
    if (c) {
        if (fresh) fprintf(c, "hash,width,height,levels,layers,type,nvn_format,dds_format,file,label\n");
        for (char *q = j->label; *q; q++) if (*q == ',' || *q == '"' || *q == '\n' || *q == '\r') *q = ' ';
        fprintf(c, "%016llx,%u,%u,%u,%u,%s,%#x,%s,%016llx_%ux%u.dds,%s\n", (unsigned long long)j->hash, j->w, j->h, j->levels,
                j->cube ? j->layers / 6 : j->layers, j->cube ? "cube" : j->layers > 1 ? "array" : "2d", j->nvn_format,
                dxgi_name(j->dxgi), (unsigned long long)j->hash, j->w, j->h, j->label);
        fclose(c);
    }
    if (n_written % 100 == 0) {
        fprintf(stderr, "[texdump] %llu textures written (%.0f MB), %llu kept from earlier runs, to %s\n", (unsigned long long)n_written,
               bytes_written / 1048576.0, (unsigned long long)n_kept, dump_dir);
        fflush(stderr);
    }
}

static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&q_lock);
        while (!q_head) pthread_cond_wait(&q_cond, &q_lock);
        Job *j = q_head;
        q_head = j->next;
        if (!q_head) q_tail = NULL;
        pthread_mutex_unlock(&q_lock);
        write_job(j);
        pthread_mutex_lock(&q_lock);
        q_bytes -= j->size;
        pthread_cond_broadcast(&q_room);
        pthread_mutex_unlock(&q_lock);
        free(j->data);
        free(j);
    }
    return NULL;
}

static int enabled = -1;
static bool init(void) {
    const char *e = getenv("AC_TEXTURE_DUMP");
    if (!e || atoi(e) == 0) return false;
    const char *d = getenv("AC_TEXTURE_DUMP_DIR");
    if (d && *d) {
        snprintf(dump_dir, sizeof dump_dir, "%s", d);
    } else {  // build\game\round-00\trace.exe -> userdata\textures\dump
        char exe[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
        if (!n || n >= sizeof exe) return false;
        for (int i = 0; i < 4; i++) { char *k = strrchr(exe, '\\'); if (!k) return false; *k = 0; }
        snprintf(dump_dir, sizeof dump_dir, "%s\\userdata\\textures\\dump", exe);
    }
    mkdirs(dump_dir);
    DWORD a = GetFileAttributesA(dump_dir);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        fprintf(stderr, "[texdump] cannot create %s: texture dumping off\n", dump_dir);
        fflush(stderr);
        return false;
    }
    pthread_t th;
    if (pthread_create(&th, NULL, worker, NULL)) return false;
    pthread_detach(th);
    fprintf(stderr, "[texdump] dumping textures to %s\n", dump_dir);
    fflush(stderr);
    return true;
}

// textures.c upload_texture: t's guest data (hash) was uploaded for the first time, or changed while not yet dynamic.
void texdump_offer(const NvnTexture *t, uint64_t hash, bool gpu_owned) {
    if (enabled < 0) enabled = init();
    if (!enabled || gpu_owned) return;
    // targets, video, images, linear (CPU-written) textures; 3D, multisampled, buffers
    // (0x4, image, is set on all of the game's textures; 0x8, compressible, on its render targets)
    if (t->flags & (0x1 | 0x2 | 0x8 | 0x10 | 0x40)) return;
    if (t->target == 2 || t->target == 5 || t->target == 6 || t->target == 10 || t->samples > 1 || t->layout.depth > 1) return;
    const FormatInfo *fi = t->layout.fmt;
    if (!fi || !fi->bpb || fi->kind == FMT_DEPTH || fi->kind == FMT_STENCIL || fi->kind == FMT_DEPTH_STENCIL) return;
    bool astc;
    uint32_t dxgi = dxgi_of(t->format, &astc);
    if (!dxgi || t->layout.width < 4 || t->layout.height < 4) return;
    if (!seen_add(hash)) return;
    uint32_t layers = t->layout.layers ? t->layout.layers : 1, levels = t->layout.levels ? t->layout.levels : 1;
    bool cube = (t->target == 8 || t->target == 9) && layers % 6 == 0;
    uint64_t size = 0;
    for (uint32_t lv = 0; lv < levels; lv++) { LevelLayout l = tegra_level(&t->layout, lv); size += (uint64_t)l.wb * l.hb * fi->bpb; }
    size *= layers;
    // all zeros: a target not drawn yet (no texture data to dump)
    const uint8_t *mem = (const uint8_t *)(uintptr_t)t->addr;
    uint64_t total = tegra_total_size(&t->layout), nz = 0;
    for (uint64_t i = 0; i < total && !nz; i += 8) { uint64_t v = 0; memcpy(&v, mem + i, total - i >= 8 ? 8 : total - i); nz = v; }
    if (!nz) return;
    Job *j = calloc(1, sizeof *j);
    uint8_t *data = j ? malloc(size) : NULL;
    if (!data) { free(j); return; }
    uint64_t layer_size = tegra_layer_size(&t->layout), o = 0;
    for (uint32_t layer = 0; layer < layers; layer++)
        for (uint32_t lv = 0; lv < levels; lv++) {
            LevelLayout l = tegra_level(&t->layout, lv);
            tegra_deswizzle(mem + layer * layer_size + l.offset, data + o, l.wb, l.hb, 1, fi->bpb, l.block_height_log2, l.block_depth_log2);
            o += (uint64_t)l.wb * l.hb * fi->bpb;
        }
    *j = (Job){.hash = hash, .nvn_format = t->format, .dxgi = dxgi, .w = t->layout.width, .h = t->layout.height, .levels = levels,
               .layers = layers, .cube = cube, .astc = astc, .expand3 = fi->bpb == 3 && fi->bw == 1, .bw = fi->bw, .bh = fi->bh,
               .bpb = fi->bpb, .data = data, .size = size};
    memcpy(j->label, t->label, sizeof j->label - 1);
    pthread_mutex_lock(&q_lock);
    while (q_bytes && q_bytes + size > Q_MAX) pthread_cond_wait(&q_room, &q_lock);
    q_bytes += size;
    if (q_tail) q_tail->next = j; else q_head = j;
    q_tail = j;
    pthread_cond_signal(&q_cond);
    pthread_mutex_unlock(&q_lock);
}

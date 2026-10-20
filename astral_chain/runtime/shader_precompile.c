/* Shader precompiler: fills the SPIR-V cache (cache/spirv in the current directory) before the game runs.
 *
 * The game's shaders come from its own files (astral_chain/shader_extract.py writes <name>.code / <name>.control
 * pairs from root.pkz). Vertex and fragment shaders are translated for the draw-time variants listed in program
 * records (the runtime's cache/programs.bin format: hashes of the vertex and fragment code, and both ShaderKeys);
 * compute shaders depend on their code only and are all translated. Translation, the SPIR-V compile and the cache
 * file names are the runtime's own code (translate-reviewed.c, vulkan/spirv_cache.c), so the game finds every result.
 *
 *   shader_precompile <shader folder> <threads> <records file>...
 *
 * Output: progress lines, then "done <variants matched> <variants without code> <compiled> <cached> <failed>". */
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "shader.h"

uint32_t *vk_glsl_to_spirv(const char *src, int stage, const char *name, size_t *nwords);
void rt_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

// --- shader code, hashed as exec.c stage_hash does (FNV-1a over const_offset bytes, seeded by the stage slot)
typedef struct { char name[160]; uint8_t *code; size_t size; uint32_t const_offset; int stage; uint64_t hash; } Code;
static Code *codes;
static int ncodes, capcodes;

static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n + 0x10000) : NULL;  // (zero tail: the translator may look past the code)
    if (d) { memset(d + n, 0, 0x10000); if (fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; } }
    fclose(f);
    if (d) *size = (size_t)n;
    return d;
}

static void load_codes(const char *folder) {
    char pattern[MAX_PATH];
    {   // subfolders first (one per DAT)
        WIN32_FIND_DATAA fd;
        snprintf(pattern, sizeof pattern, "%s\\*", folder);
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
                char sub[MAX_PATH];
                snprintf(sub, sizeof sub, "%s\\%s", folder, fd.cFileName);
                load_codes(sub);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    snprintf(pattern, sizeof pattern, "%s\\*.code", folder);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char path[MAX_PATH], cpath[MAX_PATH];
        snprintf(path, sizeof path, "%s\\%s", folder, fd.cFileName);
        snprintf(cpath, sizeof cpath, "%s\\%.*s.control", folder, (int)(strlen(fd.cFileName) - 5), fd.cFileName);
        size_t size = 0, csize = 0;
        uint8_t *code = read_file(path, &size), *control = read_file(cpath, &csize);
        if (!code || !control || csize < 0x704) { free(code); free(control); continue; }
        if (ncodes == capcodes) { capcodes = capcodes ? capcodes * 2 : 1024; codes = realloc(codes, (size_t)capcodes * sizeof *codes); }
        Code *c = &codes[ncodes++];
        snprintf(c->name, sizeof c->name, "%s", fd.cFileName);
        c->code = code;
        c->size = size;
        memcpy(&c->const_offset, control + 0x700, 4);
        free(control);
        // stage as objects.c set_shaders decides it: NVN header magic, then the SPH shader type
        uint32_t magic = 0, sph0 = 0;
        if (size >= 0x34) { memcpy(&magic, code, 4); memcpy(&sph0, code + 0x30, 4); }
        int type = (sph0 >> 10) & 0xF;
        c->stage = magic != 0x12345678 ? 5 : type == 1 ? 0 : type == 5 ? 1 : -1;
        c->hash = 0;
        if (c->stage == 0 || c->stage == 1) {
            if (c->const_offset && c->const_offset <= size) c->hash = hmix(0x9E3779B97F4A7C15ull + (uint64_t)c->stage, code, c->const_offset);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static Code *find_code(uint64_t hash, int stage) {
    for (int i = 0; i < ncodes; i++) if (codes[i].stage == stage && codes[i].hash == hash) return &codes[i];
    return NULL;
}

// --- program records (exec.c WarmRec, cache/programs.bin)
typedef struct { uint64_t vh, fh; ShaderKey vkey, fkey; } WarmRec;
#define WARM_MAGIC (0x4D524157u ^ (uint32_t)sizeof(WarmRec))

// --- compile jobs: GLSL sources, compiled on worker threads through the runtime's cache
typedef struct { char *src; int stage; } Job;
static Job *jobs;
static int njobs, capjobs;
static atomic_int next_job, compiled, cached, failed, finished;

static void add_job(const char *src, int stage) {
    for (int i = 0; i < njobs; i++) if (jobs[i].stage == stage && !strcmp(jobs[i].src, src)) return;  // (shared shaders)
    if (njobs == capjobs) { capjobs = capjobs ? capjobs * 2 : 1024; jobs = realloc(jobs, (size_t)capjobs * sizeof *jobs); }
    jobs[njobs].src = _strdup(src);
    jobs[njobs].stage = stage;
    njobs++;
}

static uint64_t src_key(const char *src, int stage) {  // (spirv_cache.c src_hash: is it in the cache already?)
    uint64_t h = (0xcbf29ce484222325ull ^ (uint64_t)stage) + 3;
    for (const char *p = src; *p; p++) { h ^= (uint8_t)*p; h *= 0x100000001B3ull; }
    return h;
}

static void *worker(void *arg) {
    (void)arg;
    for (int i; (i = atomic_fetch_add(&next_job, 1)) < njobs;) {
        char path[64];
        snprintf(path, sizeof path, "cache/spirv/%016llx.spv", (unsigned long long)src_key(jobs[i].src, jobs[i].stage));
        int had = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
        size_t n = 0;
        uint32_t *spv = vk_glsl_to_spirv(jobs[i].src, jobs[i].stage, jobs[i].stage == 0 ? "vs" : jobs[i].stage == 1 ? "fs" : "cs", &n);
        if (!spv) atomic_fetch_add(&failed, 1);
        else if (had) atomic_fetch_add(&cached, 1);
        else atomic_fetch_add(&compiled, 1);
        free(spv);
        int done = atomic_fetch_add(&finished, 1) + 1;
        if (done % 25 == 0 || done == njobs) { printf("progress %d %d\n", done, njobs); fflush(stdout); }
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: shader_precompile <shader folder> <threads> <records file>...\n"); return 2; }
    load_codes(argv[1]);
    if (!ncodes) { fprintf(stderr, "no shaders in %s\n", argv[1]); return 1; }
    maxwell_gmem_base = 0x7200000000ull;  // (as the runtime: HEAP_BASE, HEAP_MAX)
    maxwell_gmem_size = 0x100000000ull;
    int matched = 0, missing = 0;
    for (int a = 3; a < argc; a++) {
        size_t size = 0;
        uint8_t *data = read_file(argv[a], &size);
        uint32_t magic = 0;
        if (!data || size < 4 || (memcpy(&magic, data, 4), magic != WARM_MAGIC)) { free(data); continue; }
        for (size_t off = 4; off + sizeof(WarmRec) <= size; off += sizeof(WarmRec)) {
            uint32_t word;
            memcpy(&word, data + off, 4);
            if (word == WARM_MAGIC) { off += 4 - sizeof(WarmRec); continue; }  // (a session's header, see exec.c warm_load)
            WarmRec r;
            memcpy(&r, data + off, sizeof r);
            Code *vs = find_code(r.vh, 0), *fs = r.fh ? find_code(r.fh, 1) : NULL;
            if (!vs || (r.fh && !fs)) { missing++; continue; }
            // as exec.c translate_program
            ShaderInfo *vi = maxwell_translate(vs->code, vs->const_offset, STAGE_VERTEX, &r.vkey);
            if (!vi || !vi->msl) { missing++; continue; }
            r.fkey.vs_out_mask = vi->out_mask;
            ShaderInfo *fi = fs ? maxwell_translate(fs->code, fs->const_offset, STAGE_FRAGMENT, &r.fkey) : NULL;
            if (fs && (!fi || !fi->msl)) { missing++; continue; }
            add_job(vi->msl, STAGE_VERTEX);
            add_job(fi ? fi->msl : "#version 460\nvoid main() {}\n", STAGE_FRAGMENT);
            matched++;
        }
        free(data);
    }
    // compute: exec.c compute_program (key: GLSL only), window as for the others
    for (int i = 0; i < ncodes; i++) {
        if (codes[i].stage != 5) continue;
        ShaderKey k = {0};
        k.glsl = 1;
        size_t window = codes[i].const_offset ? codes[i].const_offset : codes[i].size;
        ShaderInfo *ci = maxwell_translate(codes[i].code, window, STAGE_COMPUTE, &k);
        if (ci && ci->msl) add_job(ci->msl, STAGE_COMPUTE);
    }
    printf("shaders %d, variants %d (without code %d), sources %d\n", ncodes, matched, missing, njobs);
    fflush(stdout);
    CreateDirectoryA("cache", NULL);
    CreateDirectoryA("cache\\spirv", NULL);
    int threads = atoi(argv[2]);
    if (threads < 1) threads = 1;
    if (threads > 64) threads = 64;
    pthread_t t[64];
    for (int i = 0; i < threads; i++) pthread_create(&t[i], NULL, worker, NULL);
    for (int i = 0; i < threads; i++) pthread_join(t[i], NULL);
    printf("done %d %d %d %d %d\n", matched, missing, compiled, cached, failed);
    return failed ? 1 : 0;
}

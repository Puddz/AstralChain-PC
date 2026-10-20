/* shaderc subset (shim/shaderc/shaderc.h) over the glslang validator executable.
 * AC_GLSLANG names glslang.exe; native_trace sets it to the pinned local copy.
 * Thread-safe: each compile uses its own temporary files. The NVN backend keeps
 * its own on-disk SPIR-V cache, so each distinct shader compiles once. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shim/shaderc/shaderc.h"

struct shaderc_compiler { int unused; };
struct shaderc_compile_options { int optimize; };
struct shaderc_compilation_result { shaderc_compilation_status status; char *bytes; size_t size; char *message; };

shaderc_compiler_t shaderc_compiler_initialize(void) {
    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("AC_GLSLANG", path, sizeof path);
    if (!n || n >= sizeof path || GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return NULL;
    return calloc(1, sizeof(struct shaderc_compiler));
}
void shaderc_compiler_release(shaderc_compiler_t c) { free(c); }
shaderc_compile_options_t shaderc_compile_options_initialize(void) { return calloc(1, sizeof(struct shaderc_compile_options)); }
void shaderc_compile_options_set_target_env(shaderc_compile_options_t o, shaderc_target_env e, unsigned v) { (void)o; (void)e; (void)v; }
void shaderc_compile_options_set_optimization_level(shaderc_compile_options_t o, shaderc_optimization_level l) { if (o) o->optimize = l != shaderc_optimization_level_zero; }
void shaderc_compile_options_release(shaderc_compile_options_t o) { free(o); }

static shaderc_compilation_result_t failure(const char *message) {
    shaderc_compilation_result_t r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->status = shaderc_compilation_status_compilation_error;
    r->message = _strdup(message ? message : "glslang shim failure");
    return r;
}

static char *read_all(HANDLE file, size_t *size) {
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap + 1);
    for (DWORD got; buf && ReadFile(file, buf + n, (DWORD)(cap - n), &got, NULL) && got;) {
        n += got;
        if (n == cap) { char *more = realloc(buf, cap * 2 + 1); if (!more) { free(buf); return NULL; } buf = more; cap *= 2; }
    }
    if (buf) buf[n] = 0;
    *size = n;
    return buf;
}

/* One glslang run: source on stdin (no input file for scanners to lock), SPIR-V
 * written to `spv`. Returns the exit code (-1 if it could not start); *log gets
 * glslang's combined output. */
static DWORD run_glslang(const char *glslang, const char *source, size_t size, const char *stage, const char *spv,
                         int optimize, char **log) {
    char cmd[4 * MAX_PATH];
    snprintf(cmd, sizeof cmd, "\"%s\" -V --target-env vulkan1.3 %s--stdin -S %s -o \"%s\"", glslang, optimize ? "-Os " : "", stage, spv);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE out_rd = NULL, out_wr = NULL, in_rd = NULL, in_wr = NULL;
    *log = NULL;
    if (!CreatePipe(&out_rd, &out_wr, &sa, 0)) return (DWORD)-1;
    if (!CreatePipe(&in_rd, &in_wr, &sa, 1 << 20)) { CloseHandle(out_rd); CloseHandle(out_wr); return (DWORD)-1; }
    SetHandleInformation(out_rd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_wr, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = out_wr;
    si.hStdInput = in_rd;
    PROCESS_INFORMATION pi;
    BOOL started = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(out_wr);
    CloseHandle(in_rd);
    if (!started) { CloseHandle(out_rd); CloseHandle(in_wr); return (DWORD)-1; }
    for (size_t done = 0; done < size;) {  /* glslang reads all of stdin before it writes */
        DWORD n = 0;
        if (!WriteFile(in_wr, source + done, (DWORD)(size - done > 65536 ? 65536 : size - done), &n, NULL) || !n) break;
        done += n;
    }
    CloseHandle(in_wr);
    size_t log_size = 0;
    *log = read_all(out_rd, &log_size);
    CloseHandle(out_rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

shaderc_compilation_result_t shaderc_compile_into_spv(shaderc_compiler_t c, const char *source, size_t size,
    shaderc_shader_kind kind, const char *name, const char *entry, shaderc_compile_options_t o) {
    (void)c; (void)name; (void)entry;
    static volatile LONG counter;
    char glslang[MAX_PATH], temp[MAX_PATH], spv[MAX_PATH + 64];
    if (!GetEnvironmentVariableA("AC_GLSLANG", glslang, sizeof glslang)) return failure("AC_GLSLANG not set");
    DWORD tn = GetTempPathA(sizeof temp, temp);
    if (!tn || tn >= sizeof temp) return failure("temporary directory unavailable");
    const char *stage = kind == shaderc_vertex_shader ? "vert" : kind == shaderc_fragment_shader ? "frag" : "comp";
    snprintf(spv, sizeof spv, "%sac_glslang_%lu_%ld.spv", temp, GetCurrentProcessId(), InterlockedIncrement(&counter));
    shaderc_compilation_result_t r = NULL;
    /* Transient file errors (e.g. a scanner holding the output) are retried; real
     * compile errors fail the same way every time and are returned at once. */
    for (int attempt = 0; attempt < 3 && !r; attempt++) {
        if (attempt) Sleep(50 * attempt);
        char *log = NULL;
        DWORD code = run_glslang(glslang, source, size, stage, spv, o && o->optimize, &log);
        if (code == (DWORD)-1) { free(log); continue; }
        if (code) {
            int transient = log && (strstr(log, "unable to open") || strstr(log, "cannot open"));
            if (!transient || attempt == 2) r = failure(log);
            free(log);
            continue;
        }
        free(log);
        HANDLE h = CreateFileA(spv, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
        size_t n = 0;
        char *bytes = h == INVALID_HANDLE_VALUE ? NULL : read_all(h, &n);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (!bytes || !n || n % 4) { free(bytes); if (attempt == 2) r = failure("glslang produced no readable SPIR-V"); continue; }
        if ((r = calloc(1, sizeof *r))) { r->status = shaderc_compilation_status_success; r->bytes = bytes; r->size = n; }
        else free(bytes);
    }
    DeleteFileA(spv);
    return r ? r : failure("cannot start glslang");
}
shaderc_compilation_status shaderc_result_get_compilation_status(shaderc_compilation_result_t r) {
    return r ? r->status : shaderc_compilation_status_compilation_error;
}
const char *shaderc_result_get_error_message(shaderc_compilation_result_t r) { return r && r->message ? r->message : ""; }
size_t shaderc_result_get_length(shaderc_compilation_result_t r) { return r ? r->size : 0; }
const char *shaderc_result_get_bytes(shaderc_compilation_result_t r) { return r ? r->bytes : NULL; }
void shaderc_result_release(shaderc_compilation_result_t r) {
    if (!r) return;
    free(r->bytes);
    free(r->message);
    free(r);
}

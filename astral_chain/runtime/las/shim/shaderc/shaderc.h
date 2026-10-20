/* Astral Chain shim: the subset of the shaderc C API used by the vendored NVN
 * backend, implemented in shaderc_glslang.c by running the pinned glslang
 * validator (tools/glslang, downloaded by setup) with an on-disk SPIR-V cache. */
#pragma once
#include <stddef.h>
typedef struct shaderc_compiler *shaderc_compiler_t;
typedef struct shaderc_compile_options *shaderc_compile_options_t;
typedef struct shaderc_compilation_result *shaderc_compilation_result_t;
typedef enum { shaderc_vertex_shader = 0, shaderc_fragment_shader = 1, shaderc_compute_shader = 2 } shaderc_shader_kind;
typedef enum { shaderc_target_env_vulkan = 0 } shaderc_target_env;
enum { shaderc_env_version_vulkan_1_3 = (1u << 22) | (3u << 12) };
typedef enum { shaderc_optimization_level_zero = 0, shaderc_optimization_level_performance = 2 } shaderc_optimization_level;
typedef enum { shaderc_compilation_status_success = 0, shaderc_compilation_status_compilation_error = 2 } shaderc_compilation_status;
shaderc_compiler_t shaderc_compiler_initialize(void);
void shaderc_compiler_release(shaderc_compiler_t);
shaderc_compile_options_t shaderc_compile_options_initialize(void);
void shaderc_compile_options_set_target_env(shaderc_compile_options_t, shaderc_target_env, unsigned version);
void shaderc_compile_options_set_optimization_level(shaderc_compile_options_t, shaderc_optimization_level);
void shaderc_compile_options_release(shaderc_compile_options_t);
shaderc_compilation_result_t shaderc_compile_into_spv(shaderc_compiler_t, const char *source, size_t size,
    shaderc_shader_kind kind, const char *name, const char *entry, shaderc_compile_options_t);
shaderc_compilation_status shaderc_result_get_compilation_status(shaderc_compilation_result_t);
const char *shaderc_result_get_error_message(shaderc_compilation_result_t);
size_t shaderc_result_get_length(shaderc_compilation_result_t);
const char *shaderc_result_get_bytes(shaderc_compilation_result_t);
void shaderc_result_release(shaderc_compilation_result_t);

/* Astral Chain shim: SDL CPU count for the vendored NVN backend's worker pool. */
#pragma once
int nvn_host_cpu_count(void);
static inline int SDL_GetNumLogicalCPUCores(void) { return nvn_host_cpu_count(); }

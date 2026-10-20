/* Astral Chain shim: the SDL timer calls used by the vendored NVN backend. */
#pragma once
#include <stdint.h>
#include "../../platform.h"
typedef uint32_t Uint32;
typedef uint64_t Uint64;
void nvn_host_sleep_ms(uint32_t ms);
static inline Uint64 SDL_GetTicksNS(void) { return plat_now_ns(); }
static inline Uint64 SDL_GetTicks(void) { return plat_now_ns() / 1000000u; }
static inline void SDL_Delay(Uint32 ms) { nvn_host_sleep_ms(ms); }
static inline void SDL_DelayPrecise(Uint64 ns) { plat_sleep_until_ns(plat_now_ns() + ns); }

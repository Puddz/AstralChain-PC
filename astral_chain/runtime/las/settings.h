/* Astral Chain shim for LASRecomp's settings: fixed defaults, no settings UI.
 * Enum values follow LASRecomp runtime/settings.h. */
#pragma once
enum { UPSCALE_BILINEAR = 0, UPSCALE_METALFX = 1, UPSCALE_FSR1 = 2 };
enum { AA_OFF = 0, AA_SMAA = 1 };
typedef struct { int antialias, fullscreen, upscaler; } Settings;
extern Settings g_settings;
static inline void settings_load(void) {}

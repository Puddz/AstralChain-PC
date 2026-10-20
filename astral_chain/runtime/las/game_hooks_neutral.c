/* Link's Awakening specific hooks called by the vendored LASRecomp Vulkan backend
 * (mirror mode, pots, pets, item cursor, hearts, Escape Bottle, widescreen).
 * Astral Chain has none of these: every hook reports "inactive" or the neutral
 * value, so the backend draws exactly what the game submits. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

bool mirror_draw(bool composite) { (void)composite; return false; }
bool pot_level_active(void) { return false; }
bool pot_glow_active(void) { return false; }
bool pot_tint(const float *m, float tint[8], uint64_t mesh) { (void)m; (void)tint; (void)mesh; return false; }
bool pot_instance_gold(const float *m) { (void)m; return false; }
void pot_gold_tint(float tint[8]) { (void)tint; }
bool pets_hide_item_cursor(void) { return false; }
bool texpatch_is_item_cursor(int id) { (void)id; return false; }
int texpatch_bottle_kind(int id) { (void)id; return 0; }
int texpatch_heart_kind(int id) { (void)id; return 0; }
void texpatch_heart_tint(int kind, float tint[8]) { (void)kind; (void)tint; }
void escape_bottle_tint(int kind, const float *ui, uint32_t ui_size, float tint[8]) { (void)kind; (void)ui; (void)ui_size; (void)tint; }
void widescreen_backdrop_seen(uint64_t hash) { (void)hash; }
void widescreen_scene_ubo(const float *u, uint64_t size) { (void)u; (void)size; }
float widescreen_effect_stretch(uint64_t fs_hash) { (void)fs_hash; return 1.0f; }
/* kernel.c in LASRecomp: per-thread time blocked in waits (frame pacing, off). */
__thread uint64_t g_blocked_ns;
/* Level name for texture debug labels (LASRecomp runtime/hle): none here. */
const char *game_level_name(void) { return "-"; }
/* LASRecomp's F9 debug-frame request; never raised. */
volatile int g_debug_frame_request;
/* SMAA is not vendored and g_settings.antialias is off, so these are never
 * reached; reaching them stops instead of drawing with invented data. */
#include "rt.h"
const unsigned char *smaa_area_tex(void) { FATAL("SMAA data not vendored\n"); }
const unsigned char *smaa_search_tex(void) { FATAL("SMAA data not vendored\n"); }
const char smaa_hlsl_src[] = "#error SMAA source not vendored\n";

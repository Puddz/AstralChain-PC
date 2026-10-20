/* Astral Chain: LASRecomp's Link's Awakening texture patches are not vendored.
 * texpatch_identify returns -1 (no patch), so texpatch_apply is never reached;
 * every texture is uploaded exactly as the game wrote it. */
#pragma once
#include <stdint.h>
static inline int texpatch_identify(const void *t) { (void)t; return -1; }
static inline uint32_t texpatch_generation(void) { return 0; }
#define texpatch_apply(...) ((void)0)

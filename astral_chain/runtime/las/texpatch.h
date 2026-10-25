/* Texture patches before upload (textures.c). Astral Chain: the button icon atlas is redrawn with keyboard and mouse
 * prompts (button_prompts.c). texpatch_identify returns -1 for every other texture, which is uploaded exactly as the
 * game wrote it. */
#pragma once
#include <stdint.h>
int texpatch_identify(const void *t);
uint32_t texpatch_generation(void);
void texpatch_apply(int id, uint32_t level, uint8_t *data, uint32_t w, uint32_t h, uint32_t row);

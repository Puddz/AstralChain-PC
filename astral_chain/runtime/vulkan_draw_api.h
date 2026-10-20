/* Private experimental x64 Windows ABI. Caller owns readable inputs and a
 * writable, disjoint output for the call duration. No guest pointers required.
 * 0: completed/output copied; 1: backend failure; 2: invalid inputs;
 * 3: GPU completion unknown, backend poisoned until process restart.
 * Fresh Vulkan objects per call: correctness prototype, not a 60 FPS backend. */
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct {const void *data;uint64_t size;} AcDrawInput;
typedef struct {
 uint32_t version,struct_size,mode,reserved;
 /* vertex SPIR-V, fragment SPIR-V, VBO0, VBO1, vertex CB3/CB4,
  * indices, fragment CB4, packed linear Z24S8 texture. */
 AcDrawInput inputs[9];
 void *output;uint64_t output_size;
} AcVulkanDraw;
/* Mode 0: diagnostic RGBA32 / positive viewport / no culling.
 * Mode 1: observed RF32 / negative viewport / CW front / back culling.
 * Both are fixed 1600x900 experiments, not general game-state validation. */
typedef int (*AcVulkanDrawFunction)(const AcVulkanDraw *);

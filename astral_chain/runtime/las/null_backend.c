/* Diagnostic NVN backend: records that command lists were submitted and that
 * textures were presented, without executing or drawing anything. It exists to
 * find CPU/service blockers past NVN; it is not rendering and its frames are
 * not output. The Vulkan backend (nvn/vulkan) replaces it. */
#include <stdio.h>
#include "nvn/nvn.h"
static uint64_t executed, presented, fence;
void backend_init(void) { fprintf(stderr, "NVN null backend: no rendering is performed\n"); }
void backend_execute(uint64_t handle) {
    if (++executed <= 16 || !(executed & 1023)) printf("NVN null execute handle=0x%llx count=%llu\n", (unsigned long long)handle, (unsigned long long)executed);
}
void backend_present(NvnTexture *tex) {
    ++presented;
    printf("NVN null present frame=%llu texture=%ux%u format=%u executed=%llu\n", (unsigned long long)presented,
           tex ? tex->width : 0, tex ? tex->height : 0, tex ? tex->format : 0, (unsigned long long)executed);
    fflush(stdout);
}
void backend_texture_destroyed(NvnTexture *tex) { (void)tex; }
void backend_pool_written(uint64_t addr, uint64_t size) { (void)addr; (void)size; }
void renderer_sync(void) {}
void renderer_flush(void) {}
uint64_t renderer_fence_seq(void) { return ++fence; }
void renderer_wait_seq(uint64_t seq) { (void)seq; }
uint64_t renderer_wait_target(uint64_t seq) { (void)seq; return 0; }
int renderer_seq_done(uint64_t seq) { (void)seq; return 1; }

// Executes recorded NVN command lists with Vulkan (port of renderer.m; same structure, same behaviour).
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_timer.h>
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../../kernel.h"
#include "shaders.h"
#include "vk.h"
#include "../renderer.h"
#include "../../settings.h"

static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies;
// (Astral Chain) skipped draws/dispatches by source line, the top ones listed in the stats line
static uint32_t skip_lines[8192];
#define SKIPPED() (stat_skipped++, skip_lines[__LINE__ & 8191]++)
static uint64_t stat_upload_bytes;
// LARECOMP_VK_PROF=1: render-thread time per category in the fps log (exec = whole command lists, draw = draw(),
// sets = descriptor sets incl. cbuf copies, vtx = index scan + vertex copies, submit = vkQueueSubmit2)
static int prof = -1;
static uint64_t prof_exec, prof_draw, prof_sets, prof_vtx, prof_submit, prof_pre, prof_key, prof_dyn, prof_tex, prof_up, prof_gpu;
static VkQueryPool prof_qp;
static uint64_t cum_exec, cum_wait, cum_draws, cum_submits;  // (profiling) never reset: hitch log
static uint32_t prof_end_line[4096];  // passes ended, by the exec.c line that ended them (0: submit / present)
uint64_t prof_views_created;  // textures.c vk_image_view cache misses
uint64_t vk_draws_total;  // (textures.c warm-up: draws per frame)
uint64_t cum_up_ns, cum_built_ns;  // (profiling) texture upload checks, waits for pipelines: hitch log
uint64_t prof_up_ns = ~0ull;  // textures.c upload checks (~0: profiling off)
#define PROF_T0() uint64_t prof_t0_ = prof ? SDL_GetTicksNS() : 0
#define PROF_ADD(v) do { if (prof) v += SDL_GetTicksNS() - prof_t0_; } while (0)
static _Atomic int stat_compiles;
_Atomic unsigned long long vk_pipelines_built;  // (device.c: the pipeline cache is saved after new builds)
// shader/pipeline build time per stats interval (ns): Maxwell translation, SPIR-V (cache or glslang), pipelines
static _Atomic unsigned long long build_ns_translate, build_ns_spirv, build_ns_pipe;
static _Atomic unsigned long long build_pipe_hits, build_pipe_misses, build_ns_pipe_miss;  // (pipeline cache: creation feedback)
static unsigned long long pwarm_queued, pwarm_ready, pwarm_late, pwarm_rejected;  // (pipeline warm-up, render thread)
static _Atomic unsigned long long gpl_stage_built, gpl_stage_hits, gpl_stage_ns, gpl_linked, gpl_link_ns, gpl_opt_built;  // (shader libraries)
// program warm-up (see get_program): variants queued at SetShaders, and at their first draw ready or still building
static uint64_t warm_stat_queued, warm_stat_ready, warm_stat_late;
static uint64_t op_counts[CMD_COUNT];
bool g_debug_frame;
static char stats_text[2048];
void vk_collect_dead(void);

// ================================================================== submissions
// A submission = optional upload command buffer + the main one, signalling the timeline with its number.
#define SUBS 8
typedef struct {
    VkCommandBuffer main, upload;
    bool main_begun, upload_begun, ts;  // ts: GPU timestamps written (profiling)
    uint64_t seq;
    VkDescriptorPool pools[16];
    int npools, cur_pool;
} Sub;
static Sub subs[SUBS];
static int cur_sub;
static void ss_forget(void);
static uint64_t submit_seq;  // last submitted
static pthread_mutex_t seq_lock = PTHREAD_MUTEX_INITIALIZER;
static bool in_pass;

// Astral Chain heap mirror (defined with gmem_info).
static void gmem_writeback_apply(uint64_t done);
static void gmem_sync(void);
static void gmem_wb_record(VkCommandBuffer cb, uint64_t seq);
static uint64_t stat_mirror_bytes, stat_wb_bytes, stat_wb_ranges, stat_wb_faults;  // heap mirror traffic (stats line)
static int lazy_n;  // lazy write-back entries (gmem_writeback_apply)

uint64_t vk_completed_seq(void) {
    uint64_t v = 0;
    vkGetSemaphoreCounterValue(g_vk.dev, g_vk.timeline, &v);
    return v;
}

// (LARECOMP_VK_PROF) waits on the GPU by cause: the caller names it in wait_why just before waiting
static __thread const char *wait_why;
static struct { const char *why; uint64_t ns, n; } wait_tab[12];
static pthread_mutex_t wait_tab_lock = PTHREAD_MUTEX_INITIALIZER;
static void wait_account(uint64_t ns) {
    const char *why = wait_why ? wait_why : "other";
    wait_why = NULL;
    pthread_mutex_lock(&wait_tab_lock);
    for (int i = 0; i < 12; i++)
        if (!wait_tab[i].why || wait_tab[i].why == why) { wait_tab[i].why = why; wait_tab[i].ns += ns; wait_tab[i].n++; break; }
    pthread_mutex_unlock(&wait_tab_lock);
}

static void cq_resolve_done(uint64_t done);

void vk_wait_seq(uint64_t seq) {
    if (!seq) { wait_why = NULL; return; }
    VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &g_vk.timeline,
                              .pValues = &seq};
    uint64_t t0 = SDL_GetTicksNS();
    VkResult r = vkWaitSemaphores(g_vk.dev, &wi, 10ull * 1000000000ull);
    stat_wait_ns += SDL_GetTicksNS() - t0;
    cum_wait += SDL_GetTicksNS() - t0;
    wait_account(SDL_GetTicksNS() - t0);
    if (r == VK_TIMEOUT) LOG("[vk] GPU did not finish submission %llu within 10 s (device lost?)\n", (unsigned long long)seq);
    else if (r == VK_SUCCESS) { cq_resolve_done(seq); gmem_writeback_apply(vk_completed_seq()); }  // (counters, mirror write-backs)
    else {
        extern void vk_devlost_dump(void);
        vk_devlost_dump();
        vk_device_fault_report();
        FATAL("vkWaitSemaphores: %d (device lost)\n", r);
    }
}

uint64_t vk_pending_seq(void) { return submit_seq + 1; }

// GPU hang safety stop: when submitted work makes no progress (the timeline value doesn't advance) for
// AC_GPU_HANG_MS (default 1200; 0 = off), the process exits before Windows' 2 s TDR resets the GPU. Repeated TDRs
// from runaway shaders ended in system bugchecks (0x119) on the first development PC; no correct frame keeps one
// submission busy that long. Checked from its own thread: guest threads may poll fences instead of waiting.
static _Atomic uint64_t hang_submitted;
static void *hang_watch(void *arg) {
    int64_t ms = (int64_t)(intptr_t)arg;
    uint64_t last = 0, since = SDL_GetTicksNS();
    for (;;) {
        SDL_Delay(50);
        uint64_t done = vk_completed_seq(), now = SDL_GetTicksNS();
        if (done >= atomic_load(&hang_submitted) || done != last) { last = done; since = now; continue; }
        if (now - since < (uint64_t)ms * 1000000ull) continue;
        LOG("[vk] SAFETY STOP: GPU made no progress for %lld ms (submission %llu pending, %llu done); exiting before a "
            "driver reset\n", (long long)ms, (unsigned long long)atomic_load(&hang_submitted), (unsigned long long)done);
        fflush(NULL);
        _exit(86);
    }
    return NULL;
}
static void hang_watch_submitted(uint64_t seq) {
    static int started;
    atomic_store(&hang_submitted, seq);
    if (started) return;
    started = 1;
    int64_t ms = getenv("AC_GPU_HANG_MS") ? atoll(getenv("AC_GPU_HANG_MS")) : 1200;
    LOG("[vk] GPU hang stop after %lld ms%s\n", (long long)ms, ms > 0 ? "" : " (off)");
    pthread_t t;
    if (ms > 0 && pthread_create(&t, NULL, hang_watch, (void *)(intptr_t)ms) == 0) pthread_detach(t);
}

// ---- the game's GPU timing counters (nvnCommandBufferReportCounter, two per frame)
// The game times its GPU work with these and drops to 30 fps when frames look slow. They used to get the host clock
// at translation time, so our command translation counted as GPU time: busy scenes (outdoors, ~400 draws) looked slow
// and the game locked itself at 30 even on fast GPUs, at any resolution. Now each counter also records a GPU
// timestamp at its place in the command stream; once that submission is done (the game waits on its fence before
// reading - on the Switch the GPU writes them too) the GPU time replaces the provisional host value, in the host
// clock's terms (offset calibrated at each resolve: a timestamp can't be later than the moment its submission is seen
// done). LARECOMP_VK_COUNTER=cpu: host clock only; =frozen: one value per presented frame (test); =legacy: the host
// clock in ns instead of GPU ticks, as before (test: reproduces the locked 30).
#define CQ_MAX 64
typedef struct { VkQueryPool pool; uint32_t n; uint64_t seq; uint64_t *out[CQ_MAX]; } CounterQ;
static CounterQ cq[SUBS];
static pthread_mutex_t cq_lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t cq_offset;  // host ns - GPU ns (the smallest seen)
// Counter timestamps are in the Switch GPU's ticks (614.4 MHz): the game converts the difference to ns itself
// (x 625 / 384). Writing ns made every GPU time look 1.63x as long.
static uint64_t ns_to_gpu_ticks(uint64_t ns) { return (uint64_t)((unsigned __int128)ns * 384 / 625); }
static bool cq_calibrated;
static int counter_mode = -1;  // 0 cpu, 1 frozen, 2 gpu
static uint64_t stat_counters_gpu;

static int vk_counter_mode(void) {
    if (counter_mode < 0) {
        const char *e = getenv("LARECOMP_VK_COUNTER");
        counter_mode = e && !strcmp(e, "cpu") ? 0 : e && !strcmp(e, "frozen") ? 1 : e && !strcmp(e, "legacy") ? 3 : 2;
        if (counter_mode == 2 && !g_vk.props.limits.timestampComputeAndGraphics) counter_mode = 0;
    }
    return counter_mode;
}

// ---- frame pacing: the game's own 30 fps mode when we can't hold 60
// The game advances its world one fixed step per frame in its 60 fps mode, and two steps per frame in its 30 fps mode
// (measured: Link walks twice as far per frame there) - so a 60-mode frame that takes 20 ms plays in slow motion, while
// the 30 mode plays at full speed. The game picks its mode from these counters (GPU time per frame), as on the Switch;
// but our frames can be slow for reasons its GPU time doesn't show (command translation, a slower CPU). So each
// presented frame's busy time is measured (present to present, minus the time the presenting thread was blocked in
// kernel waits and sleeps: idle - the game waits for vsync through its own threads); when
// frames don't fit 60 (12 of the last 60 over 16.2 ms), the counters report a frame of at least PACE_SLOW_NS: the game
// takes its 30 fps mode, at full speed. Back to the real times after 2 s with headroom (busy under 13 ms on average,
// at most 2 of the last 60 frames over). The game itself also tries 60 again now and then.
// LARECOMP_PACE=0: off.
#define PACE_SLOW_NS 20000000ull
extern __thread uint64_t g_blocked_ns;  // kernel.c: this thread's time blocked in waits / sleeps
static bool pace_slow;
static uint64_t stat_pace_over;

void vk_pace_frame(void) {
    static int on = -1;
    // Astral Chain: off unless LARECOMP_PACE=1. It rewrites GPU timer results to steer Link's Awakening's own
    // 30/60 fps switch; Astral Chain's use of those counters has not been established.
    if (on < 0) { const char *e = getenv("LARECOMP_PACE"); on = e && atoi(e) != 0; }
    static uint64_t last, last_wait;
    static uint8_t over[60];
    static int idx, nover, calm;
    static double avg;  // busy ms, moving average (~20 frames)
    uint64_t now = plat_now_ns(), w = g_blocked_ns;
    uint64_t p = now - last, wt = w - last_wait;
    bool first = !last;
    last = now, last_wait = w;
    if (first || p > 100000000ull) return;  // (loading, a hitch: not a pace)
    double busy = (p > wt ? p - wt : 0) / 1e6;
    avg = avg ? avg + (busy - avg) * 0.05 : busy;
    static int log = -1;  // LARECOMP_LOG_PACE=1: each frame's period, vsync wait and busy time
    if (log < 0) log = getenv("LARECOMP_LOG_PACE") != NULL;
    if (log) LOG("[pace] period %.2f blocked %.2f busy %.2f ms\n", p / 1e6, wt / 1e6, busy);
    int o = busy > 16.2;
    stat_pace_over += o;
    nover += o - over[idx];
    over[idx] = (uint8_t)o;
    idx = (idx + 1) % 60;
    if (!on) return;  // (measured and counted all the same: LARECOMP_LOG_PACE, the stats)
    if (!pace_slow && nover >= 12) {
        pace_slow = true, calm = 0;
        LOG("[pace] frames don't fit 60 fps (%d of the last 60 over 16.2 ms busy): the game's 30 fps mode\n", nover);
    } else if (pace_slow) {
        // headroom: busy well under a 60 fps frame on average, hardly a frame over (single spikes are normal play)
        calm = avg < 13.0 && nover <= 2 ? calm + 1 : 0;
        if (calm >= 120) {
            pace_slow = false, nover = 0;
            memset(over, 0, sizeof over);
            LOG("[pace] 2 s with headroom: back to the game's own choice\n");
        }
    }
}

// a pair's end counter (the 16 bytes after its start) is at least PACE_SLOW_NS after it while pace_slow
static uint64_t pace_end_counter(const uint64_t *start_entry, const uint64_t *entry, uint64_t value) {
    if (!pace_slow || !start_entry || (const uint8_t *)entry - (const uint8_t *)start_entry != 16) return value;
    uint64_t min = start_entry[1] + ns_to_gpu_ticks(PACE_SLOW_NS);
    return value < min ? min : value;
}

static void cq_resolve(int i) {  // (cq_lock held) submission slot i is done: its counters get their GPU times
    CounterQ *q = &cq[i];
    if (!q->n) return;
    uint64_t t[CQ_MAX];
    if (vkGetQueryPoolResults(g_vk.dev, q->pool, 0, q->n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod;
        int64_t cand = (int64_t)plat_now_ns() - (int64_t)((double)t[q->n - 1] * per);
        if (!cq_calibrated || cand < cq_offset) cq_offset = cand, cq_calibrated = true;
        static int log = -1;  // LARECOMP_LOG_COUNTER=1: the host (provisional) and GPU times of each counter
        if (log < 0) log = getenv("LARECOMP_LOG_COUNTER") != NULL;
        for (uint32_t k = 0; k < q->n; k++) {
            uint64_t gpu = ns_to_gpu_ticks((uint64_t)((int64_t)((double)t[k] * per) + cq_offset));
            if (log) LOG("[counter] seq %llu #%u at %llx host %.3f gpu %.3f ms\n", (unsigned long long)q->seq, k,
                         (unsigned long long)(uintptr_t)q->out[k], q->out[k][1] * 625.0 / 384 / 1e6, gpu * 625.0 / 384 / 1e6);
            q->out[k][1] = pace_end_counter(k ? q->out[k - 1] : NULL, q->out[k], gpu);
        }
        stat_counters_gpu += q->n;
    }
    q->n = 0;
}

static void cq_resolve_done(uint64_t done) {  // every slot whose submission is done
    pthread_mutex_lock(&cq_lock);
    for (int i = 0; i < SUBS; i++) if (cq[i].n && cq[i].seq <= done) cq_resolve(i);
    pthread_mutex_unlock(&cq_lock);
}

static Sub *sub(void) {
    Sub *s = &subs[cur_sub];
    if (s->seq != submit_seq + 1) {  // slot reused: its previous submission must be done
        wait_why = "ring slot";
        vk_wait_seq(s->seq);
        cq_resolve_done(s->seq);
        if (s->ts) {  // (profiling) GPU time of that submission
            uint64_t t[2];
            if (vkGetQueryPoolResults(g_vk.dev, prof_qp, (uint32_t)cur_sub * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0])
                prof_gpu += (uint64_t)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod);
            s->ts = false;
        }
        for (int i = 0; i < s->npools; i++) vkResetDescriptorPool(g_vk.dev, s->pools[i], 0);
        s->cur_pool = 0;
        s->seq = submit_seq + 1;
        s->main_begun = s->upload_begun = false;
    }
    return s;
}

static void begin_cb(VkCommandBuffer *cb, bool *begun) {
    if (*begun) return;
    if (!*cb) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_vk.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, cb));
    }
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_CHECK(vkBeginCommandBuffer(*cb, &bi));
    *begun = true;
}

static void gp_mark(VkCommandBuffer cb, const char *fmt, ...);
VkCommandBuffer vk_cmd(void) {
    Sub *s = sub();
    if (!s->main_begun) {
        begin_cb(&s->main, &s->main_begun);
        ss_forget();  // (a new command buffer starts with no state)
        if (vk_counter_mode() == 2) {  // this slot's timing counter queries (reset outside any render pass)
            CounterQ *q = &cq[cur_sub];
            if (!q->pool) {
                VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                            .queryCount = CQ_MAX};
                if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &q->pool) != VK_SUCCESS) q->pool = VK_NULL_HANDLE;
            }
            pthread_mutex_lock(&cq_lock);
            q->n = 0;  // (resolved when the slot was freed: sub())
            q->seq = s->seq;
            pthread_mutex_unlock(&cq_lock);
            if (q->pool) vkCmdResetQueryPool(s->main, q->pool, 0, CQ_MAX);
        }
        gp_mark(s->main, "(submission start; includes GPU idle before it)");
        if (prof && prof_qp) {
            vkCmdResetQueryPool(s->main, prof_qp, (uint32_t)cur_sub * 2, 2);
            vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_NONE, prof_qp, (uint32_t)cur_sub * 2);
            s->ts = true;
        }
    }
    return s->main;
}

VkCommandBuffer vk_upload_cmd(void) {
    Sub *s = sub();
    if (!s->upload_begun) {
        begin_cb(&s->upload, &s->upload_begun);
        // after everything submitted before it: a submission's commands may start while the previous one's still run,
        // and these copies overwrite what those may read (textures being replaced, vertex cache pages)
        vk_full_barrier(s->upload);
    }
    return s->upload;
}

void vk_full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static void ring_retire(void);
void vc_flush(void);

// ---- LARECOMP_GPUPROF_AT=<frame>: GPU time per pass / dispatch / copy of that frame, logged as "[gpuprof]".
// A timestamp (ALL_COMMANDS: after everything before it) at each mark; a row = the work since the previous mark.
// LARECOMP_GPUPROF_DRAWS=1: also a mark before every draw (vs/fs guest addresses), to find a slow shader in a pass.
bool g_gpuprof_frame;
enum { GP_MAX = 4096 };
static VkQueryPool gp_qp;
static int gp_n;
static char gp_label[GP_MAX][112];
static uint32_t gp_draws[GP_MAX];
// LARECOMP_VK_DEVLOST_TRACE=1: the last 64 GPU operations, dumped on device loss (with LARECOMP_SYNC_SUBMIT=1 the
// dump ends with the command list that faulted).
static char dl_ring[64][128];
static unsigned dl_n;
static int dl_on = -1;
static void dl_note(const char *fmt, ...) {
    if (dl_on < 0) dl_on = getenv("LARECOMP_VK_DEVLOST_TRACE") ? atoi(getenv("LARECOMP_VK_DEVLOST_TRACE")) : 0;
    if (!dl_on) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dl_ring[dl_n++ & 63], sizeof dl_ring[0], fmt, ap);
    va_end(ap);
}
void vk_dl_note_upload(void *img, uint32_t w, uint32_t h, uint32_t lv) { dl_note("  upload %p %ux%u lv%u", img, w, h, lv); }
void vk_devlost_dump(void) {
    if (!dl_on) return;
    LOG("[vk] last GPU operations before the device loss (oldest first):\n");
    for (unsigned i = dl_n > 64 ? dl_n - 64 : 0; i < dl_n; i++) LOG("[vk]   %s\n", dl_ring[i & 63]);
}
static void gp_mark(VkCommandBuffer cb, const char *fmt, ...) {
    if (!g_gpuprof_frame || gp_n >= GP_MAX - 1) return;
    if (!gp_qp) {
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = GP_MAX};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &gp_qp) != VK_SUCCESS) { g_gpuprof_frame = false; return; }
    }
    if (!gp_n) vkCmdResetQueryPool(cb, gp_qp, 0, GP_MAX);
    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, gp_qp, (uint32_t)gp_n);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gp_label[gp_n], sizeof gp_label[0], fmt, ap);
    va_end(ap);
    gp_draws[gp_n] = 0;
    gp_n++;
}
void renderer_flush(void);
void vk_gpuprof_report(void) {
    if (!gp_n) return;
    renderer_flush();
    static uint64_t t[GP_MAX];
    if (vkGetQueryPoolResults(g_vk.dev, gp_qp, 0, (uint32_t)gp_n, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
        double per = g_vk.props.limits.timestampPeriod / 1e6, total = (t[gp_n - 1] - t[0]) * per;
        LOG("[gpuprof] %d marks, %.2f ms first to last (rows: the work after each mark, until the next)\n", gp_n, total);
        for (int i = 0; i + 1 < gp_n; i++) {
            double ms = (t[i + 1] - t[i]) * per;
            LOG("[gpuprof] %6.3f ms %5.1f%%  %s%s", ms, 100 * ms / (total > 0 ? total : 1), gp_label[i], gp_draws[i] ? "" : "\n");
            if (gp_draws[i]) LOG(", %u draws\n", gp_draws[i]);
        }
    }
    gp_n = 0;
}

// Submit the pending work (if any). wait/signal: binary semaphores for presentation (may be null).
void vk_submit_extra(VkSemaphore wait, VkSemaphore signal, VkCommandBuffer extra) {
    (void)extra;
    vk_end_pass();
    Sub *s = sub();
    if (!s->main_begun && !s->upload_begun && !wait && !signal) return;
    VkCommandBufferSubmitInfo cbs[2];
    int n = 0;
    gmem_sync();
    vc_flush();
    if (s->upload_begun) {
        vk_full_barrier(s->upload);  // uploads complete before anything in the main buffer reads them
        VK_CHECK(vkEndCommandBuffer(s->upload));
        cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->upload};
    }
    vk_cmd();  // (an empty main buffer still carries the semaphores)
    gmem_wb_record(s->main, submit_seq + 1);
    if (s->ts) vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_qp, (uint32_t)cur_sub * 2 + 1);
    VK_CHECK(vkEndCommandBuffer(s->main));
    cbs[n++] = (VkCommandBufferSubmitInfo){.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->main};
    VkSemaphoreSubmitInfo sig[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = g_vk.timeline,
                                     .value = submit_seq + 1, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
                                    {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = signal,
                                     .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    VkSemaphoreSubmitInfo wt = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait,
                                .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = (uint32_t)n, .pCommandBufferInfos = cbs,
                        .signalSemaphoreInfoCount = signal ? 2 : 1, .pSignalSemaphoreInfos = sig,
                        .waitSemaphoreInfoCount = wait ? 1 : 0, .pWaitSemaphoreInfos = &wt};
    {
        PROF_T0();
        dl_note("submit %llu", (unsigned long long)submit_seq + 1);
        VkResult sr = vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
        if (sr == VK_ERROR_DEVICE_LOST) vk_devlost_dump();
        VK_CHECK(sr);
        PROF_ADD(prof_submit);
    }
    pthread_mutex_lock(&seq_lock);
    submit_seq++;
    hang_watch_submitted(submit_seq);
    pthread_mutex_unlock(&seq_lock);
    cur_sub = (cur_sub + 1) % SUBS;
    stat_flushes++;
    cum_submits++;
    ring_retire();
    vk_collect_dead();
}

void renderer_submit(void) { vk_submit_extra(VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE); }

void renderer_flush(void) {
    renderer_submit();
    if (!wait_why) wait_why = "flush";
    vk_wait_seq(submit_seq);
}

void renderer_sync(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    wait_why = "QueueFinish / sync all";
    vk_wait_seq(s);
}

uint64_t renderer_fence_seq(void) {
    pthread_mutex_lock(&seq_lock);
    uint64_t q = submit_seq + (subs[cur_sub].main_begun || subs[cur_sub].upload_begun ? 1 : 0);
    pthread_mutex_unlock(&seq_lock);
    return q;
}

// Astral Chain: cooperative waits. The submission a fence wait needs (capped at what has been submitted, as
// renderer_wait_seq does), and whether the GPU has finished it; nvn_hle.h parks only the waiting guest thread.
uint64_t renderer_wait_target(uint64_t seq) {
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    return seq > s ? s : seq;
}
static void gmem_writeback_apply(uint64_t done);
int renderer_seq_done(uint64_t seq) {
    uint64_t done = vk_completed_seq();
    gmem_writeback_apply(done);
    return !seq || done >= seq;
}

void renderer_wait_seq(uint64_t seq) {
    if (!seq) return;
    pthread_mutex_lock(&seq_lock);
    uint64_t s = submit_seq;
    pthread_mutex_unlock(&seq_lock);
    wait_why = seq > s ? "SyncWait (fence not submitted yet)" : "SyncWait";
    vk_wait_seq(seq > s ? s : seq);  // not submitted yet (other thread): everything submitted so far
}

VkDescriptorSet vk_alloc_set(VkDescriptorSetLayout layout) {
    Sub *s = sub();
    for (;;) {
        if (s->cur_pool == s->npools) {
            if (s->npools == 16) FATAL("descriptor pools exhausted\n");
            VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096},
                                            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
                                            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1024},
                                            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096}};  // (Astral Chain: SUST)
            VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192,
                                             .poolSizeCount = 6, .pPoolSizes = sizes};
            VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &s->pools[s->npools++]));
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = s->pools[s->cur_pool], .descriptorSetCount = 1, .pSetLayouts = &layout};
        VkDescriptorSet set;
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, &set);
        if (r == VK_SUCCESS) return set;
        s->cur_pool++;
    }
}

// ================================================================== transient memory ring
// 64 MB host-visible chunks (device-local when the GPU exposes resizable BAR); a chunk is reused once every
// submission that allocated from it has completed.
#define CHUNK_SIZE (64ull << 20)
typedef struct { VkBuffer buf; VmaAllocation mem; uint8_t *ptr; uint64_t size, used, seq; } Chunk;
static Chunk *chunks;
static int nchunks, active = -1;

static int new_chunk(uint64_t size) {
    chunks = realloc(chunks, (nchunks + 1) * sizeof *chunks);
    Chunk *c = &chunks[nchunks];
    memset(c, 0, sizeof *c);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
    if (getenv("LARECOMP_VK_RING_SYSMEM")) ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;  // (test: a GPU without resizable BAR)
    VmaAllocationInfo info;
    VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &c->buf, &c->mem, &info));
    c->ptr = info.pMappedData;
    c->size = size;
    if (!nchunks) {  // where the ring lives: device-local (resizable BAR) or system memory read across PCIe
        VkMemoryPropertyFlags f;
        vmaGetMemoryTypeProperties(g_vk.vma, info.memoryType, &f);
        LOG("[vk] transient ring: memory type %u (%s%s%s)\n", info.memoryType, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "device-local " : "system memory ",
            f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "host-visible " : "", f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "cached" : "");
    }
    return nchunks++;
}

static void ring_retire(void) {}  // chunks are recycled lazily in vk_transient

VkSpan vk_transient(uint64_t size, uint64_t align) {
    if (align < 16) align = 16;
    uint64_t pending = vk_pending_seq();
    if (active >= 0) {
        Chunk *c = &chunks[active];
        uint64_t off = (c->used + align - 1) & ~(align - 1);
        if (off + size <= c->size) {
            c->used = off + size;
            c->seq = pending;
            stat_upload_bytes += size;
            return (VkSpan){c->buf, off, c->ptr + off};
        }
    }
    uint64_t done = vk_completed_seq();
    int pick = -1;
    for (int i = 0; i < nchunks; i++)
        if (i != active && chunks[i].seq <= done && chunks[i].size >= size) { pick = i; break; }
    if (pick < 0) pick = new_chunk(size > CHUNK_SIZE ? size : CHUNK_SIZE);
    active = pick;
    chunks[pick].used = size;
    chunks[pick].seq = pending;
    stat_upload_bytes += size;
    return (VkSpan){chunks[pick].buf, 0, chunks[pick].ptr};
}

// GPU -> CPU copies: the ring is write-combined (often device-local over the BAR), where CPU reads crawl (a
// 1280x720 screenshot took 7 s). Readbacks use one host-cached buffer instead. Every user waits for the GPU and
// reads the data before the next readback (render thread), so a single grow-only buffer suffices.
VkSpan vk_readback(uint64_t size) {
    static VkBuffer buf;
    static VmaAllocation mem;
    static uint8_t *ptr;
    static uint64_t cap;
    if (size > cap) {
        if (buf) { vk_wait_seq(submit_seq); vmaDestroyBuffer(g_vk.vma, buf, mem); }
        cap = (size + (16 << 20) - 1) & ~((16ull << 20) - 1);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = cap, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, &info));
        ptr = info.pMappedData;
    }
    return (VkSpan){buf, 0, ptr};
}

VkSpan vk_transient_copy(const void *data, uint64_t size, uint64_t align) {
    VkSpan s = vk_transient(size ? size : 16, align);
    if (size) memcpy(s.ptr, data, size);
    return s;
}

// Per-draw constants that usually repeat the previous draw's (scale table, tint, driver constants): reuse that copy
// while it's identical, within one submission. slot: one per kind of data.
enum { DEDUPE_RSCALE_V, DEDUPE_RSCALE_F, DEDUPE_TINT, DEDUPE_DRV_V, DEDUPE_DRV_F, DEDUPE_DRV_C, DEDUPE_SLOTS };
static VkSpan copy_dedupe(int slot, const void *data, uint64_t size, uint64_t align) {
    static struct { uint64_t seq, size; uint8_t data[0x800]; VkSpan span; } last[DEDUPE_SLOTS];
    uint64_t seq = vk_pending_seq();
    if (size > sizeof last[0].data) return vk_transient_copy(data, size, align);
    if (last[slot].seq == seq && last[slot].size == size && !memcmp(last[slot].data, data, size)) return last[slot].span;
    VkSpan s = vk_transient_copy(data, size, align);
    last[slot].seq = seq; last[slot].size = size; memcpy(last[slot].data, data, size); last[slot].span = s;
    return s;
}

// Guest memory copied once per submission: draws binding the same range share the copy.
#define GCOPY_TAB 4096
static struct { uint64_t addr, size, seq; VkSpan span; } gcopy[GCOPY_TAB];
static uint64_t gcopy_gen;  // bumped to drop every shared copy (guest memory written mid-list)
static void guest_copy_flush(void) { gcopy_gen++; }
static VkSpan guest_copy(uint64_t addr, uint64_t size) {
    uint64_t seq = vk_pending_seq() + (gcopy_gen << 40), h = (addr * 0x9E3779B97F4A7C15ull) ^ (size * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (GCOPY_TAB - 1);
    if (gcopy[slot].seq == seq && gcopy[slot].addr == addr && gcopy[slot].size == size) return gcopy[slot].span;
    VkSpan s = vk_transient_copy((const void *)(uintptr_t)addr, size, 256);
    gcopy[slot].addr = addr; gcopy[slot].size = size; gcopy[slot].seq = seq; gcopy[slot].span = s;
    return s;
}

// ================================================================== worker threads (shader / pipeline builds)
// First-use compiles run off the render thread; draws needing them are skipped until ready (as on Metal).
// LARECOMP_SYNC_SHADERS=1 builds on first use, blocking (deterministic tests).
typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; } Job;
static Job *jobs_head, *jobs_tail;
static pthread_mutex_t jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jobs_cond = PTHREAD_COND_INITIALIZER;
// Low priority (pipeline warm-up): taken only when no other job waits, by at most half of the workers at a time.
static Job *ljobs_head, *ljobs_tail;
static int ljobs_running, ljobs_max = 2;

static void *worker(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&jobs_lock);
        while (!jobs_head && !(ljobs_head && ljobs_running < ljobs_max)) pthread_cond_wait(&jobs_cond, &jobs_lock);
        Job *j;
        bool low = !jobs_head;
        if (!low) {
            j = jobs_head;
            jobs_head = j->next;
            if (!jobs_head) jobs_tail = NULL;
        } else {
            j = ljobs_head;
            ljobs_head = j->next;
            if (!ljobs_head) ljobs_tail = NULL;
            ljobs_running++;
        }
        pthread_mutex_unlock(&jobs_lock);
        j->fn(j->arg);
        free(j);
        if (low) {
            pthread_mutex_lock(&jobs_lock);
            ljobs_running--;
            if (ljobs_head) pthread_cond_signal(&jobs_cond);
            pthread_mutex_unlock(&jobs_lock);
        }
    }
    return NULL;
}

static bool sync_shaders(void) {
    static int v = -1;
    if (v < 0) v = getenv("LARECOMP_SYNC_SHADERS") != NULL;
    return v;
}

// Shader translation: one dedicated thread (the translator isn't re-entrant - translate.c's RSC2 string ring - and
// translating on the game thread cost up to ~90 ms per frame on entering an area). Compiles go on to the workers.
static pthread_mutex_t translate_lock = PTHREAD_MUTEX_INITIALIZER;  // (compute shaders translate on the game thread)
static Job *tjobs_head, *tjobs_tail;
static pthread_mutex_t tjobs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tjobs_cond = PTHREAD_COND_INITIALIZER;
static void *translator(void *a) {
    (void)a;
    for (;;) {
        pthread_mutex_lock(&tjobs_lock);
        while (!tjobs_head) pthread_cond_wait(&tjobs_cond, &tjobs_lock);
        Job *j = tjobs_head;
        tjobs_head = j->next;
        if (!tjobs_head) tjobs_tail = NULL;
        pthread_mutex_unlock(&tjobs_lock);
        j->fn(j->arg);
        free(j);
    }
    return NULL;
}
static bool sync_shaders(void);
static void run_translate(void (*fn)(void *), void *arg) {
    if (sync_shaders()) { fn(arg); return; }
    static bool started;
    if (!started) { started = true; pthread_t t; pthread_create(&t, NULL, translator, NULL); }
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    pthread_mutex_lock(&tjobs_lock);
    if (tjobs_tail) tjobs_tail->next = j; else tjobs_head = j;
    tjobs_tail = j;
    pthread_cond_signal(&tjobs_cond);
    pthread_mutex_unlock(&tjobs_lock);
}

static void workers_start(void) {
    static bool started;
    if (!started) {
        started = true;
        // first entry into an area builds dozens of pipelines at once: use most of the CPU (draws wait for them)
        int n = SDL_GetNumLogicalCPUCores() - 4;
        if (n < 4) n = 4;
        if (n > 16) n = 16;
        ljobs_max = n / 2;
        for (int i = 0; i < n; i++) { pthread_t t; pthread_create(&t, NULL, worker, NULL); }
    }
}

static void run_job(void (*fn)(void *), void *arg) {
    if (sync_shaders()) { fn(arg); return; }
    workers_start();
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    pthread_mutex_lock(&jobs_lock);
    if (jobs_tail) jobs_tail->next = j; else jobs_head = j;
    jobs_tail = j;
    pthread_cond_signal(&jobs_cond);
    pthread_mutex_unlock(&jobs_lock);
}

static void run_job_low(void (*fn)(void *), void *arg) {
    workers_start();
    Job *j = malloc(sizeof *j);
    j->fn = fn; j->arg = arg; j->next = NULL;
    pthread_mutex_lock(&jobs_lock);
    if (ljobs_tail) ljobs_tail->next = j; else ljobs_head = j;
    ljobs_tail = j;
    pthread_cond_signal(&jobs_cond);
    pthread_mutex_unlock(&jobs_lock);
}

// (Astral Chain) a queued job a draw now waits for moves to the front of its queue: warm-up jobs (program warm-up
// below) must not delay the builds the current frame needs.
static void promote_job(Job **head, Job **tail, pthread_mutex_t *lock, void *arg) {
    pthread_mutex_lock(lock);
    for (Job *prev = NULL, *j = *head; j; prev = j, j = j->next) {
        if (j->arg != arg) continue;
        if (prev) {
            prev->next = j->next;
            if (*tail == j) *tail = prev;
            j->next = *head;
            *head = j;
        }
        break;
    }
    pthread_mutex_unlock(lock);
}

// ================================================================== state
typedef struct {
    NvnTexture *color[8];
    NvnTextureView cview[8];
    uint8_t has_cview[8];
    int ncolor;
    NvnTexture *depth;
    NvnTextureView dview;
    uint8_t has_dview;
} RtState;

typedef struct { uint64_t addr, size; } Range;

static struct {
    RtState rt;
    bool rt_dirty;
    NvnProgram *program;
    uint8_t blend[8][8];
    uint32_t channel_mask;
    uint8_t color_state[4], ds[8], ms[0x18], poly[4];
    float poly_offset[3];
    float alpha_ref;  // nvnCommandBufferSetAlphaRef (the test itself: color_state[2])
    uint32_t attrib[16];
    int nattrib;
    uint32_t stream[16][2];
    int nstream;
    Range vbuf[16], ubuf[6][18], sbuf[6][16];
    uint64_t tex[6][32], img[6][8];
    NvnDescPool *tpool, *spool;
    float viewport[16][4];
    int32_t scissor[16][4];
    float depth_range[16][2];
    float blend_color[4];
    uint32_t stencil_ref[2], stencil_mask[2], stencil_vmask[2];
    bool prim_restart;
    uint32_t restart_index;
    uint64_t copy_row_stride, copy_image_stride;
} st;

uint32_t nvn_window_origin(void);
uint32_t nvn_depth_mode(void);
bool renderer_origin_lower_left(void) { return nvn_window_origin() == 0; }

// (LARECOMP_VK_PROF) called once per presented frame: frames over 50 ms are logged with where their time went.
// present_ns: time in backend_present's acquire/blit/present.
void vk_prof_frame(uint64_t frame, uint64_t present_ns) {
    if (!prof) return;
    static uint64_t last_t, l_exec, l_wait, l_draws, l_subs, l_up, l_built;
    uint64_t now = SDL_GetTicksNS();
    if (last_t && now - last_t > 50000000) {
        uint64_t dt = now - last_t, ex = cum_exec - l_exec, wt = cum_wait - l_wait;
        LOG("[hitch] frame %llu at %.3f s: %.0f ms = backend exec %.1f (of which GPU waits %.1f, texture uploads %.1f, waits for "
            "pipelines %.1f) + present %.1f + other %.1f; %llu draws, %llu submits, compiles %d pending\n", (unsigned long long)frame,
            now / 1e9, dt / 1e6, ex / 1e6, wt / 1e6, (cum_up_ns - l_up) / 1e6, (cum_built_ns - l_built) / 1e6, present_ns / 1e6,
            ((double)dt - ex - present_ns) / 1e6, (unsigned long long)(cum_draws - l_draws), (unsigned long long)(cum_submits - l_subs),
            atomic_load(&stat_compiles));
    }
    last_t = now; l_exec = cum_exec; l_wait = cum_wait; l_draws = cum_draws; l_subs = cum_submits; l_up = cum_up_ns;
    l_built = cum_built_ns;
}

const char *renderer_stats(void) {
    static int dumps;
    if (dumps++ < 3) {
        LOG("[vk] command counts:");
        for (int i = 0; i < CMD_COUNT; i++) if (op_counts[i]) LOG(" %d:%llu", i, (unsigned long long)op_counts[i]);
        LOG("\n");
    }
    snprintf(stats_text, sizeof stats_text,
             "%llu draws, %llu skipped, %llu passes, %llu dispatches, %llu flushes, wait %.0fms, compile %d, fbcopy %llu, upload %.1fMB",
             (unsigned long long)stat_draws, (unsigned long long)stat_skipped, (unsigned long long)stat_passes,
             (unsigned long long)stat_dispatches, (unsigned long long)stat_flushes, (double)stat_wait_ns / 1e6,
             atomic_exchange(&stat_compiles, 0), (unsigned long long)stat_feedback_copies, (double)stat_upload_bytes / 1048576.0);
    stat_feedback_copies = stat_upload_bytes = 0;
    {
        unsigned long long tr = atomic_exchange(&build_ns_translate, 0), sp = atomic_exchange(&build_ns_spirv, 0),
                           pi = atomic_exchange(&build_ns_pipe, 0);
        size_t n = strlen(stats_text);
        if (tr || sp || pi)
            snprintf(stats_text + n, sizeof stats_text - n, ", build ms translate %.0f spirv %.0f pipelines %.0f", tr / 1e6, sp / 1e6, pi / 1e6);
        unsigned long long ph = atomic_exchange(&build_pipe_hits, 0), pm = atomic_exchange(&build_pipe_misses, 0),
                           pmn = atomic_exchange(&build_ns_pipe_miss, 0);
        n = strlen(stats_text);
        if (ph || pm) snprintf(stats_text + n, sizeof stats_text - n, " (cache hits %llu, misses %llu in %.0f ms)", ph, pm, pmn / 1e6);
        n = strlen(stats_text);
        if (pwarm_queued || pwarm_ready || pwarm_late || pwarm_rejected)
            snprintf(stats_text + n, sizeof stats_text - n, ", pipeline warm-up queued %llu ready %llu late %llu rejected %llu",
                     pwarm_queued, pwarm_ready, pwarm_late, pwarm_rejected);
        pwarm_queued = pwarm_ready = pwarm_late = pwarm_rejected = 0;
        unsigned long long sb = atomic_exchange(&gpl_stage_built, 0), sh = atomic_exchange(&gpl_stage_hits, 0),
                           sn = atomic_exchange(&gpl_stage_ns, 0), lk = atomic_exchange(&gpl_linked, 0),
                           ln = atomic_exchange(&gpl_link_ns, 0), ob = atomic_exchange(&gpl_opt_built, 0);
        n = strlen(stats_text);
        if (sb || sh || lk || ob)
            snprintf(stats_text + n, sizeof stats_text - n, ", shader libraries built %llu in %.0f ms (reused %llu), linked %llu in %.1f ms, optimized %llu",
                     sb, sn / 1e6, sh, lk, ln / 1e6, ob);
        n = strlen(stats_text);
        if (warm_stat_queued || warm_stat_ready || warm_stat_late)
            snprintf(stats_text + n, sizeof stats_text - n, ", warm-up queued %llu ready %llu late %llu", (unsigned long long)warm_stat_queued,
                     (unsigned long long)warm_stat_ready, (unsigned long long)warm_stat_late);
        warm_stat_queued = warm_stat_ready = warm_stat_late = 0;
    }
    {   // the game's video memory (VMA's allocations in device-local heaps), e.g. for LARECOMP_BC7 comparisons
        const VkPhysicalDeviceMemoryProperties *mp;
        vmaGetMemoryProperties(g_vk.vma, &mp);
        VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
        vmaGetHeapBudgets(g_vk.vma, budgets);
        uint64_t used = 0;
        for (uint32_t i = 0; i < mp->memoryHeapCount; i++)
            if (mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) used += budgets[i].statistics.allocationBytes;
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, ", video memory %.0fMB", used / 1048576.0);
        extern void vk_bc7_cache_stats(uint64_t *hits, uint64_t *misses);
        uint64_t bh, bm;
        vk_bc7_cache_stats(&bh, &bm);
        n = strlen(stats_text);
        if (bh || bm) snprintf(stats_text + n, sizeof stats_text - n, " (BC7 precomputed: %llu hits, %llu misses)",
                               (unsigned long long)bh, (unsigned long long)bm);
    }
    {
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, ", mirror in %.1fMB, write-back %.1fMB in %llu (%llu faults, %d lazy)",
                 (double)stat_mirror_bytes / 1048576.0, (double)stat_wb_bytes / 1048576.0, (unsigned long long)stat_wb_ranges,
                 (unsigned long long)stat_wb_faults, lazy_n);
        stat_mirror_bytes = stat_wb_bytes = stat_wb_ranges = stat_wb_faults = 0;
        for (int k = 0; k < 3; k++) {  // the three most frequent skip sites (exec.c lines)
            int best = 0;
            for (int l = 1; l < 8192; l++) if (skip_lines[l] > skip_lines[best]) best = l;
            if (!skip_lines[best]) break;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, "%s skip@%d %u", k ? "," : ";", best, skip_lines[best]);
            skip_lines[best] = 0;
        }
        memset(skip_lines, 0, sizeof skip_lines);
    }
    {
        extern void vk_alias_stats(void);
        vk_alias_stats();
    }
    if (prof) {
        prof_up = prof_up_ns;
        prof_up_ns = 0;
        size_t n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | gpu %.0f cpu exec %.0f draw %.0f (pre %.0f key %.0f dyn %.0f sets %.0f tex %.0f (upload %.0f) vtx %.0f) submit %.0f ms",
                 prof_gpu / 1e6, prof_exec / 1e6, prof_draw / 1e6, prof_pre / 1e6, prof_key / 1e6, prof_dyn / 1e6, prof_sets / 1e6,
                 prof_tex / 1e6, prof_up / 1e6, prof_vtx / 1e6, prof_submit / 1e6);
        extern uint64_t prof_tex_checks, prof_tex_samples, prof_tex_full, prof_tex_full_bytes, prof_tex_dyn;
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | tex checks %llu samples %llu full %llu (%.1f MB, dynamic %llu)",
                 (unsigned long long)prof_tex_checks, (unsigned long long)prof_tex_samples, (unsigned long long)prof_tex_full,
                 prof_tex_full_bytes / 1048576.0, (unsigned long long)prof_tex_dyn);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | pass ends:");
        for (int l = 0; l < 4096; l++)
            if (prof_end_line[l] > 20) { n = strlen(stats_text); snprintf(stats_text + n, sizeof stats_text - n, " %d:%u", l, prof_end_line[l]); }
        memset(prof_end_line, 0, sizeof prof_end_line);
        n = strlen(stats_text);
        snprintf(stats_text + n, sizeof stats_text - n, " | views created %llu | counters gpu %llu | pace %s, %llu frames over", (unsigned long long)prof_views_created,
                 (unsigned long long)stat_counters_gpu, pace_slow ? "30" : "60", (unsigned long long)stat_pace_over);
        stat_counters_gpu = stat_pace_over = 0;
        {
            extern uint64_t prof_vc_hits, prof_vc_bypass, prof_vc_copied, prof_vc_queries, prof_vc_scan_pages, prof_vc_scan_ns;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n,  " | vcache %llu hit %llu in place %.1f MB copied, %llu queries %.2f ms, %llu pages written",
                     (unsigned long long)prof_vc_hits, (unsigned long long)prof_vc_bypass, prof_vc_copied / 1048576.0,
                     (unsigned long long)prof_vc_queries, prof_vc_scan_ns / 1e6, (unsigned long long)prof_vc_scan_pages);
            prof_vc_hits = prof_vc_bypass = prof_vc_copied = prof_vc_queries = prof_vc_scan_pages = prof_vc_scan_ns = 0;
            extern uint64_t prof_tex_watch_skips, prof_tex_unknown, prof_tex_warmed;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | tex unwritten %llu untracked %llu warmed %llu", (unsigned long long)prof_tex_watch_skips,
                     (unsigned long long)prof_tex_unknown, (unsigned long long)prof_tex_warmed);
            prof_tex_watch_skips = prof_tex_unknown = prof_tex_warmed = 0;
            extern uint64_t prof_ww_calls, prof_ww_qpc, prof_ww_pages, prof_ww_mirror, prof_ww_clean, prof_ww_direct, prof_ww_span;
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, " | write watch %llu calls %.1f ms %.0f MB (textures %llu + spans %llu, mirror %llu, clean %llu)",
                     (unsigned long long)prof_ww_calls, prof_ww_qpc / 1e6, prof_ww_pages / 256.0, (unsigned long long)prof_ww_direct,
                     (unsigned long long)prof_ww_span, (unsigned long long)prof_ww_mirror, (unsigned long long)prof_ww_clean);
            prof_ww_calls = prof_ww_qpc = prof_ww_pages = prof_ww_mirror = prof_ww_clean = prof_ww_direct = prof_ww_span = 0;
        }
        pthread_mutex_lock(&wait_tab_lock);
        for (int i = 0; i < 12 && wait_tab[i].why; i++) {
            n = strlen(stats_text);
            snprintf(stats_text + n, sizeof stats_text - n, "%s%s %llux %.0f ms", i ? "," : " | waits:", wait_tab[i].why,
                     (unsigned long long)wait_tab[i].n, wait_tab[i].ns / 1e6);
        }
        memset(wait_tab, 0, sizeof wait_tab);
        pthread_mutex_unlock(&wait_tab_lock);
        prof_views_created = 0;
        prof_tex_checks = prof_tex_samples = prof_tex_full = prof_tex_full_bytes = prof_tex_dyn = 0;
        prof_exec = prof_draw = prof_sets = prof_vtx = prof_submit = prof_pre = prof_key = prof_dyn = prof_tex = prof_up = prof_gpu = 0;
    }
    stat_draws = stat_skipped = stat_passes = stat_dispatches = stat_flushes = stat_wait_ns = 0;
    return stats_text;
}

static void st_defaults(void) {
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 16; i++) {
        st.viewport[i][2] = 1280; st.viewport[i][3] = 720;
        st.scissor[i][2] = 16384; st.scissor[i][3] = 16384;
        st.depth_range[i][1] = 1.0f;
    }
    st.channel_mask = 0xFFFFFFFF;
    st.stencil_mask[0] = st.stencil_mask[1] = 0xFF;
    st.stencil_vmask[0] = st.stencil_vmask[1] = 0xFF;
    st.ds[1] = 1; st.ds[2] = 2;
    st.poly[1] = 1; st.poly[2] = 2;
    for (int i = 0; i < 8; i++) { st.blend[i][1] = 2; st.blend[i][2] = 1; st.blend[i][3] = 2; st.blend[i][4] = 1; st.blend[i][5] = 1; st.blend[i][6] = 1; }
}

static void warm_program(NvnProgram *np);
void vk_exec_init(void) {
    {   // program warm-up: nvnProgramSetShaders (objects.c) queues known variants of the program it sets
        extern void (*nvn_program_warm_hook)(NvnProgram *np);
        nvn_program_warm_hook = warm_program;
    }
    prof = getenv("LARECOMP_VK_PROF") != NULL;
    if (prof) {
        prof_up_ns = 0;
        VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = SUBS * 2};
        if (vkCreateQueryPool(g_vk.dev, &qi, NULL, &prof_qp) != VK_SUCCESS) prof_qp = VK_NULL_HANDLE;
    }
    st_defaults();
}

void renderer_texture_scale(NvnTexture *t, float *sx, float *sy);
bool renderer_texture_presented(NvnTexture *t);
bool renderer_texture_first_write(NvnTexture *t);
bool renderer_texture_level_fresh(NvnTexture *t);
bool renderer_texture_frame_fresh(NvnTexture *t);
void renderer_texture_mark_presented(NvnTexture *t);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);
static float pass_sx = 1, pass_sy = 1;
static float vp_sx = 1, vp_sy = 1, vp_ox = 0, vp_oy = 0;
static float tex_sc[2][64][2];
static uint32_t tex_wh[2][64][2];

// ================================================================== render passes
static VkFormat pass_color[8], pass_depth, pass_stencil;
static uint32_t pass_ncolor, pass_samples = 1, pass_w, pass_h;
static int debug_pass_no;

// Redundant-state filter: what draw() last set in the current command buffer, so unchanged pipeline binds, dynamic
// state and vertex / index buffer binds are not recorded again (a draw used to issue ~20 vkCmd* calls; most repeat the
// previous draw's). Forgotten (ss_forget) whenever something else may have recorded state: a new command buffer, a new
// render pass (present, ImGui, post-processing and copies all run between game passes) and the clear pipeline.
// Descriptor sets are not filtered: every draw writes new ones (sharing identical sets within a submission was tried:
// a third matched, but comparing them cost more than allocating and writing them on NVIDIA).
// LARECOMP_VK_STATE_FILTER=0: every draw sets everything, as before.
enum { SS_PIPE = 1 << 0, SS_DT = 1 << 1, SS_DW = 1 << 2, SS_DOP = 1 << 3, SS_STEN = 1 << 4, SS_SFACE0 = 1 << 5, SS_SFACE1 = 1 << 6,
       SS_BC = 1 << 7, SS_CULL = 1 << 8, SS_FF = 1 << 9, SS_TOPO = 1 << 10, SS_PRE = 1 << 11, SS_DBE = 1 << 12, SS_DB = 1 << 13,
       SS_VP = 1 << 14, SS_SC = 1 << 15, SS_IB = 1 << 16 };
static struct {
    uint32_t known;  // SS_* bits: the value below is what the command buffer has
    uint32_t vb_known;  // per vertex binding
    VkPipeline pipe;
    VkBool32 dt, dw, sten, pre, dbe;
    VkCompareOp dop;
    uint32_t sface[2][7];  // fail, pass, depth fail, compare, compare mask, write mask, reference
    float bc[4], db[3];
    VkCullModeFlags cull;
    VkFrontFace ff;
    VkPrimitiveTopology topo;
    VkViewport vp;
    VkRect2D sc;
    VkBuffer ib; VkDeviceSize io; VkIndexType it;
    VkBuffer vb[17]; VkDeviceSize vo[17];
} ss;
static int ss_on = -1;
static void ss_forget(void) { ss.known = 0; ss.vb_known = 0; }
static bool ss_enabled(void) {
    if (ss_on < 0) { const char *e = getenv("LARECOMP_VK_STATE_FILTER"); ss_on = !e || atoi(e) != 0; }
    return ss_on;
}
// true: the state must be recorded (and is remembered as known)
static inline bool ss_need(uint32_t bit, bool same) {
    if (ss_on && (ss.known & bit) && same) return false;
    ss.known |= bit;
    return true;
}

static void vk_end_pass_at(int line);
void vk_end_pass(void) { vk_end_pass_at(0); }
#define vk_end_pass() vk_end_pass_at(__LINE__)
static void vk_end_pass_at(int line) {
    if (!in_pass) return;
    if (prof && line < 4096) prof_end_line[line]++;
    vkCmdEndRendering(vk_cmd());
    in_pass = false;
    if (g_debug_frame && st.rt.ncolor && st.rt.color[0] && st.rt.color[0]->backend) {
        char path[256];
        static uint64_t debug_pass_frame = ~0ull;
        static int multi = -1;
        if (multi < 0) multi = getenv("LARECOMP_DEBUG_FRAMES") != NULL;
        if (multi && renderer_present_count() != debug_pass_frame) { debug_pass_frame = renderer_present_count(); debug_pass_no = 0; }
        if (multi) snprintf(path, sizeof path, "build/pass_%llu_%03d.png", (unsigned long long)debug_pass_frame, debug_pass_no++);
        else snprintf(path, sizeof path, "build/pass_%03d.png", debug_pass_no++);
        renderer_save_texture_tex(st.rt.color[0]->backend, path);
    }
}

static uint32_t level_extent(uint32_t v, uint32_t lv) { v >>= lv; return v ? v : 1; }

static void begin_pass(void) {
    if (in_pass && !st.rt_dirty) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each pass its own submission)
    st.rt_dirty = false;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);  // passes, copies and dispatches see each other's results
    {
        NvnTexture *t0n = st.rt.ncolor ? st.rt.color[0] : NULL;
        dl_note("pass %d color %ux%u fmt %#x depth %p", st.rt.ncolor, t0n ? t0n->width : 0, t0n ? t0n->height : 0, t0n ? t0n->format : 0,
                (void *)st.rt.depth);
    }
    if (g_gpuprof_frame) {
        NvnTexture *t0 = st.rt.ncolor ? st.rt.color[0] : NULL;
        gp_mark(cb, "pass: %d color %ux%u fmt %#x, depth %s", st.rt.ncolor, t0 ? t0->width : 0, t0 ? t0->height : 0,
                t0 ? t0->format : 0, st.rt.depth ? "yes" : "no");
    }
    VkRenderingAttachmentInfo ca[8], da = {0}, sa = {0};
    uint32_t w = 0, minw = UINT32_MAX, minh = UINT32_MAX;  // w: first attachment seen
    pass_ncolor = (uint32_t)st.rt.ncolor;
    pass_samples = 1;
    for (int i = 0; i < st.rt.ncolor; i++) {
        NvnTexture *t = st.rt.color[i];
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        pass_color[i] = VK_FORMAT_UNDEFINED;
        if (!t) continue;
        renderer_mark_gpu_owned(t);
        VkImageView v = vk_view(t, &st.rt.cview[i], st.rt.has_cview[i], true, TEXTYPE_2D);
        TexBackend *b = t->backend;
        if (!v) continue;
        ca[i].imageView = v;
        ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        bool fresh = (int)renderer_texture_level_fresh(t) | (int)renderer_texture_frame_fresh(t);  // both: each marks its state
        if (renderer_texture_first_write(t) || fresh) {
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            ca[i].clearValue.color.float32[3] = fresh ? 0.0f : 1.0f;
        }
        pass_color[i] = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 4) && vk_format(t->format) == b->format
                      ? vk_format(st.rt.cview[i].format) : b->format;
        uint32_t lv = st.rt.has_cview[i] && (st.rt.cview[i].setmask & 1) ? st.rt.cview[i].base_level : 0;
        uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
        if (!w) { renderer_texture_scale(t, &pass_sx, &pass_sy); w = ew; }
        if (ew < minw) minw = ew;
        if (eh < minh) minh = eh;
        pass_samples = b->samples;
    }
    pass_depth = pass_stencil = VK_FORMAT_UNDEFINED;
    bool has_d = false, has_s = false;
    if (st.rt.depth) {
        renderer_mark_gpu_owned(st.rt.depth);
        VkImageView v = vk_view(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, TEXTYPE_2D);
        TexBackend *b = st.rt.depth->backend;
        if (v && renderer_format_is_depth(st.rt.depth->format)) {
            VkRenderingAttachmentInfo a = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = v,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            if (b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { da = a; has_d = true; pass_depth = b->format; }
            if (b->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sa = a; has_s = true; pass_stencil = b->format; }
            uint32_t lv = st.rt.has_dview && (st.rt.dview.setmask & 1) ? st.rt.dview.base_level : 0;
            uint32_t ew = level_extent(b->width, lv), eh = level_extent(b->height, lv);
            if (!w) { renderer_texture_scale(st.rt.depth, &pass_sx, &pass_sy); w = ew; }
            if (ew < minw) minw = ew;
            if (eh < minh) minh = eh;
            if (!pass_ncolor) pass_samples = b->samples;
        }
    }
    if (!w) { pass_sx = pass_sy = 1; minw = minh = 16; }
    pass_w = minw; pass_h = minh;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {minw, minh}}, .layerCount = 1,
                          .colorAttachmentCount = pass_ncolor, .pColorAttachments = ca,
                          .pDepthAttachment = has_d ? &da : NULL, .pStencilAttachment = has_s ? &sa : NULL};
    vkCmdBeginRendering(cb, &ri);
    ss_forget();  // (whatever ran between the passes may have set its own state)
    in_pass = true;
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    stat_passes++;
}

static void pass_size(int *w, int *h) {
    *w = *h = 0;
    for (int i = 0; i < st.rt.ncolor && !*w; i++)
        if (st.rt.color[i]) { *w = (int)st.rt.color[i]->width; *h = (int)st.rt.color[i]->height; }
    if (!*w && st.rt.depth) { *w = (int)st.rt.depth->width; *h = (int)st.rt.depth->height; }
    *w = (int)lroundf(*w * pass_sx);
    *h = (int)lroundf(*h * pass_sy);
}

static VkRect2D scissor_rect(void) {
    int w, h;
    pass_size(&w, &h);
    if (!w) return (VkRect2D){{0, 0}, {pass_w, pass_h}};
    int x = st.scissor[0][0], y = st.scissor[0][1], sw = st.scissor[0][2], sh = st.scissor[0][3];
    if (vp_sx != 1 || vp_sy != 1 || vp_ox != 0 || vp_oy != 0) {
        double x0 = floor(x * (double)vp_sx + vp_ox), y0 = floor(y * (double)vp_sy + vp_oy);
        double x1 = ceil(((double)x + sw) * vp_sx + vp_ox), y1 = ceil(((double)y + sh) * vp_sy + vp_oy);
        if (sw >= 16384) x1 = 1e9;
        if (sh >= 16384) y1 = 1e9;
        if (x1 > 1e9) x1 = 1e9;
        if (y1 > 1e9) y1 = 1e9;
        x = (int)x0; y = (int)y0; sw = (int)(x1 - x0); sh = (int)(y1 - y0);
    }
    if (x < 0) { sw += x; x = 0; }
    if (y < 0) { sh += y; y = 0; }
    if (w > (int)pass_w) w = (int)pass_w;
    if (h > (int)pass_h) h = (int)pass_h;
    if (x >= w || y >= h) { x = y = 0; sw = sh = 0; }
    if (x + sw > w) sw = w - x;
    if (y + sh > h) sh = h - y;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    return (VkRect2D){{x, y}, {(uint32_t)sw, (uint32_t)sh}};
}

static void apply_scissor(void) {
    VkRect2D r = scissor_rect();
    if (!ss_need(SS_SC, !memcmp(&r, &ss.sc, sizeof r))) return;
    ss.sc = r;
    vkCmdSetScissor(vk_cmd(), 0, 1, &r);
}

// Metal convention (y up in NDC, origin top-left) through a negative-height viewport.
static void set_viewport(float x, float y, float w, float h, float zn, float zf) {
    if (w <= 0) w = 1;
    if (h == 0) h = 1;
    VkViewport vp = {x, y + h, w, -h, fminf(fmaxf(zn, 0), 1), fminf(fmaxf(zf, 0), 1)};
    if (!ss_need(SS_VP, !memcmp(&vp, &ss.vp, sizeof vp))) return;
    ss.vp = vp;
    vkCmdSetViewport(vk_cmd(), 0, 1, &vp);
}

// ================================================================== clears
static VkPipelineLayout pc_layout(void) {  // push constants only (clears)
    static VkPipelineLayout l;
    if (!l) {
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    }
    return l;
}

static const VkDynamicState DYN[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE};
#define NDYN (sizeof DYN / sizeof DYN[0])

// Fixed-function state that isn't part of a pipeline is set explicitly on every bind (it persists across pipelines).
static void dyn_defaults(VkCommandBuffer cb) {
    vkCmdSetDepthBias(cb, 0, 0, 0);
    vkCmdSetDepthBiasEnable(cb, VK_FALSE);
    float bc[4] = {0, 0, 0, 0};
    vkCmdSetBlendConstants(cb, bc);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdSetCullMode(cb, VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetDepthTestEnable(cb, VK_FALSE);
    vkCmdSetDepthWriteEnable(cb, VK_FALSE);
    vkCmdSetDepthCompareOp(cb, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilTestEnable(cb, VK_FALSE);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                      VK_COMPARE_OP_ALWAYS);
    vkCmdSetPrimitiveRestartEnable(cb, VK_FALSE);
}

typedef struct { uint64_t key; VkPipeline pipe; } PipeEnt;
#define CLEAR_TAB 256
static PipeEnt clear_tab[CLEAR_TAB];

static VkPipeline clear_pipeline(int index, int kind, uint32_t mask, bool depth, bool stencil) {
    uint64_t key = 0xcbf29ce484222325ull;
#define MIX(v) do { uint64_t v_ = (uint64_t)(v); for (int b_ = 0; b_ < 8; b_++) { key ^= (v_ >> (b_ * 8)) & 0xFF; key *= 0x100000001B3ull; } } while (0)
    MIX(index); MIX(kind); MIX(mask); MIX(depth); MIX(stencil); MIX(pass_ncolor); MIX(pass_depth); MIX(pass_stencil); MIX(pass_samples);
    for (uint32_t i = 0; i < pass_ncolor; i++) MIX(pass_color[i]);
    uint32_t slot = (uint32_t)key & (CLEAR_TAB - 1);
    for (int p = 0; p < CLEAR_TAB; p++, slot = (slot + 1) & (CLEAR_TAB - 1)) {
        if (clear_tab[slot].key == key) return clear_tab[slot].pipe;
        if (!clear_tab[slot].key) break;
    }
    char fsname[32];
    if (index >= 0) snprintf(fsname, sizeof fsname, "clear.frag.%c%d", kind == 1 ? 'i' : kind == 2 ? 'u' : 'f', index);
    else snprintf(fsname, sizeof fsname, "clear.frag.none");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("clear.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fsname), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)pass_samples};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < pass_ncolor; i++)
        if ((int)i == index) cba[i].colorWriteMask = mask & 0xF;
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = pass_ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo rinfo = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = pass_ncolor,
                                           .pColorAttachmentFormats = pass_color, .depthAttachmentFormat = pass_depth,
                                           .stencilAttachmentFormat = pass_stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &rinfo, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = pc_layout()};
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p) != VK_SUCCESS) LOG("[vk] clear pipeline failed\n");
    clear_tab[slot].key = key;
    clear_tab[slot].pipe = p;
    return p;
}

static void clear(int index, int kind, const void *color, uint32_t mask, bool depth, float dval, bool stencil, uint32_t sval,
                  uint32_t smask) {
    begin_pass();
    VkCommandBuffer cb = vk_cmd();
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    VkRect2D rect = scissor_rect();
    if (!rect.extent.width || !rect.extent.height) return;
    if (index >= 0 && (index >= (int)pass_ncolor || pass_color[index] == VK_FORMAT_UNDEFINED)) return;
    bool simple = (index < 0 || (mask & 0xF) == 0xF) && (!stencil || (smask & 0xFF) == 0xFF);
    if (simple) {
        VkClearAttachment a[2];
        uint32_t n = 0;
        if (index >= 0) {
            a[n] = (VkClearAttachment){.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)index};
            memcpy(a[n].clearValue.color.float32, color, 16);
            n++;
        }
        VkImageAspectFlags dsa = (depth && pass_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil && pass_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (dsa) a[n++] = (VkClearAttachment){.aspectMask = dsa, .clearValue.depthStencil = {dval, sval}};
        VkClearRect cr = {rect, 0, 1};
        if (n) vkCmdClearAttachments(cb, n, a, 1, &cr);
        return;
    }
    VkPipeline p = clear_pipeline(index, kind, mask, depth, stencil);
    if (!p) return;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    ss_forget();  // (its own pipeline and state: the next draw sets everything again)
    dyn_defaults(cb);
    if (depth) { vkCmdSetDepthTestEnable(cb, VK_TRUE); vkCmdSetDepthWriteEnable(cb, VK_TRUE); }
    if (stencil) {
        vkCmdSetStencilTestEnable(cb, VK_TRUE);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE,
                          VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, smask);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, sval);
    }
    int w, h;
    pass_size(&w, &h);
    VkViewport vp = {0, 0, (float)(w ? w : (int)pass_w), (float)(h ? h : (int)pass_h), 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &rect);
    float pc[8] = {0};
    if (color) memcpy(pc, color, 16);
    pc[4] = dval;
    vkCmdPushConstants(cb, pc_layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

// ================================================================== programs
#define MAX_BIND 96
typedef struct {
    ShaderInfo *vi, *fi;
    uint64_t code[2], cmax[2];  // translation inputs (copied: translated on the translator thread)
    ShaderKey vkey, fkey;
    VkShaderModule vs, fs;
    VkBindingInfo binds[MAX_BIND];
    int nbinds;
    VkDescriptorSetLayout set_layout[2];
    VkPipelineLayout layout;
    bool vs_uses_vid;
    bool warmed, drawn;  // (program warm-up: created ahead of its first draw / drawn since)
    uint64_t ident;      // its code hashes and keys as created, the same in every session (pipeline warm-up records)
    VkPipeline vs_lib, fs_lib;  // its shader libraries (shader libraries below), or NULL
    _Atomic int state;  // 0 building, 1 ready, 2 failed
} Prog;

static VkPipelineLayout make_layout(Prog *p, int nsets, VkShaderStageFlags s0, VkShaderStageFlags s1) {
    p->set_layout[0] = vk_set_layout(p->binds, p->nbinds, 0, s0);
    if (nsets > 1) p->set_layout[1] = vk_set_layout(p->binds, p->nbinds, 1, s1);
    VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = (uint32_t)nsets,
                                     .pSetLayouts = p->set_layout};
    if (g_vk.gpl && nsets == 2) ci.flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;  // (shader libraries)
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &l));
    return l;
}

// ================================================================== shader libraries (VK_EXT_graphics_pipeline_library)
// Most of a pipeline's build time is compiling its two shaders; the rest of its state (vertex formats, blend, render
// target formats) is cheap. With pipeline libraries each shader is compiled once into a library of its own: when a
// program is built, and before that, when the game loads the shader (gpl_warm_stage, with the keys draws use for
// nearly all shaders). A draw's pipeline is then linked from the two shader libraries and two small state libraries
// without compiling anything, and the optimized pipeline is built in the background to replace it. Pipeline layouts
// use independent sets: a shader library holds only its own set (vertex: 0, fragment: 1).
typedef struct { uint64_t key; VkPipeline lib; _Atomic int state; } StageLib;  // state: 0 building, 1 ready, 2 failed
#define STAGELIB_TAB 16384
static struct { uint64_t key; StageLib *lib; } stagelib_tab[STAGELIB_TAB];
static int stagelib_n;
static pthread_mutex_t stagelib_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t hash_words(uint64_t h, const void *p, size_t n);

// Depth clamp (nvnCommandBufferSetDepthClamp): Astral Chain turns it on in every command buffer and never off (counted
// through the opening cutscenes and play), so its pipelines clamp depth instead of clipping at the near and far planes,
// as on the Switch. Without it, an effect card the game places at the near plane in the opening helicopter cutscene (a
// black card that dims the view) was clipped in some frames and drawn in others as the camera moved: the whole picture
// flickered between two brightnesses. LARECOMP_VK_DEPTH_CLAMP=0: off.
static VkBool32 game_depth_clamp(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VK_DEPTH_CLAMP"); on = g_vk.depth_clamp && !(e && !atoi(e)); }
    return on ? VK_TRUE : VK_FALSE;
}

static VkPipeline gpl_build_stage(int stage, const uint32_t *spv, size_t n) {
    VkBindingInfo binds[MAX_BIND];
    int nb = vk_reflect(spv, n, binds, MAX_BIND);
    VkDescriptorSetLayout sl[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    sl[stage] = vk_set_layout(binds, nb, (uint32_t)stage, stage ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT);
    VkPipelineLayoutCreateInfo lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT, .setLayoutCount = 2, .pSetLayouts = sl};
    VkPipelineLayout layout;
    if (vkCreatePipelineLayout(g_vk.dev, &lci, NULL, &layout) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkShaderModule m = vk_module(spv, n);
    if (!m) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo ss = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                          .stage = stage ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT, .module = m,
                                          .pName = "main"};
    VkGraphicsPipelineLibraryCreateInfoEXT li = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
                                                 .flags = stage ? VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT
                                                                : VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .pNext = &li};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .depthClampEnable = game_depth_clamp(), .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                       .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR, .stageCount = 1, .pStages = &ss,
                                       .pDynamicState = &dy, .layout = layout};
    if (stage) pi.pDepthStencilState = &ds;
    else { pi.pViewportState = &vs; pi.pRasterizationState = &rs; }
    VkPipeline lib = VK_NULL_HANDLE;
    uint64_t t0 = SDL_GetTicksNS();
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &lib);
    atomic_fetch_add(&gpl_stage_ns, SDL_GetTicksNS() - t0);
    atomic_fetch_add(&gpl_stage_built, 1);
    atomic_fetch_add(&vk_pipelines_built, 1);  // (pipeline cache save)
    vkDestroyShaderModule(g_vk.dev, m, NULL);
    if (r != VK_SUCCESS) {
        static int nlog;
        if (nlog++ < 10) LOG("[vk] shader library failed: %d (stage %d)\n", r, stage);
        return VK_NULL_HANDLE;
    }
    return lib;
}

// The library of this shader (stage 0 vertex, 1 fragment), built here if no thread has yet; NULL if it cannot be.
static StageLib *gpl_stage(int stage, const uint32_t *spv, size_t n) {
    if (!g_vk.gpl || !spv) return NULL;
    uint64_t key = hash_words(0x9E3779B97F4A7C15ull + (uint64_t)stage, spv, n * 4);
    if (!key) key = 1;
    pthread_mutex_lock(&stagelib_lock);
    uint32_t slot = (uint32_t)(key ^ (key >> 29)) & (STAGELIB_TAB - 1);
    while (stagelib_tab[slot].key && stagelib_tab[slot].key != key) slot = (slot + 1) & (STAGELIB_TAB - 1);
    StageLib *sl = stagelib_tab[slot].lib;
    bool build = false;
    if (!sl && stagelib_n < STAGELIB_TAB * 3 / 4) {
        sl = calloc(1, sizeof *sl);
        sl->key = key;
        stagelib_tab[slot].key = key;
        stagelib_tab[slot].lib = sl;
        stagelib_n++;
        build = true;
    }
    pthread_mutex_unlock(&stagelib_lock);
    if (!sl) return NULL;
    if (build) {
        sl->lib = gpl_build_stage(stage, spv, n);
        atomic_store(&sl->state, sl->lib ? 1 : 2);
    } else {
        atomic_fetch_add(&gpl_stage_hits, 1);
        for (uint64_t t0 = SDL_GetTicksNS(); atomic_load(&sl->state) == 0 && SDL_GetTicksNS() - t0 < 3000000000ull;) SDL_Delay(1);
    }
    return atomic_load(&sl->state) == 1 ? sl : NULL;
}

// (nvnProgramSetShaders) the game loaded a shader: its library, built on an idle worker with the keys nearly all
// draws use (shader_precompile.c: float attributes, plus unsigned attribute 2 for skinned meshes; every varying)
typedef struct { uint64_t code, cmax; int stage; } GplWarm;
static uint64_t gpl_warm_seen[8192];
static int gpl_warm_nseen;
static void gpl_warm_job(void *arg) {
    GplWarm *w = arg;
    for (int variant = 0; variant < 2; variant++) {
        ShaderKey k;
        memset(&k, 0, sizeof k);
        k.glsl = 1;
        if (w->stage) k.vs_out_mask = 0xFFFFFFFFu;
        if (variant) k.attr_kind[2] = 2;
        pthread_mutex_lock(&translate_lock);
        maxwell_gmem_base = HEAP_BASE;
        maxwell_gmem_size = HEAP_MAX;
        ShaderInfo *si = maxwell_translate((const uint8_t *)(uintptr_t)w->code, w->cmax, w->stage ? STAGE_FRAGMENT : STAGE_VERTEX, &k);
        pthread_mutex_unlock(&translate_lock);
        bool skinned = si && si->msl && !w->stage && (si->attr_mask & 4);
        if (si && si->msl) {
            size_t n = 0;
            uint32_t *spv = vk_glsl_to_spirv(si->msl, w->stage ? STAGE_FRAGMENT : STAGE_VERTEX, w->stage ? "fs" : "vs", &n);
            gpl_stage(w->stage, spv, n);
            free(spv);
        }
        if (si) maxwell_free(si);
        if (!skinned) break;
    }
    free(w);
}
static void run_job_low(void (*fn)(void *), void *arg);
static void gpl_warm_stage(const NvnProgram *np, int s, uint64_t hash) {
    if (!g_vk.gpl || !np->stages[s].code_addr || !np->stages[s].const_offset) return;
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_GPL_WARM") != NULL || sync_shaders();
    if (off) return;
    for (int i = 0; i < gpl_warm_nseen; i++) if (gpl_warm_seen[i] == hash) return;
    if (gpl_warm_nseen == 8192) return;
    gpl_warm_seen[gpl_warm_nseen++] = hash;
    GplWarm *w = malloc(sizeof *w);
    w->code = np->stages[s].code_addr;
    w->cmax = np->stages[s].const_offset;
    w->stage = s;
    run_job_low(gpl_warm_job, w);
}

static void save_glsl(const char *src, uint64_t addr, const char *suffix) {
    if (!getenv("LARECOMP_SAVE_MSL")) return;
    mkdir("build", 0755);
    mkdir("build/glsl", 0755);
    char path[256];
    snprintf(path, sizeof path, "build/glsl/%llx_%s.glsl", (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "w");
    if (f) { fputs(src, f); fclose(f); }
}

// With LARECOMP_SAVE_MSL also the translator inputs, for offline retranslation: <addr>_<suffix>.bin is the program
// window as passed to maxwell_translate (NVN prefix + SPH + code), <addr>_<suffix>.key the ShaderKey bytes.
static void save_shader_input(uint64_t addr, uint64_t size, const ShaderKey *key, const char *suffix) {
    if (!getenv("LARECOMP_SAVE_MSL") || !addr) return;
    mkdir("build", 0755);
    mkdir("build/glsl", 0755);
    char path[256];
    snprintf(path, sizeof path, "build/glsl/%llx_%s.bin", (unsigned long long)addr, suffix);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite((const void *)(uintptr_t)addr, 1, size > 0x10000 ? 0x10000 : size, f); fclose(f); }
    snprintf(path, sizeof path, "build/glsl/%llx_%s.key", (unsigned long long)addr, suffix);
    f = fopen(path, "wb");
    if (f) { fwrite(key, 1, sizeof *key, f); fclose(f); }
}

static void compile_program(void *arg) {
    Prog *p = arg;
    size_t nv = 0, nf = 0;
    uint64_t bt0 = SDL_GetTicksNS();
    uint32_t *sv = vk_glsl_to_spirv(p->vi->msl, STAGE_VERTEX, "vs", &nv);
    uint32_t *sf = sv ? vk_glsl_to_spirv(p->fi->msl, STAGE_FRAGMENT, "fs", &nf) : NULL;
    atomic_fetch_add(&build_ns_spirv, SDL_GetTicksNS() - bt0);
    if (!sv || !sf) { free(sv); free(sf); atomic_store(&p->state, 2); return; }
    p->nbinds = vk_reflect(sv, nv, p->binds, MAX_BIND);
    p->nbinds += vk_reflect(sf, nf, p->binds + p->nbinds, MAX_BIND - p->nbinds);
    p->vs = vk_module(sv, nv);
    p->fs = vk_module(sf, nf);
    StageLib *lv = gpl_stage(0, sv, nv), *lf = lv ? gpl_stage(1, sf, nf) : NULL;
    if (lv && lf) { p->vs_lib = lv->lib; p->fs_lib = lf->lib; }
    free(sv);
    free(sf);
    p->layout = make_layout(p, 2, VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT);
    atomic_fetch_add(&stat_compiles, 1); atomic_fetch_add(&vk_pipelines_built, 1);
    atomic_store(&p->state, p->vs && p->fs ? 1 : 2);
}

static void translate_program(void *arg) {
    Prog *p = arg;
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    uint64_t bt0 = SDL_GetTicksNS();
    pthread_mutex_lock(&translate_lock);
    save_shader_input(p->code[0], p->cmax[0], &p->vkey, "vs");
    p->vi = maxwell_translate((const uint8_t *)(uintptr_t)p->code[0], p->cmax[0], STAGE_VERTEX, &p->vkey);
    if (p->vi->msl) {
        p->fkey.vs_out_mask = p->vi->out_mask;
        if (p->code[1]) {
            save_shader_input(p->code[1], p->cmax[1], &p->fkey, "fs");
            p->fi = maxwell_translate((const uint8_t *)(uintptr_t)p->code[1], p->cmax[1], STAGE_FRAGMENT, &p->fkey);
        } else {  // vertex-only program (depth-only pass): an empty fragment stage, no outputs or bindings
            p->fi = calloc(1, sizeof *p->fi);
            p->fi->stage = STAGE_FRAGMENT;
            p->fi->msl = strdup("#version 460\nvoid main() {}\n");
        }
    }
    pthread_mutex_unlock(&translate_lock);
    atomic_fetch_add(&build_ns_translate, SDL_GetTicksNS() - bt0);
    if (!p->vi->msl) {
        LOG("[shader] VS translate failed: %s (code %llx)\n", p->vi->error, (unsigned long long)p->code[0]);
        atomic_store(&p->state, 2);
        return;
    }
    if (!p->fi->msl) {
        LOG("[shader] FS translate failed: %s\n", p->fi->error);
        atomic_store(&p->state, 2);
        return;
    }
    p->vs_uses_vid = strstr(p->vi->msl, "gl_VertexIndex") != NULL;
    save_glsl(p->vi->msl, p->code[0], "vs");
    save_glsl(p->fi->msl, p->code[1], "fs");
    run_job(compile_program, p);
}

#define PROG_TAB 4096
static struct { uint64_t key; Prog *prog; } prog_tab[PROG_TAB];

static void warm_record(const NvnProgram *np, const ShaderKey *vkey, const ShaderKey *fkey);
static Prog *get_program_ex(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey, bool from_draw);
static uint64_t stage_hash(const NvnProgram *np, int i);
static uint64_t hmix(uint64_t h, const void *p, size_t n);
static void pipe_warm_add(Prog *p);

// Draws: the program for this state, built at its first use (or by the warm-up). A draw waiting for one that is
// still queued moves it to the front.
static Prog *get_program(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey) {
    Prog *p = get_program_ex(np, vkey, fkey, true);
    if (p && !p->drawn) {
        p->drawn = true;
        if (p->warmed) { if (atomic_load(&p->state) == 0) warm_stat_late++; else warm_stat_ready++; }
    }
    if (p && atomic_load(&p->state) == 0) {
        promote_job(&tjobs_head, &tjobs_tail, &tjobs_lock, p);
        promote_job(&jobs_head, &jobs_tail, &jobs_lock, p);
    }
    return p;
}

static Prog *get_program_ex(NvnProgram *np, ShaderKey *vkey, ShaderKey *fkey, bool from_draw) {
    // (Astral Chain) vertex-only programs (depth-only passes such as shadow maps) get an empty fragment stage
    // (translate_program); they were skipped (~1750 draws/s in the highway chase, run 437).
    if (!(np->stage_mask & 1)) {  // (other stage bits occur too, e.g. 0x23: ignored, as before)
        static int nlog;
        if (nlog++ < 10) LOG("[shader] program with stage mask %#x skipped\n", np->stage_mask);
        return NULL;
    }
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < 2; i++) { uint64_t a = (np->stage_mask & (1u << i)) ? np->stages[i].code_addr : 0; for (int k = 0; k < 8; k++) { h ^= (a >> (k * 8)) & 0xFF; h *= 0x100000001B3ull; } }
    const uint8_t *vb = (const uint8_t *)vkey;
    for (size_t i = 0; i < sizeof *vkey; i++) { h ^= vb[i]; h *= 0x100000001B3ull; }
    for (int i = 0; i < 8; i++) { h ^= fkey->rt_kind[i]; h *= 0x100000001B3ull; }
    const uint8_t at[3] = {fkey->alpha_func, (uint8_t)fkey->alpha_ref, (uint8_t)(fkey->alpha_ref >> 8)};  // (alpha test)
    for (int i = 0; i < 3; i++) { h ^= at[i]; h *= 0x100000001B3ull; }
    if (!h) h = 1;
    uint32_t slot = (uint32_t)(h ^ (h >> 31)) & (PROG_TAB - 1);
    while (prog_tab[slot].key && prog_tab[slot].key != h) slot = (slot + 1) & (PROG_TAB - 1);
    if (prog_tab[slot].key) return prog_tab[slot].prog;
    Prog *p = calloc(1, sizeof *p);
    prog_tab[slot].key = h;
    prog_tab[slot].prog = p;
    for (int i = 0; i < 2; i++) {
        p->code[i] = (np->stage_mask & (1u << i)) ? np->stages[i].code_addr : 0;  // (fragment 0: vertex-only)
        p->cmax[i] = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
    }
    p->vkey = *vkey;
    p->fkey = *fkey;
    uint64_t sh[2] = {stage_hash(np, 0), stage_hash(np, 1)};
    p->ident = hmix(hmix(hmix(0xcbf29ce484222325ull, sh, sizeof sh), vkey, sizeof *vkey), fkey, sizeof *fkey);
    pipe_warm_add(p);
    p->warmed = !from_draw;
    if (from_draw) warm_record(np, vkey, fkey);
    else warm_stat_queued++;
    run_translate(translate_program, p);
    return p;
}

// ================================================================== program warm-up (Astral Chain)
// Programs were translated and compiled at their first draw. A scene's first frame could need dozens at once and
// waited for them one by one (the opening cutscene's first frame: 69 programs, ~340 ms in the draw path). The game
// hands its shader code to NVN while loading (nvnProgramSetShaders); the draw-time state that also selects a program
// (vertex attribute and render target kinds: ShaderKey) is only known at the draw. So every program a draw creates is
// recorded in cache/programs.bin (hashes of its vertex and fragment code, both keys), and when the game sets the same
// code again (this or a later session) the recorded variants are queued for translation and compilation at once.
// Compute programs depend on their code only: they are translated at SetShaders directly. LARECOMP_NO_WARM=1: off.
typedef struct { uint64_t vh, fh; ShaderKey vkey, fkey; } WarmRec;
#define WARM_MAX 16384
#define WARM_MAGIC (0x4D524157u ^ (uint32_t)sizeof(WarmRec))  // "WARM", record size: a layout change starts afresh
static WarmRec warm_rec[WARM_MAX];
static int warm_n = -1;  // -1: cache/programs.bin not read yet
static FILE *warm_out;
static uint64_t hmix(uint64_t h, const void *p, size_t n);
static void warm_compute(NvnProgram *np);  // (compute_program, defined with the compute programs)

static bool warm_off(void) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_WARM") != NULL || sync_shaders();
    return off;
}
static uint64_t stage_hash(const NvnProgram *np, int i) {
    if (!(np->stage_mask & (1u << i)) || !np->stages[i].code_addr) return 0;
    uint64_t n = np->stages[i].const_offset ? np->stages[i].const_offset : 0x10000;
    return hmix(0x9E3779B97F4A7C15ull + (uint64_t)i, (const void *)(uintptr_t)np->stages[i].code_addr, (size_t)n);
}
static void warm_load(void) {
    warm_n = 0;
    FILE *f = fopen("cache/programs.bin", "rb");
    if (!f) return;
    uint32_t magic = 0;
    if (fread(&magic, 4, 1, f) == 1 && magic == WARM_MAGIC)
        // (files written before the fix below hold a header again at each session's start: skipped)
        for (uint32_t word; warm_n < WARM_MAX && fread(&word, 4, 1, f) == 1;) {
            if (word == WARM_MAGIC) continue;
            memcpy(&warm_rec[warm_n], &word, 4);
            if (fread((uint8_t *)&warm_rec[warm_n] + 4, sizeof warm_rec[0] - 4, 1, f) != 1) break;
            warm_n++;
        }
    fclose(f);
    if (magic != WARM_MAGIC) remove("cache/programs.bin");  // (older layout: rebuilt from the next draws)
    LOG("[vk] program warm-up: %d recorded variants\n", warm_n);
}
static void warm_record(const NvnProgram *np, const ShaderKey *vkey, const ShaderKey *fkey) {
    if (warm_off()) return;
    if (warm_n < 0) warm_load();
    WarmRec r;
    memset(&r, 0, sizeof r);
    r.vh = stage_hash(np, 0);
    r.fh = stage_hash(np, 1);
    r.vkey = *vkey;
    r.fkey = *fkey;
    for (int i = 0; i < warm_n; i++) if (!memcmp(&warm_rec[i], &r, sizeof r)) return;
    if (warm_n == WARM_MAX) return;
    warm_rec[warm_n++] = r;
    if (!warm_out) {
        mkdir("cache", 0755);
        warm_out = fopen("cache/programs.bin", "ab");
        // (ftell is 0 right after opening for append, also on a non-empty file: the header was written every session)
        if (warm_out && fseek(warm_out, 0, SEEK_END) == 0 && ftell(warm_out) == 0) { uint32_t magic = WARM_MAGIC; fwrite(&magic, 4, 1, warm_out); }
    }
    if (warm_out) { fwrite(&r, sizeof r, 1, warm_out); fflush(warm_out); }
}
// nvnProgramSetShaders (objects.c hook; NVN calls are serialized, as are draws). The game sets one stage per program
// object and binds a vertex and a fragment program separately; draws see the pair. So the stages set so far are
// remembered by code hash, and a recorded pair is queued once both of its stages have been set in this session.
#define WARM_SEEN 8192
static struct { uint64_t hash; NvnShaderStage stage; } warm_seen[WARM_SEEN];
static int warm_nseen;
static const NvnShaderStage *warm_seen_stage(uint64_t h) {
    for (int i = warm_nseen - 1; i >= 0; i--) if (warm_seen[i].hash == h) return &warm_seen[i].stage;
    return NULL;
}
static void warm_program(NvnProgram *np) {
    for (int s = 0; s < 2; s++) if (np->stage_mask & (1u << s)) gpl_warm_stage(np, s, stage_hash(np, s));  // (own switch)
    if (warm_off()) return;
    if (np->stage_mask & (1u << 5)) { warm_compute(np); return; }
    if (warm_n < 0) warm_load();
    if (!warm_n) return;
    for (int s = 0; s < 2; s++) {
        if (!(np->stage_mask & (1u << s))) continue;
        uint64_t h = stage_hash(np, s);
        if (warm_nseen < WARM_SEEN && !warm_seen_stage(h)) {
            warm_seen[warm_nseen].hash = h;
            warm_seen[warm_nseen++].stage = np->stages[s];
        }
        for (int i = 0; i < warm_n; i++) {
            if ((s == 0 ? warm_rec[i].vh : warm_rec[i].fh) != h) continue;
            const NvnShaderStage *vs = warm_seen_stage(warm_rec[i].vh);
            const NvnShaderStage *fs = warm_rec[i].fh ? warm_seen_stage(warm_rec[i].fh) : NULL;
            if (!vs || (warm_rec[i].fh && !fs)) continue;  // (the other stage is not set yet)
            NvnProgram pair;
            memset(&pair, 0, sizeof pair);
            pair.stages[0] = *vs;
            pair.stage_mask = 1;
            if (fs) { pair.stages[1] = *fs; pair.stage_mask |= 2; }
            ShaderKey vk = warm_rec[i].vkey, fk = warm_rec[i].fkey;
            get_program_ex(&pair, &vk, &fk, false);
        }
    }
}

// ================================================================== vertex formats / fixed function tables
static int attr_kind(uint32_t fmt) {
    switch (fmt) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23:
    case 0x27: case 0x2C: case 0x2F: case 0x3E:
        return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24:
    case 0x28: case 0x2D: case 0x30:
        return 1;
    default: return 0;
    }
}

static VkFormat vertex_format(uint32_t f) {
    switch (f) {
    case 0x01: return VK_FORMAT_R8_UNORM;
    case 0x02: return VK_FORMAT_R8_SNORM;
    case 0x03: return VK_FORMAT_R8_UINT;
    case 0x04: return VK_FORMAT_R8_SINT;
    case 0x05: return VK_FORMAT_R16_SFLOAT;
    case 0x06: return VK_FORMAT_R16_UNORM;
    case 0x07: return VK_FORMAT_R16_SNORM;
    case 0x08: return VK_FORMAT_R16_UINT;
    case 0x09: return VK_FORMAT_R16_SINT;
    case 0x0A: return VK_FORMAT_R32_SFLOAT;
    case 0x0B: return VK_FORMAT_R32_UINT;
    case 0x0C: return VK_FORMAT_R32_SINT;
    case 0x0D: return VK_FORMAT_R8G8_UNORM;
    case 0x0E: return VK_FORMAT_R8G8_SNORM;
    case 0x0F: return VK_FORMAT_R8G8_UINT;
    case 0x10: return VK_FORMAT_R8G8_SINT;
    case 0x11: return VK_FORMAT_R16G16_SFLOAT;
    case 0x12: return VK_FORMAT_R16G16_UNORM;
    case 0x13: return VK_FORMAT_R16G16_SNORM;
    case 0x14: return VK_FORMAT_R16G16_UINT;
    case 0x15: return VK_FORMAT_R16G16_SINT;
    case 0x16: return VK_FORMAT_R32G32_SFLOAT;
    case 0x17: return VK_FORMAT_R32G32_UINT;
    case 0x18: return VK_FORMAT_R32G32_SINT;
    case 0x19: return VK_FORMAT_R8G8B8_UNORM;
    case 0x1A: return VK_FORMAT_R8G8B8_SNORM;
    case 0x1B: return VK_FORMAT_R8G8B8_UINT;
    case 0x1C: return VK_FORMAT_R8G8B8_SINT;
    case 0x1D: return VK_FORMAT_R16G16B16_SFLOAT;
    case 0x1E: return VK_FORMAT_R16G16B16_UNORM;
    case 0x1F: return VK_FORMAT_R16G16B16_SNORM;
    case 0x20: return VK_FORMAT_R16G16B16_UINT;
    case 0x21: return VK_FORMAT_R16G16B16_SINT;
    case 0x22: return VK_FORMAT_R32G32B32_SFLOAT;
    case 0x23: return VK_FORMAT_R32G32B32_UINT;
    case 0x24: return VK_FORMAT_R32G32B32_SINT;
    case 0x25: return VK_FORMAT_R8G8B8A8_UNORM;
    case 0x26: return VK_FORMAT_R8G8B8A8_SNORM;
    case 0x27: return VK_FORMAT_R8G8B8A8_UINT;
    case 0x28: return VK_FORMAT_R8G8B8A8_SINT;
    case 0x29: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 0x2A: return VK_FORMAT_R16G16B16A16_UNORM;
    case 0x2B: return VK_FORMAT_R16G16B16A16_SNORM;
    case 0x2C: return VK_FORMAT_R16G16B16A16_UINT;
    case 0x2D: return VK_FORMAT_R16G16B16A16_SINT;
    case 0x2E: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 0x2F: return VK_FORMAT_R32G32B32A32_UINT;
    case 0x30: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x51: return VK_FORMAT_R8_UINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

// 3-component 8/16-bit vertex formats aren't universally supported: read 4 components instead.
static VkFormat vertex_format_supported(VkFormat f) {
    static uint8_t known[256];  // 1 ok, 2 not
    uint32_t i = f < 256 ? f : 0;
    if (!known[i]) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
        known[i] = (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : 2;
    }
    if (known[i] == 1) return f;
    { static uint8_t told[256]; if (!told[i]++) LOG("[vk] vertex format %d unsupported: read as 4 components\n", f); }
    switch (f) {
    case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case VK_FORMAT_R8G8B8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R8G8B8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case VK_FORMAT_R16G16B16_SFLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VK_FORMAT_R16G16B16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case VK_FORMAT_R16G16B16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case VK_FORMAT_R16G16B16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R16G16B16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

static VkBlendFactor blend_factor(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: {
        static uint8_t warned[256];
        if (!warned[f]++) LOG("[vk] unknown blend factor %#x\n", f);
        return VK_BLEND_FACTOR_ONE;
    }
    }
}

static VkBlendOp blend_op(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkCompareOp compare_fn(uint8_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}

static VkStencilOp stencil_op(uint8_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static int rt_kind(uint32_t f) {
    switch (f) {
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x27: case 0x2C: case 0x2F: case 0x3E: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x28: case 0x2D: case 0x30: return 1;
    default: return 0;
    }
}

// ================================================================== graphics pipelines
// Everything a pipeline bakes in, captured by value so it can be built on a worker thread.
#define ZERO_BINDING 16
typedef struct {
    Prog *prog;
    VkFormat color[8], depth, stencil;
    uint32_t ncolor, samples;
    uint32_t attr_fmt[32], attr_off[32], attr_bind[32];
    uint32_t attr_mask;
    uint32_t stride[17], divisor[17], used_streams;
    uint8_t blend[8][8];
    uint8_t blend_on, a2c, topo_class, poly_fill;
    uint32_t write_mask[8];
} PipeKey;

// state: 0 building, 1 ready, 2 failed. claimed: its build has started (a warm-up build still queued is taken over
// by the draw that needs it). warmed: queued by the pipeline warm-up.
// opt: the optimized pipeline that replaces a linked one (shader libraries), once built.
typedef struct { PipeKey k; uint64_t hash; VkPipeline pipe; _Atomic int state, claimed; bool warmed, drawn; _Atomic uint64_t opt; } Pipe;
#define PIPE_TAB 8192
static struct { uint64_t key; Pipe *pipe; } pipe_tab[PIPE_TAB];

// 8 bytes a step (PipeKey is ~700 bytes, hashed per draw); in-memory tables only - hmix values are compared with
// stored constants (fragment-shader hashes) and must not change.
static uint64_t hash_words(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (; n >= 8; n -= 8, b += 8) { uint64_t w; memcpy(&w, b, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; n; n--, b++) { h = (h ^ *b) * 0x100000001B3ull; }
    return h ^ (h >> 32);
}

static uint64_t hmix(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static VkPrimitiveTopology topo_class_base(uint8_t c) {
    return c == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : c == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

static bool skip_unbuilt(void);
static VkResult create_pipeline(const PipeKey *k, VkPipeline *out) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = k->prog->vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = k->prog->fs, .pName = "main"}};
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = topo_class_base(k->topo_class)};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .depthClampEnable = game_depth_clamp(), .polygonMode = k->poly_fill ? VK_POLYGON_MODE_FILL : VK_POLYGON_MODE_LINE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b[1]);
            cba[i].dstColorBlendFactor = blend_factor(b[2]);
            cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            cba[i].colorBlendOp = blend_op(b[5]);
            cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor, .pAttachments = cba};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor,
                                        .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth,
                                        .stencilAttachmentFormat = k->stencil};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = k->prog->layout};
    VkPipelineCreationFeedback fb_pipe = {0}, fb_stages[2] = {0};
    VkPipelineCreationFeedbackCreateInfo fb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                                               .pPipelineCreationFeedback = &fb_pipe, .pipelineStageCreationFeedbackCount = 2,
                                               .pPipelineStageCreationFeedbacks = fb_stages};
    ri.pNext = &fb;
    uint64_t bt0 = SDL_GetTicksNS();
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, out);
    uint64_t bdt = SDL_GetTicksNS() - bt0;
    atomic_fetch_add(&build_ns_pipe, bdt);
    if (fb_pipe.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) {
        if (fb_pipe.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) atomic_fetch_add(&build_pipe_hits, 1);
        else { atomic_fetch_add(&build_pipe_misses, 1); atomic_fetch_add(&build_ns_pipe_miss, bdt); }
    }
    if (r != VK_SUCCESS) {
        static int n;
        if (n++ < 10) LOG("[vk] pipeline creation failed: %d\n", r);
    }
    atomic_fetch_add(&stat_compiles, 1); atomic_fetch_add(&vk_pipelines_built, 1);
    return r;
}

static void build_pipeline(void *arg) {
    Pipe *pp = arg;
    VkResult r = create_pipeline(&pp->k, &pp->pipe);
    atomic_store(&pp->state, r == VK_SUCCESS ? 1 : 2);
}

// (low priority) the optimized pipeline for one that was linked from libraries
static void build_pipeline_opt(void *arg) {
    Pipe *pp = arg;
    VkPipeline p = VK_NULL_HANDLE;
    if (create_pipeline(&pp->k, &p) == VK_SUCCESS) { atomic_store(&pp->opt, (uint64_t)(uintptr_t)p); atomic_fetch_add(&gpl_opt_built, 1); }
}

// State libraries: vertex input (formats, streams, topology) and fragment output (target formats, blend, samples),
// cached by their part of the key. Render thread only.
typedef struct { uint64_t key; VkPipeline lib; } StateLib;
#define STATELIB_TAB 4096
static StateLib vi_libs[STATELIB_TAB], fo_libs[STATELIB_TAB];
static VkPipeline state_lib(StateLib *tab, uint64_t key, const VkGraphicsPipelineCreateInfo *ci) {
    if (!key) key = 1;
    uint32_t slot = (uint32_t)(key ^ (key >> 29)) & (STATELIB_TAB - 1);
    for (int probe = 0; probe < 64; probe++, slot = (slot + 1) & (STATELIB_TAB - 1)) {
        if (tab[slot].key == key) return tab[slot].lib;
        if (!tab[slot].key) break;
    }
    VkPipeline lib = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, ci, NULL, &lib) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (!tab[slot].key) { tab[slot].key = key; tab[slot].lib = lib; }
    return lib;
}

// The pipeline of pp linked from libraries (no compiling); false if that is not possible (then built as before).
static bool gpl_link(Pipe *pp) {
    const PipeKey *k = &pp->k;
    Prog *p = k->prog;
    if (!g_vk.gpl || !p->vs_lib || !p->fs_lib || !k->poly_fill) return false;
    uint64_t t0 = SDL_GetTicksNS();
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = NDYN, .pDynamicStates = DYN};
    // vertex input interface
    VkVertexInputAttributeDescription va[32];
    VkVertexInputBindingDescription vb[17];
    VkVertexInputBindingDivisorDescriptionEXT vd[17];
    uint32_t na = 0, nb = 0, nd = 0;
    for (int i = 0; i < 32; i++)
        if (k->attr_mask & (1u << i)) va[na++] = (VkVertexInputAttributeDescription){(uint32_t)i, k->attr_bind[i], k->attr_fmt[i], k->attr_off[i]};
    for (uint32_t s = 0; s < 17; s++) {
        if (!(k->used_streams & (1u << s))) continue;
        bool inst = k->divisor[s] != 0;
        vb[nb++] = (VkVertexInputBindingDescription){s, k->stride[s], inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (inst && k->divisor[s] > 1 && g_vk.divisor) vd[nd++] = (VkVertexInputBindingDivisorDescriptionEXT){s, k->divisor[s]};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT dvi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT,
                                                          .vertexBindingDivisorCount = nd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nd ? &dvi : NULL,
                                               .vertexBindingDescriptionCount = nb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = na, .pVertexAttributeDescriptions = va};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = topo_class_base(k->topo_class)};
    VkGraphicsPipelineLibraryCreateInfoEXT vli = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
                                                  .flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT};
    VkGraphicsPipelineCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &vli,
                                        .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR, .pVertexInputState = &vi,
                                        .pInputAssemblyState = &ia, .pDynamicState = &dy};
    uint64_t vkey = hash_words(0x7631ull, k->attr_fmt, sizeof k->attr_fmt);
    vkey = hash_words(vkey, k->attr_off, sizeof k->attr_off);
    vkey = hash_words(vkey, k->attr_bind, sizeof k->attr_bind);
    uint32_t vmisc[3] = {k->attr_mask, k->used_streams, k->topo_class};
    vkey = hash_words(vkey, vmisc, sizeof vmisc);
    vkey = hash_words(vkey, k->stride, sizeof k->stride);
    vkey = hash_words(vkey, k->divisor, sizeof k->divisor);
    // fragment output interface
    VkPipelineColorBlendAttachmentState cba[8] = {0};
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cba[i].colorWriteMask = k->write_mask[i];
        if ((k->blend_on >> i) & 1) {
            const uint8_t *b = k->blend[i];
            cba[i].blendEnable = VK_TRUE;
            cba[i].srcColorBlendFactor = blend_factor(b[1]);
            cba[i].dstColorBlendFactor = blend_factor(b[2]);
            cba[i].srcAlphaBlendFactor = blend_factor(b[3]);
            cba[i].dstAlphaBlendFactor = blend_factor(b[4]);
            cba[i].colorBlendOp = blend_op(b[5]);
            cba[i].alphaBlendOp = blend_op(b[6]);
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor, .pAttachments = cba};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples, .alphaToCoverageEnable = k->a2c};
    VkGraphicsPipelineLibraryCreateInfoEXT fli = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
                                                  .flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .pNext = &fli,
                                        .colorAttachmentCount = k->ncolor, .pColorAttachmentFormats = k->color,
                                        .depthAttachmentFormat = k->depth, .stencilAttachmentFormat = k->stencil};
    VkGraphicsPipelineCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                        .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR, .pColorBlendState = &cb,
                                        .pMultisampleState = &ms, .pDynamicState = &dy};
    uint64_t fkey = hash_words(0x666full, k->color, sizeof k->color);
    uint32_t fmisc[6] = {k->depth, k->stencil, k->ncolor, k->samples, k->a2c, k->blend_on};
    fkey = hash_words(fkey, fmisc, sizeof fmisc);
    fkey = hash_words(fkey, k->blend, sizeof k->blend);
    fkey = hash_words(fkey, k->write_mask, sizeof k->write_mask);
    VkPipeline libs[4] = {state_lib(vi_libs, vkey, &vci), p->vs_lib, p->fs_lib, state_lib(fo_libs, fkey, &fci)};
    if (!libs[0] || !libs[3]) return false;
    VkPipelineLibraryCreateInfoKHR lci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR, .libraryCount = 4, .pLibraries = libs};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &lci, .layout = p->layout};
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pi, NULL, &pp->pipe);
    atomic_fetch_add(&gpl_link_ns, SDL_GetTicksNS() - t0);
    if (r != VK_SUCCESS) {
        static int nlog;
        if (nlog++ < 10) LOG("[vk] pipeline link failed: %d\n", r);
        return false;
    }
    atomic_fetch_add(&gpl_linked, 1);
    atomic_store(&pp->state, 1);
    run_job_low(build_pipeline_opt, pp);
    return true;
}

// ================================================================== pipeline warm-up (Astral Chain)
// Pipelines were created at their first draw, on the render thread. The driver's compile takes up to ~100 ms each,
// and a scene's first frame can need dozens: 2.4 s in one frame at the start of character creation on a first play.
// Every pipeline a draw creates is recorded in cache/pipelines.bin (its program's identity and the pipeline state);
// once a program with recorded pipelines is ready (program warm-up, or a draw), they are built on idle workers ahead
// of their draws. Records may come from another GPU (astral_chain/data/pipeline_variants.bin, merged in at launch):
// their formats are checked against this one first. LARECOMP_NO_PIPE_WARM=1: off.
typedef struct { uint64_t ident; PipeKey k; } PipeRec;  // (k.prog: NULL)
#define PREC_MAX 65536
#define PREC_MAGIC (0x45504950u ^ (uint32_t)sizeof(PipeRec))  // "PIPE", record size: a layout change starts afresh
static PipeRec *prec;
static int32_t *prec_next;   // records chained by ident bucket (1-based, 0: end)
static int32_t prec_head[4096];
static int prec_n = -1, prec_cap;  // -1: cache/pipelines.bin not read yet
static FILE *prec_out;
static int pipe_tab_used;
static pthread_mutex_t pwarm_lock = PTHREAD_MUTEX_INITIALIZER;
static Prog **pwarm_pending;
static int pwarm_n, pwarm_cap;

static bool pipe_warm_off(void) {
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_PIPE_WARM") != NULL || sync_shaders();
    return off;
}

static int prec_find(uint64_t ident, const PipeKey *k) {
    for (int32_t i = prec_head[ident & 4095]; i; i = prec_next[i - 1])
        if (prec[i - 1].ident == ident && !memcmp(&prec[i - 1].k, k, sizeof *k)) return i - 1;
    return -1;
}

static bool prec_add(const PipeRec *r) {
    if (prec_n >= PREC_MAX) return false;
    if (prec_n == prec_cap) {
        int cap = prec_cap ? prec_cap * 2 : 1024;
        PipeRec *np = realloc(prec, (size_t)cap * sizeof *np);
        if (!np) return false;
        prec = np;
        int32_t *nn = realloc(prec_next, (size_t)cap * sizeof *nn);
        if (!nn) return false;
        prec_next = nn;
        prec_cap = cap;
    }
    prec[prec_n] = *r;
    prec_next[prec_n] = prec_head[r->ident & 4095];
    prec_head[r->ident & 4095] = ++prec_n;
    return true;
}

static void prec_load(void) {
    prec_n = 0;
    FILE *f = fopen("cache/pipelines.bin", "rb");
    if (!f) return;
    uint32_t magic = 0;
    PipeRec r;
    if (fread(&magic, 4, 1, f) == 1 && magic == PREC_MAGIC)
        while (fread(&r, sizeof r, 1, f) == 1)
            if (prec_find(r.ident, &r.k) < 0 && !prec_add(&r)) break;
    fclose(f);
    if (magic != PREC_MAGIC) remove("cache/pipelines.bin");  // (older layout: rebuilt from the next draws)
    LOG("[vk] pipeline warm-up: %d recorded pipelines\n", prec_n);
}

// (draws) a pipeline created by a draw: recorded for the next sessions
static void pipe_record(const Prog *p, const PipeKey *k) {
    if (pipe_warm_off()) return;
    if (prec_n < 0) prec_load();
    PipeRec r;
    memset(&r, 0, sizeof r);
    r.ident = p->ident;
    r.k = *k;
    r.k.prog = NULL;
    if (prec_find(r.ident, &r.k) >= 0 || !prec_add(&r)) return;
    if (!prec_out) {
        mkdir("cache", 0755);
        prec_out = fopen("cache/pipelines.bin", "ab");
        if (prec_out && fseek(prec_out, 0, SEEK_END) == 0 && ftell(prec_out) == 0) { uint32_t m = PREC_MAGIC; fwrite(&m, 4, 1, prec_out); }
    }
    if (prec_out) { fwrite(&r, sizeof r, 1, prec_out); fflush(prec_out); }
}

// (any thread) a new program: its recorded pipelines are queued once it is ready
static void pipe_warm_add(Prog *p) {
    if (pipe_warm_off()) return;
    pthread_mutex_lock(&pwarm_lock);
    if (pwarm_n == pwarm_cap) {
        int cap = pwarm_cap ? pwarm_cap * 2 : 256;
        Prog **np = realloc(pwarm_pending, (size_t)cap * sizeof *np);
        if (np) { pwarm_pending = np; pwarm_cap = cap; }
    }
    if (pwarm_n < pwarm_cap) pwarm_pending[pwarm_n++] = p;
    pthread_mutex_unlock(&pwarm_lock);
}

static bool format_has(VkFormat f, VkFormatFeatureFlags want) {
    static struct { VkFormat f; VkFormatFeatureFlags have; } known[64];
    static int nknown;
    for (int i = 0; i < nknown; i++) if (known[i].f == f) return (known[i].have & want) == want;
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &p);
    if (nknown < 64) { known[nknown].f = f; known[nknown++].have = p.optimalTilingFeatures; }
    return (p.optimalTilingFeatures & want) == want;
}

// A recorded key as this GPU's draws would make it, or false if this GPU cannot build it.
static bool pipe_key_for_device(PipeKey *k) {
    extern VkFormat vk_depth24_format(void);
    if (k->ncolor > 8 || !k->samples || (k->samples & (k->samples - 1)) || k->samples > 64) return false;
    for (uint32_t i = 0; i < k->ncolor; i++) {
        if (k->color[i] == VK_FORMAT_UNDEFINED) continue;
        VkFormatFeatureFlags want = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
        if ((k->blend_on >> i) & 1) want |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        if (!format_has(k->color[i], want)) return false;
    }
    if (k->depth == VK_FORMAT_D24_UNORM_S8_UINT) k->depth = vk_depth24_format();  // (AMD: D32S8, as its draws use)
    if (k->stencil == VK_FORMAT_D24_UNORM_S8_UINT) k->stencil = vk_depth24_format();
    if (k->depth && !format_has(k->depth, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) return false;
    if (k->stencil && !format_has(k->stencil, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) return false;
    for (int i = 0; i < 32; i++) {
        if (!(k->attr_mask & (1u << i))) continue;
        if (k->attr_bind[i] > ZERO_BINDING) return false;
        if (k->attr_bind[i] != ZERO_BINDING) k->attr_fmt[i] = vertex_format_supported(k->attr_fmt[i]);
    }
    for (int s = 0; s < 17; s++) if (k->divisor[s] > 1 && !g_vk.divisor) k->divisor[s] = 1;
    return k->topo_class <= 2;
}

static void build_pipeline_warm(void *arg) {
    Pipe *pp = arg;
    if (atomic_exchange(&pp->claimed, 1)) return;  // (a draw needed it first and built it)
    build_pipeline(pp);
}

// Once a frame (device.c), on the render thread: queue the recorded pipelines of programs that became ready.
void vk_pipeline_warmup(void) {
    if (pipe_warm_off()) return;
    if (prec_n < 0) prec_load();
    Prog *ready[256];
    int nready = 0;
    pthread_mutex_lock(&pwarm_lock);
    int keep = 0;
    for (int i = 0; i < pwarm_n; i++) {
        Prog *p = pwarm_pending[i];
        int s = atomic_load(&p->state);
        if (s == 0 || (s == 1 && nready == 256)) pwarm_pending[keep++] = p;
        else if (s == 1) ready[nready++] = p;
    }
    pwarm_n = keep;
    pthread_mutex_unlock(&pwarm_lock);
    for (int n = 0; n < nready; n++) {
        Prog *p = ready[n];
        for (int32_t i = prec_head[p->ident & 4095]; i; i = prec_next[i - 1]) {
            if (prec[i - 1].ident != p->ident) continue;
            if (pipe_tab_used >= PIPE_TAB * 3 / 4) return;
            PipeKey k = prec[i - 1].k;
            if (!pipe_key_for_device(&k)) { pwarm_rejected++; continue; }
            k.prog = p;
            uint64_t ph = hash_words(0xcbf29ce484222325ull, &k, sizeof k);
            if (!ph) ph = 1;
            uint32_t slot = (uint32_t)(ph ^ (ph >> 29)) & (PIPE_TAB - 1);
            while (pipe_tab[slot].key && pipe_tab[slot].key != ph) slot = (slot + 1) & (PIPE_TAB - 1);
            if (pipe_tab[slot].pipe) continue;
            Pipe *pp = calloc(1, sizeof *pp);
            pp->k = k;
            pp->warmed = true;
            pipe_tab[slot].key = ph;
            pipe_tab[slot].pipe = pp;
            pipe_tab_used++;
            pwarm_queued++;
            run_job_low(build_pipeline_warm, pp);
        }
    }
}

// ================================================================== feedback snapshots (renderer.m resolve_feedback)
static NvnTexture *feedback_tex[4];
static VkImageView feedback_view[4];
static int nfeedback;
static uint64_t g_cur_fs_hash;
static NvnTexture *snap_tex[8];
static bool snap_ok[8];
static const char *snap_why[8];
static const char *snap_reason = "?";
static void snapshots_invalidate(NvnTexture *t) {
    for (int i = 0; i < 8; i++) if ((!t || snap_tex[i] == t) && snap_ok[i]) { snap_ok[i] = false; snap_why[i] = snap_reason; }
}

typedef struct { NvnTexture *key; TexBackend copy; } Snap;
static Snap snaps[16];
static int nsnaps;

static NvnTexture *shader_texture_object(int stage, ShaderTexture *st_) {
    int slot = ((int)st_->handle_offset - 0x20) / 8;
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    if (st_->is_image) {  // (Astral Chain) SUST: image handles follow the 32 texture handles (0x120 + 8 * slot)
        slot = ((int)st_->handle_offset - 0x120) / 8;
        h = (slot >= 0 && slot < 8 && stage < 6) ? st.img[stage][slot] : 0;
    }
    uint32_t tic = h & 0xFFFFF;
    if (!h || !st.tpool || tic >= (uint32_t)st.tpool->count) return NULL;
    return ((NvnTexEntry *)st.tpool->entries)[tic].tex;
}

static bool is_attachment(NvnTexture *t) {
    if (!t) return false;
    if (t == st.rt.depth) return true;
    for (int i = 0; i < st.rt.ncolor; i++) if (t == st.rt.color[i]) return true;
    return false;
}

static TexBackend *snapshot_image(NvnTexture *t, TexBackend *src) {
    Snap *s = NULL;
    for (int i = 0; i < nsnaps; i++) if (snaps[i].key == t) s = &snaps[i];
    if (!s) {
        if (nsnaps == 16) { static int rr; s = &snaps[rr++ & 15]; } else s = &snaps[nsnaps++];
        s->key = t;
    }
    TexBackend *d = &s->copy;
    if (d->image && (d->width != src->width || d->height != src->height || d->format != src->format)) {
        vk_views_forget(d);  // (resolution change)
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = src->format,
                                .extent = {src->width, src->height, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = (VkSampleCountFlagBits)src->samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = src->format; d->width = src->width; d->height = src->height; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = src->samples; d->aspect = src->aspect; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

// (debug frames) a depth image's depth values as a greyscale PGM, stretched over its own min..max
static void debug_save_depth(TexBackend *b, const char *path) {
    if (!b || !b->image || !(b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) || b->format != VK_FORMAT_D32_SFLOAT_S8_UINT) return;
    uint64_t n = (uint64_t)b->width * b->height;
    VkSpan out = vk_readback(n * 4);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                           .imageExtent = {b->width, b->height, 1}};
    vkCmdCopyImageToBuffer(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    const float *d = out.ptr;
    float lo = 1e30f, hi = -1e30f;
    for (uint64_t i = 0; i < n; i++) { if (d[i] < lo) lo = d[i]; if (d[i] > hi) hi = d[i]; }
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P5 %u %u 255\n", b->width, b->height);
    for (uint64_t i = 0; i < n; i++) fputc(hi > lo ? (int)((d[i] - lo) / (hi - lo) * 255.0f) : 0, f);
    fclose(f);
    LOG("[vk] saved %s (%ux%u depth %.6f..%.6f)\n", path, b->width, b->height, lo, hi);
}

// (debug) LARECOMP_COPY_TRACE=<frame>: draws' attachments / sampled textures, copies and snapshots of that frame ("[ct]")
static bool ct_on(void) {
    static const char *e = (const char *)1;
    if (e == (const char *)1) e = getenv("LARECOMP_COPY_TRACE");
    if (!e) return false;
    static uint64_t memo_f = ~0ull;
    static bool memo;
    uint64_t f = renderer_present_count();
    if (f == memo_f) return memo;
    memo_f = f;
    memo = false;
    for (const char *p = e; *p;) {
        if ((uint64_t)strtoll(p, (char **)&p, 10) == f) { memo = true; break; }
        if (*p == ',') p++; else break;
    }
    if (memo) LOG("[ct] ===== frame %llu\n", (unsigned long long)f);
    return memo;
}
static void ct_draw(Prog *pr, const char *what) {
    LOG("[ct] %s fs %016llx rt", what, (unsigned long long)g_cur_fs_hash);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" c%d=%p/%#x", i, (void *)st.rt.color[i], st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d=%p/%#x%s", (void *)st.rt.depth, st.rt.depth->format, st.ds[0] && st.ds[1] ? "W" : "");
    LOG(" cm %08x vp %.0f %.0f %.0f %.0f sc %d %d %d %d | tex", st.channel_mask, st.viewport[0][0], st.viewport[0][1], st.viewport[0][2],
        st.viewport[0][3], st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < (infos[sg] ? infos[sg]->ntex : 0); i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (t) LOG(" %s%p/%#x", sg ? "" : "v:", (void *)t, t->format);
        }
    LOG("\n");
}

// ================================================================== copy elision (depth copies)
// The game copies its main depth buffer whole, every frame, into textures that are then only sampled while the
// source keeps its depth (the water / transparency depth input), or that get their depth cleared right away
// (stencil kept). At 3x internal resolution each such copy is ~0.15 ms of pure bandwidth. Instead of copying, the
// destination becomes an alias: sampling it (and copying from it) reads the source's image. The alias ends before
// anything changes either side:
//   - the source is about to be overwritten whole (both aspects cleared): the two images swap - the destination
//     keeps the contents without a copy, the source is cleared in the destination's old image;
//   - the destination is about to be overwritten in some aspects (its depth cleared): only the other aspects are
//     copied (stencil, a quarter of the bytes);
//   - anything else (a draw writing depth / stencil, a partial clear, a copy or upload into either, the
//     destination bound as an attachment, a texture freed or reallocated): the real copy is made first.
// Only same-size, same-format, single-level, single-sample depth/stencil images, whole-image copies.
// LARECOMP_VK_COPY_OPT=0: always copy (the old behaviour).
typedef struct { NvnTexture *src, *dst; VkImage simg, dimg; } CopyAlias;
extern uint32_t vk_view_gen;
static CopyAlias aliases[8];
static int naliases;
static uint64_t stat_alias_made, stat_alias_renamed, stat_alias_partial, stat_alias_copied;

static int copy_opt(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VK_COPY_OPT"); on = e && *e ? atoi(e) : 1; }
    return on;
}

// still valid: both textures keep the images the alias was made with (a resolution change reallocates them)
static bool alias_valid(const CopyAlias *a) {
    const TexBackend *s = a->src->backend, *d = a->dst->backend;
    return s && d && s->image == a->simg && d->image == a->dimg;
}

static void alias_drop(int i) {
    aliases[i] = aliases[--naliases];
    vk_view_gen++;  // (exec.c's per-frame view memo: the destination's views change)
}

// the real copy of the aspects asked for, into the destination's own image
static void alias_copy(CopyAlias *a, VkImageAspectFlags aspects, const char *why) {
    TexBackend *s = a->src->backend, *d = a->dst->backend;
    aspects &= d->aspect;
    if (!aspects) return;
    vk_end_pass();
    st.rt_dirty = true;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "copy texture (deferred, %s%s) %ux%u", why, aspects == d->aspect ? "" : ", stencil only", a->dst->width, a->dst->height);
    VkImageCopy c = {.srcSubresource = {aspects, 0, 0, 1}, .dstSubresource = {aspects, 0, 0, 1}, .extent = {d->width, d->height, 1}};
    vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    if (aspects == d->aspect) stat_alias_copied++; else stat_alias_partial++;
    if (ct_on()) LOG("[ct] alias copy %p -> %p aspects %#x (%s)\n", (void *)a->src, (void *)a->dst, aspects, why);
}

// t is about to be written; full: the aspects of it the write overwrites whole (0: unknown / partial)
static void alias_before_write(NvnTexture *t, VkImageAspectFlags full, const char *why) {
    if (!naliases || !t) return;
    for (int i = naliases - 1; i >= 0; i--) {
        CopyAlias *a = &aliases[i];
        if (!alias_valid(a)) { alias_drop(i); continue; }
        if (a->dst != t) continue;
        // (LARECOMP_VK_COPY_OPT=2: all aspects even then - for comparing the stencil-only copy's cost)
        alias_copy(a, ((TexBackend *)t->backend)->aspect & (copy_opt() == 2 ? ~0u : ~full), why);
        alias_drop(i);
    }
    NvnTexture *heir = NULL;  // t overwritten whole: one destination can take its image instead of a copy
    if (t->backend && (((TexBackend *)t->backend)->aspect & ~full) == 0)
        for (int i = 0; i < naliases && !heir; i++) if (aliases[i].src == t) heir = aliases[i].dst;
    for (int i = naliases - 1; i >= 0; i--) {
        CopyAlias *a = &aliases[i];
        if (a->src != t || a->dst == heir) continue;
        alias_copy(a, ((TexBackend *)a->dst->backend)->aspect, why);
        alias_drop(i);
    }
    int rename = -1;
    for (int i = 0; i < naliases; i++) if (heir && aliases[i].src == t && aliases[i].dst == heir) rename = i;
    if (rename >= 0) {
        CopyAlias *a = &aliases[rename];
        TexBackend *s = a->src->backend, *d = a->dst->backend;
        vk_end_pass();
        st.rt_dirty = true;
        VkImage im = s->image; s->image = d->image; d->image = im;
        VmaAllocation m = s->mem; s->mem = d->mem; d->mem = m;
        vk_views_forget(s);
        vk_views_forget(d);
        stat_alias_renamed++;
        if (ct_on()) LOG("[ct] alias rename %p -> %p (%s)\n", (void *)a->src, (void *)a->dst, why);
        alias_drop(rename);
    }
}

// the texture whose image holds t's contents (t itself unless it is an alias)
static NvnTexture *alias_read(NvnTexture *t) {
    for (int i = 0; i < naliases; i++)
        if (aliases[i].dst == t) {
            if (alias_valid(&aliases[i])) return aliases[i].src;
            alias_drop(i);
            return t;
        }
    return t;
}

// copy_texture: a whole-image copy that can be an alias instead (true: done)
static bool alias_make(NvnTexture *src, NvnTexture *dst, TexBackend *s, TexBackend *d) {
    if (!copy_opt() || src == dst) return false;
    if (!(s->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) return false;
    if (s->format != d->format || s->width != d->width || s->height != d->height || s->aspect != d->aspect || s->levels != 1 ||
        d->levels != 1 || s->layers != 1 || d->layers != 1 || s->depth != 1 || d->depth != 1 || s->samples != 1 || d->samples != 1 ||
        s->base_type != d->base_type || s->presented || d->presented || !s->image || !d->image || !s->gpu_owned || !d->gpu_owned)
        return false;
    // the destination's previous contents (or alias) are replaced whole; a destination that is itself the source of
    // aliases gets those resolved first
    alias_before_write(dst, d->aspect, "copy dst");
    src = alias_read(src);  // (a copy of an alias: of its source)
    s = src->backend;
    if (src == dst) return true;  // (copied back onto itself)
    if (naliases == (int)(sizeof aliases / sizeof aliases[0])) return false;
    aliases[naliases++] = (CopyAlias){src, dst, s->image, d->image};
    vk_view_gen++;
    stat_alias_made++;
    return true;
}

// textures.c: t is being freed - an alias source hands its contents over first
void vk_alias_texture_destroyed(NvnTexture *t) {
    if (naliases) alias_before_write(t, 0, "freed");
}
void vk_alias_reset(void) { naliases = 0; }  // (reboot: every texture goes)

// (renderer_stats, once per stats interval) copies made aliases / renamed / partly or fully copied later
void vk_alias_stats(void) {
    static int n;
    if (!stat_alias_made || (n++ > 5 && n % 30)) return;
    LOG("[vk] copy alias: %llu made, %llu renamed, %llu stencil-only, %llu copied later (live %d)\n",
        (unsigned long long)stat_alias_made, (unsigned long long)stat_alias_renamed, (unsigned long long)stat_alias_partial,
        (unsigned long long)stat_alias_copied, naliases);
    stat_alias_made = stat_alias_renamed = stat_alias_partial = stat_alias_copied = 0;
}

// draws: attachments written (or a destination bound at all) end the aliases involved
static void alias_draw_check(void) {
    if (!naliases) return;
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i]) alias_before_write(st.rt.color[i], 0, "color target");
    NvnTexture *d = st.rt.depth;
    if (!d) return;
    bool writes = (st.ds[0] && st.ds[1]) || (st.ds[3] && ((st.stencil_mask[0] | st.stencil_mask[1]) & 0xFF));
    bool is_dst = false;
    for (int i = 0; i < naliases; i++) is_dst |= aliases[i].dst == d;
    if (writes || is_dst) alias_before_write(d, 0, writes ? "depth write" : "dst bound");
}

static void resolve_feedback(Prog *pr) {
    nfeedback = 0;
    static int off = -1;
    if (off < 0) off = getenv("LARECOMP_NO_FEEDBACK") != NULL;
    if (off) return;
    ShaderInfo *infos[2] = {pr->vi, pr->fi};
    for (int sg = 0; sg < 2; sg++)
        for (int i = 0; i < infos[sg]->ntex; i++) {
            NvnTexture *t = shader_texture_object(sg, &infos[sg]->tex[i]);
            if (!is_attachment(t)) continue;
            bool have = false;
            for (int k = 0; k < nfeedback; k++) have |= feedback_tex[k] == t;
            if (have || nfeedback == 4) continue;
            static int nosnap = -1;
            if (nosnap < 0) nosnap = getenv("LARECOMP_NO_SNAPSHOT_REUSE") != NULL;
            int si = -1;
            for (int k = 0; k < 8; k++) if (snap_tex[k] == t) si = k;
            bool self_write = t == st.rt.depth && st.ds[0] && st.ds[1];
            TexBackend *src = vk_tex(t, true);
            Snap *existing = NULL;
            for (int k = 0; k < nsnaps; k++) if (snaps[k].key == t && snaps[k].copy.image) existing = &snaps[k];
            VkImageAspectFlags sa = src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : src->aspect;
            static const VkComponentMapping id = {0};
            if (!nosnap && si >= 0 && (snap_ok[si] || self_write) && existing) {
                feedback_tex[nfeedback] = t;
                feedback_view[nfeedback++] = vk_image_view(&existing->copy, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
                continue;
            }
            stat_feedback_copies++;
            if (ct_on()) LOG("[ct] snapshot %p %ux%u (was %s, si %d)\n", (void *)t, src->width, src->height, si >= 0 ? snap_why[si] : "new", si);
            vk_end_pass();
            TexBackend *dst = snapshot_image(t, src);
            VkCommandBuffer cb = vk_cmd();
            vk_full_barrier(cb);
            gp_mark(cb, "feedback snapshot %ux%u fmt %d", src->width, src->height, src->format);
            VkImageCopy rg = {.srcSubresource = {src->aspect, 0, 0, 1}, .dstSubresource = {src->aspect, 0, 0, 1},
                              .extent = {src->width, src->height, 1}};
            vkCmdCopyImage(cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);
            if (g_debug_frame && (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                static int nd;
                char path[128];
                snprintf(path, sizeof path, "build/fbdepth_%03d_src.pgm", nd);
                debug_save_depth(src, path);
                snprintf(path, sizeof path, "build/fbdepth_%03d_snap.pgm", nd++);
                debug_save_depth(dst, path);
                cb = vk_cmd();
            }
            feedback_tex[nfeedback] = t;
            feedback_view[nfeedback++] = vk_image_view(dst, VK_IMAGE_VIEW_TYPE_2D, src->format, 0, 1, 0, 1, id, sa);
            if (si < 0) { static int rr; si = rr++ & 7; snap_tex[si] = t; }
            snap_ok[si] = true;
            st.rt_dirty = true;
        }
}

// ================================================================== resource binding
static VkDescriptorBufferInfo ubo_span(VkSpan s, uint64_t size) {
    uint64_t max = g_vk.props.limits.maxUniformBufferRange;
    return (VkDescriptorBufferInfo){s.buf, s.off, size > max ? max : size};
}

// Constant buffer k of a stage (renderer.m bind_cbufs), copied into transient memory.
static VkDescriptorBufferInfo cbuf_info(int stage, ShaderInfo *si, int k, uint32_t gx, uint32_t gy, uint32_t gz) {
    static const uint8_t zeros[0x10000];
    if (k == 0) {
        uint32_t drv[0x800 / 4] = {0};
        if (stage == 5) {
            drv[0x200 / 4] = gx; drv[0x204 / 4] = gy; drv[0x208 / 4] = gz;
            for (int i = 0; i < 16; i++) {
                uint32_t *u = &drv[(0x210 + 16 * i) / 4];
                u[0] = (uint32_t)st.ubuf[5][i].addr; u[1] = (uint32_t)(st.ubuf[5][i].addr >> 32); u[2] = (uint32_t)st.ubuf[5][i].size;
                uint32_t *e = &drv[(0x310 + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[5][i].addr; e[1] = (uint32_t)(st.sbuf[5][i].addr >> 32); e[2] = (uint32_t)st.sbuf[5][i].size;
            }
        } else {
            uint32_t base = 0x110 + 0x100 * (stage == 1 ? 4 : 0);
            for (int i = 0; i < 16; i++) {
                uint32_t *e = &drv[(base + 16 * i) / 4];
                e[0] = (uint32_t)st.sbuf[stage][i].addr;
                e[1] = (uint32_t)(st.sbuf[stage][i].addr >> 32);
                e[2] = (uint32_t)st.sbuf[stage][i].size;
            }
        }
        uint64_t size = si->cbuf_size[0] > sizeof drv ? sizeof drv : (si->cbuf_size[0] ? si->cbuf_size[0] : 256);
        if (size < 0x500 && stage == 5) size = 0x500;
        return ubo_span(copy_dedupe(stage == 5 ? DEDUPE_DRV_C : stage == 1 ? DEDUPE_DRV_F : DEDUPE_DRV_V, drv, size, 256), size);
    }
    if (k == 1) {  // compiler constants stored after the shader code
        NvnShaderStage *ss = &st.program->stages[stage];
        uint64_t size = ss->const_size ? ss->const_size : 16;
        if (size < si->cbuf_size[1]) size = si->cbuf_size[1];
        return ubo_span(guest_copy(ss->code_addr + ss->const_offset, size), size);  // (constant: shared per submission)
    }
    int idx = k - 3;  // cbuf_base
    Range r = (idx >= 0 && idx < 18) ? st.ubuf[stage][idx] : (Range){0, 0};
    uint64_t size = si->cbuf_size[k] ? si->cbuf_size[k] : 256;
    if (!r.addr) return ubo_span(vk_transient_copy(zeros, size, 256), size);
    if ((si->cbuf_dynamic & (1u << k)) && r.size) size = r.size > 0x10000 ? 0x10000 : r.size;
    if (size > r.size && r.size) size = r.size;
    if (size < 16) size = 16;
    // snapshot: later CPU writes must not leak in. Shared by the draws of one submission binding the same range:
    // within a command list nothing writes guest memory (no inline UpdateUniformBuffer: that writes through below
    // and drops the shared copies).
    return ubo_span(guest_copy(r.addr, size), size);
}

// Global memory (shader LDG/STG, maxwell_gmem_base = HEAP_BASE): the guest heap imported as a GPU buffer
// (VK_EXT_external_memory_host), zero-copy like metal.m's g_guest_buffer, so compute results land in guest
// memory. The import pins its pages, so it covers only the heap the game set up (SetHeapSize), made on first use.
// LARECOMP_VK_NO_GMEM=1: no import, reads return zero.
static VkBuffer gmem_buf;
static VkDeviceMemory gmem_mem;
static uint64_t gmem_size;

static bool gmem_import(uint64_t size) {
    PFN_vkGetMemoryHostPointerPropertiesEXT props =
        (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult pr = props ? props(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)(uintptr_t)HEAP_BASE, &mp) : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (pr != VK_SUCCESS || !mp.memoryTypeBits) {
        LOG("[vk] heap import: host pointer properties %d types %#x base %#llx\n", pr, mp.memoryTypeBits, (unsigned long long)HEAP_BASE);
        return false;
    }
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = (void *)(uintptr_t)HEAP_BASE};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
                               .memoryTypeIndex = (uint32_t)__builtin_ctz(mp.memoryTypeBits)};
    VkDeviceMemory mem;
    // Astral Chain: no-access heap pages (stack/transfer-memory sources) are made pinnable for the import.
    extern int nvn_heap_prepare_import(uint64_t size);
    extern void nvn_heap_finish_import(int imported);
    VkResult ar = nvn_heap_prepare_import(size) ? vkAllocateMemory(g_vk.dev, &ai, NULL, &mem) : VK_ERROR_MEMORY_MAP_FAILED;
    nvn_heap_finish_import(ar == VK_SUCCESS);
    if (ar != VK_SUCCESS) {
        LOG("[vk] heap import: allocate %llu bytes type %u -> %d\n", (unsigned long long)size, ai.memoryTypeIndex, ar);
        return false;
    }
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    VkBuffer buf;
    if (vkCreateBuffer(g_vk.dev, &bi, NULL, &buf) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, mem, NULL); return false; }
    vkBindBufferMemory(g_vk.dev, buf, mem, 0);
    if (gmem_buf) {  // the heap grew: the old import may still be in use by submitted work
        vk_wait_seq(submit_seq);
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
    }
    gmem_buf = buf;
    gmem_mem = mem;
    gmem_size = size;
    LOG("[vk] guest heap imported for shader global memory: %llu MB\n", (unsigned long long)(size >> 20));
    return true;
}

static uint64_t gmem_tried;  // heap size the import was last attempted for (gmem_info imports / re-imports)

// ---- Astral Chain: device-local mirror of the guest heap (default; the host-pointer import is opt-in)
// The GPU reads its own copy of the heap. Before each submission the pages the CPU wrote since the last copy
// (runtime write tracking) are copied in through the upload buffer. Ranges that storing shaders may write are
// copied back after the submission and written into guest memory once it completes (gmem_writeback_apply),
// before a guest thread waiting on its fence resumes. No guest memory is pinned or handed to the driver.
extern int nvn_heap_mirror_dirty(uint64_t limit, uint64_t *pages, int max);
extern void nvn_heap_mirror_mark_all(uint64_t limit);
extern void nvn_heap_mirror_clean(uint64_t addr, uint64_t size);
extern void nvn_heap_mirror_interest(uint64_t addr, uint64_t size);
extern int nvn_guest_readable(uint64_t addr, uint64_t size);
static VmaAllocation gmem_alloc;  // non-null: gmem_buf is the mirror

static bool gmem_mirror(uint64_t size) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkBuffer buf;
    VmaAllocation mem;
    VkResult r = vmaCreateBuffer(g_vk.vma, &bi, &ai, &buf, &mem, NULL);
    if (r != VK_SUCCESS) { LOG("[vk] heap mirror: %llu MB buffer failed (%d)\n", (unsigned long long)(size >> 20), r); return false; }
    if (gmem_buf) {  // the heap grew: the old mirror may still be in use by submitted work
        vk_wait_seq(submit_seq);
        if (gmem_alloc) vmaDestroyBuffer(g_vk.vma, gmem_buf, gmem_alloc);
        else { vkDestroyBuffer(g_vk.dev, gmem_buf, NULL); vkFreeMemory(g_vk.dev, gmem_mem, NULL); }
    }
    gmem_buf = buf;
    gmem_alloc = mem;
    gmem_size = size;
    nvn_heap_mirror_mark_all(size);  // the whole committed heap is copied in by the next sync
    LOG("[vk] guest heap mirrored in device memory for shader global memory: %llu MB\n", (unsigned long long)(size >> 20));
    return true;
}

static void gmem_sync(void) {
    if (!gmem_buf || !gmem_alloc) return;
    static uint64_t pages[65536];
    VkCommandBuffer cb = VK_NULL_HANDLE;
    for (;;) {
        int n = nvn_heap_mirror_dirty(gmem_size, pages, 65536);
        for (int i = 0; i < n;) {
            int j = i + 1;
            while (j < n && pages[j] == pages[j - 1] + 4096 && j - i < 16384) j++;  // runs of up to 64 MB
            uint64_t addr = pages[i], len = (uint64_t)(j - i) * 4096;
            if (nvn_guest_readable(addr, len)) {  // (no-access stack/transfer sources keep their old copy)
                if (!cb) cb = vk_upload_cmd();
                VkSpan sp = vk_transient(len, 16);
                memcpy(sp.ptr, (const void *)(uintptr_t)addr, len);
                VkBufferCopy cp = {sp.off, addr - HEAP_BASE, len};
                vkCmdCopyBuffer(cb, sp.buf, gmem_buf, 1, &cp);
                stat_mirror_bytes += len;
            }
            i = j;
        }
        if (n < 65536) break;
    }
    if (cb) vk_full_barrier(cb);  // mirror copies land before the vertex cache copies from the mirror
}

// Ranges storing shaders may have written in the pending submission; copied back at its end.
typedef struct { uint64_t seq, addr, size; VkBuffer buf; VmaAllocation mem; void *ptr; } GmemWb;
static GmemWb wb_pending[64], wb_flight[256];
static int wb_npend, wb_nflight;
static void gmem_wb_note(uint64_t addr, uint64_t size) {
    if (!gmem_alloc || !size || addr < HEAP_BASE || addr - HEAP_BASE + size > gmem_size) return;
    for (int i = 0; i < wb_npend; i++)
        if (wb_pending[i].addr == addr && wb_pending[i].size == size) return;
    if (wb_npend == 64) FATAL("heap mirror: more than 64 GPU-written ranges in one submission\n");
    wb_pending[wb_npend++] = (GmemWb){.addr = addr, .size = size};
}
static void gmem_wb_record(VkCommandBuffer cb, uint64_t seq) {
    if (!wb_npend) return;
    vk_full_barrier(cb);
    for (int i = 0; i < wb_npend; i++) {
        GmemWb *w = &wb_pending[i];
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = w->size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &w->buf, &w->mem, &info));
        w->ptr = info.pMappedData;
        w->seq = seq;
        VkBufferCopy cp = {w->addr - HEAP_BASE, 0, w->size};
        vkCmdCopyBuffer(cb, gmem_buf, w->buf, 1, &cp);
        if (wb_nflight == 256) FATAL("heap mirror: too many GPU write-backs in flight\n");
        wb_flight[wb_nflight++] = *w;
    }
    wb_npend = 0;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                           .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}
// Lazy write-back (Astral Chain). Copying every storing shader's bound buffers back after each submission moved
// 0.25-1 GB/s through memcpy (run 414: ~10% of the guest thread), nearly all GPU-only data (light lists). A completed
// write-back instead stays in its host-visible staging buffer and its guest pages are made PAGE_NOACCESS. A CPU
// access faults and resolves that one page: every pending entry covering it copies its part in, oldest first (newer
// results win), and the page's protection is lifted.
// Protection changes split the heap's VAD and made GetWriteWatch (track_harvest) several times slower (run 416: 18%),
// so churn is avoided: a range the CPU has faulted on is "hot" and written back eagerly from then on (no protection);
// a newer write-back of exactly the same range takes over the older entry and re-protects only the pages that were
// resolved; pages of older entries whose bytes the new range covers completely are dropped without a copy. File I/O on
// guest buffers resolves its range first (nvn_lazy_flush_hook). LARECOMP_VK_LAZY_WB=0: copy at completion.
typedef struct { uint64_t addr, size, seq, lo, hi; VkBuffer buf; VmaAllocation mem; void *ptr; uint8_t *done; uint32_t left, faults; } Lazy;
enum { LAZY_MAX = 512, LAZY_HOT_MAX = 1024 };
static Lazy lazy_tab[LAZY_MAX], lazy_free[LAZY_MAX];
static int lazy_nfree;
static struct { uint64_t addr, size; } lazy_hot[LAZY_HOT_MAX];  // ranges the CPU reads: eager write-back
static int lazy_nhot;
static pthread_mutex_t lazy_lock = PTHREAD_MUTEX_INITIALIZER;
extern int nvn_page_protect(uint64_t addr, uint64_t size, int noaccess);
extern void (*nvn_lazy_flush_hook)(uint64_t addr, uint64_t size);
extern int (*nvn_lazy_fault_hook)(uint64_t addr);

static bool lazy_is_hot(uint64_t addr, uint64_t size) {
    for (int i = 0; i < lazy_nhot; i++) if (lazy_hot[i].addr == addr && lazy_hot[i].size == size) return true;
    return false;
}
// Entries with every page resolved move to lazy_free (staging destroyed by the GPU thread). Caller holds lazy_lock.
static void lazy_compact(void) {
    int k = 0;
    for (int i = 0; i < lazy_n; i++) {
        if (!lazy_tab[i].left) { free(lazy_tab[i].done); lazy_free[lazy_nfree++] = lazy_tab[i]; stat_wb_ranges++; continue; }
        lazy_tab[k++] = lazy_tab[i];
    }
    lazy_n = k;
}
static bool lazy_pending(uint64_t p) {
    for (int i = 0; i < lazy_n; i++)
        if (p >= lazy_tab[i].lo && p < lazy_tab[i].hi && !lazy_tab[i].done[(p - lazy_tab[i].lo) >> 12]) return true;
    return false;
}
// Copy page p's pending parts in, oldest entry first, and lift its protection. Caller holds lazy_lock and compacts.
static void lazy_resolve_page(uint64_t p, bool fault) {
    if (!nvn_page_protect(p, 4096, 0)) FATAL("lazy write-back: unprotect %llx failed\n", (unsigned long long)p);
    for (uint64_t last = 0;;) {
        int best = -1;
        for (int i = 0; i < lazy_n; i++) {
            Lazy *e = &lazy_tab[i];
            if (p < e->lo || p >= e->hi || e->done[(p - e->lo) >> 12] || e->seq < last) continue;
            if (best < 0 || e->seq < lazy_tab[best].seq) best = i;
        }
        if (best < 0) break;
        Lazy *e = &lazy_tab[best];
        uint64_t a = p > e->addr ? p : e->addr, b = p + 4096 < e->addr + e->size ? p + 4096 : e->addr + e->size;
        memcpy((void *)(uintptr_t)a, (const uint8_t *)e->ptr + (a - e->addr), b - a);
        nvn_heap_mirror_clean(a, b - a);  // (the mirror holds these bytes already)
        stat_wb_bytes += b - a;
        e->done[(p - e->lo) >> 12] = 1;
        e->left--;
        last = e->seq;
        // Hot: faults resolved at least half of the range's pages (it is read through; copying it at once is cheaper).
        if (fault && ++e->faults * 2 >= (uint32_t)((e->hi - e->lo) >> 12) && !lazy_is_hot(e->addr, e->size) && lazy_nhot < LAZY_HOT_MAX)
            lazy_hot[lazy_nhot++] = (typeof(lazy_hot[0])){e->addr, e->size};
    }
}
// Pending pages of [na, nb): a page whose pending bytes all lie inside [na, nb) is dropped (a newer write-back of
// [na, nb) replaces them; it stays protected if keep), any other is resolved. Caller holds lazy_lock.
static void lazy_supersede(uint64_t na, uint64_t nb, bool keep) {
    for (uint64_t p = na & ~4095ull; p < nb; p += 4096) {
        if (!lazy_pending(p)) continue;
        bool covered = true;
        for (int i = 0; i < lazy_n && covered; i++) {
            Lazy *e = &lazy_tab[i];
            if (p < e->lo || p >= e->hi || e->done[(p - e->lo) >> 12]) continue;
            uint64_t a = p > e->addr ? p : e->addr, b = p + 4096 < e->addr + e->size ? p + 4096 : e->addr + e->size;
            covered = a >= na && b <= nb;
        }
        if (!covered) { lazy_resolve_page(p, false); continue; }
        for (int i = 0; i < lazy_n; i++) {
            Lazy *e = &lazy_tab[i];
            if (p >= e->lo && p < e->hi && !e->done[(p - e->lo) >> 12]) { e->done[(p - e->lo) >> 12] = 1; e->left--; }
        }
        if (!keep && !nvn_page_protect(p, 4096, 0)) FATAL("lazy write-back: unprotect %llx failed\n", (unsigned long long)p);
    }
    lazy_compact();
}
static int lazy_fault(uint64_t addr) {
    extern int nvn_page_state(uint64_t addr);
    pthread_mutex_lock(&lazy_lock);
    uint64_t p = addr & ~4095ull;
    int hit = lazy_pending(p);
    if (hit) { stat_wb_faults++; lazy_resolve_page(p, true); lazy_compact(); }
    else if (p >= HEAP_BASE && p - HEAP_BASE < HEAP_MAX) {
        // Not pending. Guest threads run in parallel: another one may have resolved the page between this fault and
        // the lock (it is accessible now: retry the access). A no-access page that no entry holds is a bookkeeping
        // gap: lift the protection rather than stop the game (one run died here at the end of the highway chase).
        int state = nvn_page_state(p);
        if (state == 1) hit = 1;
        else if (state == 2 && nvn_page_protect(p, 4096, 0)) {
            static int nlog;
            if (nlog++ < 20) LOG("[vk] lazy write-back: unexpected protected page %llx lifted\n", (unsigned long long)p);
            hit = 1;
        }
    }
    pthread_mutex_unlock(&lazy_lock);
    return hit;
}
static void lazy_flush(uint64_t addr, uint64_t size) {
    pthread_mutex_lock(&lazy_lock);
    for (uint64_t p = addr & ~4095ull; p < addr + size; p += 4096)
        if (lazy_pending(p)) lazy_resolve_page(p, false);
    lazy_compact();
    pthread_mutex_unlock(&lazy_lock);
}

// Write the GPU's results of completed submissions into guest memory (before a fence waiter resumes).
static void gmem_writeback_apply(uint64_t done) {
    static int lazy_on = -1;
    if (lazy_on < 0) {
        const char *e = getenv("LARECOMP_VK_LAZY_WB");
        lazy_on = !e || atoi(e) != 0;
        if (lazy_on) { nvn_lazy_fault_hook = lazy_fault; nvn_lazy_flush_hook = lazy_flush; }
    }
    pthread_mutex_lock(&lazy_lock);
    int k = 0;
    for (int i = 0; i < wb_nflight; i++) {
        GmemWb *w = &wb_flight[i];
        if (w->seq > done) { wb_flight[k++] = *w; continue; }
        if (!nvn_guest_readable(w->addr, w->size)) { vmaDestroyBuffer(g_vk.vma, w->buf, w->mem); continue; }
        nvn_heap_mirror_clean(w->addr, w->size);
        uint64_t lo = w->addr & ~4095ull, hi = (w->addr + w->size + 4095) & ~4095ull;
        uint32_t pages = (uint32_t)((hi - lo) >> 12);
        bool lazy = lazy_on && !lazy_is_hot(w->addr, w->size);
        int same = -1;
        bool overlap = false;
        for (int j = 0; j < lazy_n; j++)
            if (lazy_tab[j].lo < hi && lo < lazy_tab[j].hi) {
                if (lazy_tab[j].addr == w->addr && lazy_tab[j].size == w->size) same = j;
                else overlap = true;
            }
        if (lazy && same >= 0 && !overlap) {  // takes over the older entry: re-protect the pages it had resolved
            Lazy *o = &lazy_tab[same];
            for (uint32_t a = 0; a < pages;) {
                if (!o->done[a]) { a++; continue; }
                uint32_t b = a;
                while (b < pages && o->done[b]) b++;
                if (!nvn_page_protect(lo + ((uint64_t)a << 12), (uint64_t)(b - a) << 12, 1)) FATAL("lazy write-back: protect failed\n");
                a = b;
            }
            memset(o->done, 0, pages);
            lazy_free[lazy_nfree++] = *o;
            o->seq = w->seq; o->buf = w->buf; o->mem = w->mem; o->ptr = w->ptr; o->left = pages; o->faults = 0;
            continue;
        }
        if (same >= 0 || overlap) lazy_supersede(w->addr, w->addr + w->size, lazy);
        uint8_t *bits = lazy && lazy_n < LAZY_MAX ? calloc(pages, 1) : NULL;
        if (bits && nvn_page_protect(lo, hi - lo, 1)) {
            lazy_tab[lazy_n++] = (Lazy){w->addr, w->size, w->seq, lo, hi, w->buf, w->mem, w->ptr, bits, pages};
            continue;
        }
        free(bits);
        // (Table full or protection refused: pages superseded above stay protected without an entry; lift them. No
        // other entry has pending bytes inside this range any more.)
        if (lazy && (same >= 0 || overlap) && !nvn_page_protect(lo, hi - lo, 0)) FATAL("lazy write-back: unprotect failed\n");
        memcpy((void *)(uintptr_t)w->addr, w->ptr, w->size);  // eager: hot range, lazy off, or no room
        nvn_heap_mirror_clean(w->addr, w->size);  // (after the copy: the mirror holds these bytes already)
        stat_wb_bytes += w->size;
        stat_wb_ranges++;
        vmaDestroyBuffer(g_vk.vma, w->buf, w->mem);
    }
    wb_nflight = k;
    for (int i = 0; i < lazy_nfree; i++) vmaDestroyBuffer(g_vk.vma, lazy_free[i].buf, lazy_free[i].mem);
    lazy_nfree = 0;
    pthread_mutex_unlock(&lazy_lock);
}

static VkDescriptorBufferInfo gmem_info(void) {
    static int off = -1;
    // Astral Chain: the device-local mirror by default. The host-pointer import (AC_VK_HEAP_IMPORT=1) made the
    // NVIDIA driver log bursts of GPU faults (nvlddmkm 153) and the machine bug-checked (0x119, 2026-10-19).
    static int import = -1;
    if (import < 0) import = getenv("AC_VK_HEAP_IMPORT") && g_vk.host_import;
    if (off < 0) off = getenv("LARECOMP_VK_NO_GMEM") != NULL;
    uint64_t want = (kernel_heap_size() + 0xFFFF) & ~0xFFFFull;
    if (want > HEAP_MAX) want = HEAP_MAX;
    if (!off && want > gmem_size && !(import ? gmem_import(want) : gmem_mirror(want))) {
        LOG("[vk] guest heap for shader global memory unavailable (%llu MB): reads return zero\n", (unsigned long long)(want >> 20));
        off = 1;
    }
    if (gmem_buf) return (VkDescriptorBufferInfo){gmem_buf, 0, gmem_size};
    static uint32_t zero[1024];
    VkSpan s = vk_transient_copy(zero, sizeof zero, 256);
    return (VkDescriptorBufferInfo){s.buf, s.off, sizeof zero};
}

// Compute dispatches that use global memory: the imported heap is host memory across PCIe on a discrete GPU, and a
// shader storing to it in a loop crawls (the little chain chomps in Madam MeowMeow's house: ~8 ms a frame on a GTX 1660
// Super and on an RTX 3080 Ti alike, which the game then waits for on its fence - 49-53 fps there on the 1660). Before
// such a dispatch its bound storage buffers (sorted, merged) are copied into a device-local buffer, the address table
// (binding GMAP_BUFFER_INDEX) tells the shader (translate.c, GLSL compute) where each range sits, loads and stores
// inside them use that copy, and a shader that stores gets the ranges copied back to the heap right after it (the
// game alternates its buffer sets per frame: the CPU isn't writing them meanwhile). Accesses outside the table use
// the heap as before. LARECOMP_VK_GMEM_FAST=0: no copy (empty table: everything goes to the heap, the old behaviour).
#define GFAST_SIZE (32ull << 20)
static VkBuffer gfast_buf;
static VmaAllocation gfast_mem;
static uint32_t gmap_tab[(GMAP_MAX + 1) * 4];  // this dispatch's table (stage_set copies it into binding 20)
static VkBufferCopy gfast_cp[GMAP_MAX];       // its copies heap -> gfast_buf (reversed after a storing dispatch)
static int gfast_n;

static VkDescriptorBufferInfo gfast_info(void) {
    if (!gfast_buf) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GFAST_SIZE,
                                 .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &gfast_buf, &gfast_mem, NULL));
    }
    return (VkDescriptorBufferInfo){gfast_buf, 0, GFAST_SIZE};
}

static void gfast_prepare(VkCommandBuffer cb) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("LARECOMP_VK_GMEM_FAST"); on = !e || atoi(e) != 0; }
    memset(gmap_tab, 0, sizeof gmap_tab);
    gfast_n = 0;
    gmem_info();  // (the heap import, made on first use)
    gfast_info();
    if (!on || !gmem_buf) return;
    struct { uint64_t lo, hi; } r[16];
    int n = 0;
    for (int i = 0; i < 16; i++) {  // heap offsets of the bound storage buffers, 256-aligned
        uint64_t a = st.sbuf[5][i].addr, sz = st.sbuf[5][i].size;
        if (!a || !sz || a < HEAP_BASE || a - HEAP_BASE + sz > gmem_size) continue;
        uint64_t lo = (a - HEAP_BASE) & ~255ull, hi = (a - HEAP_BASE + sz + 255) & ~255ull;
        if (hi > gmem_size) hi = gmem_size;
        int k = n++;
        while (k > 0 && r[k - 1].lo > lo) { r[k] = r[k - 1]; k--; }  // (insertion sort by start)
        r[k].lo = lo; r[k].hi = hi;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {  // merge overlapping / nearby ranges (a small gap costs less than another table entry)
        if (m && r[i].lo <= r[m - 1].hi + 4096) { if (r[i].hi > r[m - 1].hi) r[m - 1].hi = r[i].hi; }
        else r[m++] = r[i];
    }
    VkBufferCopy cp[16];
    uint64_t off = 0;
    for (int i = 0; i < m; i++) {
        uint64_t len = r[i].hi - r[i].lo;
        if (m > GMAP_MAX || off + len > GFAST_SIZE) return;  // (too much: the heap as before)
        cp[i] = (VkBufferCopy){r[i].lo, off, len};
        gmap_tab[i * 4 + 0] = (uint32_t)r[i].lo;
        gmap_tab[i * 4 + 1] = (uint32_t)len;
        gmap_tab[i * 4 + 2] = (uint32_t)(off >> 2);
        off += (len + 255) & ~255ull;
    }
    if (!m) return;
    if (getenv("LARECOMP_LOG_GFAST")) {  // (debug) =<shader code address hex>: that shader's first tables
        static int nlog;
        if (st.program->stages[5].code_addr == strtoull(getenv("LARECOMP_LOG_GFAST"), NULL, 16) && nlog++ < 40) {
            LOG("[gfast] cs %llx: %d ranges, %llu KB:", (unsigned long long)st.program->stages[5].code_addr, m, (unsigned long long)(off >> 10));
            for (int i = 0; i < m; i++) LOG(" %llx+%llx", (unsigned long long)(r[i].lo + HEAP_BASE), (unsigned long long)(r[i].hi - r[i].lo));
            LOG("\n");
        }
    }
    vkCmdCopyBuffer(cb, gmem_buf, gfast_buf, (uint32_t)m, cp);
    vk_full_barrier(cb);
    gmap_tab[GMAP_MAX * 4] = (uint32_t)m;
    memcpy(gfast_cp, cp, (size_t)m * sizeof cp[0]);
    gfast_n = m;
}

// After a storing dispatch: its ranges back to the heap (before anything else reads them: draws, the CPU after the fence).
static void gfast_finish(VkCommandBuffer cb) {
    if (!gfast_n) return;
    VkBufferCopy back[GMAP_MAX];
    for (int i = 0; i < gfast_n; i++) back[i] = (VkBufferCopy){gfast_cp[i].dstOffset, gfast_cp[i].srcOffset, gfast_cp[i].size};
    vk_full_barrier(cb);
    vkCmdCopyBuffer(cb, gfast_buf, gmem_buf, (uint32_t)gfast_n, back);
    vk_full_barrier(cb);
    gfast_n = 0;
}

// ---- vertex / index cache
// Reading vertex / index data in place from the imported heap crosses PCIe for every vertex fetched on a discrete GPU
// (outdoors at 3x internal resolution on an RTX 3080 Ti: ~0.7-1.9 ms of GPU a frame; more on PCIe 3.0), and copying it
// per draw instead moves about as much onto the CPU. So each range bound for drawing gets a device-local copy, refreshed
// only where the CPU wrote since (the OS's page write tracking, plat_written_pages, asked at submission: vc_flush): one
// sequential GPU copy of the written pages in the submission's upload buffer (which completes before its draws) instead
// of fetches across the bus. Overlapping ranges are separate entries; the per-page generation keeps them all current.
// Pages the GPU writes itself (a compute dispatch's copy-back, gfast_finish) are never cached: draws over them read the
// heap in place as before, and so does everything when the OS gives no write tracking. LARECOMP_VK_VCACHE=0: off.
#define VC_PAGE 4096ull
#define VC_TAB 8192                   // entries (open addressing)
#define VC_MAX_RANGE (64ull << 20)    // larger bound ranges: read in place
#define VC_BUDGET (768ull << 20)      // device-local bytes kept; the least recently used go first
typedef struct { uint64_t lo, hi, used_seq; uint32_t gen, epoch; VkBuffer buf; VmaAllocation mem; bool gpu; } VcEnt;
static VcEnt vc_tab[VC_TAB];
static uint32_t vc_pend[VC_TAB];  // entries the pending submission draws from (vc_flush brings them up to date)
static int vc_npend;
static uint32_t *vc_page_gen;  // per heap page: the generation of its last CPU write seen (vc_gen when observed)
static uint8_t *vc_page_gpu;   // per heap page: written by the GPU
static uint32_t *vc_page_ggen;  // per heap page: the generation of the last recorded GPU write (texture checks)
static uint32_t *vc_page_gframe;  // per heap page: the frame it was last recorded (GPU writes still in flight)
static uint32_t vc_gen = 1, vc_epoch = 1;  // (epoch: bumped when pages turn GPU-written: entries check again)
static uint64_t vc_bytes;
#define VC_DEAD 1024
static struct { VkBuffer b; VmaAllocation m; uint64_t seq; } vc_dead[VC_DEAD];
static int vc_ndead;
static int vc_on = -1;
// (profiler: draws served / read in place, bytes refreshed, write tracking queries, written pages seen, query time)
uint64_t prof_vc_hits, prof_vc_bypass, prof_vc_copied, prof_vc_queries, prof_vc_scan_pages, prof_vc_scan_ns;

static void vc_free_dead(bool all) {
    uint64_t done = vk_completed_seq();
    int k = 0;
    for (int i = 0; i < vc_ndead; i++) {
        if (!all && vc_dead[i].seq > done) { vc_dead[k++] = vc_dead[i]; continue; }
        vmaDestroyBuffer(g_vk.vma, vc_dead[i].b, vc_dead[i].m);
    }
    vc_ndead = k;
}

static bool vc_drop(VcEnt *e) {  // (freed once the submissions that may read it are done)
    if (vc_ndead == VC_DEAD) { vk_wait_seq(vk_pending_seq() - 1); vc_free_dead(false); }
    if (vc_ndead == VC_DEAD) return false;  // (all read by the pending submission: keep it)
    vc_dead[vc_ndead].b = e->buf; vc_dead[vc_ndead].m = e->mem; vc_dead[vc_ndead++].seq = e->used_seq;
    vc_bytes -= e->hi - e->lo;
    memset(e, 0, sizeof *e);
    return true;
}

// The page tables (vc_page_gen / vc_page_gpu) exist whenever the OS gives write tracking: the vertex cache and the
// texture upload checks (vk_pages_changed) share them. Each write tracking query resets the OS bits, so every query
// stamps the pages it saw written with a new generation, and each user compares against the generation it last saw.
static int pt_state = -1;  // 1: tracking, 0: none
static bool pt_init(void) {
    if (pt_state >= 0) return pt_state;
    uint64_t pg = VC_PAGE, one;
    int n = plat_written_pages(HEAP_BASE, VC_PAGE, &one, 1, &pg);  // (is there write tracking?)
    pt_state = n >= 0 && pg == VC_PAGE;
    if (!pt_state) { LOG("[vk] no page write tracking (%d, page %llu)\n", n, (unsigned long long)pg); return false; }
    vc_page_gen = calloc(HEAP_MAX / VC_PAGE, sizeof *vc_page_gen);
    vc_page_gpu = calloc(HEAP_MAX / VC_PAGE, 1);
    vc_page_ggen = calloc(HEAP_MAX / VC_PAGE, sizeof *vc_page_ggen);
    vc_page_gframe = calloc(HEAP_MAX / VC_PAGE, sizeof *vc_page_gframe);
    vc_page_gen[0] = ++vc_gen;  // (the probe took page 0's tracking)
    return true;
}

// Texture upload checks (textures.c): may guest memory [addr, addr + size) have changed since generation `since`?
// 0: no write since, 1: written, -1: unknown (no tracking, outside the heap, or GPU writes recorded in the last
// few frames: they may not have run yet). GPU writes count by the generation they were recorded at: pages a
// storing shader may write were a permanent "unknown" before, which left textures there on a sampled hash only
// (stale blocks in large textures - character faces). *now: the generation to pass next time.
// Batched texture checks. A write tracking query costs ~6 us however small its range (+ ~2.3 ns a page), and a busy
// frame checks ~800 textures, one query each: ~600 ms in 2 s, a third of the render thread, in the street after the
// opening helicopter (33-45 fps there, at any resolution). The ranges checked in the last frame are merged into spans
// (gaps under 8 MB cost less to scan than a query); within one command list (backend_execute) a span is queried once,
// at the first check inside it, and the later checks inside it read the stamps without a system call. A write made
// after that query is seen at the next command list, as a write just after a texture's own query was before; on the
// Switch the game has to finish its writes before it submits. Ranges outside the spans are queried as before.
// LARECOMP_VK_WW_BATCH=0: one query per check.
extern int plat_write_harvest(uint64_t addr, uint64_t size);
extern int plat_written_pages_harvested(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page);
enum { WW_REC = 16384, WW_SPAN = 1024, WW_GAP = 2048 * VC_PAGE };
typedef struct { uint64_t lo, hi; } WwRange;
static WwRange ww_rec[WW_REC], ww_span[WW_SPAN];
static int ww_nrec, ww_nspan, ww_on = -1;
static uint32_t ww_span_list[WW_SPAN], ww_list;  // (the command list a span was last queried in)
static bool ww_in_list;
uint64_t prof_ww_direct, prof_ww_span;
static uint64_t ww_frame;
static int ww_cmp(const void *a, const void *b) {
    uint64_t x = ((const WwRange *)a)->lo, y = ((const WwRange *)b)->lo;
    return x < y ? -1 : x > y;
}
static void ww_new_frame(uint64_t frame) {  // spans from the ranges checked in the frame that ended
    ww_frame = frame;
    qsort(ww_rec, (size_t)ww_nrec, sizeof ww_rec[0], ww_cmp);
    ww_nspan = 0;
    for (int i = 0; i < ww_nrec && ww_nspan < WW_SPAN; i++) {
        if (ww_nspan && ww_rec[i].lo <= ww_span[ww_nspan - 1].hi + WW_GAP) {
            if (ww_rec[i].hi > ww_span[ww_nspan - 1].hi) ww_span[ww_nspan - 1].hi = ww_rec[i].hi;
        } else ww_span[ww_nspan++] = ww_rec[i];
    }
    memset(ww_span_list, 0, sizeof ww_span_list);
    ww_nrec = 0;
}
static int ww_written_pages(uint64_t lo, uint64_t hi, uint64_t *pages, int max, uint64_t *pg) {  // (heap offsets)
    if (ww_on < 0) { const char *e = getenv("LARECOMP_VK_WW_BATCH"); ww_on = !e || atoi(e) != 0; }
    if (!ww_on || !ww_in_list) { prof_ww_direct++; return plat_written_pages(HEAP_BASE + lo, hi - lo, pages, max, pg); }
    extern uint64_t vk_frame_counter(void);
    uint64_t frame = vk_frame_counter();
    if (frame != ww_frame) ww_new_frame(frame);
    if (ww_nrec < WW_REC) ww_rec[ww_nrec++] = (WwRange){lo, hi};
    int a = 0, b = ww_nspan;  // (the last span starting at or before lo)
    while (a < b) { int m = (a + b) / 2; if (ww_span[m].lo <= lo) a = m + 1; else b = m; }
    int k = a - 1;
    if (k < 0 || hi > ww_span[k].hi) { prof_ww_direct++; return plat_written_pages(HEAP_BASE + lo, hi - lo, pages, max, pg); }
    if (ww_span_list[k] != ww_list) {
        prof_ww_span++;
        if (plat_write_harvest(HEAP_BASE + ww_span[k].lo, ww_span[k].hi - ww_span[k].lo) < 0)
            return plat_written_pages(HEAP_BASE + lo, hi - lo, pages, max, pg);
        ww_span_list[k] = ww_list;
    }
    return plat_written_pages_harvested(HEAP_BASE + lo, hi - lo, pages, max, pg);
}

int vk_pages_changed(uint64_t addr, uint64_t size, uint32_t since, uint32_t *now) {
    if (!size || addr < HEAP_BASE || addr - HEAP_BASE + size > HEAP_MAX || !pt_init()) return -1;
    uint64_t lo = (addr - HEAP_BASE) & ~(VC_PAGE - 1), hi = (addr - HEAP_BASE + size + VC_PAGE - 1) & ~(VC_PAGE - 1);
    static uint64_t pages[4096];
    uint64_t pg = VC_PAGE;
    int n = ww_written_pages(lo, hi, pages, 4096, &pg);
    if (n < 0) return -1;
    uint32_t g = ++vc_gen;
    if (n >= 4096) for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) vc_page_gen[p] = g;
    else for (int k = 0; k < n; k++) vc_page_gen[(pages[k] - HEAP_BASE) / VC_PAGE] = g;
    *now = g;
    int changed = 0;
    extern uint64_t vk_frame_counter(void);
    uint32_t frame = (uint32_t)vk_frame_counter();
    for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) {
        if (vc_page_gpu[p]) {
            if (frame - vc_page_gframe[p] <= 4) return -1;
            if (vc_page_ggen[p] > since) changed = 1;
        }
        if (vc_page_gen[p] > since) changed = 1;
    }
    return changed;
}

// Heap pages [lo, hi) (offsets) written by the GPU: never cached from now on.
static void vc_mark_gpu(uint64_t lo, uint64_t hi) {
    if (!pt_init()) return;
    extern uint64_t vk_frame_counter(void);
    uint32_t g = ++vc_gen, frame = (uint32_t)vk_frame_counter();
    for (uint64_t p = lo / VC_PAGE; p < (hi + VC_PAGE - 1) / VC_PAGE && p < HEAP_MAX / VC_PAGE; p++) {
        if (!vc_page_gpu[p]) { vc_page_gpu[p] = 1; vc_epoch++; }
        vc_page_ggen[p] = g;
        vc_page_gframe[p] = frame;
    }
}

// A storing shader (stage 0 / 1 / 5) may write any of its bound storage buffers.
static void vc_mark_sbufs(int stage) {
    for (int i = 0; i < 16; i++) {
        uint64_t a = st.sbuf[stage][i].addr, sz = st.sbuf[stage][i].size;
        if (a >= HEAP_BASE && sz && a - HEAP_BASE < HEAP_MAX) {
            static int big;  // (wide storage bindings turn much of the heap GPU-written: textures there lose page tracking)
            if (sz >= (8u << 20) && big < 16) { big++; LOG("[vk] storage buffer stage %d slot %d: 0x%llx + %.1f MB\n", stage, i, (unsigned long long)a, sz / 1048576.0); }
            vc_mark_gpu(a - HEAP_BASE, a - HEAP_BASE + sz); gmem_wb_note(a, sz);
        }
    }
}

static bool vcache_lookup(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    if (vc_on < 0) {
        const char *e = getenv("LARECOMP_VK_VCACHE");
        vc_on = !e || atoi(e) != 0;
        if (vc_on && !pt_init()) {
            LOG("[vk] vertex cache off: no page write tracking\n");
            vc_on = 0;
        }
    }
    if (!vc_on || !gmem_buf || size > VC_MAX_RANGE) return false;
    uint64_t lo = (addr - HEAP_BASE) & ~(VC_PAGE - 1), hi = (addr - HEAP_BASE + size + VC_PAGE - 1) & ~(VC_PAGE - 1);
    if (hi > gmem_size) return false;
    uint64_t pend = vk_pending_seq();
    uint64_t h = (lo * 0x9E3779B97F4A7C15ull) ^ (hi * 0xC2B2AE3D27D4EB4Full);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VC_TAB - 1), free_slot = ~0u, old_slot = ~0u;
    VcEnt *e = NULL;
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VC_TAB - 1)) {
        VcEnt *c = &vc_tab[slot];
        if (c->buf && c->lo == lo && c->hi == hi) { e = c; break; }
        if (!c->buf && free_slot == ~0u) free_slot = slot;
        if (c->buf && c->used_seq < pend && (old_slot == ~0u || c->used_seq < vc_tab[old_slot].used_seq)) old_slot = slot;
    }
    if (!e) {  // a new range: its own device-local buffer (gen 0: copied whole at the submission)
        if (vc_npend == VC_TAB) return false;
        if (vc_ndead) vc_free_dead(false);
        if (free_slot == ~0u) {
            if (old_slot == ~0u || !vc_drop(&vc_tab[old_slot])) return false;
            free_slot = old_slot;
        }
        while (vc_bytes + (hi - lo) > VC_BUDGET) {  // over budget: the least recently used entry goes
            int lru = -1;
            for (int i = 0; i < VC_TAB; i++)
                if (vc_tab[i].buf && vc_tab[i].used_seq < pend && (lru < 0 || vc_tab[i].used_seq < vc_tab[lru].used_seq)) lru = i;
            if (lru < 0 || !vc_drop(&vc_tab[lru])) return false;
        }
        e = &vc_tab[free_slot];
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = hi - lo,
                                 .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        if (vmaCreateBuffer(g_vk.vma, &bi, &ai, &e->buf, &e->mem, NULL) != VK_SUCCESS) { memset(e, 0, sizeof *e); return false; }
        e->lo = lo; e->hi = hi;
        vc_bytes += hi - lo;
    }
    if (e->epoch != vc_epoch) {  // (pages turned GPU-written since: is this range one of them?)
        e->gpu = false;
        for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE && !e->gpu; p++) e->gpu = vc_page_gpu[p];
        e->epoch = vc_epoch;
    }
    if (e->gpu) { prof_vc_bypass++; return false; }
    if (e->used_seq != pend) {  // first use in this submission: brought up to date by vc_flush before it runs
        e->used_seq = pend;
        vc_pend[vc_npend++] = (uint32_t)(e - vc_tab);
    }
    prof_vc_hits++;
    *buf = e->buf;
    *off = addr - HEAP_BASE - lo;
    return true;
}

static int vc_pend_cmp(const void *a, const void *b) {
    uint64_t x = vc_tab[*(const uint32_t *)a].lo, y = vc_tab[*(const uint32_t *)b].lo;
    return x < y ? -1 : x > y;
}

// At submission (vk_submit_extra, before the upload buffer closes): the pages the CPU wrote in the ranges this
// submission draws from - one write tracking query per run of nearby ranges (a query costs about as much as scanning
// ~64 pages: ranges closer than that share one) - and their copies into the cache, ahead of the draws.
void vc_flush(void) {
    if (!vc_npend) return;
    qsort(vc_pend, (size_t)vc_npend, sizeof vc_pend[0], vc_pend_cmp);
    uint32_t g = ++vc_gen;
    static uint64_t pages[4096];
    uint64_t t0 = SDL_GetTicksNS();
    for (int i = 0; i < vc_npend;) {
        uint64_t lo = vc_tab[vc_pend[i]].lo, hi = vc_tab[vc_pend[i]].hi;
        int j = i + 1;
        for (; j < vc_npend && vc_tab[vc_pend[j]].lo <= hi + 64 * VC_PAGE; j++)
            if (vc_tab[vc_pend[j]].hi > hi) hi = vc_tab[vc_pend[j]].hi;
        uint64_t pg = VC_PAGE;
        int n = plat_written_pages(HEAP_BASE + lo, hi - lo, pages, 4096, &pg);
        prof_vc_queries++;
        if (n < 0 || n >= 4096) for (uint64_t p = lo / VC_PAGE; p < hi / VC_PAGE; p++) vc_page_gen[p] = g;  // (all)
        else for (int k = 0; k < n; k++) vc_page_gen[(pages[k] - HEAP_BASE) / VC_PAGE] = g;
        if (n > 0) prof_vc_scan_pages += (uint64_t)n;
        i = j;
    }
    prof_vc_scan_ns += SDL_GetTicksNS() - t0;
    VkCommandBuffer cb = vk_upload_cmd();
    for (int i = 0; i < vc_npend; i++) {
        VcEnt *e = &vc_tab[vc_pend[i]];
        VkBufferCopy cp[64];
        int nc = 0;
        for (uint64_t p = e->lo / VC_PAGE; p < e->hi / VC_PAGE; p++) {
            if (e->gen && vc_page_gen[p] <= e->gen) continue;
            uint64_t o = p * VC_PAGE;
            if (nc && cp[nc - 1].srcOffset + cp[nc - 1].size == o) { cp[nc - 1].size += VC_PAGE; continue; }
            if (nc == 64) { nc = 1; cp[0] = (VkBufferCopy){e->lo, 0, e->hi - e->lo}; break; }  // (scattered: the whole range)
            cp[nc++] = (VkBufferCopy){o, o - e->lo, VC_PAGE};
        }
        if (nc) {
            vkCmdCopyBuffer(cb, gmem_buf, e->buf, (uint32_t)nc, cp);
            for (int k = 0; k < nc; k++) prof_vc_copied += cp[k].size;
        }
        e->gen = g;
    }
    vc_npend = 0;
}

// Vertex / index data from the imported guest heap (renderer.m bind_guest reads g_guest_buffer the same way): the
// device-local cache above, else read in place - no per-draw copy, no index scan for the copy size. The game fences its
// CPU writes as on the Switch. Falls back to copies outside the heap or without the import; LARECOMP_VK_COPY_VERTICES=1
// forces copies.
static bool guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    static int copy = -1;
    if (copy < 0) copy = getenv("LARECOMP_VK_COPY_VERTICES") != NULL;
    if (copy || addr < HEAP_BASE) return false;
    if (addr + size > HEAP_BASE + gmem_size && kernel_heap_size() != gmem_tried) { gmem_tried = kernel_heap_size(); gmem_info(); }
    if (!gmem_buf || addr + size > HEAP_BASE + gmem_size) return false;
    if (gmem_alloc) nvn_heap_mirror_interest(addr, size);  // (heap mirror: vertex / index data read from it)
    if (vcache_lookup(addr, size, buf, off)) return true;
    *buf = gmem_buf;
    *off = addr - HEAP_BASE;
    return true;
}

static int g_cs_n;  // (compute program table below)

// In-place reboot (device.c backend_reset; GPU idle): drop what is keyed by guest addresses - programs by code
// address (the old Prog / Pipe objects are leaked, so pipeline keys holding their pointers can't collide), compute
// programs, shared copies - and the heap import: its pages are replaced (re-imported on first use).
void vk_exec_reset(void) {
    memset(prog_tab, 0, sizeof prog_tab);
    memset(pipe_tab, 0, sizeof pipe_tab);
    g_cs_n = 0;
    gcopy_gen++;
    if (gmem_buf) {
        vkDestroyBuffer(g_vk.dev, gmem_buf, NULL);
        vkFreeMemory(g_vk.dev, gmem_mem, NULL);
        gmem_buf = VK_NULL_HANDLE;
        gmem_mem = VK_NULL_HANDLE;
    }
    gmem_size = gmem_tried = 0;
    for (int i = 0; i < VC_TAB; i++)  // (the vertex cache: copies of the old pages)
        if (vc_tab[i].buf) vmaDestroyBuffer(g_vk.vma, vc_tab[i].buf, vc_tab[i].mem);
    memset(vc_tab, 0, sizeof vc_tab);
    vc_npend = 0;
    vc_free_dead(true);
    vc_bytes = 0;
    if (vc_page_gpu) memset(vc_page_gpu, 0, HEAP_MAX / VC_PAGE);
    vc_epoch++;
    st_defaults();
}

static void texture_info(int stage, ShaderInfo *si, int i, VkDescriptorImageInfo *out, float sc[2], uint32_t wh[2]) {
    ShaderTexture *st_ = &si->tex[i];
    int slot = ((int)st_->handle_offset - 0x20) / 8;  // 64-bit handles from offset 0x20
    uint64_t h = (slot >= 0 && slot < 32) ? st.tex[stage][slot] : 0;
    VkImageView view = VK_NULL_HANDLE;
    VkSampler smp = VK_NULL_HANDLE;
    uint32_t tic = h & 0xFFFFF, tsc = (uint32_t)(h >> 20) & 0xFFF;
    sc[0] = sc[1] = 1;
    wh[0] = wh[1] = 0;
    if (h && st.tpool && tic < (uint32_t)st.tpool->count) {
        NvnTexEntry *e = &((NvnTexEntry *)st.tpool->entries)[tic];
        if (e->tex) {
            // Per-frame memo: ~2400 lookups per overworld frame recomputed the view (format / range math, cache
            // probe). The first lookup of a frame still runs vk_view (upload check); a view that may have been
            // destroyed (eviction, image re-created) bumps vk_view_gen. LARECOMP_VK_NO_TEX_MEMO=1 disables.
            extern uint32_t vk_view_gen;
            extern uint64_t vk_frame_counter(void);
            static int nomemo = -1;
            if (nomemo < 0) nomemo = getenv("LARECOMP_VK_NO_TEX_MEMO") != NULL;
            static struct { NvnTexture *tex; uint64_t vh, frame; uint32_t gen; uint8_t type, has_view; VkImageView view; } memo[1024];
            uint64_t vh = e->has_view ? hash_words(0, &e->view, sizeof e->view) : 0, frame = vk_frame_counter();
            uint32_t ms = (uint32_t)((((uintptr_t)e->tex >> 4) ^ vh ^ (uint64_t)st_->type * 0x9E3779B97F4A7C15ull) & 1023);
            if (!nomemo && memo[ms].tex == e->tex && memo[ms].vh == vh && memo[ms].frame == frame && memo[ms].gen == vk_view_gen &&
                memo[ms].type == st_->type && memo[ms].has_view == e->has_view) {
                view = memo[ms].view;
            } else {
                view = vk_view(naliases ? alias_read(e->tex) : e->tex, &e->view, e->has_view, false, st_->type);
                memo[ms].tex = e->tex; memo[ms].vh = vh; memo[ms].frame = frame; memo[ms].gen = vk_view_gen;
                memo[ms].type = (uint8_t)st_->type; memo[ms].has_view = e->has_view; memo[ms].view = view;
            }
            renderer_texture_scale(e->tex, &sc[0], &sc[1]);
            wh[0] = e->tex->width; wh[1] = e->tex->height;
            for (int k = 0; k < nfeedback; k++)
                if (e->tex == feedback_tex[k] && (st_->type == TEXTYPE_2D || st_->type == TEXTYPE_2D_ARRAY)) {
                    view = feedback_view[k];
                    if (st_->type == TEXTYPE_2D_ARRAY) view = vk_dummy_view(TEXTYPE_2D_ARRAY);  // (2D snapshot as array: rare)
                }
        }
    }
    if (h && st.spool && tsc < (uint32_t)st.spool->count) smp = vk_sampler(&((NvnSamplerDesc *)st.spool->entries)[tsc]);
    if (!view) view = vk_dummy_view(st_->type);
    if (!smp) smp = vk_default_sampler(true);
    *out = (VkDescriptorImageInfo){smp, view, VK_IMAGE_LAYOUT_GENERAL};
}

// Writes the descriptor set of one stage. gx..gz: compute group counts (driver constants).
// pre_ii: texture descriptors already resolved by the caller (draw: fragment textures, needed earlier for the
// viewport decision), or NULL.
static float g_mirror_w;  // mirror mode's composite: its render target width (rscale[65].x; 0 otherwise)

static VkDescriptorSet stage_set(Prog *p, VkDescriptorSetLayout layout, uint32_t set, int stage, ShaderInfo *si, const float *rscale_row0,
                                 const float tint[8], uint32_t gx, uint32_t gy, uint32_t gz, const VkDescriptorImageInfo *pre_ii) {
    VkDescriptorSet ds = vk_alloc_set(layout);
    VkWriteDescriptorSet w[MAX_BIND];
    VkDescriptorBufferInfo bi[MAX_BIND];
    VkDescriptorImageInfo ii[MAX_BIND];
    int n = 0;
    int sg = stage == 1 ? 1 : 0;
    float rs[66][4] = {{1, 1, 0, 0}};  // ([65]: mirror mode, renderer.m)
    if (rscale_row0) memcpy(rs[0], rscale_row0, 16);
    rs[65][0] = g_mirror_w;
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b->binding >= 32) {
            int ti = (int)b->binding - 32;
            float sc[2]; uint32_t wh[2];
            if (ti < si->ntex && pre_ii && ti < 64) { ii[n] = pre_ii[ti]; sc[0] = tex_sc[sg][ti][0]; sc[1] = tex_sc[sg][ti][1]; wh[0] = tex_wh[sg][ti][0]; wh[1] = tex_wh[sg][ti][1]; }
            else if (ti < si->ntex) texture_info(stage, si, ti, &ii[n], sc, wh);
            else { ii[n] = (VkDescriptorImageInfo){vk_default_sampler(true), vk_dummy_view(TEXTYPE_2D), VK_IMAGE_LAYOUT_GENERAL}; sc[0] = sc[1] = 1; wh[0] = wh[1] = 0; }
            if (ti < 64) {
                tex_sc[sg][ti][0] = sc[0]; tex_sc[sg][ti][1] = sc[1];
                tex_wh[sg][ti][0] = wh[0]; tex_wh[sg][ti][1] = wh[1];
            }
            w[n].pImageInfo = &ii[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE && b->binding >= 32) {  // (Astral Chain) SUST targets
            int ti = (int)b->binding - 32;
            NvnTexture *t = ti < si->ntex ? shader_texture_object(stage, &si->tex[ti]) : NULL;
            VkImageView view = t ? vk_storage_view(t, si->tex[ti].type) : VK_NULL_HANDLE;
            if (!view) view = vk_dummy_storage_view(ti < si->ntex ? si->tex[ti].type : TEXTYPE_2D);
            ii[n] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
            w[n].pImageInfo = &ii[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && b->binding < 17) {
            bi[n] = cbuf_info(stage, si, (int)b->binding, gx, gy, gz);
            w[n].pBufferInfo = &bi[n];
        } else if (b->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            // Astral Chain heap mirror: the shader reaches guest memory through this stage's storage buffers.
            if (gmem_alloc && stage >= 0 && stage < 6)
                for (int k = 0; k < 16; k++) if (st.sbuf[stage][k].addr) nvn_heap_mirror_interest(st.sbuf[stage][k].addr, st.sbuf[stage][k].size);
            bi[n] = b->binding == GFAST_BUFFER_INDEX ? gfast_info() : gmem_info();
            w[n].pBufferInfo = &bi[n];
        } else {
            continue;  // rscale / tint: written below, once the textures (and their scales) are known
        }
        n++;
    }
    for (int i = 0; i < p->nbinds; i++) {
        VkBindingInfo *b = &p->binds[i];
        if (b->set != set || b->type != VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
            (b->binding != 18 && b->binding != 19 && b->binding != GMAP_BUFFER_INDEX)) continue;
        w[n] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding,
                                      .descriptorCount = 1, .descriptorType = b->type};
        if (b->binding == GMAP_BUFFER_INDEX) {  // (compute) this dispatch's fast-copy table: gfast_prepare
            bi[n] = ubo_span(vk_transient_copy(gmap_tab, sizeof gmap_tab, 256), sizeof gmap_tab);
        } else if (b->binding == 18) {
            for (int t = 0; t < si->ntex && t < 64; t++) { rs[1 + t][0] = tex_sc[sg][t][0]; rs[1 + t][1] = tex_sc[sg][t][1]; }
            bi[n] = ubo_span(copy_dedupe(sg ? DEDUPE_RSCALE_F : DEDUPE_RSCALE_V, rs, sizeof rs, 256), sizeof rs);
        } else {
            float zero[8] = {0};
            bi[n] = ubo_span(copy_dedupe(DEDUPE_TINT, tint ? tint : zero, 32, 256), 32);
        }
        w[n].pBufferInfo = &bi[n];
        n++;
    }
    if (n) vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    return ds;
}

// ================================================================== compute
typedef struct { Prog p; ShaderInfo *si; VkPipeline pipe; uint32_t local[3]; } CsProg;

static void build_compute(void *arg) {
    CsProg *cp = arg;
    size_t n = 0;
    uint32_t *spv = vk_glsl_to_spirv(cp->si->msl, STAGE_COMPUTE, "cs", &n);
    if (!spv) { atomic_store(&cp->p.state, 2); return; }
    cp->p.nbinds = vk_reflect(spv, n, cp->p.binds, MAX_BIND);
    VkShaderModule m = vk_module(spv, n);
    free(spv);
    cp->p.layout = make_layout(&cp->p, 1, VK_SHADER_STAGE_COMPUTE_BIT, 0);
    VkSpecializationMapEntry me[3] = {{0, 0, 4}, {1, 4, 4}, {2, 8, 4}};
    VkSpecializationInfo spec = {3, me, 12, cp->local};
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main",
                                                .pSpecializationInfo = &spec},
                                      .layout = cp->p.layout};
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &cp->pipe);
    atomic_fetch_add(&stat_compiles, 1); atomic_fetch_add(&vk_pipelines_built, 1);
    atomic_store(&cp->p.state, r == VK_SUCCESS ? 1 : 2);
    if (r != VK_SUCCESS) LOG("[compute] pipeline failed: %d\n", r);
}

static struct CsEnt { uint64_t addr; CsProg *cp; } g_cs_tab[256];  // compute programs by code address

static CsProg *compute_program(NvnProgram *np) {
    NvnShaderStage *ss = &np->stages[5];
    if (!ss->code_addr) return NULL;
    struct CsEnt *tab = g_cs_tab;
    int n = g_cs_n;
    for (int i = 0; i < n; i++) if (tab[i].addr == ss->code_addr) return tab[i].cp;
    CsProg *cp = calloc(1, sizeof *cp);
    if (n < 256) { tab[n].addr = ss->code_addr; tab[n].cp = cp; g_cs_n = n + 1; }
    maxwell_gmem_base = HEAP_BASE;
    maxwell_gmem_size = HEAP_MAX;
    ShaderKey k = {0};
    k.glsl = 1;
    pthread_mutex_lock(&translate_lock);
    save_shader_input(ss->code_addr, ss->const_offset ? ss->const_offset : 0x10000, &k, "cs");
    cp->si = maxwell_translate((const uint8_t *)(uintptr_t)ss->code_addr, ss->const_offset ? ss->const_offset : 0x10000, STAGE_COMPUTE, &k);
    pthread_mutex_unlock(&translate_lock);
    cp->p.vi = cp->si;
    if (!cp->si->msl) {
        LOG("[compute] translate failed: %s (code %llx)\n", cp->si->error, (unsigned long long)ss->code_addr);
        atomic_store(&cp->p.state, 2);
        return cp;
    }
    for (int i = 0; i < 3; i++) {
        cp->local[i] = ss->control ? *(const uint32_t *)(ss->control + 0x748 + 4 * i) : 1;
        if (!cp->local[i]) cp->local[i] = 1;
    }
    save_glsl(cp->si->msl, ss->code_addr, "cs");
    run_job(build_compute, cp);
    return cp;
}
static void warm_compute(NvnProgram *np) { compute_program(np); }

// A draw or dispatch whose program / pipeline is still building waits for it (bounded) instead of being skipped.
// Skipped work is lost for good when it runs once: the game bakes textures (the player character's face after a
// hairstyle change: blocky dark patches stayed in it) and sets up effects that way, and a first session with an
// empty shader cache skipped ~10000 draws. The wait costs a stall the first time a shader is met (the SPIR-V cache
// and the driver's pipeline cache keep later sessions short). LARECOMP_SKIP_UNBUILT=1: skip as before.
static bool skip_unbuilt(void) {
    static int skip = -1;
    if (skip < 0) skip = getenv("LARECOMP_SKIP_UNBUILT") != NULL;
    return skip;
}
// Waits poll without sleeping for the first 20 ms: a build takes ~0.1-2 ms, and Sleep(1) (1-2 ms each) made the
// 78 new pipelines of a cutscene's first frame cost 148 ms of waiting for ~20 ms of building.
static void wait_built(_Atomic int *state) {
    if (skip_unbuilt() || atomic_load(state) != 0) return;
    uint64_t t0 = SDL_GetTicksNS(), now = t0;
    while (atomic_load(state) == 0 && now - t0 < 3000000000ull) {
        if (now - t0 < 20000000ull) sched_yield(); else SDL_Delay(1);
        now = SDL_GetTicksNS();
    }
    cum_built_ns += SDL_GetTicksNS() - t0;
}

static void dispatch_compute(uint32_t gx, uint32_t gy, uint32_t gz) {
    snap_reason = "compute"; snapshots_invalidate(NULL);
    if (!st.program || !gx || !gy || !gz) return;
    CsProg *cp = compute_program(st.program);
    if (cp) wait_built(&cp->p.state);
    if (!cp || atomic_load(&cp->p.state) != 1) { SKIPPED(); return; }
    {   // LARECOMP_SKIP_CS=<code address hex> (debug): skip that compute shader's dispatches, to see what it does
        static uint64_t skip = ~0ull;
        if (skip == ~0ull) { const char *e = getenv("LARECOMP_SKIP_CS"); skip = e ? strtoull(e, NULL, 16) : 0; }
        if (skip && st.program->stages[5].code_addr == skip) { SKIPPED(); return; }
    }
    if (cp->si->uses_gmem && getenv("LARECOMP_LOG_CS_SBUF")) {  // (debug) the storage buffers bound to gmem dispatches
        static uint64_t seen_cs[16];
        static int seen_n[16];
        int k = 0;
        while (k < 15 && seen_cs[k] && seen_cs[k] != st.program->stages[5].code_addr) k++;
        seen_cs[k] = st.program->stages[5].code_addr;
        if (seen_n[k]++ < 24) {
            LOG("[cs] %llx %ux%ux%u:", (unsigned long long)st.program->stages[5].code_addr, gx, gy, gz);
            for (int i = 0; i < 16; i++)
                if (st.sbuf[5][i].addr) LOG(" sb%d %llx+%llx", i, (unsigned long long)st.sbuf[5][i].addr, (unsigned long long)st.sbuf[5][i].size);
            LOG("\n");
        }
    }
    vk_end_pass();
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    gp_mark(cb, "dispatch %ux%ux%u (local %ux%ux%u) cs %llx gmem %d", gx, gy, gz, cp->local[0], cp->local[1], cp->local[2],
            (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    dl_note("dispatch %ux%ux%u cs %llx gmem %d", gx, gy, gz, (unsigned long long)st.program->stages[5].code_addr, cp->si->uses_gmem);
    nfeedback = 0;
    if (ct_on()) {
        LOG("[ct] dispatch cs %llx %ux%ux%u | tex", (unsigned long long)st.program->stages[5].code_addr, gx, gy, gz);
        for (int i = 0; i < cp->si->ntex; i++) {
            NvnTexture *t = shader_texture_object(5, &cp->si->tex[i]);
            if (t) LOG(" %s%p/%#x", cp->si->tex[i].is_image ? "I:" : "", (void *)t, t->format);
        }
        LOG("\n");
    }
    if (cp->si->uses_gmem_map) gfast_prepare(cb);
    VkDescriptorSet ds = stage_set(&cp->p, cp->p.set_layout[0], 0, 5, cp->si, NULL, NULL, gx, gy, gz, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p.layout, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, gx, gy, gz);
    if (cp->si->uses_gmem_map && cp->si->uses_gmem_store) gfast_finish(cb);
    if (cp->si->uses_gmem_store) vc_mark_sbufs(5);
    stat_dispatches++;
}

// ================================================================== draws
static void debug_draw(Prog *pr, CmdRec *r, bool indexed);

// Quads / fans / polygons as a 32-bit triangle index list.
static uint32_t *expand_indices(uint32_t mode, uint32_t count, const void *indices, int itype, uint32_t first, uint32_t *out_count) {
    uint32_t tris = mode == 7 ? count / 4 * 2 : count >= 3 ? count - 2 : 0;
    uint32_t *out = malloc(tris * 3 * 4 + 4);
    uint32_t n = 0;
#define IDX(i) (indices ? (itype == 0 ? ((const uint8_t *)indices)[i] : itype == 1 ? ((const uint16_t *)indices)[i] : ((const uint32_t *)indices)[i]) : first + (i))
    if (mode == 7) {
        for (uint32_t q = 0; q + 3 < count; q += 4) {
            out[n++] = IDX(q); out[n++] = IDX(q + 1); out[n++] = IDX(q + 2);
            out[n++] = IDX(q); out[n++] = IDX(q + 2); out[n++] = IDX(q + 3);
        }
    } else {
        for (uint32_t i = 1; i + 1 < count; i++) { out[n++] = IDX(0); out[n++] = IDX(i); out[n++] = IDX(i + 1); }
    }
#undef IDX
    *out_count = n;
    return out;
}

static bool topology(uint32_t mode, VkPrimitiveTopology *t, uint8_t *cls) {
    switch (mode) {
    case 0: *t = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; *cls = 0; return true;
    case 1: *t = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; *cls = 1; return true;
    case 3: *t = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; *cls = 1; return true;
    case 4: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return true;
    case 5: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; *cls = 2; return true;
    default: *t = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; *cls = 2; return false;
    }
}

static void draw_(CmdRec *r, bool indexed);
static void draw(CmdRec *r, bool indexed) {
    PROF_T0();
    draw_(r, indexed);
    PROF_ADD(prof_draw);
}
static uint64_t prof_tp_;
static uint16_t half_bits(float f) {  // (round to nearest)
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u, m = x & 0x7FFFFFu;
    int e = (int)((x >> 23) & 0xFF) - 112;
    if (((x >> 23) & 0xFF) == 0xFF) return (uint16_t)(sign | 0x7C00u | (m ? 0x200u : 0));
    if (e >= 31) return (uint16_t)(sign | 0x7C00u);
    if (e <= 0) {
        if (e < -10) return (uint16_t)sign;
        m |= 0x800000u;
        uint32_t sh = (uint32_t)(14 - e), h = m >> sh;
        return (uint16_t)(sign | (h + ((m >> (sh - 1)) & 1)));
    }
    return (uint16_t)((sign | ((uint32_t)e << 10) | (m >> 13)) + ((m >> 12) & 1));
}
static void draw_(CmdRec *r, bool indexed) {
    if (prof) prof_tp_ = SDL_GetTicksNS();
    if (!st.program) { SKIPPED(); return; }
    ShaderKey vkey = {0}, fkey = {0};
    vkey.glsl = fkey.glsl = 1;
    vkey.flip_y = fkey.flip_y = nvn_window_origin() == 0;
    vkey.depth_minus_one = nvn_depth_mode() == 0;
    for (int i = 0; i < st.nattrib; i++) vkey.attr_kind[i] = (uint8_t)attr_kind(st.attrib[i] & 0xFF);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) fkey.rt_kind[i] = (uint8_t)rt_kind(st.rt.color[i]->format);
    if (st.color_state[2] && st.color_state[2] < 8) {  // alpha test: done by the fragment shader (no Vulkan equivalent)
        fkey.alpha_func = st.color_state[2];
        fkey.alpha_ref = half_bits(st.alpha_ref);
    }
    Prog *pr = get_program(st.program, &vkey, &fkey);
    if (pr) wait_built(&pr->state);
    if (!pr || atomic_load(&pr->state) != 1) { SKIPPED(); return; }
    g_mirror_w = 0;
    {   // mirror mode (runtime/hle/mirror.c; as renderer.m): the scene's composite into the window image is mirrored
        extern bool mirror_draw(bool composite);
        bool composite = false;
        if (st.rt.ncolor && st.rt.color[0] && renderer_texture_presented(st.rt.color[0]) && !(st.color_state[0] & 1) &&
            r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
            st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height && pr->fi->ntex) {
            NvnTexture *t0 = shader_texture_object(1, &pr->fi->tex[0]);
            composite = t0 && t0->width == st.rt.color[0]->width && t0->height == st.rt.color[0]->height;
        }
        if (mirror_draw(composite)) {
            vkey.flip_x = fkey.flip_x = true;
            pr = get_program(st.program, &vkey, &fkey);
            if (pr) wait_built(&pr->state);
    if (!pr || atomic_load(&pr->state) != 1) { SKIPPED(); return; }
            g_mirror_w = (float)st.rt.color[0]->width;
        }
    }
    {   // fragment-shader hash: interior backdrop detection (widescreen), skip lists, item twinkle (renderer.m)
        static uint64_t last_fs, last_hash;
        uint64_t fs = (st.program->stage_mask & 2) ? st.program->stages[1].code_addr : 0;
        if (fs != last_fs) {
            last_fs = fs;
            last_hash = fs ? hmix(1469598103934665603ull, (const void *)(uintptr_t)(fs + 0x30), 0x200) : 0;
        }
        static uint64_t skip[16];
        static int nskip = -1;
        if (nskip < 0) {
            nskip = 0;
            for (const char *p = getenv("LARECOMP_SKIP_FSHASH"); p && *p && nskip < 16;) {
                skip[nskip++] = strtoull(p, (char **)&p, 16);
                while (*p == ',') p++;
            }
        }
        for (int i = 0; i < nskip; i++)  // (either hash: this one, or the programs.bin / [draw] log one)
            if (last_hash == skip[i] || stage_hash(st.program, 1) == skip[i]) { SKIPPED(); return; }
        extern bool pot_level_active(void);
        if (last_hash == 0xa2177dbb72197bd9ull && pot_level_active()) { SKIPPED(); return; }
        if (g_debug_frame && getenv("LARECOMP_LOG_FSHASH")) LOG("[fshash] fs=%llx hash %016llx\n", (unsigned long long)fs, (unsigned long long)last_hash);
        g_cur_fs_hash = last_hash;
        extern void widescreen_backdrop_seen(uint64_t hash);
        if (last_hash) widescreen_backdrop_seen(last_hash);
        extern void widescreen_scene_ubo(const float *u, uint64_t size);  // (the camera's view-projection)
        if (st.ubuf[0][5].addr) widescreen_scene_ubo((const float *)(uintptr_t)st.ubuf[0][5].addr, st.ubuf[0][5].size);
    }
    if (g_debug_frame) debug_draw(pr, r, indexed);
    {   // LARECOMP_WATCH_FS=<fragment stage hash>: log that shader's draws (blend, constants) in every frame
        static uint64_t watch = 1;
        static int nwatch;
        if (watch == 1) { const char *e = getenv("LARECOMP_WATCH_FS"); watch = e ? strtoull(e, NULL, 16) : 0; }
        if (watch && nwatch < 4000 && stage_hash(st.program, 1) == watch) {
            nwatch++;
            extern volatile int g_debug_frame_request;  // LARECOMP_WATCH_DEBUG=<n>: debug-dump the frame of the n-th draw
            const char *wd = getenv("LARECOMP_WATCH_DEBUG");
            if (wd && nwatch == atoi(wd)) g_debug_frame_request = 1;
            LOG("[watch] alpha test in key %u, shader %s\n", pr->fkey.alpha_func,
                pr->fi && pr->fi->msl && strstr(pr->fi->msl, ")) discard;") ? "tests" : "does not test");
            LOG("[watch] frame %llu count %llu blend %#x [%u %u %u %u %u %u %u %u] a2c %u alpha test %u ref %g ds %02x%02x%02x |",
                (unsigned long long)renderer_present_count(), (unsigned long long)r->x[2], st.color_state[0], st.blend[0][0],
                st.blend[0][1], st.blend[0][2], st.blend[0][3], st.blend[0][4], st.blend[0][5], st.blend[0][6], st.blend[0][7],
                st.ms[0xC], st.color_state[2], st.alpha_ref, st.ds[0], st.ds[1], st.ds[2]);
            for (int s = 0; s < 2; s++) {  // shader cbuf k = binding k - 3 (cbuf 0: driver, 1: compiler constants)
                uint32_t mask = s ? pr->fi->cbuf_mask : pr->vi->cbuf_mask;
                for (int k = 3; k < 21; k++) {
                    if (!((mask >> k) & 1)) continue;
                    Range u = st.ubuf[s][k - 3];
                    LOG(" | %s cb%d", s ? "fs" : "vs", k);
                    if (!u.addr) { LOG(" unbound"); continue; }
                    for (int i = 0; i < 24 && i * 4 < (int)u.size; i++) LOG(" %.4g", ((const float *)(uintptr_t)u.addr)[i]);
                }
            }
            LOG("\n");
        }
    }
    if (ct_on()) ct_draw(pr, "draw");
    alias_draw_check();
    resolve_feedback(pr);
    snap_reason = "draw-depthwrite";
    if (st.rt.depth && st.ds[0] && st.ds[1]) snapshots_invalidate(st.rt.depth);
    snap_reason = "draw-colorwrite";
    for (int i = 0; i < st.rt.ncolor; i++)
        if (st.rt.color[i] && ((st.channel_mask >> (i * 4)) & 0xF)) snapshots_invalidate(st.rt.color[i]);
    {   // LARECOMP_DEBUG_EACH=1 (debug frame): end the pass before each draw, so each draw's result is its own image
        static int each = -1;
        if (each < 0) each = getenv("LARECOMP_DEBUG_EACH") != NULL;
        if (g_debug_frame && each && in_pass) { LOG("[draw]   (state before this draw: pass image %d)\n", debug_pass_no); vk_end_pass(); }
    }
    begin_pass();
    VkCommandBuffer cb = vk_cmd();

    uint32_t mode = (uint32_t)r->x[0];
    VkPrimitiveTopology topo;
    uint8_t cls;
    bool native = topology(mode, &topo, &cls);
    if (!native && mode != 6 && mode != 7 && mode != 9) { SKIPPED(); return; }

    uint64_t prof_tk_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_pre += prof_tk_ - prof_tp_;
    // ---- pipeline
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.prog = pr;
    k.ncolor = pass_ncolor;
    memcpy(k.color, pass_color, sizeof k.color);
    k.depth = pass_depth;
    k.stencil = pass_stencil;
    k.samples = pass_samples;
    for (int i = 0; i < 32; i++) {
        if (!(pr->vi->attr_mask & (1u << i))) continue;
        k.attr_mask |= 1u << i;
        uint32_t a = i < st.nattrib ? st.attrib[i] : 0x2E;
        uint32_t fmt = a & 0xFF, stream = (a >> 8) & 0x1F, off = a >> 13;
        if (i >= st.nattrib || fmt == 0 || stream >= 16) {  // unbound attribute: (0, 0, 0, 1) from a constant stream
            k.attr_fmt[i] = VK_FORMAT_R32G32B32A32_SFLOAT;
            k.attr_bind[i] = ZERO_BINDING;
            k.used_streams |= 1u << ZERO_BINDING;
            continue;
        }
        k.attr_fmt[i] = vertex_format_supported(vertex_format(fmt));
        k.attr_off[i] = off;
        k.attr_bind[i] = stream;
        k.used_streams |= 1u << stream;
    }
    for (int s = 0; s < 16; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        k.stride[s] = s < st.nstream ? st.stream[s][0] : 0;
        k.divisor[s] = s < st.nstream && k.stride[s] ? st.stream[s][1] : 0;
        if (k.divisor[s] > 1 && !g_vk.divisor) k.divisor[s] = 1;
    }
    for (uint32_t i = 0; i < k.ncolor && i < 8; i++) {
        if (!st.rt.color[i] || k.color[i] == VK_FORMAT_UNDEFINED) continue;
        // Targets the fragment shader doesn't output keep their contents on Metal, but are undefined in Vulkan
        // (NVIDIA writes black): mask them. Foliage depth pre-passes left black silhouettes (user 2026-10-08).
        k.write_mask[i] = ((pr->fi->out_mask >> (i * 4)) & 0xF) ? (st.channel_mask >> (i * 4)) & 0xF : 0;
        if (((st.color_state[0] >> i) & 1) && !rt_kind(st.rt.color[i]->format)) {
            k.blend_on |= (uint8_t)(1u << i);
            memcpy(k.blend[i], st.blend[i], 8);
        }
    }
    k.a2c = st.ms[0xC] != 0;
    k.topo_class = cls;
    k.poly_fill = st.poly[2] != 1;
    uint64_t ph = hash_words(0xcbf29ce484222325ull, &k, sizeof k);
    if (!ph) ph = 1;
    uint32_t pslot = (uint32_t)(ph ^ (ph >> 29)) & (PIPE_TAB - 1);
    while (pipe_tab[pslot].key && pipe_tab[pslot].key != ph) pslot = (pslot + 1) & (PIPE_TAB - 1);
    Pipe *pp = pipe_tab[pslot].pipe;
    if (!pp) {
        pp = calloc(1, sizeof *pp);
        pp->k = k;
        pp->claimed = 1;
        pipe_tab[pslot].key = ph;
        pipe_tab[pslot].pipe = pp;
        pipe_tab_used++;
        pipe_record(pr, &k);
        // the draw needs it now: linked from shader libraries, or built here rather than queued for a worker and
        // waited for (see wait_built)
        if (gpl_link(pp)) {}
        else if (skip_unbuilt()) run_job(build_pipeline, pp);
        else build_pipeline(pp);
    } else if (pp->warmed && !pp->drawn) {
        pp->drawn = true;
        if (atomic_load(&pp->state) == 0) pwarm_late++; else pwarm_ready++;
        // (a warm-up build not started yet: linked or built here, as for a new pipeline)
        if (atomic_load(&pp->state) == 0 && !skip_unbuilt() && !atomic_exchange(&pp->claimed, 1) && !gpl_link(pp)) build_pipeline(pp);
    }
    wait_built(&pp->state);
    if (atomic_load(&pp->state) != 1) { SKIPPED(); return; }
    ss_enabled();
    VkPipeline use = (VkPipeline)(uintptr_t)atomic_load(&pp->opt);
    if (!use) use = pp->pipe;
    if (ss_need(SS_PIPE, ss.pipe == use)) { ss.pipe = use; vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, use); }
    uint64_t prof_td_ = prof ? SDL_GetTicksNS() : 0;
    if (prof) prof_key += prof_td_ - prof_tk_;

    // ---- depth / stencil (dynamic)
    static int nodt = -1;
    if (nodt < 0) nodt = getenv("LARECOMP_NO_DEPTH_TEST") != NULL;
    bool dt = st.ds[0] && !nodt && pass_depth;
    if (ss_need(SS_DT, ss.dt == (VkBool32)dt)) { ss.dt = dt; vkCmdSetDepthTestEnable(cb, dt); }
    VkBool32 dw = dt && st.ds[1];
    if (ss_need(SS_DW, ss.dw == dw)) { ss.dw = dw; vkCmdSetDepthWriteEnable(cb, dw); }
    VkCompareOp dop = dt ? compare_fn(st.ds[2]) : VK_COMPARE_OP_ALWAYS;
    // EQUAL against a depth pre-pass written by a different vertex shader (foliage, trees) needs both shaders to land
    // on identical depths. They do since the translator stopped using unpackHalf2x16 (NVIDIA ran the half math after
    // it in fp16, differently per shader: black slashes in foliage). LARECOMP_VK_EQUAL_TOL=1 brings back the old
    // workaround: LESS_OR_EQUAL with a few ulps of bias towards the camera, which also let walls paint over the
    // plants stuck flat on them (beach) - so it is off.
    static int tol = -1;
    if (tol < 0) tol = getenv("LARECOMP_VK_EQUAL_TOL") != NULL;
    bool equal_tol = dop == VK_COMPARE_OP_EQUAL && tol;
    if (equal_tol) dop = VK_COMPARE_OP_LESS_OR_EQUAL;
    if (ss_need(SS_DOP, ss.dop == dop)) { ss.dop = dop; vkCmdSetDepthCompareOp(cb, dop); }
    bool sten = st.ds[3] && pass_stencil;
    if (ss_need(SS_STEN, ss.sten == (VkBool32)sten)) { ss.sten = sten; vkCmdSetStencilTestEnable(cb, sten); }
    if (sten) {
        uint32_t ops = st.ds[5] | st.ds[6] << 8 | st.ds[7] << 16;
        for (int f = 0; f < 2; f++) {
            uint32_t o = f ? ops >> 12 : ops;
            VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
            uint32_t v[7] = {stencil_op(o & 0xF), stencil_op((o >> 8) & 0xF), stencil_op((o >> 4) & 0xF),
                             compare_fn(f ? st.ds[4] >> 4 : st.ds[4] & 0xF), st.stencil_vmask[f] & 0xFF, st.stencil_mask[f] & 0xFF,
                             st.stencil_ref[f]};
            if (!ss_need(f ? SS_SFACE1 : SS_SFACE0, !memcmp(v, ss.sface[f], sizeof v))) continue;
            memcpy(ss.sface[f], v, sizeof v);
            vkCmdSetStencilOp(cb, face, (VkStencilOp)v[0], (VkStencilOp)v[1], (VkStencilOp)v[2], (VkCompareOp)v[3]);
            vkCmdSetStencilCompareMask(cb, face, v[4]);
            vkCmdSetStencilWriteMask(cb, face, v[5]);
            vkCmdSetStencilReference(cb, face, v[6]);
        }
    }
    if (ss_need(SS_BC, !memcmp(ss.bc, st.blend_color, sizeof ss.bc))) { memcpy(ss.bc, st.blend_color, sizeof ss.bc); vkCmdSetBlendConstants(cb, st.blend_color); }

    // ---- rasterizer
    uint8_t cull = st.poly[0];
    if (vkey.flip_x) cull = 0;  // (mirror mode's composite: one full-screen quad, either winding)
    VkCullModeFlags cm = cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    if (ss_need(SS_CULL, ss.cull == cm)) { ss.cull = cm; vkCmdSetCullMode(cb, cm); }
    bool ccw = st.poly[1] == 1;
    static int flipw = -1;  // LARECOMP_VK_FLIP_WINDING: invert the front-face mapping (A/B against Metal)
    if (flipw < 0) flipw = getenv("LARECOMP_VK_FLIP_WINDING") != NULL;
    bool front_ccw = (ccw != vkey.flip_y) != (flipw != 0);
    VkFrontFace ff = front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    if (ss_need(SS_FF, ss.ff == ff)) { ss.ff = ff; vkCmdSetFrontFace(cb, ff); }
    VkPrimitiveTopology tp = native ? topo : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    if (ss_need(SS_TOPO, ss.topo == tp)) { ss.topo = tp; vkCmdSetPrimitiveTopology(cb, tp); }
    VkBool32 pre = st.prim_restart && native && (mode == 3 || mode == 5);
    if (ss_need(SS_PRE, ss.pre == pre)) { ss.pre = pre; vkCmdSetPrimitiveRestartEnable(cb, pre); }
    {
        float f[3] = {0, 0, 0};
        static int off = -1;
        if (off < 0) off = getenv("LARECOMP_NO_DEPTH_BIAS") != NULL;
        bool on = (st.poly[3] & 4) && !off;
        if (on) memcpy(f, st.poly_offset, sizeof f);
        if (equal_tol) { f[1] -= 8; on = true; }  // (see the EQUAL comment above: a few depth ulps nearer)
        if (ss_need(SS_DBE, ss.dbe == (VkBool32)on)) { ss.dbe = on; vkCmdSetDepthBiasEnable(cb, on); }
        if (ss_need(SS_DB, !memcmp(ss.db, f, sizeof f))) { memcpy(ss.db, f, sizeof f); vkCmdSetDepthBias(cb, f[1], f[2], f[0]); }
    }

    // ---- textures first (their scales feed the widescreen decision and the rscale table), then the viewport
    float *vp = st.viewport[0];
    float zn = st.depth_range[0][0], zf = st.depth_range[0][1];
    vp_sx = pass_sx; vp_sy = pass_sy; vp_ox = 0; vp_oy = 0;
    {   // the Items page's cursor brackets while the cursor is on the companion card (runtime/hle/pets.c)
        extern bool pets_hide_item_cursor(void);
        extern bool texpatch_is_item_cursor(int id);
        if (pets_hide_item_cursor())
            for (int i = 0; i < pr->fi->ntex; i++) {
                NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
                const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
                if (b && b->tp > 0 && texpatch_is_item_cursor(b->tp - 1)) { SKIPPED(); return; }
            }
    }
    float tint[8] = {0};
    if (pr->fi->uses_tint) {
        extern bool pot_tint(const float *m, float tint[8], uint64_t mesh);
        extern bool pot_glow_active(void);
        if (st.ubuf[0][2].addr && st.ubuf[0][2].size >= 48 && pot_glow_active())
            pot_tint((const float *)(uintptr_t)st.ubuf[0][2].addr, tint, st.vbuf[0].addr);
        // the gold Escape Bottle (runtime/hle/escape.c): draws sampling the bottle's item icon / model map
        // and the HUD hearts (heart colour, Cosmetics tab)
        extern int texpatch_bottle_kind(int id);
        extern int texpatch_heart_kind(int id);
        extern void texpatch_heart_tint(int kind, float tint[8]);
        int bottle = 0, heart = 0;
        for (int i = 0; i < pr->fi->ntex; i++) {
            NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
            const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
            if (b && b->tp > 0 && !bottle) bottle = texpatch_bottle_kind(b->tp - 1);
            if (b && b->tp > 0 && !heart) heart = texpatch_heart_kind(b->tp - 1);
        }
        if (heart) texpatch_heart_tint(heart, tint);
        if (bottle) {
            extern void escape_bottle_tint(int kind, const float *ui, uint32_t ui_size, float tint[8]);
            int u = bottle == 3 ? 3 : 0;  // (the fairy model: its bone matrices, for where it stands - souls.c)
            escape_bottle_tint(bottle, (const float *)(uintptr_t)st.ubuf[0][u].addr, (uint32_t)st.ubuf[0][u].size, tint);
        }
    }
    // fragment textures are bound before deciding the viewport (tex_sc / tex_wh), rscale depends on the decision:
    // build the vertex set now and the fragment set after the decision (stage_set fills tex_sc for stage 1).
    if (prof) prof_dyn += SDL_GetTicksNS() - prof_td_;
    PROF_T0();
    VkDescriptorSet vset = stage_set(pr, pr->set_layout[0], 0, 0, pr->vi, NULL, NULL, 0, 0, 0, NULL);
    PROF_ADD(prof_sets);
    VkDescriptorImageInfo fii[64];
    uint64_t prof_tx_ = prof ? SDL_GetTicksNS() : 0;
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {
        float sc[2]; uint32_t wh[2];
        texture_info(1, pr->fi, i, &fii[i], sc, wh);
        tex_sc[1][i][0] = sc[0]; tex_sc[1][i][1] = sc[1]; tex_wh[1][i][0] = wh[0]; tex_wh[1][i][1] = wh[1];
    }
    if (prof) prof_tex += SDL_GetTicksNS() - prof_tx_;
    if (fabsf(pass_sx - pass_sy) > 0.01f * pass_sy && st.rt.ncolor && renderer_texture_presented(st.rt.color[0])) {
        // Draws into the swapchain image (see renderer.m): composites fill the window, UI goes in a centred 16:9 box.
        bool reads_scene = false;
        for (int i = 0; i < pr->fi->ntex && i < 64; i++)
            if ((tex_sc[1][i][0] != 1 || tex_sc[1][i][1] != 1) && tex_wh[1][i][0] == st.rt.color[0]->width &&
                tex_wh[1][i][1] == st.rt.color[0]->height)
                reads_scene = true;
        bool fullscreen_quad = r->x[2] <= 6 && st.viewport[0][0] == 0 && st.viewport[0][1] == 0 &&
                               st.viewport[0][2] >= st.rt.color[0]->width && st.viewport[0][3] >= st.rt.color[0]->height;
        bool fullscreen_fill = pr->fi->ntex == 0 && fullscreen_quad;
        if (fullscreen_quad && !reads_scene && pr->fi->ntex == 1 && (tex_sc[1][0][0] != 1 || tex_sc[1][0][1] != 1)) reads_scene = true;
        static uint64_t wide_frame = ~0ull;
        if (reads_scene) wide_frame = renderer_present_count();
        bool fade_fill = false;
        if (fullscreen_fill && st.ubuf[1][0].addr && st.ubuf[1][0].size >= 12) {
            const float *c = (const float *)(uintptr_t)st.ubuf[1][0].addr;
            bool black = c[0] < 0.01f && c[1] < 0.01f && c[2] < 0.01f, white = c[0] > 0.99f && c[1] > 0.99f && c[2] > 0.99f;
            fade_fill = black || white;
        }
        bool composite = reads_scene || fade_fill || (fullscreen_fill && wide_frame == renderer_present_count());
        if (!composite) wide_frame = ~0ull;
        if (!composite) {
            int pw, ph;
            pass_size(&pw, &ph);
            float u = fminf(pass_sx, pass_sy);
            vp_sx = vp_sy = u;
            vp_ox = (pw - st.rt.color[0]->width * u) * 0.5f;
            vp_oy = (ph - st.rt.color[0]->height * u) * 0.5f;
        }
    }
    {   // camera-attached effects sized for 16:9 are widened with the view (runtime/hle/widescreen.c)
        extern float widescreen_effect_stretch(uint64_t fs_hash);
        float k = widescreen_effect_stretch(g_cur_fs_hash);
        float vx = vp[0] * vp_sx + vp_ox, vw = vp[2] * vp_sx;
        if (k > 1.0f) { vx -= vw * (k - 1) / 2; vw *= k; }
        set_viewport(vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, zn, zf);
        if (g_debug_frame) {
            VkRect2D sr = scissor_rect();
            LOG("[draw]   vp %.2f %.2f %.2f %.2f (stretch %.3f, fs hash %016llx) scissor %d %d %u %u | guest sc %d %d %d %d | pass %ux%u s %.4f %.4f\n",
                vx, vp[1] * vp_sy + vp_oy, vw, vp[3] * vp_sy, k, (unsigned long long)g_cur_fs_hash, sr.offset.x, sr.offset.y,
                sr.extent.width, sr.extent.height, st.scissor[0][0], st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], pass_w, pass_h,
                vp_sx, vp_sy);
        }
    }
    apply_scissor();
    float rs0[4] = {vp_sx, vp_sy, vp_ox, vp_oy};
    uint64_t prof_t1_ = prof ? SDL_GetTicksNS() : 0;
    VkDescriptorSet fset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, tint, 0, 0, 0, fii);
    if (prof) prof_sets += SDL_GetTicksNS() - prof_t1_;
    VkDescriptorSet sets[2] = {vset, fset};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 0, 2, sets, 0, NULL);  // (always new sets)
    if (pr->vi->uses_gmem_store) vc_mark_sbufs(0);
    if (pr->fi->uses_gmem_store) vc_mark_sbufs(1);

    uint64_t prof_t2_ = prof ? SDL_GetTicksNS() : 0;
    // ---- indices (copied; the largest index bounds the vertex data copied below)
    uint32_t first = 0, count = (uint32_t)r->x[2], inst, base_inst, max_index = 0;
    int32_t base_vertex = 0;
    VkSpan ib = {0};
    VkIndexType itype_vk = VK_INDEX_TYPE_UINT32;
    uint32_t icount = 0;
    bool use_index = indexed || !native;
    const void *scan_src = NULL;  // deferred index scan (direct index buffer)
    uint32_t scan_isz = 0, scan_restart = 0;
    if (!indexed) {
        first = (uint32_t)r->x[1]; base_inst = (uint32_t)r->x[3]; inst = (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 1;
        max_index = first + count;
        if (!native) {
            uint32_t *e = expand_indices(mode, count, NULL, 0, first, &icount);
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
    } else {
        uint32_t itype = (uint32_t)r->x[1];
        uint64_t iaddr = r->x[3];
        base_vertex = (int32_t)r->x[4];
        base_inst = (uint32_t)r->x[5];
        inst = (uint32_t)r->x[6] ? (uint32_t)r->x[6] : 1;
        const void *src = (const void *)(uintptr_t)iaddr;
        uint32_t restart = st.prim_restart ? st.restart_index : 0xFFFFFFFFu;
        if (native && itype != 0) {
            uint32_t isz = itype == 1 ? 2 : 4;
            itype_vk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
            icount = count;
            if (iaddr % isz == 0 && guest_direct(iaddr, (uint64_t)count * isz, &ib.buf, &ib.off)) {
                scan_src = src; scan_isz = isz; scan_restart = restart;  // max_index only if a stream gets copied
            } else {
                ib = vk_transient_copy(src, (uint64_t)count * isz + 4, 4);
                if (isz == 2) { const uint16_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != (uint16_t)restart) max_index = s[i]; }
                else { const uint32_t *s = src; for (uint32_t i = 0; i < count; i++) if (s[i] > max_index && s[i] != restart) max_index = s[i]; }
            }
        } else if (native) {  // 8-bit indices: widen to 16
            ib = vk_transient((uint64_t)count * 2 + 4, 4);
            uint16_t *w = ib.ptr;
            const uint8_t *s = src;
            for (uint32_t i = 0; i < count; i++) { w[i] = s[i] == 0xFF && st.prim_restart ? 0xFFFF : s[i]; if (s[i] > max_index && w[i] != 0xFFFF) max_index = s[i]; }
            itype_vk = VK_INDEX_TYPE_UINT16;
            icount = count;
        } else {
            uint32_t *e = expand_indices(mode, count, src, (int)itype, 0, &icount);
            for (uint32_t i = 0; i < icount; i++) if (e[i] > max_index) max_index = e[i];
            ib = vk_transient_copy(e, (uint64_t)icount * 4 + 4, 4);
            free(e);
        }
        if (!scan_src) {
            max_index += 1;
            if (base_vertex > 0) max_index += (uint32_t)base_vertex;
        }
    }
    if (use_index && ss_need(SS_IB, ss.ib == ib.buf && ss.io == ib.off && ss.it == itype_vk)) {
        ss.ib = ib.buf; ss.io = ib.off; ss.it = itype_vk;
        vkCmdBindIndexBuffer(cb, ib.buf, ib.off, itype_vk);
    }

    // ---- vertex streams: the range this draw can read, copied once per submission
    for (int s = 0; s < 17; s++) {
        if (!(k.used_streams & (1u << s))) continue;
        VkSpan sp;
        if (s == ZERO_BINDING) {
            static const float zero[4] = {0, 0, 0, 1};
            sp = vk_transient_copy(zero, 16, 16);
        } else {
            Range v = st.vbuf[s];
            VkBuffer dbuf;
            VkDeviceSize doff;
            if (v.addr && v.size && guest_direct(v.addr, v.size, &dbuf, &doff)) {
                if (ss_on && (ss.vb_known >> s & 1) && ss.vb[s] == dbuf && ss.vo[s] == doff) continue;
                ss.vb[s] = dbuf; ss.vo[s] = doff; ss.vb_known |= 1u << s;
                vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &dbuf, &doff);
                continue;
            }
            if (scan_src) {  // copying after all: the index scan bounds the copy
                if (scan_isz == 2) { const uint16_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != (uint16_t)scan_restart) max_index = q[i]; }
                else { const uint32_t *q = scan_src; for (uint32_t i = 0; i < icount; i++) if (q[i] > max_index && q[i] != scan_restart) max_index = q[i]; }
                max_index += 1;
                if (base_vertex > 0) max_index += (uint32_t)base_vertex;
                scan_src = NULL;
            }
            uint64_t stride = k.stride[s], need;
            if (!stride) need = 256;
            else if (k.divisor[s]) need = ((uint64_t)base_inst + inst + k.divisor[s] - 1) / k.divisor[s] * stride + 256;
            else need = (uint64_t)max_index * stride + 256;
            if (v.size && need > v.size) need = v.size;
            if (!v.addr) { static const uint8_t zero[256]; sp = vk_transient_copy(zero, 256, 16); }
            else sp = guest_copy(v.addr, need ? need : 16);
        }
        VkDeviceSize off = sp.off;
        if (ss_on && (ss.vb_known >> s & 1) && ss.vb[s] == sp.buf && ss.vo[s] == off) continue;
        ss.vb[s] = sp.buf; ss.vo[s] = off; ss.vb_known |= 1u << s;
        vkCmdBindVertexBuffers(cb, (uint32_t)s, 1, &sp.buf, &off);
    }

    if (prof) prof_vtx += SDL_GetTicksNS() - prof_t2_;
    // ---- draw
    // Instanced draws with item spots among their instances (bushsanity: bushes are drawn in batches, each instance's
    // model matrix in vertex storage buffer 0, indexed by instance id): drawn as runs of instances, gold runs tinted
    // (as renderer.m; gl_InstanceIndex includes the base instance, so a run [b + k, b + k + n) draws exactly those).
    enum { MAX_RUNS = 64 };
    uint32_t run_at[MAX_RUNS], run_n[MAX_RUNS];
    bool run_gold[MAX_RUNS];
    int nruns = 0;
    if (pr->fi->uses_tint && inst > 1 && st.sbuf[0][0].addr) {
        extern bool pot_glow_active(void);
        extern bool pot_instance_gold(const float *m);
        uint64_t recs = st.sbuf[0][0].size / 64;
        if (pot_glow_active() && (uint64_t)base_inst + inst <= recs) {
            const float *m = (const float *)(uintptr_t)st.sbuf[0][0].addr;
            for (uint32_t i = 0; i < inst; i++) {
                bool g = pot_instance_gold(m + (size_t)(base_inst + i) * 16);
                if (nruns && run_gold[nruns - 1] == g) { run_n[nruns - 1]++; continue; }
                if (nruns == MAX_RUNS) { nruns = 0; break; }  // (too fragmented: draw as is)
                run_at[nruns] = i; run_n[nruns] = 1; run_gold[nruns] = g; nruns++;
            }
            if (nruns == 1 && !run_gold[0]) nruns = 0;
        }
    }
    if (!nruns) { run_at[0] = 0; run_n[0] = inst; nruns = 1; run_gold[0] = false; }
    if (g_gpuprof_frame) {
        static int per_draw = -1;
        if (per_draw < 0) per_draw = getenv("LARECOMP_GPUPROF_DRAWS") != NULL;
        if (per_draw)
            gp_mark(cb, "draw vs %llx fs %llx fs hash %016llx count %u inst %u", (unsigned long long)st.program->stages[0].code_addr,
                    (unsigned long long)st.program->stages[1].code_addr, (unsigned long long)g_cur_fs_hash, use_index ? icount : count, inst);
    }
    for (int ri = 0; ri < nruns; ri++) {
        if (nruns > 1 || run_gold[0]) {  // this run's tint: a fragment set of its own
            float rtint[8] = {0};
            extern void pot_gold_tint(float tint[8]);
            if (run_gold[ri]) pot_gold_tint(rtint);
            VkDescriptorSet rset = stage_set(pr, pr->set_layout[1], 1, 1, pr->fi, rs0, rtint, 0, 0, 0, fii);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pr->layout, 1, 1, &rset, 0, NULL);
        }
        if (!use_index) vkCmdDraw(cb, count, run_n[ri], first, base_inst + run_at[ri]);
        else vkCmdDrawIndexed(cb, icount, run_n[ri], 0, base_vertex, base_inst + run_at[ri]);
    }
    stat_draws++; vk_draws_total++;
    cum_draws++;
    if (g_gpuprof_frame && gp_n) {
        if (!gp_draws[gp_n - 1]) {  // first draw of the row: its fragment shader hash
            size_t l = strlen(gp_label[gp_n - 1]);
            snprintf(gp_label[gp_n - 1] + l, sizeof gp_label[0] - l, ", first fs %016llx", (unsigned long long)g_cur_fs_hash);
        }
        gp_draws[gp_n - 1]++;
    }
}

static void debug_draw(Prog *pr, CmdRec *r, bool indexed) {
    LOG("[draw] %s inst %llu mode %llu count %llu prog vs=%llx fs=%llx (%016llx %016llx) | vs cb %#x fs cb %#x | rt %d", indexed ? "e" : "a",
        (unsigned long long)(indexed ? r->x[6] : r->x[4]), (unsigned long long)r->x[0], (unsigned long long)r->x[2],
        (unsigned long long)st.program->stages[0].code_addr, (unsigned long long)st.program->stages[1].code_addr,
        (unsigned long long)stage_hash(st.program, 0), (unsigned long long)stage_hash(st.program, 1),
        pr->vi->cbuf_mask, pr->fi->cbuf_mask, st.rt.ncolor);
    for (int i = 0; i < st.rt.ncolor; i++) if (st.rt.color[i]) LOG(" %ux%u/f%#x", st.rt.color[i]->width, st.rt.color[i]->height, st.rt.color[i]->format);
    if (st.rt.depth) LOG(" d%#x %p", st.rt.depth->format, (void *)st.rt.depth);
    LOG(" | vp %.0f %.0f %.0f %.0f | attrs %#x blend %#x ds %02x%02x%02x poly %02x%02x chmask %08x\n", st.viewport[0][0], st.viewport[0][1],
        st.viewport[0][2], st.viewport[0][3], pr->vi->attr_mask, st.color_state[0], st.ds[0], st.ds[1], st.ds[2], st.poly[0], st.poly[1],
        st.channel_mask);
    if (getenv("LARECOMP_DEBUG_DATA")) {  // vertex streams 0-1 and fragment cbufs: the first floats
        for (int s = 0; s < 2; s++) {
            if (!st.vbuf[s].addr) continue;
            const float *v = (const float *)(uintptr_t)st.vbuf[s].addr;
            LOG("[draw]   vbuf%d %llx size %llu:", s, (unsigned long long)st.vbuf[s].addr, (unsigned long long)st.vbuf[s].size);
            for (int k = 0; k < 16; k++) LOG(" %.4g", v[k]);
            LOG("\n");
        }
        for (int c = 0; c < 18; c++) {
            if (!((pr->fi->cbuf_mask >> c) & 1) || !st.ubuf[1][c].addr) continue;
            const float *u = (const float *)(uintptr_t)st.ubuf[1][c].addr;
            LOG("[draw]   fcb%d %llx size %u:", c, (unsigned long long)st.ubuf[1][c].addr, (unsigned)st.ubuf[1][c].size);
            for (int k = 0; k < 96 && k * 4 < (int)st.ubuf[1][c].size; k++) LOG(" %.6g", u[k]);
            LOG("\n");
        }
    }
    for (int i = 0; i < pr->fi->ntex && i < 64; i++) {  // fragment textures: guest size / format, allocation, scale
        NvnTexture *t = shader_texture_object(1, &pr->fi->tex[i]);
        const TexBackend *b = t ? (const TexBackend *)t->backend : NULL;
        if (!t) { LOG("[draw]   tex%d: none\n", i); continue; }
        LOG("[draw]   tex%d: %p %ux%u f%#x lv %u -> %ux%u scale %.4f %.4f%s\n", i, (void *)t, t->width, t->height, t->format,
            b ? b->levels : 0, b ? b->width : 0, b ? b->height : 0, b ? b->sx : 0, b ? b->sy : 0, b && b->scalable ? " scalable" : "");
    }
}

// ================================================================== texture copies
typedef struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; } TexRefRec;

static void copy_region_dims(NvnTexture *t, const TexRefRec *ref, const FormatInfo **fi, int32_t rg[6], int *level,
                             uint64_t *row, uint64_t *img) {
    *fi = nvn_format_info(t->format);
    memcpy(rg, ref->region, sizeof ref->region);
    if (!rg[3]) rg[3] = (int32_t)t->width;
    if (!rg[4]) rg[4] = (int32_t)t->height;
    if (!rg[5]) rg[5] = 1;
    *level = ref->has_view && (ref->view.setmask & 1) ? ref->view.base_level : 0;
    uint32_t wb = ((uint32_t)rg[3] + (*fi)->bw - 1) / (*fi)->bw, hb = ((uint32_t)rg[4] + (*fi)->bh - 1) / (*fi)->bh;
    *row = st.copy_row_stride ? st.copy_row_stride : (uint64_t)wb * (*fi)->bpb;
    *img = st.copy_image_stride ? st.copy_image_stride : *row * hb;
}

static void copy_buffer_to_texture(uint64_t src, const TexRefRec *dst) {
    snap_reason = "buf2tex"; snapshots_invalidate(NULL);
    NvnTexture *t = (NvnTexture *)(uintptr_t)dst->tex;
    if (!t || !src) return;
    alias_before_write(t, 0, "upload");
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, dst, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb) return;
    TexBackend *b = vk_tex(t, false);  // prior CPU contents uploaded first
    renderer_mark_gpu_owned(t);
    if (b->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) return;
    if (b->astc_bw) {  // (blocks would need the CPU decode: not seen in the game yet)
        static int nlog;
        if (nlog++ < 5) LOG("[vk] buffer -> ASTC texture copy skipped (%ux%u)\n", t->width, t->height);
        return;
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t hb = ((uint32_t)rg[4] + fi->bh - 1) / fi->bh, wb = ((uint32_t)rg[3] + fi->bw - 1) / fi->bw;
    uint32_t out_bpb = fi->bpb == 3 ? 4 : fi->bpb;
    uint32_t nz = (uint32_t)rg[5];
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    uint64_t out_row = (uint64_t)wb * out_bpb;
    VkSpan s = vk_transient(out_row * hb * nz, 16);
    for (uint32_t z = 0; z < nz; z++)
        for (uint32_t y = 0; y < hb; y++) {
            const uint8_t *sr = (const uint8_t *)(uintptr_t)(src + (uint64_t)z * img + (uint64_t)y * row);
            uint8_t *dr = (uint8_t *)s.ptr + ((uint64_t)z * hb + y) * out_row;
            if (out_bpb == fi->bpb) memcpy(dr, sr, out_row);
            else for (uint32_t x = 0; x < wb; x++) { memcpy(dr + x * 4, sr + x * 3, 3); dr[x * 4 + 3] = 255; }
        }
    VkBufferImageCopy c = {.bufferOffset = s.off, .bufferRowLength = wb * fi->bw, .bufferImageHeight = hb * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

static void copy_texture_to_buffer(const TexRefRec *src, uint64_t dst) {
    NvnTexture *t = (NvnTexture *)(uintptr_t)src->tex;
    if (!t || !dst || dst < HEAP_BASE) return;
    t = alias_read(t);
    vk_end_pass();
    const FormatInfo *fi;
    int32_t rg[6];
    int level;
    uint64_t row, img;
    copy_region_dims(t, src, &fi, rg, &level, &row, &img);
    if (!fi || !fi->bpb || fi->bpb == 3 || row % fi->bpb) return;
    TexBackend *b = vk_tex(t, false);
    if (b->bc7) return;  // (BC7-stored ASTC, bc7_gpu.c: no readback of its blocks)
    float tsx, tsy;
    renderer_texture_scale(t, &tsx, &tsy);
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImage srcimg = b->image;
    VmaAllocation small_mem = NULL;
    if ((tsx != 1 || tsy != 1) && level == 0 && !renderer_format_is_depth(t->format)) {
        // the CPU expects guest-sized texels: filter the scaled allocation down first
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = b->format,
                                .extent = {t->width, t->height, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &srcimg, &small_mem, NULL));
        vk_image_init_layout(cb, srcimg, VK_IMAGE_ASPECT_COLOR_BIT);
        VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int32_t)b->width, (int32_t)b->height, 1}},
                          .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int32_t)t->width, (int32_t)t->height, 1}}};
        vkCmdBlitImage(cb, b->image, VK_IMAGE_LAYOUT_GENERAL, srcimg, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
        vk_full_barrier(cb);
    }
    bool array = b->layers > 1 && b->depth == 1;
    uint32_t nz = (uint32_t)rg[5];
    uint64_t total = img * nz;
    VkSpan out = vk_readback(total);
    VkBufferImageCopy c = {.bufferOffset = out.off, .bufferRowLength = (uint32_t)(row / fi->bpb) * fi->bw,
                           .bufferImageHeight = (uint32_t)(img / row) * fi->bh,
                           .imageSubresource = {b->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : b->aspect,
                                                (uint32_t)level, array ? (uint32_t)rg[2] : 0, array ? nz : 1},
                           .imageOffset = {rg[0], rg[1], array ? 0 : rg[2]},
                           .imageExtent = {(uint32_t)rg[3], (uint32_t)rg[4], array ? 1 : nz}};
    vkCmdCopyImageToBuffer(cb, srcimg, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    wait_why = "texture -> buffer readback";
    renderer_flush();  // the CPU may read the result right after the fence
    memcpy((void *)(uintptr_t)dst, out.ptr, total);
    if (small_mem) vmaDestroyImage(g_vk.vma, srcimg, small_mem);
}

static void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags) {
    snap_reason = "tex2tex"; snapshots_invalidate(NULL);
    NvnTexture *st_ = (NvnTexture *)(uintptr_t)src->tex, *dt = (NvnTexture *)(uintptr_t)dst->tex;
    if (!st_ || !dt) return;
    vk_end_pass();
    if (dl_on == 2) renderer_flush();  // (LARECOMP_VK_DEVLOST_TRACE=2: each copy its own submission, waited on)
    gp_mark(vk_cmd(), "copy texture %ux%u -> %ux%u", st_->width, st_->height, dt->width, dt->height);
    dl_note("copy texture %ux%u f%#x -> %ux%u f%#x (%p -> %p)", st_->width, st_->height, st_->format, dt->width, dt->height,
            dt->format, (void *)st_, (void *)dt);
    TexBackend *s = vk_tex(st_, false), *d = vk_tex(dt, true);
    renderer_mark_gpu_owned(dt);
    if (renderer_texture_presented(dt) && st_->width == dt->width && st_->height == dt->height) renderer_texture_mark_presented(st_);
    renderer_texture_first_write(dt);
    renderer_texture_level_fresh(dt);
    const int32_t *sr = src->region, *dr = dst->region;
    float ssx, ssy, dsx, dsy;
    renderer_texture_scale(st_, &ssx, &ssy);
    renderer_texture_scale(dt, &dsx, &dsy);
    int sx0 = (int)lroundf(sr[0] * ssx), sy0 = (int)lroundf(sr[1] * ssy);
    int dx0 = (int)lroundf(dr[0] * dsx), dy0 = (int)lroundf(dr[1] * dsy);
    int sw = sr[3] ? (int)lroundf(sr[3] * ssx) : (int)s->width, sh = sr[4] ? (int)lroundf(sr[4] * ssy) : (int)s->height;
    int dw = dr[3] ? (int)lroundf(dr[3] * dsx) : (int)d->width, dh = dr[4] ? (int)lroundf(dr[4] * dsy) : (int)d->height;
    uint32_t slv = src->has_view && (src->view.setmask & 1) ? src->view.base_level : 0;
    uint32_t dlv = dst->has_view && (dst->view.setmask & 1) ? dst->view.base_level : 0;
    uint32_t sly = src->has_view && (src->view.setmask & 2) ? src->view.min_layer : 0;
    uint32_t dly = dst->has_view && (dst->view.setmask & 2) ? dst->view.min_layer : 0;
    if (slv >= s->levels || dlv >= d->levels || sly >= s->layers || dly >= d->layers) return;
    {   // whole-image regions are the level's extent, and every region stays inside its level: scaled allocations
        // (internal resolution below 1x) round guest regions past the edge, and out-of-bounds copies / blits are
        // undefined in Vulkan (garbage that changed every frame on NVIDIA: flicker in the depth pyramid's users)
        int slw = (int)level_extent(s->width, slv), slh = (int)level_extent(s->height, slv);
        int dlw = (int)level_extent(d->width, dlv), dlh = (int)level_extent(d->height, dlv);
        if (!sr[3]) sw = slw - sx0;
        if (!sr[4]) sh = slh - sy0;
        if (!dr[3]) dw = dlw - dx0;
        if (!dr[4]) dh = dlh - dy0;
        if (sx0 + sw > slw) sw = slw - sx0;
        if (sy0 + sh > slh) sh = slh - sy0;
        if (dx0 + dw > dlw) dw = dlw - dx0;
        if (dy0 + dh > dlh) dh = dlh - dy0;
        if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;
    }
    if (g_debug_frame || ct_on())
        LOG("[copytex] %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] -> %p %ux%u f%#x lv%u (%d,%d %dx%d) [%ux%u] phys (%d,%d %dx%d) -> (%d,%d %dx%d) flags %llx\n",
            (void *)st_, st_->width, st_->height, st_->format, slv, sr[0], sr[1], sr[3], sr[4], s->width, s->height, (void *)dt, dt->width,
            dt->height, dt->format, dlv, dr[0], dr[1], dr[3], dr[4], d->width, d->height, sx0, sy0, sw, sh, dx0, dy0, dw, dh,
            (unsigned long long)flags);
    if (copy_opt() && sx0 == 0 && sy0 == 0 && dx0 == 0 && dy0 == 0 && sw == (int)s->width && sh == (int)s->height && dw == (int)d->width &&
        dh == (int)d->height && slv == 0 && dlv == 0 && sly == 0 && dly == 0 && alias_make(st_, dt, s, d)) {
        if (ct_on()) LOG("[ct] alias made %p -> %p\n", (void *)st_, (void *)dt);
        return;
    }
    alias_before_write(dt, 0, "copy dst");
    if (naliases) s = alias_read(st_)->backend;
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    VkImageAspectFlags sa = s->aspect, da = d->aspect;
    if (s->samples > 1 && d->samples == 1 && sw == dw && sh == dh && s->format == d->format && !(sa & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        VkImageResolve rv = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                             .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdResolveImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rv);
        return;
    }
    if (s->samples != d->samples) return;
    if ((s->bc7 || d->bc7) && s->format != d->format) {  // (BC7-stored ASTC, bc7_gpu.c: no conversions to or from it)
        static int nlog;
        if (nlog++ < 5) LOG("[vk] copy to or from a BC7 texture skipped (%ux%u -> %ux%u)\n", s->width, s->height, d->width, d->height);
        return;
    }
    if (sw == dw && sh == dh && s->format == d->format) {
        VkImageCopy c = {.srcSubresource = {sa, slv, sly, 1}, .srcOffset = {sx0, sy0, 0}, .dstSubresource = {da, dlv, dly, 1},
                         .dstOffset = {dx0, dy0, 0}, .extent = {(uint32_t)sw, (uint32_t)sh, 1}};
        vkCmdCopyImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }
    // scaled / converting copy: the game builds its depth pyramid this way (nearest), colour copies filter if asked
    bool depth = (sa | da) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    if (depth && s->format != d->format) return;
    VkImageBlit bl = {.srcSubresource = {depth ? (sa & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : sa) : sa, slv, sly, 1},
                      .srcOffsets = {{sx0, sy0, 0}, {sx0 + sw, sy0 + sh, 1}},
                      .dstSubresource = {depth ? (da & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : da) : da, dlv, dly, 1},
                      .dstOffsets = {{dx0, dy0, 0}, {dx0 + dw, dy0 + dh, 1}}};
    bool linear = (flags & 1) && !depth && !rt_kind(st_->format);
    dl_note("  blit %p lv%u/%u %ux%u (%d,%d %dx%d) -> %p lv%u/%u %ux%u (%d,%d %dx%d) %s", (void *)s->image, slv, s->levels, s->width,
            s->height, sx0, sy0, sw, sh, (void *)d->image, dlv, d->levels, d->width, d->height, dx0, dy0, dw, dh, linear ? "linear" : "nearest");
    vkCmdBlitImage(cb, s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

// ================================================================== present helpers (device.c)
static VkPipelineLayout present_layout;
static VkDescriptorSetLayout present_set_layout;

static VkPipeline present_pipeline(VkFormat fmt) {
    static struct { VkFormat f; VkPipeline p; } cache[4];
    for (int i = 0; i < 4; i++) if (cache[i].p && cache[i].f == fmt) return cache[i].p;
    if (!present_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &present_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_VERTEX_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &present_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &present_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("present.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader("present.frag"), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = present_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 4; i++) if (!cache[i].p) { cache[i].f = fmt; cache[i].p = p; break; }
    return p;
}

// Scanout shows the bytes the game wrote: sRGB images are read through a UNORM view (no decode), like metal.m.
static VkImageView raw_view(TexBackend *b) {
    VkFormat f = b->format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
               : b->format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : b->format;
    static const VkComponentMapping id = {0};
    return vk_image_view(b, VK_IMAGE_VIEW_TYPE_2D, f, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// ---- post passes (Video tab "Anti-aliasing" / "Upscaler"): SMAA 1x at the game image's size, then AMD FSR 1 (EASU
// upscale to the image's size in the window, RCAS sharpen) when it is smaller than its place in the window, as
// metal.m does with MetalFX. Each pass: a fullscreen triangle into an RGBA8 image (gamma-encoded, like the input);
// up to three inputs (bindings 0-2), sampled linear / clamp.
static VkPipelineLayout post_layout;
static VkDescriptorSetLayout post_set_layout;

static VkPipeline post_pipeline(const char *fs) {
    static struct { const char *fs; VkPipeline p; } cache[8];
    for (int i = 0; i < 8; i++) if (cache[i].p && cache[i].fs == fs) return cache[i].p;
    if (!post_layout) {
        VkDescriptorSetLayoutBinding b[3];
        for (uint32_t i = 0; i < 3; i++)
            b[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &post_set_layout));
        VkPushConstantRange r = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                         .pSetLayouts = &post_set_layout, .pushConstantRangeCount = 1, .pPushConstantRanges = &r};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &ci, NULL, &post_layout));
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vk_builtin_shader("post.vert"), .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = vk_builtin_shader(fs), .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xF};
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1,
                                              .pAttachments = &cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &fmt};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri, .stageCount = 2,
                                       .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vs,
                                       .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds,
                                       .pColorBlendState = &cb, .pDynamicState = &dy, .layout = post_layout};
    VkPipeline p;
    VK_CHECK(vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p));
    for (int i = 0; i < 8; i++) if (!cache[i].p) { cache[i].fs = fs; cache[i].p = p; break; }
    return p;
}

// Post image `i` at w x h (RGBA8, kept until the size changes). 0, 1: SMAA edges / weights, 2: SMAA output,
// 3, 4: FSR EASU / RCAS.
static TexBackend *post_image(int i, uint32_t w, uint32_t h) {
    static TexBackend img[5];
    TexBackend *d = &img[i];
    if (d->image && (d->width != w || d->height != h)) {
        vk_views_forget(d);
        vk_defer_image(d->image, d->mem);
        d->image = VK_NULL_HANDLE;
        d->mem = VK_NULL_HANDLE;
    }
    if (!d->image) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1,
                                .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = VK_FORMAT_R8G8B8A8_UNORM; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        vk_image_init_layout(vk_cmd(), d->image, d->aspect);
    }
    return d;
}

static VkImageView post_view(TexBackend *d) {
    static const VkComponentMapping id = {0};
    return vk_image_view(d, VK_IMAGE_VIEW_TYPE_2D, d->format, 0, 1, 0, 1, id, VK_IMAGE_ASPECT_COLOR_BIT);
}

// SMAA's lookup textures (runtime/nvn/smaa_data.c): 0 = area (160 x 560 R8G8), 1 = search (64 x 16 R8), uploaded once.
static VkImageView smaa_lut(int i) {
    static TexBackend t[2];
    TexBackend *d = &t[i];
    if (!d->image) {
        extern const unsigned char *smaa_area_tex(void);
        extern const unsigned char *smaa_search_tex(void);
        uint32_t w = i ? 64 : 160, h = i ? 16 : 560, bpp = i ? 1 : 2;
        VkFormat f = i ? VK_FORMAT_R8_UNORM : VK_FORMAT_R8G8_UNORM;
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                                .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &d->image, &d->mem, NULL));
        d->format = f; d->width = w; d->height = h; d->depth = 1; d->layers = 1; d->levels = 1;
        d->samples = 1; d->aspect = VK_IMAGE_ASPECT_COLOR_BIT; d->base_type = VK_IMAGE_VIEW_TYPE_2D;
        VkCommandBuffer cb = vk_cmd();
        vk_image_init_layout(cb, d->image, d->aspect);
        VkSpan s = vk_transient_copy(i ? smaa_search_tex() : smaa_area_tex(), (uint64_t)w * h * bpp, 16);
        VkBufferImageCopy c = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyBufferToImage(cb, s.buf, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    return post_view(d);
}

// One post pass (outside the window image's rendering): srcs (bindings 0..n-1) -> dst, fragment shader `fs`.
// clear: start from zero (SMAA's edge pass only writes edge pixels; its weights / output read them).
static void post_pass(VkCommandBuffer cb, const char *fs, const VkImageView *srcs, int n, TexBackend *dst, const float pc[8],
                      bool clear) {
    VkPipeline pipe = post_pipeline(fs);  // (before the rendering begins: created inside it, the NVIDIA driver crashed)
    vk_full_barrier(cb);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = post_view(dst),
                                    .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {dst->width, dst->height}},
                          .layerCount = 1, .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkDescriptorSet ds = vk_alloc_set(post_set_layout);
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet w[3];
    for (int i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){vk_default_sampler(true), srcs[i], VK_IMAGE_LAYOUT_GENERAL};
        w[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = (uint32_t)i,
                                      .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                      .pImageInfo = &ii[i]};
    }
    vkUpdateDescriptorSets(g_vk.dev, (uint32_t)n, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)dst->width, (float)dst->height, 0, 1};
    VkRect2D sc = {{0, 0}, {dst->width, dst->height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdPushConstants(cb, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
}

// The view the present samples: the game image, or its anti-aliased / upscaled version. ow x oh: the whole image's
// size in the window (its crop is drawn into the viewport).
static VkImageView post_process(VkCommandBuffer cb, TexBackend *b, double ow_d, double oh_d) {
    VkImageView v = raw_view(b);
    bool smaa = g_settings.antialias == AA_SMAA;
    uint32_t ow = (uint32_t)lround(ow_d), oh = (uint32_t)lround(oh_d);
    if (ow > 16384) ow = 16384;
    if (oh > 16384) oh = 16384;
    bool fsr = g_settings.upscaler == UPSCALE_FSR1 && (ow > b->width + 1 || oh > b->height + 1);  // (an upscale only)
    if (!smaa && !fsr) return v;
    extern void vk_frame_suspend(void);
    extern void vk_frame_resume(void);
    vk_frame_suspend();
    if (smaa) {  // SMAA 1x: luma edges -> blending weights -> neighbourhood blending
        float w = (float)b->width, h = (float)b->height;
        float m[8] = {1.0f / w, 1.0f / h, w, h};
        TexBackend *edges = post_image(0, b->width, b->height), *weights = post_image(1, b->width, b->height);
        TexBackend *out = post_image(2, b->width, b->height);
        VkImageView in[3] = {v};
        post_pass(cb, "smaa_edge.frag", in, 1, edges, m, true);
        VkImageView wi[3] = {post_view(edges), smaa_lut(0), smaa_lut(1)};
        post_pass(cb, "smaa_weight.frag", wi, 3, weights, m, true);
        VkImageView bi[3] = {v, post_view(weights)};
        post_pass(cb, "smaa_blend.frag", bi, 2, out, m, false);
        v = post_view(out);
    }
    if (fsr) {
        float iw = (float)b->width, ih = (float)b->height;
        TexBackend *e = post_image(3, ow, oh), *r = post_image(4, ow, oh);
        float pe[8] = {iw / ow, ih / oh, 0.5f * iw / ow - 0.5f, 0.5f * ih / oh - 0.5f, iw, ih};
        VkImageView ei[3] = {v};
        post_pass(cb, "easu.frag", ei, 1, e, pe, false);
        float pr[8] = {exp2f(-0.2f), 0, 0, 0, (float)ow, (float)oh};  // (sharpness 0.2 stops: AMD's default)
        VkImageView ri_[3] = {post_view(e)};
        post_pass(cb, "rcas.frag", ri_, 1, r, pr, false);
        v = post_view(r);
    }
    vk_full_barrier(cb);
    vk_frame_resume();
    {
        static uint32_t piw, pih, pow_, poh;
        static int psm = -1, pfs = -1;
        if (b->width != piw || b->height != pih || ow != pow_ || oh != poh || smaa != psm || fsr != pfs)
            LOG("[post] %ux%u%s%s -> %ux%u\n", b->width, b->height, smaa ? " smaa" : "", fsr ? " fsr1" : "", ow, oh);
        piw = b->width; pih = b->height; pow_ = ow; poh = oh; psm = smaa; pfs = fsr;
    }
    return v;
}

// Inside the window image's rendering (device.c vk_frame_begin): the game image into (vx, vy, vw, vh).
void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    VkImageView src = post_process(cb, b, pv[6] > 0 ? vw / pv[6] : vw, pv[7] > 0 ? vh / pv[7] : vh);
    VkPipeline p = present_pipeline(g_vk.sc_format);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {vx, vy, vw, vh, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    VkViewport full = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &full);
}

// Screenshot of a game texture (presented crop applied to build/frame_* shots), via the present shader into RGBA8.
void renderer_save_texture_tex(TexBackend *b, const char *path) {
    if (!b || !b->image || (b->aspect & ~VK_IMAGE_ASPECT_COLOR_BIT)) return;
    extern const float *vk_shot_crop(void);
    float crop[4] = {0, 0, 1, 1};
    if (strstr(path, "frame_")) memcpy(crop, vk_shot_crop(), sizeof crop);
    uint32_t w = (uint32_t)(b->width * crop[2]), h = (uint32_t)(b->height * crop[3]);
    if (!w || !h) return;
    vk_end_pass();
    VkImage img;
    VmaAllocation mem;
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VK_CHECK(vmaCreateImage(g_vk.vma, &ci, &ai, &img, &mem, NULL));
    VkCommandBuffer cb = vk_cmd();
    vk_full_barrier(cb);
    vk_image_init_layout(cb, img, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageViewCreateInfo vci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView tv;
    VK_CHECK(vkCreateImageView(g_vk.dev, &vci, NULL, &tv));
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = tv, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cb, &ri);
    VkPipeline p = present_pipeline(VK_FORMAT_R8G8B8A8_UNORM);
    VkDescriptorSet ds = vk_alloc_set(present_set_layout);
    VkDescriptorImageInfo ii = {vk_default_sampler(true), raw_view(b), VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &wr, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, present_layout, 0, 1, &ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pv[8] = {renderer_origin_lower_left() && !strstr(path, "window") ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
    vkCmdPushConstants(cb, present_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 32, pv);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    vk_full_barrier(cb);
    VkSpan out = vk_readback((uint64_t)w * h * 4);
    VkBufferImageCopy c = {.bufferOffset = out.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, out.buf, 1, &c);
    renderer_flush();
    vk_save_png(path, out.ptr, w, h);
    vkDestroyImageView(g_vk.dev, tv, NULL);
    vmaDestroyImage(g_vk.vma, img, mem);
}

// ================================================================== executor (renderer.m exec_list)
static void exec_list(uint64_t handle, int depth) {
    uint64_t p = handle;
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        void *x = r->extra;
        if (r->op < CMD_COUNT) op_counts[r->op]++;
        switch (r->op) {
        case CMD_END: return;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = x;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) exec_list(h[i], depth + 1);
            break;
        }
        case CMD_BIND_PROGRAM: {
            // Astral Chain: NVN binds the selected stages (x[1], NVNshaderStageBits: vertex 1, fragment 2,
            // geometry 4, tess control 8, tess eval 0x10, compute 0x20) and keeps the others. The game binds
            // separate single-stage programs, so the effective program is a composite of per-stage bindings.
            static NvnProgram composite;
            static const int stage_of_bit[6] = {0, 1, 2, 3, 4, 5};
            NvnProgram *p = (NvnProgram *)(uintptr_t)r->x[0];
            uint32_t bits = (uint32_t)r->x[1];
            for (int b = 0; b < 6; b++) {
                if (!(bits & (1u << b))) continue;
                int s = stage_of_bit[b];
                if (p && (p->stage_mask & (1u << s))) { composite.stages[s] = p->stages[s]; composite.stage_mask |= 1u << s; }
                else { memset(&composite.stages[s], 0, sizeof composite.stages[s]); composite.stage_mask &= ~(1u << s); }
            }
            st.program = composite.stage_mask ? &composite : NULL;
            break;
        }
        case CMD_BIND_BLEND: { uint8_t *b = x; memcpy(st.blend[b[0] & 7], b, 8); break; }
        case CMD_BIND_CHANNEL_MASK: memcpy(&st.channel_mask, x, 4); break;
        case CMD_BIND_COLOR: memcpy(st.color_state, x, 4); break;
        case CMD_BIND_DEPTH_STENCIL: memcpy(st.ds, x, 8); break;
        case CMD_BIND_MULTISAMPLE: memcpy(st.ms, x, 0x18); break;
        case CMD_BIND_POLYGON: memcpy(st.poly, x, 4); break;
        case CMD_BIND_VERTEX_ATTRIB:
            st.nattrib = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.attrib, x, st.nattrib * 4);
            break;
        case CMD_BIND_VERTEX_STREAM:
            st.nstream = (int)r->x[0] > 16 ? 16 : (int)r->x[0];
            memcpy(st.stream, x, st.nstream * 8);
            break;
        case CMD_BIND_VERTEX_BUFFERS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(&st.vbuf[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_UNIFORM_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 18 && r->x[0] < 6; i++)
                memcpy(&st.ubuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_STORAGE_BUFFERS:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 16 && r->x[0] < 6; i++)
                memcpy(&st.sbuf[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_BIND_TEXTURES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 32 && r->x[0] < 6; i++)
                memcpy(&st.tex[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_BIND_IMAGES:
            for (uint64_t i = 0; i < r->x[2] && r->x[1] + i < 8 && r->x[0] < 6; i++)
                memcpy(&st.img[r->x[0]][r->x[1] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_TEXTURE_POOL: st.tpool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_SAMPLER_POOL: st.spool = (NvnDescPool *)(uintptr_t)r->x[0]; break;
        case CMD_SET_RENDER_TARGETS: {
            const struct { uint64_t tex[8]; NvnTextureView views[8]; uint8_t has_view[8]; uint64_t depth;
                           NvnTextureView dview; uint8_t has_dview; } *rt = x;
            RtState prev = st.rt;  // the same targets again: keep the pass (a restart = full barrier + reload)
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = (int)r->x[0] > 8 ? 8 : (int)r->x[0];
            for (int i = 0; i < st.rt.ncolor; i++) {
                st.rt.color[i] = (NvnTexture *)(uintptr_t)rt->tex[i];
                st.rt.cview[i] = rt->views[i];
                st.rt.has_cview[i] = rt->has_view[i];
            }
            st.rt.depth = (NvnTexture *)(uintptr_t)rt->depth;
            st.rt.dview = rt->dview;
            st.rt.has_dview = rt->has_dview;
            static int always = -1;
            if (always < 0) always = getenv("LARECOMP_VK_PASS_RESTART") != NULL;
            if (always || memcmp(&prev, &st.rt, sizeof prev)) st.rt_dirty = true;
            break;
        }
        case CMD_SET_VIEWPORTS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.viewport[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_SCISSORS:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.scissor[r->x[0] + i], (uint8_t *)x + i * 16, 16);
            break;
        case CMD_SET_DEPTH_RANGES:
            for (uint64_t i = 0; i < r->x[1] && r->x[0] + i < 16; i++) memcpy(st.depth_range[r->x[0] + i], (uint8_t *)x + i * 8, 8);
            break;
        case CMD_SET_BLEND_COLOR: memcpy(st.blend_color, x, 16); break;
        case CMD_SET_STENCIL: {
            int face = (int)r->x[0];
            for (int f = 0; f < 2; f++) {
                if (!(face & (1 << f))) continue;
                if (r->x[6] == 0) st.stencil_ref[f] = (uint32_t)r->x[1];
                else if (r->x[6] == 1) st.stencil_mask[f] = (uint32_t)r->x[1];
                else st.stencil_vmask[f] = (uint32_t)r->x[1];
            }
            break;
        }
        case CMD_SET_POLYGON_OFFSET: memcpy(st.poly_offset, r->f, sizeof st.poly_offset); break;
        case CMD_SET_ALPHA_REF: st.alpha_ref = r->f[0]; break;
        case CMD_SET_PRIMITIVE_RESTART: st.prim_restart = r->x[0] != 0; st.restart_index = (uint32_t)r->x[1]; break;
        case CMD_SET_COPY_STRIDES: st.copy_row_stride = r->x[0]; st.copy_image_stride = r->x[1]; break;
        case CMD_CLEAR_COLOR: {
            snap_reason = "clear-color"; snapshots_invalidate(NULL);
            int index = (int)r->x[0];
            if (getenv("LARECOMP_LOG_CLEARS") && index < st.rt.ncolor && st.rt.color[index])
                LOG("[cclear] frame %llu idx %d %ux%u f%#x mask %#x\n", (unsigned long long)renderer_present_count(), index,
                    st.rt.color[index]->width, st.rt.color[index]->height, st.rt.color[index]->format, (uint32_t)r->x[2]);
            if (ct_on() && index < st.rt.ncolor && st.rt.color[index])
                LOG("[ct] clear-color %p mask %#x sc %d %d %d %d\n", (void *)st.rt.color[index], (uint32_t)r->x[2], st.scissor[0][0], st.scissor[0][1],
                    st.scissor[0][2], st.scissor[0][3]);
            if (index < st.rt.ncolor && st.rt.color[index]) alias_before_write(st.rt.color[index], 0, "clear");
            if (index < st.rt.ncolor && st.rt.color[index]) clear(index, (int)r->x[5], x, (uint32_t)r->x[2], false, 0, false, 0, 0);
            break;
        }
        case CMD_CLEAR_TEXTURE: {
            // ClearTexture(cmd, texture, view, region, color, mask): a render-target clear of the texture alone (renderer.m)
            const struct { uint64_t tex; NvnTextureView view; uint8_t has_view; int32_t region[6]; float color[4]; } *ct = x;
            NvnTexture *t = (NvnTexture *)(uintptr_t)ct->tex;
            if (!t) break;
            snap_reason = "clear-texture"; snapshots_invalidate(NULL);
            alias_before_write(t, 0, "clear");
            RtState saved = st.rt;
            int32_t sc[4];
            memcpy(sc, st.scissor[0], sizeof sc);
            vk_end_pass();
            memset(&st.rt, 0, sizeof st.rt);
            st.rt.ncolor = 1;
            st.rt.color[0] = t;
            if (ct->has_view) { st.rt.cview[0] = ct->view; st.rt.has_cview[0] = 1; }
            st.rt_dirty = true;
            const int32_t *rg = ct->region;
            if (rg[3] > 0 && rg[4] > 0) { st.scissor[0][0] = rg[0]; st.scissor[0][1] = rg[1]; st.scissor[0][2] = rg[3]; st.scissor[0][3] = rg[4]; }
            else { st.scissor[0][0] = st.scissor[0][1] = 0; st.scissor[0][2] = st.scissor[0][3] = 16384; }
            clear(0, (int)r->x[6], ct->color, (uint32_t)r->x[4] ? (uint32_t)r->x[4] : 0xF, false, 0, false, 0, 0);
            vk_end_pass();
            st.rt = saved;
            memcpy(st.scissor[0], sc, sizeof sc);
            st.rt_dirty = true;
            break;
        }
        case CMD_CLEAR_DEPTH_STENCIL:
            snap_reason = "clear-depth"; snapshots_invalidate(NULL);
            if (ct_on() && st.rt.depth)
                LOG("[ct] clear-depth %p d%d s%d sc %d %d %d %d\n", (void *)st.rt.depth, r->x[0] != 0, r->x[2] != 0, st.scissor[0][0],
                    st.scissor[0][1], st.scissor[0][2], st.scissor[0][3]);
            if (st.rt.depth && naliases) {  // (copy aliases: which aspects of the depth target this clear replaces whole)
                NvnTexture *d = st.rt.depth;
                bool whole = st.scissor[0][0] <= 0 && st.scissor[0][1] <= 0 && (int64_t)st.scissor[0][0] + st.scissor[0][2] >= (int64_t)d->width &&
                             (int64_t)st.scissor[0][1] + st.scissor[0][3] >= (int64_t)d->height &&
                             !(st.rt.has_dview && (((st.rt.dview.setmask & 1) && st.rt.dview.base_level) ||
                                                   ((st.rt.dview.setmask & 2) && st.rt.dview.min_layer)));
                for (int i = 0; i < st.rt.ncolor; i++)  // (the clear covers the smallest attachment only)
                    if (st.rt.color[i] && (st.rt.color[i]->width < d->width || st.rt.color[i]->height < d->height)) whole = false;
                VkImageAspectFlags full = 0;
                if (whole && r->x[0] != 0) full |= VK_IMAGE_ASPECT_DEPTH_BIT;
                if (whole && renderer_format_has_stencil(d->format) && (r->x[2] & 0xFF) == 0xFF) full |= VK_IMAGE_ASPECT_STENCIL_BIT;
                if (!renderer_format_has_stencil(d->format)) full |= VK_IMAGE_ASPECT_STENCIL_BIT;
                if (ct_on())
                    LOG("[ct] alias clear %p full %#x (whole %d, x0 %llu x1 %#llx x2 %#llx, dview %d/%#x, ncolor %d)\n", (void *)d, full, whole,
                        (unsigned long long)r->x[0], (unsigned long long)r->x[1], (unsigned long long)r->x[2], st.rt.has_dview,
                        st.rt.dview.setmask, st.rt.ncolor);
                alias_before_write(d, full, "clear");
            }
            if (st.rt.depth) {
                bool has_stencil = renderer_format_has_stencil(st.rt.depth->format);
                clear(-1, 0, NULL, 0, r->x[0] != 0, r->f[0], has_stencil && r->x[2] != 0, (uint32_t)r->x[1], (uint32_t)r->x[2]);
            }
            break;
        case CMD_DISPATCH: dispatch_compute((uint32_t)r->x[0], (uint32_t)r->x[1], (uint32_t)r->x[2]); break;
        case CMD_DRAW_ARRAYS: draw(r, false); break;
        case CMD_DRAW_ELEMENTS: draw(r, true); break;
        case CMD_UPDATE_UNIFORM_BUFFER:
            memcpy((void *)(uintptr_t)(r->x[0] + r->x[2]), x, r->x[3]);
            guest_copy_flush();  // (cbuf copies shared within the submission are stale now)
            break;
        case CMD_FENCE_SYNC: {
            static int fence_flush = -1;
            if (fence_flush < 0) fence_flush = getenv("LARECOMP_FENCE_FLUSH") ? 1 : 0;
            if (fence_flush) renderer_flush();
            extern void nvn_sync_set_seq(uint64_t guest, uint64_t seq);
            if (r->x[1]) nvn_sync_set_seq(r->x[1], renderer_fence_seq());
            break;
        }
        case CMD_REPORT_COUNTER: {
            // The game times its GPU work with these (two per frame) and drops to 30 fps when that looks slow: the
            // host clock now, replaced by the GPU's time at this point once the submission is done (cq_resolve).
            uint64_t *out = (uint64_t *)(uintptr_t)r->x[1];
            uint32_t type = (uint32_t)r->x[0];
            static uint32_t seen;
            if (type < 32 && !(seen & (1u << type))) { seen |= 1u << type; LOG("[nvn] ReportCounter type %u\n", type); }
            int mode = vk_counter_mode();
            static uint64_t frame_seen = ~0ull, frame_ts;
            extern uint64_t vk_frame_counter(void);
            if (mode == 1 && vk_frame_counter() != frame_seen) { frame_seen = vk_frame_counter(); frame_ts = ns_to_gpu_ticks(plat_now_ns()); }
            if (!out) break;
            out[0] = 0;
            out[1] = mode == 1 ? frame_ts : mode == 3 ? SDL_GetTicksNS() : ns_to_gpu_ticks(plat_now_ns());  // (3 legacy: ns, the old bug)
            static uint64_t *prev_out;  // (frame pacing: the pair's start)
            if (mode == 0 || mode == 2) out[1] = pace_end_counter(prev_out, out, out[1]);
            prev_out = out;
            {   // (test) LARECOMP_TEST_COUNTER_DELAY_MS=<ms>: stall the host after a frame's first counter - a slow CPU
                static double delay = -1;
                static uint64_t delayed_frame = ~0ull;
                if (delay < 0) delay = getenv("LARECOMP_TEST_COUNTER_DELAY_MS") ? atof(getenv("LARECOMP_TEST_COUNTER_DELAY_MS")) : 0;
                if (delay > 0 && vk_frame_counter() != delayed_frame) {
                    delayed_frame = vk_frame_counter();
                    SDL_DelayPrecise((Uint64)(delay * 1e6));
                }
            }
            if (mode == 2 && type == 0) {  // (type 0: timestamp)
                VkCommandBuffer cb = vk_cmd();  // (first: may begin the buffer, which resets the slot's queries)
                CounterQ *q = &cq[cur_sub];
                pthread_mutex_lock(&cq_lock);
                if (q->pool && q->n < CQ_MAX) {
                    vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, q->pool, q->n);
                    q->out[q->n++] = out;
                }
                pthread_mutex_unlock(&cq_lock);
            }
            break;
        }
        case CMD_COPY_TEXTURE_TO_TEXTURE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], r->x[6]); break; }
        case CMD_DOWNSAMPLE: { const TexRefRec *t = x; copy_texture(&t[0], &t[1], 1); break; }
        case CMD_COPY_BUFFER_TO_TEXTURE: copy_buffer_to_texture(r->x[0], x); break;
        case CMD_COPY_TEXTURE_TO_BUFFER: copy_texture_to_buffer(x, r->x[3]); break;
        case CMD_COPY_BUFFER_TO_BUFFER:
            wait_why = "buffer -> buffer copy";
            renderer_flush();
            memmove((void *)(uintptr_t)r->x[1], (void *)(uintptr_t)r->x[0], r->x[2]);
            break;
        default: break;
        }
        p += r->size;
    }
}

void backend_execute(uint64_t handle) {
    PROF_T0();
    if (!++ww_list) ww_list = 1;  // (0: never queried)
    ww_in_list = true;
    exec_list(handle, 0);
    ww_in_list = false;
    PROF_ADD(prof_exec);
    if (prof) cum_exec += SDL_GetTicksNS() - prof_t0_;
    static int sync_submit = -1;
    if (sync_submit < 0) sync_submit = getenv("LARECOMP_SYNC_SUBMIT") ? 1 : 0;
    if (sync_submit) renderer_flush();
    else renderer_submit();
}

// ================================================================== GPU warm-up (AC_GPU_WARM=<shader folder>)
// The launcher runs the game's executable in this mode before the game, with the game closed (the NVIDIA driver files
// its Vulkan cache under the executable's name, so it is the game's own cache that fills). It compiles, for this GPU
// and driver, what the game would otherwise compile while it plays: the programs of the recorded variants
// (cache/programs.bin) and their shader libraries, a shader library for every vertex and fragment shader of the game
// (game/shaders, the keys nearly all draws use), and every recorded pipeline (cache/pipelines.bin). The results go to
// the driver's cache and cache/vk_pipelines.bin; in the game the same builds are then cache hits (well under a
// millisecond). AC_GPU_WARM_STATE: the launcher's description of the inputs; with this GPU and driver it is kept in
// cache/gpu_warm.txt, and an unchanged state skips the work. Prints "progress <done> <total>" and "done ..." lines.
// AC_GPU_WARM_PREV and AC_GPU_WARM_FROM (shader_cache.py, when the build is the same and the record files only grew,
// as the game appends to them in play): the state the last warm-up would have kept with the record files as they were
// then, and their sizes then ("<programs.bin bytes> <pipelines.bin bytes>"). If that is the state on file, only the
// records added since are compiled, with the programs their new pipelines need (the rest is in the caches already).
typedef struct { uint8_t *code; size_t size; uint32_t const_offset; int stage; uint64_t hash; } GwCode;
static GwCode *gw_codes;
static int gw_ncodes, gw_cap;
static _Atomic int gw_done;

static uint8_t *gw_read(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? malloc((size_t)n + 0x10000) : NULL;  // (zero tail: the translator may look past the code)
    if (d) { memset(d + n, 0, 0x10000); if (fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; } }
    fclose(f);
    if (d) *size = (size_t)n;
    return d;
}

// <name>.code / <name>.control pairs (astral_chain/shader_cache.py), as shader_precompile.c load_codes reads them
static void gw_file(const char *path, void *ctx) {
    (void)ctx;
    size_t len = strlen(path);
    if (len < 6 || strcmp(path + len - 5, ".code")) return;
    char cpath[1024];
    snprintf(cpath, sizeof cpath, "%.*s.control", (int)(len - 5), path);
    size_t size = 0, csize = 0;
    uint8_t *code = gw_read(path, &size), *control = gw_read(cpath, &csize);
    if (!code || !control || csize < 0x704) { free(code); free(control); return; }
    uint32_t co, magic = 0, sph0 = 0;
    memcpy(&co, control + 0x700, 4);
    free(control);
    if (size >= 0x34) { memcpy(&magic, code, 4); memcpy(&sph0, code + 0x30, 4); }
    int type = (sph0 >> 10) & 0xF, stage = magic != 0x12345678 ? 5 : type == 1 ? 0 : type == 5 ? 1 : -1;
    if ((stage != 0 && stage != 1) || !co || co > size) { free(code); return; }
    if (gw_ncodes == gw_cap) { gw_cap = gw_cap ? gw_cap * 2 : 1024; gw_codes = realloc(gw_codes, (size_t)gw_cap * sizeof *gw_codes); }
    gw_codes[gw_ncodes++] = (GwCode){code, size, co, stage, hmix(0x9E3779B97F4A7C15ull + (uint64_t)stage, code, co)};
}
static void gw_load(const char *folder) {
    extern void vk_walk_files(const char *folder, void (*fn)(const char *path, void *ctx), void *ctx);
    vk_walk_files(folder, gw_file, NULL);
}

static GwCode *gw_find(uint64_t hash, int stage) {
    for (int i = 0; i < gw_ncodes; i++) if (gw_codes[i].stage == stage && gw_codes[i].hash == hash) return &gw_codes[i];
    return NULL;
}

// records of cache/programs.bin within its first bytes, counted as warm_load reads them
static int gw_old_programs(long bytes) {
    FILE *f = fopen("cache/programs.bin", "rb");
    if (!f) return 0;
    int n = 0;
    uint32_t magic = 0;
    if (fread(&magic, 4, 1, f) == 1 && magic == WARM_MAGIC)
        for (uint32_t word; n < warm_n && fread(&word, 4, 1, f) == 1;) {
            if (word == WARM_MAGIC) continue;
            if (fseek(f, (long)sizeof(WarmRec) - 4, SEEK_CUR) || ftell(f) > bytes) break;
            n++;
        }
    fclose(f);
    return n;
}
// prec entries made from the records within the first bytes of cache/pipelines.bin (prec_load: in file order)
static int gw_old_pipelines(long bytes) {
    FILE *f = fopen("cache/pipelines.bin", "rb");
    if (!f) return 0;
    int n = 0;
    uint32_t magic = 0;
    PipeRec r;
    if (fread(&magic, 4, 1, f) == 1 && magic == PREC_MAGIC)
        while (ftell(f) + (long)sizeof r <= bytes && fread(&r, sizeof r, 1, f) == 1) {
            int i = prec_find(r.ident, &r.k);
            if (i >= n) n = i + 1;
        }
    fclose(f);
    return n;
}

static void gw_stage_job(void *arg) {
    gpl_warm_job(arg);  // (frees arg)
    atomic_fetch_add(&gw_done, 1);
}

typedef struct { PipeKey k; } GwPipe;
static void gw_pipe_job(void *arg) {
    GwPipe *g = arg;
    VkPipeline p = VK_NULL_HANDLE;
    if (create_pipeline(&g->k, &p) == VK_SUCCESS) vkDestroyPipeline(g_vk.dev, p, NULL);
    free(g);
    atomic_fetch_add(&gw_done, 1);
}

int vk_gpu_warm(const char *folder) {
    extern void vk_init_for_warm(void);
    extern void vk_pipeline_cache_save(void);
    uint64_t t0 = SDL_GetTicksNS();
    vk_init_for_warm();
    char gpu[512], state[1024], old[1024] = {0};
    snprintf(gpu, sizeof gpu, " | %s %u.%u.%u (%08x) | shader libraries %d", g_vk.props.deviceName,
             VK_API_VERSION_MAJOR(g_vk.props.driverVersion), VK_API_VERSION_MINOR(g_vk.props.driverVersion),
             VK_API_VERSION_PATCH(g_vk.props.driverVersion), g_vk.props.driverVersion, g_vk.gpl);
    snprintf(state, sizeof state, "%s%s", getenv("AC_GPU_WARM_STATE") ? getenv("AC_GPU_WARM_STATE") : "", gpu);
    {
        FILE *f = fopen("cache/gpu_warm.txt", "rb");
        if (f) { size_t n = fread(old, 1, sizeof old - 1, f); old[n] = 0; fclose(f); }
        if (!strcmp(old, state)) { printf("done skipped (unchanged)\n"); fflush(stdout); return 0; }
    }
    long from_prog = -1, from_pipe = -1;
    {
        const char *prev = getenv("AC_GPU_WARM_PREV"), *from = getenv("AC_GPU_WARM_FROM");
        char was[1024];
        if (prev && from) snprintf(was, sizeof was, "%s%s", prev, gpu);
        if (!prev || !from || strcmp(old, was) || sscanf(from, "%ld %ld", &from_prog, &from_pipe) != 2) from_prog = from_pipe = -1;
    }
    gw_load(folder);
    if (!gw_ncodes) { printf("done failed (no shaders in %s)\n", folder); fflush(stdout); return 1; }
    warm_load();
    prec_load();
    bool added = from_prog >= 0;  // (only what was added since the last warm-up)
    int old_progs = added ? gw_old_programs(from_prog) : 0, old_pipes = added ? gw_old_pipelines(from_pipe) : 0;
    // programs of the recorded variants (their shader libraries come with them: compile_program)
    Prog **progs = calloc((size_t)(warm_n > 0 ? warm_n : 1), sizeof *progs);
    int nprogs = 0;
    for (int i = 0; i < warm_n; i++) {
        if (i < old_progs) {  // (an older one: only if a new pipeline needs it)
            uint64_t sh[2] = {warm_rec[i].vh, warm_rec[i].fh};
            uint64_t ident = hmix(hmix(hmix(0xcbf29ce484222325ull, sh, sizeof sh), &warm_rec[i].vkey, sizeof warm_rec[i].vkey),
                                  &warm_rec[i].fkey, sizeof warm_rec[i].fkey);
            bool needed = false;
            for (int32_t r = prec_head[ident & 4095]; r && !needed; r = prec_next[r - 1]) needed = r - 1 >= old_pipes && prec[r - 1].ident == ident;
            if (!needed) continue;
        }
        GwCode *vs = gw_find(warm_rec[i].vh, 0), *fs = warm_rec[i].fh ? gw_find(warm_rec[i].fh, 1) : NULL;
        if (!vs || (warm_rec[i].fh && !fs)) continue;
        NvnProgram pair;
        memset(&pair, 0, sizeof pair);
        pair.stages[0].code_addr = (uint64_t)(uintptr_t)vs->code;
        pair.stages[0].const_offset = vs->const_offset;
        pair.stage_mask = 1;
        if (fs) { pair.stages[1].code_addr = (uint64_t)(uintptr_t)fs->code; pair.stages[1].const_offset = fs->const_offset; pair.stage_mask |= 2; }
        ShaderKey vk = warm_rec[i].vkey, fk = warm_rec[i].fkey;
        Prog *p = get_program_ex(&pair, &vk, &fk, false);
        if (p) progs[nprogs++] = p;
    }
    // a shader library for every shader, as the game builds them when it loads one (gpl_warm_stage)
    int nstage = 0;
    if (g_vk.gpl && !added) {
        for (int i = 0; i < gw_ncodes; i++) {
            GplWarm *w = malloc(sizeof *w);
            w->code = (uint64_t)(uintptr_t)gw_codes[i].code;
            w->cmax = gw_codes[i].const_offset;
            w->stage = gw_codes[i].stage;
            run_job(gw_stage_job, w);
            nstage++;
        }
    }
    int total = nprogs + nstage + (prec_n > old_pipes ? prec_n - old_pipes : 0), queued_pipes = 0, last = -1;
    printf("warm-up: %d shaders, %d recorded programs, %d recorded pipelines%s\n", gw_ncodes, nprogs, total - nprogs - nstage,
           added ? " (added since the last time)" : "");
    fflush(stdout);
    // the recorded pipelines, once their program is ready
    bool *queued = calloc((size_t)(prec_n > 0 ? prec_n : 1), 1);
    for (int r = 0; r < old_pipes && r < prec_n; r++) queued[r] = true;
    for (;;) {
        int ready = 0;
        for (int i = 0; i < nprogs; i++) {
            if (atomic_load(&progs[i]->state) == 0) continue;
            ready++;
            if (atomic_load(&progs[i]->state) != 1) continue;
            for (int r = 0; r < prec_n; r++) {
                if (queued[r] || prec[r].ident != progs[i]->ident) continue;
                queued[r] = true;
                GwPipe *g = malloc(sizeof *g);
                g->k = prec[r].k;
                if (!pipe_key_for_device(&g->k)) { free(g); atomic_fetch_add(&gw_done, 1); continue; }
                g->k.prog = progs[i];
                run_job(gw_pipe_job, g);
                queued_pipes++;
            }
        }
        int done = ready + atomic_load(&gw_done);
        if (done / 25 != last / 25) { last = done; printf("progress %d %d\n", done, total); fflush(stdout); }
        int pipes_done = 0;
        for (int r = old_pipes; r < prec_n; r++) pipes_done += queued[r];
        if (ready == nprogs && atomic_load(&gw_done) >= nstage + pipes_done && SDL_GetTicksNS() - t0 > 1000000ull) {
            // (records whose program never got made: counted as done)
            if (done < total) { printf("progress %d %d\n", total, total); fflush(stdout); }
            break;
        }
        SDL_Delay(20);
    }
    vk_pipeline_cache_save();
    FILE *f = fopen("cache/gpu_warm.txt", "wb");
    if (f) { fputs(state, f); fclose(f); }
    printf("done %d programs, %d shader libraries, %d pipelines in %.1f s%s\n", nprogs, nstage, queued_pipes, (SDL_GetTicksNS() - t0) / 1e9,
           added ? " (added since the last time)" : "");
    fflush(stdout);
    return 0;
}

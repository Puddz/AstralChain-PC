/* Astral Chain shim for LASRecomp's runtime/rt.h: only what the NVN layer uses. */
#pragma once
#include <stdbool.h>
#include <stdio.h>
#include "cpu.h"
#include "platform.h"
#define LOG(...) fprintf(stderr, __VA_ARGS__)
void nvn_glue_fatal(const char *message) __attribute__((noreturn));
#define FATAL(...) (fprintf(stderr, "FATAL: " __VA_ARGS__), nvn_glue_fatal("las-fatal"))
typedef void (*HostFn)(Cpu *c);
void guest_call(Cpu *c, uint64_t addr);  /* nvn_hle.h: nested guest callback */
/* Guest heap (runtime heap_base, HEAP_LIMIT); see nvn_hle.h. */
uint64_t nvn_heap_base(void);
#define HEAP_BASE nvn_heap_base()
#define HEAP_MAX 0x100000000ull

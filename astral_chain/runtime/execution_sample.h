#pragma once
#include <stdint.h>
#include <stdio.h>
#ifndef EXECUTION_SAMPLE_STEPS
#define EXECUTION_SAMPLE_STEPS 0
#endif
/* Optional diagnostics at existing scheduler checkpoints, before context switch.
 * No guest memory reads or register writes. Rate is measured in guest steps. */
static void execution_sample(const Cpu *c,uint64_t step,unsigned thread) {
#if EXECUTION_SAMPLE_STEPS > 0
 static uint64_t previous;
 static unsigned sampled;
 if(sampled && (step<=previous || step-previous<EXECUTION_SAMPLE_STEPS)) return;
 previous=step;sampled=1;
 printf("EXECUTION_SAMPLE step=%llu thread=%u pc=0x%llx sp=0x%llx",
  (unsigned long long)step,thread,(unsigned long long)c->pc,(unsigned long long)c->sp);
 for(unsigned i=0;i<31;i++) printf(" x%u=0x%llx",i,(unsigned long long)c->x[i]);
 putchar('\n');
#else
 (void)c;(void)step;(void)thread;
#endif
}

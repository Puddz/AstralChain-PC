/* Astral Chain shim for LASRecomp's runtime/platform.h, implemented in nvn_glue.c. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
uint64_t plat_now_ns(void);
void plat_sleep_until_ns(uint64_t t);
/* Pages of [addr,addr+size) written since the last query; see nvn_glue.c. */
int plat_written_pages(uint64_t addr, uint64_t size, uint64_t *pages, int max, uint64_t *page);

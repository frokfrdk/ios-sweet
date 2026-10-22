#pragma once
#include <stdint.h>

struct fdt_range {
	uint64_t base, size;
};

/* Collect the reg ranges of the top-level /memory nodes (filled in by ABL). Returns the count. */
int fdt_memory(uintptr_t fdt, struct fdt_range *r, int max);

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

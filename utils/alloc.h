/* Manual-allocation primitives for the Khudra runtime.
 *
 * Compiled as C11 and consumed from C++ across the extern "C" boundary. These
 * back the `manual` memory strategy and the Procedure engine's raw chunks;
 * the counters exist so the Phase 5 stress tests can assert "zero leaks"
 * without depending on an external allocator. */
#ifndef KHU_UTILS_ALLOC_H
#define KHU_UTILS_ALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocates `size` bytes of zeroed manual memory. Returns NULL on failure. */
void* khu_manual_alloc(size_t size);

/* Frees a pointer previously returned by khu_manual_alloc. NULL is a no-op. */
void khu_manual_free(void* pointer);

/* Resizes a block, preserving as much of its contents as still fits and zeroing
 * anything new. NULL allocates; a `size` of 0 frees and returns NULL. Returns
 * NULL without touching the block when it cannot allocate, and when `pointer`
 * is not one of ours. */
void* khu_manual_realloc(void* pointer, size_t size);

/* How many bytes a block holds, or 0 when `pointer` is NULL or was not handed
 * out by khu_manual_alloc. This is what lets the standard library's memory
 * operations bounds-check a raw pointer instead of trusting a count. */
size_t khu_manual_block_size(const void* pointer);

/* Live-allocation accounting, used by leak checks in the test suite. */
size_t khu_manual_live_bytes(void);
size_t khu_manual_live_blocks(void);

/* Total blocks handed out since process start (never decreases). */
size_t khu_manual_total_blocks(void);

/* Resets the counters. Only valid when no manual blocks are live. */
void khu_manual_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* KHU_UTILS_ALLOC_H */

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

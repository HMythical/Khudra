#include "alloc.h"

#include <stdlib.h>
#include <string.h>

/* Every manual block carries a small header so free() can report its size back
 * to the counters, and so a corrupted/foreign pointer is caught rather than
 * silently miscounted. */
#define KHU_ALLOC_MAGIC 0x4b48554duL /* "KHUM" */

typedef struct KhuBlockHeader {
    size_t size;
    unsigned long magic;
} KhuBlockHeader;

/* Header size rounded up so the payload stays suitably aligned for any type. */
#define KHU_MAX_ALIGN (sizeof(long double))
#define KHU_HEADER_SIZE \
    ((((sizeof(KhuBlockHeader) + KHU_MAX_ALIGN - 1) / KHU_MAX_ALIGN) * KHU_MAX_ALIGN))

static size_t g_live_bytes = 0;
static size_t g_live_blocks = 0;
static size_t g_total_blocks = 0;

void* khu_manual_alloc(size_t size) {
    unsigned char* raw;
    KhuBlockHeader* header;

    if (size == 0) size = 1;
    raw = (unsigned char*)calloc(1, KHU_HEADER_SIZE + size);
    if (raw == NULL) return NULL;

    header = (KhuBlockHeader*)raw;
    header->size = size;
    header->magic = KHU_ALLOC_MAGIC;

    g_live_bytes += size;
    g_live_blocks += 1;
    g_total_blocks += 1;
    return raw + KHU_HEADER_SIZE;
}

void khu_manual_free(void* pointer) {
    unsigned char* raw;
    KhuBlockHeader* header;

    if (pointer == NULL) return;
    raw = (unsigned char*)pointer - KHU_HEADER_SIZE;
    header = (KhuBlockHeader*)raw;
    if (header->magic != KHU_ALLOC_MAGIC) {
        /* Not ours: refusing to touch it is safer than corrupting the heap. */
        return;
    }

    g_live_bytes -= header->size;
    g_live_blocks -= 1;
    header->magic = 0;
    free(raw);
}

size_t khu_manual_live_bytes(void) { return g_live_bytes; }
size_t khu_manual_live_blocks(void) { return g_live_blocks; }
size_t khu_manual_total_blocks(void) { return g_total_blocks; }

void khu_manual_reset_stats(void) {
    if (g_live_blocks != 0) return;
    g_live_bytes = 0;
    g_total_blocks = 0;
}

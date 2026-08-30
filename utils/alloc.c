#include "alloc.h"

#include <stdint.h>
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

/* The live-block registry.
 *
 * The header records a block's size, but reading it back is only safe while the
 * block is alive -- and the question "is this pointer alive?" is exactly the one
 * a double release asks. Reading a freed header to answer it is undefined, and
 * a sanitizer says so. So liveness is tracked outside the blocks: an open-
 * addressed table of the payload pointers currently handed out.
 *
 * `khu_manual_block_size` and `khu_manual_free` both go through it, so neither
 * ever touches memory it has already given back. */
#define KHU_SLOT_EMPTY ((void*)0)
#define KHU_SLOT_DEAD ((void*)1)

typedef struct KhuSlot {
    void* pointer;
    size_t size;
} KhuSlot;

static KhuSlot* g_slots = NULL;
static size_t g_slot_capacity = 0;
static size_t g_slot_used = 0;   /* live entries */
static size_t g_slot_filled = 0; /* live entries plus tombstones */

static size_t slot_hash(const void* pointer, size_t capacity) {
    /* The low bits of an allocation are mostly alignment padding, so they are
     * shifted out before mixing. */
    size_t bits = (size_t)(uintptr_t)pointer >> 4;
    bits ^= bits >> 15;
    bits *= (size_t)0x2545f4914f6cdd1dULL;
    bits ^= bits >> 27;
    return bits & (capacity - 1);
}

static void registry_insert_into(KhuSlot* slots, size_t capacity, void* pointer, size_t size) {
    size_t at = slot_hash(pointer, capacity);
    while (slots[at].pointer != KHU_SLOT_EMPTY && slots[at].pointer != KHU_SLOT_DEAD) {
        at = (at + 1) & (capacity - 1);
    }
    slots[at].pointer = pointer;
    slots[at].size = size;
}

/* Returns 0 when the table could not grow, which makes the allocation fail
 * rather than lose track of a block. */
static int registry_reserve(void) {
    size_t capacity;
    KhuSlot* fresh;
    size_t i;

    if (g_slots != NULL && (g_slot_filled + 1) * 10 < g_slot_capacity * 7) return 1;

    capacity = g_slot_capacity ? g_slot_capacity * 2 : 64;
    /* Rehashing drops the tombstones, so a table full of them shrinks back. */
    while (capacity > 64 && (g_slot_used + 1) * 10 < capacity * 3) capacity /= 2;
    fresh = (KhuSlot*)calloc(capacity, sizeof(KhuSlot));
    if (fresh == NULL) return 0;

    for (i = 0; i < g_slot_capacity; ++i) {
        void* pointer = g_slots[i].pointer;
        if (pointer == KHU_SLOT_EMPTY || pointer == KHU_SLOT_DEAD) continue;
        registry_insert_into(fresh, capacity, pointer, g_slots[i].size);
    }
    free(g_slots);
    g_slots = fresh;
    g_slot_capacity = capacity;
    g_slot_filled = g_slot_used;
    return 1;
}

static KhuSlot* registry_find(const void* pointer) {
    size_t at;
    size_t stepped;

    if (g_slots == NULL || pointer == NULL) return NULL;
    at = slot_hash(pointer, g_slot_capacity);
    for (stepped = 0; stepped < g_slot_capacity; ++stepped) {
        if (g_slots[at].pointer == KHU_SLOT_EMPTY) return NULL;
        if (g_slots[at].pointer == pointer) return &g_slots[at];
        at = (at + 1) & (g_slot_capacity - 1);
    }
    return NULL;
}

/* Releases the table once nothing is live, so the accounting itself does not
 * look like a leak at exit. */
static void registry_compact_if_empty(void) {
    if (g_slot_used != 0) return;
    free(g_slots);
    g_slots = NULL;
    g_slot_capacity = 0;
    g_slot_filled = 0;
}

void* khu_manual_alloc(size_t size) {
    unsigned char* raw;
    KhuBlockHeader* header;

    if (size == 0) size = 1;
    if (!registry_reserve()) return NULL;
    raw = (unsigned char*)calloc(1, KHU_HEADER_SIZE + size);
    if (raw == NULL) return NULL;

    header = (KhuBlockHeader*)raw;
    header->size = size;
    header->magic = KHU_ALLOC_MAGIC;

    registry_insert_into(g_slots, g_slot_capacity, raw + KHU_HEADER_SIZE, size);
    g_slot_used += 1;
    g_slot_filled += 1;

    g_live_bytes += size;
    g_live_blocks += 1;
    g_total_blocks += 1;
    return raw + KHU_HEADER_SIZE;
}

void khu_manual_free(void* pointer) {
    KhuSlot* slot;
    unsigned char* raw;

    if (pointer == NULL) return;
    /* The registry, not the header: a pointer that is not live may not be there
     * to read. Not ours, or already released -- either way, refusing to touch it
     * is safer than corrupting the heap. */
    slot = registry_find(pointer);
    if (slot == NULL) return;

    g_live_bytes -= slot->size;
    g_live_blocks -= 1;
    slot->pointer = KHU_SLOT_DEAD;
    slot->size = 0;
    g_slot_used -= 1;

    raw = (unsigned char*)pointer - KHU_HEADER_SIZE;
    ((KhuBlockHeader*)raw)->magic = 0;
    free(raw);
    registry_compact_if_empty();
}

size_t khu_manual_block_size(const void* pointer) {
    const KhuSlot* slot = registry_find(pointer);
    return slot ? slot->size : 0;
}

void* khu_manual_realloc(void* pointer, size_t size) {
    size_t old_size;
    void* fresh;

    if (pointer == NULL) return khu_manual_alloc(size);
    old_size = khu_manual_block_size(pointer);
    /* Not ours: the header is not there to read, so there is nothing safe to
     * copy out of it. */
    if (old_size == 0) return NULL;

    if (size == 0) {
        khu_manual_free(pointer);
        return NULL;
    }

    fresh = khu_manual_alloc(size);
    if (fresh == NULL) return NULL;
    memcpy(fresh, pointer, size < old_size ? size : old_size);
    khu_manual_free(pointer);
    return fresh;
}

size_t khu_manual_live_bytes(void) { return g_live_bytes; }
size_t khu_manual_live_blocks(void) { return g_live_blocks; }
size_t khu_manual_total_blocks(void) { return g_total_blocks; }

void khu_manual_reset_stats(void) {
    if (g_live_blocks != 0) return;
    g_live_bytes = 0;
    g_total_blocks = 0;
}

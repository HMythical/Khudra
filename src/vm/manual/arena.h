// Manual arenas.
//
// One arena per class (KHU-PLAN.md: "bump/block allocators per class or
// call-site"). Because every object of a class is the same size, an arena is a
// bump pointer over large blocks plus a free list of released chunks -- no
// size-class bookkeeping, and a released object is reused immediately.
//
// Blocks come from the C runtime (utils/alloc.c) so its live-byte accounting
// covers every manual byte in the process, which is what the leak checks read.
#ifndef KHU_VM_MANUAL_ARENA_H
#define KHU_VM_MANUAL_ARENA_H

#include <cstddef>

#include "util/array.h"

namespace khu::vm {

class ManualArena {
public:
    // `chunk_size` is the size of one object; blocks hold many.
    explicit ManualArena(std::size_t chunk_size, std::size_t chunks_per_block = 64);
    ManualArena(const ManualArena&) = delete;
    ManualArena& operator=(const ManualArena&) = delete;
    ~ManualArena();

    // Returns zeroed storage for one chunk, or nullptr.
    void* allocate();
    // Returns a chunk to the free list. It stays mapped, ready to be reused.
    void release(void* chunk);

    // Frees every block. Any outstanding chunk becomes invalid.
    void reset();

    std::size_t chunk_size() const { return chunk_size_; }
    std::size_t live_chunks() const { return live_chunks_; }
    std::size_t block_count() const { return blocks_.size(); }
    std::size_t reserved_bytes() const { return blocks_.size() * block_bytes_; }

private:
    struct FreeChunk {
        FreeChunk* next;
    };

    bool add_block();

    util::Array<void*> blocks_;
    FreeChunk* free_list_ = nullptr;
    unsigned char* bump_ = nullptr;
    std::size_t bump_remaining_ = 0;
    std::size_t chunk_size_;
    std::size_t chunks_per_block_;
    std::size_t block_bytes_;
    std::size_t live_chunks_ = 0;
};

}  // namespace khu::vm

#endif  // KHU_VM_MANUAL_ARENA_H

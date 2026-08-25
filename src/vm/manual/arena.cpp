#include "manual/arena.h"

#include <cstring>

extern "C" {
#include "alloc.h"
}

namespace khu::vm {
namespace {

// A released chunk stores the free-list link in its own first bytes, so it must
// be at least pointer-sized and pointer-aligned.
std::size_t round_up_chunk(std::size_t bytes) {
    std::size_t minimum = sizeof(void*);
    if (bytes < minimum) bytes = minimum;
    std::size_t align = alignof(std::max_align_t);
    return ((bytes + align - 1) / align) * align;
}

}  // namespace

ManualArena::ManualArena(std::size_t chunk_size, std::size_t chunks_per_block)
    : chunk_size_(round_up_chunk(chunk_size)),
      chunks_per_block_(chunks_per_block ? chunks_per_block : 1) {
    block_bytes_ = chunk_size_ * chunks_per_block_;
}

ManualArena::~ManualArena() { reset(); }

bool ManualArena::add_block() {
    void* block = khu_manual_alloc(block_bytes_);
    if (!block) return false;
    blocks_.push(block);
    bump_ = static_cast<unsigned char*>(block);
    bump_remaining_ = block_bytes_;
    return true;
}

void* ManualArena::allocate() {
    // A released chunk is the cheapest thing to hand back.
    if (free_list_) {
        FreeChunk* chunk = free_list_;
        free_list_ = chunk->next;
        std::memset(chunk, 0, chunk_size_);
        ++live_chunks_;
        return chunk;
    }

    if (bump_remaining_ < chunk_size_ && !add_block()) return nullptr;

    void* result = bump_;
    bump_ += chunk_size_;
    bump_remaining_ -= chunk_size_;
    std::memset(result, 0, chunk_size_);
    ++live_chunks_;
    return result;
}

void ManualArena::release(void* chunk) {
    if (!chunk) return;
    auto* node = static_cast<FreeChunk*>(chunk);
    node->next = free_list_;
    free_list_ = node;
    if (live_chunks_ > 0) --live_chunks_;
}

void ManualArena::reset() {
    for (void* block : blocks_) khu_manual_free(block);
    blocks_.clear();
    free_list_ = nullptr;
    bump_ = nullptr;
    bump_remaining_ = 0;
    live_chunks_ = 0;
}

}  // namespace khu::vm

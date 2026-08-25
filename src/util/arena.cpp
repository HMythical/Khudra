#include "util/arena.h"

#include <cstdlib>
#include <cstring>

namespace khu::util {

Arena::Arena(std::size_t block_size) : block_size_(block_size ? block_size : 4096) {}

Arena::~Arena() { reset(); }

void Arena::add_block(std::size_t minimum) {
    std::size_t capacity = block_size_;
    while (capacity < minimum) capacity *= 2;
    auto* memory = static_cast<std::uint8_t*>(std::malloc(capacity));
    if (!memory) std::abort();
    blocks_.push(Block{memory, capacity, 0});
}

void* Arena::allocate(std::size_t bytes, std::size_t align) {
    if (bytes == 0) bytes = 1;
    if (blocks_.empty()) add_block(bytes + align);

    Block* block = &blocks_.back();
    std::size_t offset = block->used;
    std::size_t misalign = reinterpret_cast<std::uintptr_t>(block->memory + offset) % align;
    if (misalign != 0) offset += align - misalign;

    if (offset + bytes > block->capacity) {
        add_block(bytes + align);
        block = &blocks_.back();
        offset = 0;
        misalign = reinterpret_cast<std::uintptr_t>(block->memory) % align;
        if (misalign != 0) offset += align - misalign;
    }

    void* result = block->memory + offset;
    block->used = offset + bytes;
    bytes_allocated_ += bytes;
    return result;
}

std::string_view Arena::copy_string(std::string_view text) {
    char* memory = static_cast<char*>(allocate(text.size() + 1, alignof(char)));
    if (!text.empty()) std::memcpy(memory, text.data(), text.size());
    memory[text.size()] = '\0';
    return std::string_view(memory, text.size());
}

void Arena::reset() {
    // Destructors run newest-first so an object may still reference anything it
    // was constructed after.
    for (std::size_t i = cleanups_.size(); i > 0; --i) {
        const Cleanup& cleanup = cleanups_[i - 1];
        cleanup.destroy(cleanup.object);
    }
    cleanups_.clear();
    for (Block& block : blocks_) std::free(block.memory);
    blocks_.clear();
    bytes_allocated_ = 0;
}

}  // namespace khu::util

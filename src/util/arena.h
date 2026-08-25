// Bump-pointer arena for compiler internals (AST nodes, symbols, scopes).
//
// The whole front end has a single lifetime: everything allocated while
// compiling one unit dies together. An arena removes per-node ownership
// bookkeeping and makes AST nodes cheap to allocate and safe to reference by
// raw pointer. Types that need cleanup are registered so ~Arena still runs
// their destructors.
#ifndef KHU_UTIL_ARENA_H
#define KHU_UTIL_ARENA_H

#include <cstddef>
#include <cstdint>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

#include "util/array.h"

namespace khu::util {

class Arena {
public:
    explicit Arena(std::size_t block_size = 64 * 1024);
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    ~Arena();

    void* allocate(std::size_t bytes, std::size_t align);

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* memory = allocate(sizeof(T), alignof(T));
        T* value = new (memory) T(std::forward<Args>(args)...);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            cleanups_.push(Cleanup{value, &destroy_as<T>});
        }
        return value;
    }

    // Copies `text` into the arena and returns a view of the copy. The result
    // is NUL-terminated so it can be handed to C APIs.
    std::string_view copy_string(std::string_view text);

    // Runs every registered destructor and releases all blocks.
    void reset();

    std::size_t bytes_allocated() const { return bytes_allocated_; }
    std::size_t block_count() const { return blocks_.size(); }

private:
    struct Block {
        std::uint8_t* memory;
        std::size_t capacity;
        std::size_t used;
    };

    struct Cleanup {
        void* object;
        void (*destroy)(void*);
    };

    template <typename T>
    static void destroy_as(void* object) {
        static_cast<T*>(object)->~T();
    }

    void add_block(std::size_t minimum);

    Array<Block> blocks_;
    Array<Cleanup> cleanups_;
    std::size_t block_size_;
    std::size_t bytes_allocated_ = 0;
};

}  // namespace khu::util

#endif  // KHU_UTIL_ARENA_H

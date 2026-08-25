// Open-addressing string-keyed hash map.
//
// Used for symbol tables, the string interner and the keyword table. Keys are
// copied into the map, so callers may hand it transient string_views.
#ifndef KHU_UTIL_HASHMAP_H
#define KHU_UTIL_HASHMAP_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "util/array.h"

namespace khu::util {

inline std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 0x100000001b3ull;
    }
    return hash;
}

template <typename V>
class StringMap {
public:
    struct Entry {
        std::string key;
        V value{};
        bool occupied = false;
    };

    StringMap() { slots_.resize(16); }

    // Inserts or overwrites. Returns a pointer to the stored value.
    V* insert(std::string_view key, V value) {
        if ((count_ + 1) * 4 >= slots_.size() * 3) grow();
        std::size_t index = probe(key);
        Entry& slot = slots_[index];
        if (!slot.occupied) {
            slot.occupied = true;
            slot.key = std::string(key);
            ++count_;
        }
        slot.value = std::move(value);
        return &slot.value;
    }

    // Inserts only when absent. Returns nullptr when the key already exists.
    V* insert_new(std::string_view key, V value) {
        if (find(key)) return nullptr;
        return insert(key, std::move(value));
    }

    V* find(std::string_view key) {
        Entry& slot = slots_[probe(key)];
        return slot.occupied ? &slot.value : nullptr;
    }

    const V* find(std::string_view key) const {
        const Entry& slot = slots_[probe(key)];
        return slot.occupied ? &slot.value : nullptr;
    }

    bool contains(std::string_view key) const { return find(key) != nullptr; }

    std::size_t size() const { return count_; }
    bool empty() const { return count_ == 0; }

    // Iteration visits occupied slots in unspecified order.
    template <typename Fn>
    void for_each(Fn&& fn) const {
        for (const Entry& slot : slots_) {
            if (slot.occupied) fn(std::string_view(slot.key), slot.value);
        }
    }

private:
    std::size_t probe(std::string_view key) const {
        std::size_t mask = slots_.size() - 1;
        std::size_t index = static_cast<std::size_t>(fnv1a(key)) & mask;
        while (slots_[index].occupied && slots_[index].key != key) {
            index = (index + 1) & mask;
        }
        return index;
    }

    void grow() {
        Array<Entry> old = std::move(slots_);
        slots_ = Array<Entry>();
        slots_.resize(old.size() * 2);
        count_ = 0;
        for (Entry& slot : old) {
            if (!slot.occupied) continue;
            std::size_t index = probe(slot.key);
            slots_[index].occupied = true;
            slots_[index].key = std::move(slot.key);
            slots_[index].value = std::move(slot.value);
            ++count_;
        }
    }

    Array<Entry> slots_;
    std::size_t count_ = 0;
};

}  // namespace khu::util

#endif  // KHU_UTIL_HASHMAP_H

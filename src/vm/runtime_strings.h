// The store for strings a program computes rather than writes.
//
// A string literal lives in the constant pool: it is immutable, it is in the
// image, and every `Value` that names it can point straight at it. A string a
// program *produces* -- `io.readLine`, a number rendered by
// `khuStdConv.toString`, a substring -- has nowhere to live, so the backend
// that produced it owns it and the `Value` points into that.
//
// Both backends keep their own store, because a store's lifetime is a run's,
// but the allocation and release path is this one file: a leak in a runtime
// string would be a leak in both backends, and one of them fixing it quietly
// is exactly the drift the byte-identical invariant is meant to rule out.
//
// The store is deliberately append-only. Nothing tracks whether a produced
// string is still reachable, so nothing frees one early; they are released
// together when the run ends. That is the same trade `io.readLine` already
// made, and it keeps `Value::as_text` a plain pointer with no ownership on it.
#ifndef KHU_VM_RUNTIME_STRINGS_H
#define KHU_VM_RUNTIME_STRINGS_H

#include <cstddef>
#include <string>
#include <utility>

#include "util/array.h"

namespace khu::vm {

class RuntimeStringStore {
public:
    RuntimeStringStore() = default;
    RuntimeStringStore(const RuntimeStringStore&) = delete;
    RuntimeStringStore& operator=(const RuntimeStringStore&) = delete;
    ~RuntimeStringStore() { release(); }

    // Takes ownership of `text` and returns a pointer that stays valid for the
    // rest of the run.
    const std::string* intern(std::string text) {
        auto* stored = new std::string(std::move(text));
        strings_.push(stored);
        return stored;
    }

    // Frees every string the store is holding. Called from the backend's
    // destructor; separate so a test can assert the accounting.
    void release() {
        for (std::string* text : strings_) delete text;
        strings_.clear();
    }

    std::size_t size() const { return strings_.size(); }

private:
    util::Array<std::string*> strings_;
};

}  // namespace khu::vm

#endif  // KHU_VM_RUNTIME_STRINGS_H

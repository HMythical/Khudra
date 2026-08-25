// Dynamic array for compiler internals.
//
// Deliberately hand-rolled rather than pulling in std::vector everywhere: the
// plan calls for a self-contained utility layer, and having our own growth
// policy keeps allocation behaviour predictable when the front end starts
// churning through large ASTs.
#ifndef KHU_UTIL_ARRAY_H
#define KHU_UTIL_ARRAY_H

#include <cstddef>
#include <cstdlib>
#include <initializer_list>
#include <new>
#include <type_traits>
#include <utility>

namespace khu::util {

template <typename T>
class Array {
public:
    Array() = default;

    Array(std::initializer_list<T> values) {
        reserve(values.size());
        for (const T& value : values) push(value);
    }

    Array(const Array& other) {
        reserve(other.size_);
        for (std::size_t i = 0; i < other.size_; ++i) push(other.data_[i]);
    }

    Array(Array&& other) noexcept
        : data_(other.data_), size_(other.size_), capacity_(other.capacity_) {
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    Array& operator=(const Array& other) {
        if (this == &other) return *this;
        clear();
        reserve(other.size_);
        for (std::size_t i = 0; i < other.size_; ++i) push(other.data_[i]);
        return *this;
    }

    Array& operator=(Array&& other) noexcept {
        if (this == &other) return *this;
        destroy();
        data_ = other.data_;
        size_ = other.size_;
        capacity_ = other.capacity_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
        return *this;
    }

    ~Array() { destroy(); }

    void reserve(std::size_t wanted) {
        if (wanted <= capacity_) return;
        std::size_t new_capacity = capacity_ ? capacity_ : 8;
        while (new_capacity < wanted) new_capacity *= 2;
        T* fresh = static_cast<T*>(std::malloc(new_capacity * sizeof(T)));
        if (!fresh) std::abort();
        for (std::size_t i = 0; i < size_; ++i) {
            new (fresh + i) T(std::move(data_[i]));
            data_[i].~T();
        }
        std::free(data_);
        data_ = fresh;
        capacity_ = new_capacity;
    }

    void push(const T& value) {
        reserve(size_ + 1);
        new (data_ + size_) T(value);
        ++size_;
    }

    void push(T&& value) {
        reserve(size_ + 1);
        new (data_ + size_) T(std::move(value));
        ++size_;
    }

    template <typename... Args>
    T& emplace(Args&&... args) {
        reserve(size_ + 1);
        new (data_ + size_) T(std::forward<Args>(args)...);
        return data_[size_++];
    }

    void pop() {
        if (size_ == 0) return;
        --size_;
        data_[size_].~T();
    }

    void clear() {
        for (std::size_t i = 0; i < size_; ++i) data_[i].~T();
        size_ = 0;
    }

    void resize(std::size_t wanted, const T& fill = T()) {
        if (wanted < size_) {
            while (size_ > wanted) pop();
            return;
        }
        reserve(wanted);
        while (size_ < wanted) push(fill);
    }

    T& operator[](std::size_t index) { return data_[index]; }
    const T& operator[](std::size_t index) const { return data_[index]; }

    T& back() { return data_[size_ - 1]; }
    const T& back() const { return data_[size_ - 1]; }

    T* begin() { return data_; }
    T* end() { return data_ + size_; }
    const T* begin() const { return data_; }
    const T* end() const { return data_ + size_; }

    T* data() { return data_; }
    const T* data() const { return data_; }

    std::size_t size() const { return size_; }
    std::size_t capacity() const { return capacity_; }
    bool empty() const { return size_ == 0; }

private:
    void destroy() {
        clear();
        std::free(data_);
        data_ = nullptr;
        capacity_ = 0;
    }

    T* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t capacity_ = 0;
};

}  // namespace khu::util

#endif  // KHU_UTIL_ARRAY_H

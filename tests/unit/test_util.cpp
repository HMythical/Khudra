// Phase 0: the compiler-internal utility layer.
#include "test_harness.h"

#include <string>

#include "util/arena.h"
#include "util/array.h"
#include "util/file.h"
#include "util/hashmap.h"
#include "util/string_builder.h"

using khu::util::Arena;
using khu::util::Array;
using khu::util::StringBuilder;
using khu::util::StringMap;

KHU_TEST(array, grows_and_preserves_order) {
    Array<int> values;
    for (int i = 0; i < 100; ++i) values.push(i * 3);

    KHU_CHECK_EQ(values.size(), static_cast<std::size_t>(100));
    KHU_CHECK_EQ(values[0], 0);
    KHU_CHECK_EQ(values[99], 297);
    KHU_CHECK(values.capacity() >= values.size());
}

KHU_TEST(array, holds_non_trivial_elements) {
    Array<std::string> names;
    names.push("bring");
    names.push("class");
    names.emplace("Procedures");

    KHU_CHECK_EQ(names.size(), static_cast<std::size_t>(3));
    KHU_CHECK_EQ(names.back(), std::string("Procedures"));

    Array<std::string> moved = std::move(names);
    KHU_CHECK_EQ(moved.size(), static_cast<std::size_t>(3));
    KHU_CHECK_EQ(moved[1], std::string("class"));

    moved.pop();
    KHU_CHECK_EQ(moved.size(), static_cast<std::size_t>(2));
    moved.clear();
    KHU_CHECK(moved.empty());
}

KHU_TEST(hashmap, inserts_finds_and_rejects_duplicates) {
    StringMap<int> map;
    map.insert("int32", 1);
    map.insert("uint8", 2);

    KHU_CHECK(map.find("int32") != nullptr);
    KHU_CHECK_EQ(*map.find("int32"), 1);
    KHU_CHECK(map.find("missing") == nullptr);
    KHU_CHECK(map.insert_new("int32", 9) == nullptr);
    KHU_CHECK_EQ(*map.find("int32"), 1);

    map.insert("int32", 7);
    KHU_CHECK_EQ(*map.find("int32"), 7);
    KHU_CHECK_EQ(map.size(), static_cast<std::size_t>(2));
}

KHU_TEST(hashmap, survives_rehashing) {
    StringMap<int> map;
    for (int i = 0; i < 500; ++i) map.insert("key" + std::to_string(i), i);

    KHU_CHECK_EQ(map.size(), static_cast<std::size_t>(500));
    for (int i = 0; i < 500; ++i) {
        const int* found = map.find("key" + std::to_string(i));
        KHU_CHECK(found != nullptr);
        if (found) KHU_CHECK_EQ(*found, i);
    }

    std::size_t visited = 0;
    map.for_each([&](std::string_view, const int&) { ++visited; });
    KHU_CHECK_EQ(visited, static_cast<std::size_t>(500));
}

KHU_TEST(string_builder, builds_text_with_indentation) {
    StringBuilder builder;
    builder.append("class ").append("Example").append_line(" {");
    builder.indent(1).append("x = ").append_int(-42).append_line(";");
    builder.indent(1).append("u = ").append_uint(9000ull).append_line(";");
    builder.append_line("}");

    KHU_CHECK_EQ(builder.str(),
                 std::string("class Example {\n    x = -42;\n    u = 9000;\n}\n"));
}

namespace {
struct Tracked {
    static int live;
    std::string name;
    explicit Tracked(std::string value) : name(std::move(value)) { ++live; }
    ~Tracked() { --live; }
};
int Tracked::live = 0;
}  // namespace

KHU_TEST(arena, allocates_aligned_memory_and_runs_destructors) {
    {
        Arena arena(256);
        auto* a = arena.create<Tracked>("procedures");
        auto* b = arena.create<Tracked>("constructor");
        KHU_CHECK_EQ(Tracked::live, 2);
        KHU_CHECK_EQ(a->name, std::string("procedures"));
        KHU_CHECK_EQ(b->name, std::string("constructor"));

        // Force several blocks so the block-spanning path is covered.
        for (int i = 0; i < 64; ++i) {
            auto* number = arena.create<std::uint64_t>(static_cast<std::uint64_t>(i));
            KHU_CHECK_EQ(reinterpret_cast<std::uintptr_t>(number) % alignof(std::uint64_t),
                         static_cast<std::uintptr_t>(0));
        }
        KHU_CHECK(arena.block_count() >= 2);
    }
    KHU_CHECK_EQ(Tracked::live, 0);
}

KHU_TEST(arena, copies_strings_with_nul_terminator) {
    Arena arena;
    std::string_view copy = arena.copy_string("bring khu::stdlib;");
    KHU_CHECK_EQ(std::string(copy), std::string("bring khu::stdlib;"));
    KHU_CHECK_EQ(copy.data()[copy.size()], '\0');
}

KHU_TEST(file, reports_missing_files) {
    std::string contents;
    khu::util::FileError status =
        khu::util::read_file("definitely/not/a/real/path.khu", contents);
    KHU_CHECK(status == khu::util::FileError::NotFound);
    KHU_CHECK_EQ(std::string(khu::util::describe(status)), std::string("no such file"));
}

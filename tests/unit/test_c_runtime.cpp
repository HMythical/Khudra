// Phase 0: the C runtime links cleanly through the extern "C" boundary.
#include "test_harness.h"

#include <cstdint>

extern "C" {
#include "alloc.h"
#include "bool_impl.h"
#include "proc_engine.h"
}

KHU_TEST(c_runtime, manual_alloc_tracks_live_bytes) {
    std::size_t before = khu_manual_live_bytes();
    void* block = khu_manual_alloc(128);
    KHU_CHECK(block != nullptr);
    KHU_CHECK_EQ(khu_manual_live_bytes(), before + 128);

    // Manual memory is handed out zeroed.
    auto* bytes = static_cast<unsigned char*>(block);
    KHU_CHECK_EQ(static_cast<int>(bytes[0]), 0);
    KHU_CHECK_EQ(static_cast<int>(bytes[127]), 0);

    khu_manual_free(block);
    KHU_CHECK_EQ(khu_manual_live_bytes(), before);
    khu_manual_free(nullptr);  // no-op
}

// A block's length is tracked outside the block, so asking about a pointer that
// is not live never reads memory that has been given back. That is what lets
// khuStdMem tell a double release from a first one.
KHU_TEST(c_runtime, block_sizes_come_from_the_live_registry) {
    void* block = khu_manual_alloc(48);
    KHU_CHECK(block != nullptr);
    KHU_CHECK_EQ(khu_manual_block_size(block), static_cast<std::size_t>(48));
    KHU_CHECK_EQ(khu_manual_block_size(nullptr), static_cast<std::size_t>(0));

    khu_manual_free(block);
    // The same address, no longer live: the answer comes from the registry, so
    // nothing reads the freed block to produce it.
    KHU_CHECK_EQ(khu_manual_block_size(block), static_cast<std::size_t>(0));
    // And releasing it again is a no-op rather than a second decrement.
    std::size_t live = khu_manual_live_blocks();
    khu_manual_free(block);
    KHU_CHECK_EQ(khu_manual_live_blocks(), live);
}

KHU_TEST(c_runtime, realloc_keeps_what_still_fits) {
    std::size_t before = khu_manual_live_bytes();

    auto* block = static_cast<unsigned char*>(khu_manual_alloc(8));
    KHU_CHECK(block != nullptr);
    for (int i = 0; i < 8; ++i) block[i] = static_cast<unsigned char>(i + 1);

    auto* grown = static_cast<unsigned char*>(khu_manual_realloc(block, 32));
    KHU_CHECK(grown != nullptr);
    KHU_CHECK_EQ(khu_manual_block_size(grown), static_cast<std::size_t>(32));
    for (int i = 0; i < 8; ++i) KHU_CHECK_EQ(static_cast<int>(grown[i]), i + 1);
    // Anything new is zeroed, like a fresh allocation.
    for (int i = 8; i < 32; ++i) KHU_CHECK_EQ(static_cast<int>(grown[i]), 0);

    auto* shrunk = static_cast<unsigned char*>(khu_manual_realloc(grown, 4));
    KHU_CHECK(shrunk != nullptr);
    KHU_CHECK_EQ(khu_manual_block_size(shrunk), static_cast<std::size_t>(4));
    for (int i = 0; i < 4; ++i) KHU_CHECK_EQ(static_cast<int>(shrunk[i]), i + 1);

    // A size of 0 releases and answers null.
    KHU_CHECK(khu_manual_realloc(shrunk, 0) == nullptr);
    KHU_CHECK_EQ(khu_manual_live_bytes(), before);

    // A null pointer allocates.
    void* fresh = khu_manual_realloc(nullptr, 16);
    KHU_CHECK_EQ(khu_manual_block_size(fresh), static_cast<std::size_t>(16));
    khu_manual_free(fresh);
    KHU_CHECK_EQ(khu_manual_live_bytes(), before);
}

KHU_TEST(c_runtime, the_registry_survives_many_blocks) {
    std::size_t before = khu_manual_live_bytes();
    void* blocks[300];
    for (int i = 0; i < 300; ++i) {
        blocks[i] = khu_manual_alloc(static_cast<std::size_t>(i + 1));
        KHU_CHECK(blocks[i] != nullptr);
    }
    // Every one is still findable after the table has grown and rehashed.
    for (int i = 0; i < 300; ++i) {
        KHU_CHECK_EQ(khu_manual_block_size(blocks[i]), static_cast<std::size_t>(i + 1));
    }
    // Releasing every other one leaves tombstones the probes still walk past.
    for (int i = 0; i < 300; i += 2) khu_manual_free(blocks[i]);
    for (int i = 1; i < 300; i += 2) {
        KHU_CHECK_EQ(khu_manual_block_size(blocks[i]), static_cast<std::size_t>(i + 1));
    }
    for (int i = 1; i < 300; i += 2) khu_manual_free(blocks[i]);
    KHU_CHECK_EQ(khu_manual_live_bytes(), before);
}

KHU_TEST(c_runtime, bool_representation_is_canonical) {
    KHU_CHECK_EQ(static_cast<int>(khu_bool_from_int(5)), static_cast<int>(KHU_TRUE));
    KHU_CHECK_EQ(static_cast<int>(khu_bool_from_int(0)), static_cast<int>(KHU_FALSE));
    KHU_CHECK_EQ(static_cast<int>(khu_bool_not(KHU_TRUE)), static_cast<int>(KHU_FALSE));
    KHU_CHECK_EQ(static_cast<int>(khu_bool_and(KHU_TRUE, KHU_FALSE)), static_cast<int>(KHU_FALSE));
    KHU_CHECK_EQ(static_cast<int>(khu_bool_or(KHU_TRUE, KHU_FALSE)), static_cast<int>(KHU_TRUE));
    KHU_CHECK_EQ(static_cast<int>(khu_bool_xor(KHU_TRUE, KHU_TRUE)), static_cast<int>(KHU_FALSE));
    KHU_CHECK_EQ(std::string(khu_bool_name(KHU_TRUE)), std::string("true"));
}

KHU_TEST(c_runtime, procedure_engine_refuses_to_run_without_a_host) {
    // The engine is pure C and holds no VM state: with no host installed it
    // reports that rather than dereferencing anything.
    KHU_CHECK_EQ(khu_proc_engine_ready(), 0);
    KHU_CHECK(khu_proc_engine_host() == nullptr);

    KhuObject* object = reinterpret_cast<KhuObject*>(0x1);
    KhuProcStatus status = khu_proc_materialize(0, KHU_STRATEGY_GC, 0, &object);
    KHU_CHECK(status == KHU_PROC_NOT_IMPLEMENTED);
    KHU_CHECK(object == nullptr);
    KHU_CHECK_EQ(std::string(khu_proc_status_name(status)),
                 std::string("procedure engine not implemented"));
    KHU_CHECK_EQ(khu_proc_depth(), static_cast<std::uint32_t>(0));
}

KHU_TEST(c_runtime, procedure_engine_names_every_status) {
    KHU_CHECK_EQ(std::string(khu_proc_status_name(KHU_PROC_OK)), std::string("ok"));
    KHU_CHECK_EQ(std::string(khu_proc_status_name(KHU_PROC_DEPTH_EXCEEDED)),
                 std::string("materialization nested too deeply"));
    KHU_CHECK_EQ(std::string(khu_proc_status_name(KHU_PROC_CONSTRUCTOR_FAILED)),
                 std::string("constructor failed"));
    KHU_CHECK(KHU_PROC_MAX_DEPTH > 0);
}

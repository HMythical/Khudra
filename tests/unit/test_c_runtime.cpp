// Phase 0: the C runtime links cleanly through the extern "C" boundary.
#include "test_harness.h"

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

KHU_TEST(c_runtime, bool_representation_is_canonical) {
    KHU_CHECK_EQ(static_cast<int>(khu_bool_from_int(5)), static_cast<int>(KHU_TRUE));
    KHU_CHECK_EQ(static_cast<int>(khu_bool_from_int(0)), static_cast<int>(KHU_FALSE));
    KHU_CHECK_EQ(static_cast<int>(khu_bool_not(KHU_TRUE)), static_cast<int>(KHU_FALSE));
    KHU_CHECK_EQ(static_cast<int>(khu_bool_and(KHU_TRUE, KHU_FALSE)), static_cast<int>(KHU_FALSE));
    KHU_CHECK_EQ(static_cast<int>(khu_bool_or(KHU_TRUE, KHU_FALSE)), static_cast<int>(KHU_TRUE));
    KHU_CHECK_EQ(static_cast<int>(khu_bool_xor(KHU_TRUE, KHU_TRUE)), static_cast<int>(KHU_FALSE));
    KHU_CHECK_EQ(std::string(khu_bool_name(KHU_TRUE)), std::string("true"));
}

KHU_TEST(c_runtime, procedure_engine_abi_is_reachable) {
    // Phase 0 only guarantees the ABI links and reports "not implemented".
    KHU_CHECK_EQ(khu_proc_engine_ready(), 0);
    KhuObject* object = reinterpret_cast<KhuObject*>(0x1);
    KhuProcStatus status = khu_proc_materialize(0, KHU_STRATEGY_GC, 0, &object);
    KHU_CHECK(status == KHU_PROC_NOT_IMPLEMENTED);
    KHU_CHECK(object == nullptr);
    KHU_CHECK_EQ(std::string(khu_proc_status_name(status)),
                 std::string("procedure engine not implemented"));
}

// Layout parity between the native ABI and the runtime object model.
//
// The rule: native structs must match src/vm/object.h byte for byte, or the
// collector and the C runtime will misread memory. The native backend
// routes every object access through the host today, so nothing depends on this
// by accident -- which is exactly why it is worth pinning: the moment a future
// emitter touches a slot directly, this test is what stops it being wrong.
//
// The type-tag and flag constants are pinned here for the same reason: the
// emitted C writes tag bytes as integer literals, so a renumbered TypeTag has
// to fail here rather than at run time.
#include "test_harness.h"

#include <cstddef>

#include "bytecode/opcode.h"
#include "host/khu_native_abi.h"
#include "host/native_host.h"
#include "vm/object.h"
#include "vm/value.h"
#include "vm/vm.h"

namespace {

using khu::bytecode::TypeTag;

}  // namespace

KHU_TEST(native_abi, value_layout_matches_the_vm) {
    KHU_CHECK_EQ(sizeof(KhuValue), sizeof(khu::vm::Value));
    KHU_CHECK_EQ(alignof(KhuValue), alignof(khu::vm::Value));
    KHU_CHECK_EQ(offsetof(KhuValue, tag), offsetof(khu::vm::Value, tag));
    KHU_CHECK_EQ(offsetof(KhuValue, v), offsetof(khu::vm::Value, as_int));
    KHU_CHECK_EQ(sizeof(KhuValue{}.v), sizeof(std::uint64_t));
}

KHU_TEST(native_abi, object_header_layout_matches_the_vm) {
    KHU_CHECK_EQ(sizeof(KhuObjectHeader), sizeof(khu::vm::ObjectHeader));
    KHU_CHECK_EQ(alignof(KhuObjectHeader), alignof(khu::vm::ObjectHeader));
    KHU_CHECK_EQ(offsetof(KhuObjectHeader, class_id), offsetof(khu::vm::ObjectHeader, class_id));
    KHU_CHECK_EQ(offsetof(KhuObjectHeader, flags), offsetof(khu::vm::ObjectHeader, flags));
    KHU_CHECK_EQ(offsetof(KhuObjectHeader, pin_count), offsetof(khu::vm::ObjectHeader, pin_count));
    KHU_CHECK_EQ(offsetof(KhuObjectHeader, size), offsetof(khu::vm::ObjectHeader, size));
    KHU_CHECK_EQ(offsetof(KhuObjectHeader, vtable), offsetof(khu::vm::ObjectHeader, vtable));
    KHU_CHECK_EQ(offsetof(KhuObjectHeader, gc_link), offsetof(khu::vm::ObjectHeader, gc_link));

    // Field storage starts immediately after the header in both models.
    KHU_CHECK_EQ(sizeof(khu::vm::Object), sizeof(KhuObjectHeader));
}

KHU_TEST(native_abi, object_flags_match_the_vm) {
    KHU_CHECK_EQ(KHU_OBJECT_GC, static_cast<unsigned>(khu::vm::kObjectGc));
    KHU_CHECK_EQ(KHU_OBJECT_MANUAL, static_cast<unsigned>(khu::vm::kObjectManual));
    KHU_CHECK_EQ(KHU_OBJECT_MATERIALIZED, static_cast<unsigned>(khu::vm::kObjectMaterialized));
    KHU_CHECK_EQ(KHU_OBJECT_MARKED, static_cast<unsigned>(khu::vm::kObjectMarked));
}

KHU_TEST(native_abi, type_tags_match_the_bytecode_enum) {
    KHU_CHECK_EQ(KHU_T_VOID, static_cast<int>(TypeTag::Void));
    KHU_CHECK_EQ(KHU_T_INT8, static_cast<int>(TypeTag::Int8));
    KHU_CHECK_EQ(KHU_T_INT16, static_cast<int>(TypeTag::Int16));
    KHU_CHECK_EQ(KHU_T_INT32, static_cast<int>(TypeTag::Int32));
    KHU_CHECK_EQ(KHU_T_INT64, static_cast<int>(TypeTag::Int64));
    KHU_CHECK_EQ(KHU_T_UINT8, static_cast<int>(TypeTag::UInt8));
    KHU_CHECK_EQ(KHU_T_UINT16, static_cast<int>(TypeTag::UInt16));
    KHU_CHECK_EQ(KHU_T_UINT32, static_cast<int>(TypeTag::UInt32));
    KHU_CHECK_EQ(KHU_T_UINT64, static_cast<int>(TypeTag::UInt64));
    KHU_CHECK_EQ(KHU_T_FLOAT32, static_cast<int>(TypeTag::Float32));
    KHU_CHECK_EQ(KHU_T_FLOAT64, static_cast<int>(TypeTag::Float64));
    KHU_CHECK_EQ(KHU_T_BOOL, static_cast<int>(TypeTag::Bool));
    KHU_CHECK_EQ(KHU_T_STRING, static_cast<int>(TypeTag::String));
    KHU_CHECK_EQ(KHU_T_ARRAY, static_cast<int>(TypeTag::Array));
    KHU_CHECK_EQ(KHU_T_REF, static_cast<int>(TypeTag::Ref));
    KHU_CHECK_EQ(KHU_T_PTR, static_cast<int>(TypeTag::Ptr));
    KHU_CHECK_EQ(KHU_T_MEMORY, static_cast<int>(TypeTag::Memory));
    KHU_CHECK_EQ(KHU_T_NULL, static_cast<int>(TypeTag::Null));
}

KHU_TEST(native_abi, type_predicates_match_the_bytecode_helpers) {
    for (int tag = 0; tag < static_cast<int>(TypeTag::Count); ++tag) {
        auto typed = static_cast<TypeTag>(tag);
        auto byte = static_cast<std::uint8_t>(tag);
        KHU_CHECK_EQ(khu_is_signed(byte) != 0, khu::bytecode::is_signed_integer(typed));
        KHU_CHECK_EQ(khu_is_integer(byte) != 0, khu::bytecode::is_integer(typed));
        KHU_CHECK_EQ(khu_is_float(byte) != 0, khu::bytecode::is_float(typed));
        KHU_CHECK_EQ(khu_is_numeric(byte) != 0, khu::bytecode::is_numeric(typed));
        KHU_CHECK_EQ(khu_type_width(byte), khu::bytecode::type_width(typed));
    }
}

KHU_TEST(native_abi, the_call_depth_guard_matches_the_vm) {
    KHU_CHECK_EQ(khu::native::kMaxCallDepth, khu::vm::kMaxCallDepth);
}

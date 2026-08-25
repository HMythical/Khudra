/* Khudra `bool` support in the C runtime.
 *
 * Khudra bools are one byte with exactly two bit patterns (0 and 1); this
 * module owns the canonical representation so the VM, the Procedure engine and
 * any future FFI agree on it. */
#include "bool_impl.h"

khu_bool khu_bool_from_int(long long value) { return value != 0 ? KHU_TRUE : KHU_FALSE; }

khu_bool khu_bool_not(khu_bool value) { return value == KHU_FALSE ? KHU_TRUE : KHU_FALSE; }

khu_bool khu_bool_and(khu_bool left, khu_bool right) {
    return (left != KHU_FALSE && right != KHU_FALSE) ? KHU_TRUE : KHU_FALSE;
}

khu_bool khu_bool_or(khu_bool left, khu_bool right) {
    return (left != KHU_FALSE || right != KHU_FALSE) ? KHU_TRUE : KHU_FALSE;
}

khu_bool khu_bool_xor(khu_bool left, khu_bool right) {
    return ((left != KHU_FALSE) != (right != KHU_FALSE)) ? KHU_TRUE : KHU_FALSE;
}

const char* khu_bool_name(khu_bool value) { return value != KHU_FALSE ? "true" : "false"; }

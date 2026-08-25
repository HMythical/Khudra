/* Canonical Khudra boolean representation. */
#ifndef KHU_UTILS_BOOL_IMPL_H
#define KHU_UTILS_BOOL_IMPL_H

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char khu_bool;

#define KHU_FALSE ((khu_bool)0)
#define KHU_TRUE ((khu_bool)1)

khu_bool khu_bool_from_int(long long value);
khu_bool khu_bool_not(khu_bool value);
khu_bool khu_bool_and(khu_bool left, khu_bool right);
khu_bool khu_bool_or(khu_bool left, khu_bool right);
khu_bool khu_bool_xor(khu_bool left, khu_bool right);

/* "true" / "false" -- used by io.print and by disassembler output. */
const char* khu_bool_name(khu_bool value);

#ifdef __cplusplus
}
#endif

#endif /* KHU_UTILS_BOOL_IMPL_H */

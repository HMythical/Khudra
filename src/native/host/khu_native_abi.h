/* Khudra native backend ABI.
 *
 * The boundary between emitted C and the native host. Both sides include this
 * file, so it must stay valid C11 and valid C++17 -- the same rule
 * utils/proc_engine.h lives by.
 *
 * Two kinds of thing live here:
 *
 *   - Layout mirrors of the runtime object model (`KhuValue`, `KhuObjectHeader`).
 *     They must match src/vm/value.h and src/vm/object.h byte for byte, or the
 *     collector and the C runtime will misread memory. tests/unit/test_native_abi
 *     asserts every offset.
 *   - The `khu_rt_*` calls the emitted C makes for anything stateful: fields,
 *     allocation, materialization, calls, traps. Anything *pure* -- arithmetic,
 *     comparison, conversion -- is a static inline below instead, so the
 *     translated code carries the semantics itself rather than calling back.
 *
 * The emitted C never sees a bytecode dispatcher: control flow is C control
 * flow, the operand stack is a C array with a statically known height, and the
 * only reason to enter the host is a service the interpreter would also have
 * gone outside its loop for.
 */
#ifndef KHU_NATIVE_ABI_H
#define KHU_NATIVE_ABI_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- type tags -------------------------------------------------------------
 * The numbering is bytecode::TypeTag's. test_native_abi pins each one against
 * the enum so the two cannot drift. */
#define KHU_T_VOID 0
#define KHU_T_INT8 1
#define KHU_T_INT16 2
#define KHU_T_INT32 3
#define KHU_T_INT64 4
#define KHU_T_UINT8 5
#define KHU_T_UINT16 6
#define KHU_T_UINT32 7
#define KHU_T_UINT64 8
#define KHU_T_FLOAT32 9
#define KHU_T_FLOAT64 10
#define KHU_T_BOOL 11
#define KHU_T_STRING 12
#define KHU_T_ARRAY 13
#define KHU_T_REF 14
#define KHU_T_PTR 15
#define KHU_T_MEMORY 16
#define KHU_T_NULL 17

/* --- values ---------------------------------------------------------------
 * The mirror of khu::vm::Value: a one-byte tag, then a naturally aligned
 * 8-byte payload. The union member names differ from the C++ side only because
 * `as_int` reads better than `v.i` in hand-written code and worse in generated
 * code; the layout is what matters. */
typedef struct KhuValue {
    uint8_t tag;
    union {
        int64_t as_int;
        uint64_t as_uint;
        double as_float;
        void* as_ref;
        const void* as_text;
        void* as_raw;
    } v;
} KhuValue;

/* The mirror of khu::vm::ObjectHeader. Field storage -- one KhuValue per slot,
 * indexed the way getfield/putfield index it -- follows immediately. */
typedef struct KhuObjectHeader {
    uint32_t class_id;
    uint32_t flags;
    uint32_t pin_count;
    uint32_t size;
    void* vtable;
    void* gc_link;
} KhuObjectHeader;

#define KHU_OBJECT_GC 1u
#define KHU_OBJECT_MANUAL 2u
#define KHU_OBJECT_MATERIALIZED 4u
#define KHU_OBJECT_MARKED 8u

/* --- frames ---------------------------------------------------------------
 * A native frame is a plain C struct the emitted function puts on its own
 * stack and links into the host's chain. `slots` is the scanned-locals region
 * (docs/native.md, section 4): locals first, then the operand stack. A collection can
 * only happen at a safepoint -- an allocation or a call -- and the emitted code
 * publishes `ip` and `height` immediately before every one of those, so the
 * host walks exactly the live references and nothing stale. */
typedef struct KhuFrame {
    struct KhuFrame* parent;
    KhuValue* slots;
    KhuValue receiver;
    uint32_t method_index;
    uint32_t local_count;
    /* Live operand-stack entries above the locals. */
    uint32_t height;
    /* Bytecode offset just past the instruction in flight, so the host's line
     * lookup lands where the interpreter's `ip - 1` lands. */
    uint32_t ip;
} KhuFrame;

/* A lowered method. Returns 0 on success, non-zero once a trap is pending. */
typedef int (*KhuNativeMethod)(KhuValue self, const KhuValue* argv, KhuValue* out);

/* --- what the emitted translation unit exports ----------------------------
 * The host indexes this table by module method index, so it can drive field
 * initializers, Procedures blocks, constructors and `main` without knowing
 * anything about the program. */
extern const KhuNativeMethod khu_native_methods[];
extern const uint32_t khu_native_method_count;
/* The .kbc image the translation unit was lowered from. The host reloads it to
 * build the class table, the constant pool and the line tables -- the same
 * bytes `khudra run` would have loaded. */
extern const unsigned char khu_native_image[];
extern const uint32_t khu_native_image_size;

/* --- what the host exports to the emitted code ---------------------------- */

/* The constant pool, laid out to match Module::constants. LoadConst is an
 * index into this, so a string keeps the pointer identity the VM gives it. */
extern const KhuValue* khu_rt_constants;

void khu_rt_frame_enter(KhuFrame* frame, KhuValue* slots, uint32_t method_index,
                        uint32_t local_count, uint32_t slot_count, KhuValue receiver,
                        const KhuValue* argv, uint32_t argc);
void khu_rt_frame_leave(KhuFrame* frame);

/* Every one of these returns 0 on success and non-zero with a trap already
 * reported, so the emitted code propagates with a bare `return`. */
int khu_rt_getfield(KhuFrame* frame, const KhuValue* receiver, uint16_t slot, KhuValue* out);
int khu_rt_putfield(KhuFrame* frame, const KhuValue* receiver, uint16_t slot,
                    const KhuValue* value);
int khu_rt_materialize(KhuFrame* frame, uint16_t class_id, uint8_t strategy,
                       const KhuValue* args, uint32_t argc, KhuValue* out);
int khu_rt_alloc(KhuFrame* frame, uint16_t class_id, int manual, KhuValue* out);
int khu_rt_free(KhuFrame* frame, const KhuValue* target);
int khu_rt_pin(KhuFrame* frame, const KhuValue* target, int pin);
int khu_rt_call_direct(KhuFrame* frame, uint16_t method_index, const KhuValue* receiver,
                       const KhuValue* argv, KhuValue* out);
/* `expects_result` is what the emitter predicted from the vtable slot; the host
 * checks the resolved method against it rather than trusting the prediction. */
int khu_rt_call_virtual(KhuFrame* frame, uint16_t slot, uint8_t argc, const KhuValue* receiver,
                        const KhuValue* argv, KhuValue* out, int expects_result);
int khu_rt_call_native(KhuFrame* frame, uint16_t native_id, uint8_t argc, const KhuValue* argv,
                       KhuValue* out, int expects_result);
/* Contents comparison for strings, pointer identity otherwise -- the refeq
 * rule needs to look inside a std::string, so it cannot be inline C. */
int khu_rt_ref_same(const KhuValue* left, const KhuValue* right);
int khu_rt_trap(KhuFrame* frame, const char* message);

/* --- pure value operations ------------------------------------------------
 * Straight translations of KhudraVm::arithmetic, ::compare and ::convert.
 * They are inline in the emitted translation unit because they are the parts
 * with no state behind them; tests/unit/test_native_values differentially
 * checks every one against the interpreter. */

static inline uint64_t khu_mask_of(uint32_t width) {
    return width >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << width) - 1);
}

static inline int64_t khu_sign_extend(uint64_t bits, uint32_t width) {
    uint64_t sign;
    if (width >= 64) return (int64_t)bits;
    sign = (uint64_t)1 << (width - 1);
    bits &= khu_mask_of(width);
    return (int64_t)((bits ^ sign) - sign);
}

static inline int khu_is_signed(uint8_t tag) { return tag >= KHU_T_INT8 && tag <= KHU_T_INT64; }
static inline int khu_is_integer(uint8_t tag) { return tag >= KHU_T_INT8 && tag <= KHU_T_UINT64; }
static inline int khu_is_float(uint8_t tag) {
    return tag == KHU_T_FLOAT32 || tag == KHU_T_FLOAT64;
}
static inline int khu_is_numeric(uint8_t tag) { return khu_is_integer(tag) || khu_is_float(tag); }

static inline uint32_t khu_type_width(uint8_t tag) {
    switch (tag) {
        case KHU_T_INT8:
        case KHU_T_UINT8: return 8;
        case KHU_T_INT16:
        case KHU_T_UINT16: return 16;
        case KHU_T_INT32:
        case KHU_T_UINT32:
        case KHU_T_FLOAT32: return 32;
        case KHU_T_INT64:
        case KHU_T_UINT64:
        case KHU_T_FLOAT64: return 64;
        default: return 0;
    }
}

/* Overflow wraps, so every integer result is truncated back to its width. */
static inline KhuValue khu_normalize_int(uint8_t tag, uint64_t bits) {
    KhuValue result;
    uint32_t width = khu_type_width(tag);
    result.tag = tag;
    if (khu_is_signed(tag)) {
        result.v.as_int = khu_sign_extend(bits, width);
    } else {
        result.v.as_uint = bits & khu_mask_of(width);
    }
    return result;
}

/* A 32-bit float must round through `float` so its precision is real. */
static inline KhuValue khu_normalize_float(uint8_t tag, double value) {
    KhuValue result;
    result.tag = tag;
    result.v.as_float = tag == KHU_T_FLOAT32 ? (double)(float)value : value;
    return result;
}

static inline KhuValue khu_make_bool(int value) {
    KhuValue result;
    result.tag = KHU_T_BOOL;
    result.v.as_uint = value ? 1u : 0u;
    return result;
}

static inline KhuValue khu_make_null(void) {
    KhuValue result;
    result.tag = KHU_T_NULL;
    result.v.as_ref = NULL;
    return result;
}

static inline KhuValue khu_make_void(void) {
    KhuValue result;
    result.tag = KHU_T_VOID;
    result.v.as_uint = 0;
    return result;
}

static inline int khu_truthy(const KhuValue* value) { return value->v.as_uint != 0; }

static inline int khu_is_null_reference(const KhuValue* value) {
    return value->tag == KHU_T_NULL || (value->tag == KHU_T_REF && value->v.as_ref == NULL);
}

/* Binary and unary arithmetic. `op` is a KhuArith; the emitter has already
 * decided which single trap message a given (op, tag) pair can produce, so a
 * non-zero result needs no code with it. */
enum KhuArith {
    KHU_A_ADD, KHU_A_SUB, KHU_A_MUL, KHU_A_DIV, KHU_A_REM, KHU_A_NEG,
    KHU_A_AND, KHU_A_OR, KHU_A_XOR, KHU_A_NOT, KHU_A_SHL, KHU_A_SHR
};

static inline int khu_val_unary(int op, uint8_t tag, const KhuValue* operand, KhuValue* out) {
    if (khu_is_float(tag)) {
        if (op == KHU_A_NOT) return 1; /* '~' is not defined for floating point */
        *out = khu_normalize_float(tag, -operand->v.as_float);
        return 0;
    }
    *out = khu_normalize_int(tag, op == KHU_A_NEG ? (uint64_t)0 - operand->v.as_uint
                                                  : ~operand->v.as_uint);
    return 0;
}

static inline int khu_val_arith(int op, uint8_t tag, const KhuValue* left, const KhuValue* right,
                                KhuValue* out) {
    uint32_t width;
    int is_signed;

    if (khu_is_float(tag)) {
        double a = left->v.as_float;
        double b = right->v.as_float;
        switch (op) {
            case KHU_A_ADD: *out = khu_normalize_float(tag, a + b); return 0;
            case KHU_A_SUB: *out = khu_normalize_float(tag, a - b); return 0;
            case KHU_A_MUL: *out = khu_normalize_float(tag, a * b); return 0;
            case KHU_A_DIV: *out = khu_normalize_float(tag, a / b); return 0;
            case KHU_A_REM: *out = khu_normalize_float(tag, fmod(a, b)); return 0;
            default: return 1; /* not defined for floating point */
        }
    }

    width = khu_type_width(tag);
    is_signed = khu_is_signed(tag);

    switch (op) {
        case KHU_A_ADD:
            *out = khu_normalize_int(tag, left->v.as_uint + right->v.as_uint);
            return 0;
        case KHU_A_SUB:
            *out = khu_normalize_int(tag, left->v.as_uint - right->v.as_uint);
            return 0;
        case KHU_A_MUL:
            *out = khu_normalize_int(tag, left->v.as_uint * right->v.as_uint);
            return 0;
        case KHU_A_AND:
            *out = khu_normalize_int(tag, left->v.as_uint & right->v.as_uint);
            return 0;
        case KHU_A_OR:
            *out = khu_normalize_int(tag, left->v.as_uint | right->v.as_uint);
            return 0;
        case KHU_A_XOR:
            *out = khu_normalize_int(tag, left->v.as_uint ^ right->v.as_uint);
            return 0;

        case KHU_A_DIV:
        case KHU_A_REM: {
            if ((right->v.as_uint & khu_mask_of(width)) == 0) return 1; /* by zero */
            if (is_signed) {
                int64_t a = left->v.as_int;
                int64_t b = right->v.as_int;
                /* The one signed division that overflows; wrapping keeps it
                 * consistent with every other arithmetic result. */
                if (b == -1) {
                    *out = khu_normalize_int(
                        tag, op == KHU_A_DIV ? (uint64_t)0 - (uint64_t)a : (uint64_t)0);
                    return 0;
                }
                *out = khu_normalize_int(tag, (uint64_t)(op == KHU_A_DIV ? a / b : a % b));
                return 0;
            }
            {
                uint64_t a = left->v.as_uint & khu_mask_of(width);
                uint64_t b = right->v.as_uint & khu_mask_of(width);
                *out = khu_normalize_int(tag, op == KHU_A_DIV ? a / b : a % b);
                return 0;
            }
        }

        case KHU_A_SHL: {
            uint64_t count = right->v.as_uint % (width ? width : 64);
            *out = khu_normalize_int(tag, left->v.as_uint << count);
            return 0;
        }
        case KHU_A_SHR: {
            uint64_t count = right->v.as_uint % (width ? width : 64);
            if (is_signed) {
                int64_t value = khu_sign_extend(left->v.as_uint, width);
                *out = khu_normalize_int(tag, (uint64_t)(value >> count));
            } else {
                *out = khu_normalize_int(tag, (left->v.as_uint & khu_mask_of(width)) >> count);
            }
            return 0;
        }

        default: return 1;
    }
}

enum KhuCompare { KHU_C_EQ, KHU_C_NE, KHU_C_LT, KHU_C_LE, KHU_C_GT, KHU_C_GE };

static inline KhuValue khu_val_compare(int op, uint8_t tag, const KhuValue* left,
                                       const KhuValue* right) {
    int order;
    if (khu_is_float(tag)) {
        double a = left->v.as_float;
        double b = right->v.as_float;
        order = a < b ? -1 : (a > b ? 1 : 0);
    } else if (khu_is_signed(tag)) {
        int64_t a = left->v.as_int;
        int64_t b = right->v.as_int;
        order = a < b ? -1 : (a > b ? 1 : 0);
    } else {
        uint64_t a = left->v.as_uint;
        uint64_t b = right->v.as_uint;
        order = a < b ? -1 : (a > b ? 1 : 0);
    }

    switch (op) {
        case KHU_C_EQ: return khu_make_bool(order == 0);
        case KHU_C_NE: return khu_make_bool(order != 0);
        case KHU_C_LT: return khu_make_bool(order < 0);
        case KHU_C_LE: return khu_make_bool(order <= 0);
        case KHU_C_GT: return khu_make_bool(order > 0);
        default: return khu_make_bool(order >= 0);
    }
}

static inline int khu_val_convert(uint8_t from, uint8_t to, const KhuValue* value, KhuValue* out) {
    uint64_t bits;

    if (!khu_is_numeric(from) || !khu_is_numeric(to)) return 1;

    if (khu_is_float(to)) {
        double result;
        if (khu_is_float(from)) {
            result = value->v.as_float;
        } else if (khu_is_signed(from)) {
            result = (double)value->v.as_int;
        } else {
            result = (double)value->v.as_uint;
        }
        *out = khu_normalize_float(to, result);
        return 0;
    }

    if (khu_is_float(from)) {
        /* Out-of-range float-to-integer conversions truncate toward zero and
         * then wrap, matching the language's wrapping overflow rule. */
        double source = value->v.as_float;
        if (source != source) { /* NaN */
            bits = 0;
        } else if (khu_is_signed(to)) {
            bits = (uint64_t)(int64_t)source;
        } else {
            bits = source < 0 ? (uint64_t)(int64_t)source : (uint64_t)source;
        }
    } else if (khu_is_signed(from)) {
        bits = (uint64_t)value->v.as_int;
    } else {
        bits = value->v.as_uint;
    }
    *out = khu_normalize_int(to, bits);
    return 0;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* KHU_NATIVE_ABI_H */

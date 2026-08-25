/* Khudra Procedure engine -- the C half of object materialization.
 *
 * A Procedure is a named block in a class body that runs *immediately* when an
 * instance is materialized, before the constructor body and before any member
 * is reachable. Driving that pipeline is this module's whole job; every other
 * piece of runtime logic stays in the C++ VM and is reached through the host
 * callback table below.
 *
 * This header is the extern "C" ABI boundary between the C runtime and the
 * C++ VM. Both sides include it, so it must stay valid C11 and valid C++17.
 *
 * The pipeline (see docs/procedures.md):
 *   1. allocate per strategy      2. link object (header, defaults, field inits)
 *   3. bind allocation-site args  4. execute the Procedures blocks
 *   5. execute the constructors   6. register as live -> return the reference
 *
 * Steps 2, 4 and 5 walk the inheritance chain from the root down, so a derived
 * class never observes uninitialized inherited state. */
#ifndef KHU_UTILS_PROC_ENGINE_H
#define KHU_UTILS_PROC_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocation strategy selected at a materialization site. */
typedef enum KhuStrategy {
    KHU_STRATEGY_CLASS_DEFAULT = 0, /* use the class's `type` field */
    KHU_STRATEGY_GC = 1,            /* MemoryAllocationTypeObject.setStandard() */
    KHU_STRATEGY_MANUAL = 2         /* MemoryAllocationTypeObject.setManual() */
} KhuStrategy;

/* Result of a materialization attempt. */
typedef enum KhuProcStatus {
    KHU_PROC_OK = 0,
    KHU_PROC_OUT_OF_MEMORY = 1,
    KHU_PROC_UNKNOWN_CLASS = 2,
    KHU_PROC_PROCEDURE_FAILED = 3,
    KHU_PROC_CONSTRUCTOR_FAILED = 4,
    KHU_PROC_NOT_IMPLEMENTED = 5,
    KHU_PROC_DEPTH_EXCEEDED = 6,
    KHU_PROC_BAD_ARGUMENTS = 7
} KhuProcStatus;

const char* khu_proc_status_name(KhuProcStatus status);

/* Nested materializations run depth-first, so a Procedures block that
 * materializes its own class recurses. This is the guard that turns that into
 * a diagnosable error instead of a stack overflow (docs/procedures.md, 4). */
#define KHU_PROC_MAX_DEPTH 128

/* Opaque handles owned by the C++ side. */
typedef struct KhuVmHost KhuVmHost;
typedef struct KhuObject KhuObject;

/* Callbacks the engine uses to drive the VM. Every entry must be non-NULL.
 * Every `int` result is 0 for success and non-zero for a failure the VM has
 * already reported. */
typedef struct KhuProcHostApi {
    /* --- the inheritance chain, root first --- */
    uint32_t (*chain_length)(KhuVmHost* host, uint32_t class_id);
    /* `index` 0 is the root of the chain, chain_length-1 is `class_id`. */
    uint32_t (*chain_at)(KhuVmHost* host, uint32_t class_id, uint32_t index);

    /* --- step 1: allocate --- */
    /* Allocates and stamps the header (class id, flags, pin 0, size, vtable)
     * and sets every field to its default. NULL on failure. */
    KhuObject* (*allocate)(KhuVmHost* host, uint32_t class_id, KhuStrategy strategy);

    /* --- step 2: link --- */
    int (*has_field_init)(KhuVmHost* host, uint32_t class_id);
    int (*run_field_init)(KhuVmHost* host, KhuObject* object, uint32_t class_id);

    /* --- step 3: bind the allocation-site arguments ---
     * take_arguments lifts `argc` values off the VM stack into a saved frame
     * and roots them, so a nested materialization inside a Procedures block
     * cannot disturb them. push_arguments stages that frame for the next call.
     * drop_arguments pops the frame. */
    int (*take_arguments)(KhuVmHost* host, uint32_t argc);
    void (*push_arguments)(KhuVmHost* host, uint32_t argc);
    void (*drop_arguments)(KhuVmHost* host);

    /* --- steps 4 and 5 --- */
    int (*has_procedures)(KhuVmHost* host, uint32_t class_id);
    int (*run_procedures)(KhuVmHost* host, KhuObject* object, uint32_t class_id, uint32_t argc);
    int (*has_constructor)(KhuVmHost* host, uint32_t class_id);
    int (*run_constructor)(KhuVmHost* host, KhuObject* object, uint32_t class_id, uint32_t argc);

    /* --- step 6 and failure handling --- */
    void (*register_live)(KhuVmHost* host, KhuObject* object);
    /* Tears down a partially materialized object after a mid-pipeline abort. */
    void (*discard)(KhuVmHost* host, KhuObject* object);
    /* Keeps a half-built object alive across a collection triggered inside the
     * pipeline; nothing on the VM stack refers to it yet. */
    void (*retain)(KhuVmHost* host, KhuObject* object);
    void (*release)(KhuVmHost* host, KhuObject* object);

    /* Reports a runtime error with a Khudra source location if one is known. */
    void (*report_error)(KhuVmHost* host, const char* message);
} KhuProcHostApi;

/* Installs the host callback table. Must be called once before materializing. */
void khu_proc_engine_init(const KhuProcHostApi* api, KhuVmHost* host);
void khu_proc_engine_shutdown(void);
int khu_proc_engine_ready(void);
/* The currently installed host, or NULL. */
KhuVmHost* khu_proc_engine_host(void);

/* Runs the full materialization pipeline for `class_id`.
 *
 * `argc` is the number of allocation-site arguments the VM has staged on its
 * stack; they are bound to the Procedures block's parameters and to the
 * constructor's. On success `*out_object` receives the new reference. */
KhuProcStatus khu_proc_materialize(uint32_t class_id, KhuStrategy strategy, uint32_t argc,
                                   KhuObject** out_object);

/* Number of completed materializations. */
uint64_t khu_proc_materialize_count(void);

/* Current depth of nested materializations. Allocations performed *inside* a
 * Procedures block run depth-first, so this is > 1 while they are in flight. */
uint32_t khu_proc_depth(void);

/* Resets the counters. */
void khu_proc_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* KHU_UTILS_PROC_ENGINE_H */

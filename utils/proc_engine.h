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
 *   1. allocate per strategy      2. link object (header + vtable)
 *   3. bind allocation-site args  4. execute the Procedures block
 *   5. execute the constructor    6. register as live -> return the reference
 *
 * Phase 0 ships the ABI shape and a stub implementation; Phase 6 fills it in. */
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
    KHU_PROC_NOT_IMPLEMENTED = 5
} KhuProcStatus;

const char* khu_proc_status_name(KhuProcStatus status);

/* Opaque handles owned by the C++ side. */
typedef struct KhuVmHost KhuVmHost;
typedef struct KhuObject KhuObject;

/* Callbacks the engine uses to drive the VM. Every entry must be non-NULL. */
typedef struct KhuProcHostApi {
    /* Allocates `size` bytes under `strategy`, or NULL. */
    KhuObject* (*allocate)(KhuVmHost* host, uint32_t class_id, KhuStrategy strategy);
    /* Stamps the object header (class id, flags, pin 0, size, vtable) and sets
     * every field to its default (`null` = not instantiated yet). */
    void (*link_object)(KhuVmHost* host, KhuObject* object, uint32_t class_id,
                        KhuStrategy strategy);
    /* Runs the class's Procedures block against the allocation-site arguments
     * already staged on the VM stack. Returns 0 on success. */
    int (*run_procedures)(KhuVmHost* host, KhuObject* object, uint32_t class_id, uint32_t argc);
    /* Runs the constructor body. Returns 0 on success. */
    int (*run_constructor)(KhuVmHost* host, KhuObject* object, uint32_t class_id, uint32_t argc);
    /* Publishes the object to the GC / manual bookkeeping as live. */
    void (*register_live)(KhuVmHost* host, KhuObject* object);
    /* Tears down a partially materialized object after a mid-pipeline abort. */
    void (*discard)(KhuVmHost* host, KhuObject* object);
    /* Reports a runtime error with a Khudra source location if one is known. */
    void (*report_error)(KhuVmHost* host, const char* message);
    /* True when the class declares a Procedures block / a constructor. */
    int (*has_procedures)(KhuVmHost* host, uint32_t class_id);
    int (*has_constructor)(KhuVmHost* host, uint32_t class_id);
} KhuProcHostApi;

/* Installs the host callback table. Must be called once before materializing. */
void khu_proc_engine_init(const KhuProcHostApi* api, KhuVmHost* host);
void khu_proc_engine_shutdown(void);
int khu_proc_engine_ready(void);

/* Runs the full materialization pipeline for `class_id`.
 *
 * `argc` is the number of allocation-site arguments the VM has already staged;
 * they are bound to the Procedures block's parameters. On success `*out_object`
 * receives the new reference. */
KhuProcStatus khu_proc_materialize(uint32_t class_id, KhuStrategy strategy, uint32_t argc,
                                   KhuObject** out_object);

/* Number of completed materializations -- exercised by the Phase 6 tests. */
uint64_t khu_proc_materialize_count(void);

/* Current depth of nested materializations. Allocations performed *inside* a
 * Procedures block run depth-first, so this is > 1 while they are in flight. */
uint32_t khu_proc_depth(void);

#ifdef __cplusplus
}
#endif

#endif /* KHU_UTILS_PROC_ENGINE_H */

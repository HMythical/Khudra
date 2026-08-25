#include "proc_engine.h"

#include <stddef.h>

/* The engine owns the pipeline; the VM owns everything the pipeline touches.
 * Keeping the order of operations here -- in C, behind a stable ABI -- is the
 * point: it is the piece most likely to be reused by an FFI or an alternate
 * backend, and it is the piece whose ordering guarantees the language makes
 * promises about. */

static const KhuProcHostApi* g_api = NULL;
static KhuVmHost* g_host = NULL;
static uint64_t g_materialize_count = 0;
static uint32_t g_depth = 0;

const char* khu_proc_status_name(KhuProcStatus status) {
    switch (status) {
        case KHU_PROC_OK: return "ok";
        case KHU_PROC_OUT_OF_MEMORY: return "out of memory";
        case KHU_PROC_UNKNOWN_CLASS: return "unknown class id";
        case KHU_PROC_PROCEDURE_FAILED: return "Procedures block failed";
        case KHU_PROC_CONSTRUCTOR_FAILED: return "constructor failed";
        case KHU_PROC_NOT_IMPLEMENTED: return "procedure engine not implemented";
        case KHU_PROC_DEPTH_EXCEEDED: return "materialization nested too deeply";
        case KHU_PROC_BAD_ARGUMENTS: return "allocation arguments could not be bound";
    }
    return "unknown status";
}

void khu_proc_engine_init(const KhuProcHostApi* api, KhuVmHost* host) {
    g_api = api;
    g_host = host;
    g_depth = 0;
}

void khu_proc_engine_shutdown(void) {
    g_api = NULL;
    g_host = NULL;
    g_depth = 0;
}

int khu_proc_engine_ready(void) { return g_api != NULL; }

KhuVmHost* khu_proc_engine_host(void) { return g_host; }

uint64_t khu_proc_materialize_count(void) { return g_materialize_count; }

uint32_t khu_proc_depth(void) { return g_depth; }

void khu_proc_reset_stats(void) { g_materialize_count = 0; }

/* Runs one stage over the whole inheritance chain, root first.
 *
 * `has` says whether a class takes part; `run` performs the step. Every stage
 * completes for every class in the chain before the next stage begins, which is
 * what keeps "the Procedures block runs before the constructor" true for an
 * inherited object and not just per class. */
static int khu_proc_run_stage(uint32_t class_id, KhuObject* object, uint32_t argc,
                              int stage_needs_arguments,
                              int (*has)(KhuVmHost*, uint32_t),
                              int (*run)(KhuVmHost*, KhuObject*, uint32_t, uint32_t)) {
    uint32_t length = g_api->chain_length(g_host, class_id);
    uint32_t index;

    for (index = 0; index < length; ++index) {
        uint32_t step = g_api->chain_at(g_host, class_id, index);
        if (!has(g_host, step)) continue;
        if (stage_needs_arguments) g_api->push_arguments(g_host, argc);
        if (run(g_host, object, step, argc) != 0) return -1;
    }
    return 0;
}

/* Adapter so the two-argument field-initializer callback matches the stage
 * signature above. */
static int khu_proc_run_field_init(KhuVmHost* host, KhuObject* object, uint32_t class_id,
                                   uint32_t argc) {
    (void)argc;
    return g_api->run_field_init(host, object, class_id);
}

KhuProcStatus khu_proc_materialize(uint32_t class_id, KhuStrategy strategy, uint32_t argc,
                                   KhuObject** out_object) {
    KhuObject* object;
    KhuProcStatus status;

    if (out_object != NULL) *out_object = NULL;
    if (g_api == NULL) return KHU_PROC_NOT_IMPLEMENTED;

    if (g_depth >= KHU_PROC_MAX_DEPTH) {
        g_api->report_error(g_host,
                            "materialization nested too deeply; a Procedures block that "
                            "materializes its own class recurses without end");
        return KHU_PROC_DEPTH_EXCEEDED;
    }

    /* Step 3 first: lift the allocation-site arguments off the VM stack before
     * anything else can touch it. A Procedures block may materialize other
     * objects, and those run depth-first on the same stack. */
    if (g_api->take_arguments(g_host, argc) != 0) return KHU_PROC_BAD_ARGUMENTS;

    /* Step 1: allocate per strategy, and stamp the header. */
    object = g_api->allocate(g_host, class_id, strategy);
    if (object == NULL) {
        g_api->drop_arguments(g_host);
        return KHU_PROC_OUT_OF_MEMORY;
    }
    /* Nothing on the VM stack refers to it yet, and the steps below can
     * allocate. */
    g_api->retain(g_host, object);
    ++g_depth;

    /* Step 2: field initializers, base class first. */
    status = KHU_PROC_OK;
    if (khu_proc_run_stage(class_id, object, argc, 0, g_api->has_field_init,
                           khu_proc_run_field_init) != 0) {
        status = KHU_PROC_PROCEDURE_FAILED;
    }

    /* Step 4: the Procedures blocks -- "immediately executes when loaded into
     * memory". They receive the allocation-site arguments, and every one in the
     * chain runs before any constructor body does. */
    if (status == KHU_PROC_OK &&
        khu_proc_run_stage(class_id, object, argc, 1, g_api->has_procedures,
                           g_api->run_procedures) != 0) {
        status = KHU_PROC_PROCEDURE_FAILED;
    }

    /* Step 5: the constructor bodies. */
    if (status == KHU_PROC_OK &&
        khu_proc_run_stage(class_id, object, argc, 1, g_api->has_constructor,
                           g_api->run_constructor) != 0) {
        status = KHU_PROC_CONSTRUCTOR_FAILED;
    }

    --g_depth;
    g_api->drop_arguments(g_host);

    if (status != KHU_PROC_OK) {
        /* Failure semantics (docs/procedures.md, 5): the constructor does not
         * run, the object is never registered as live, its reference is never
         * returned, and the partially materialized object is discarded. Objects
         * the block already finished materializing stay live -- rolling those
         * back would mean undoing arbitrary outward calls. */
        g_api->discard(g_host, object);
        g_api->release(g_host, object);
        return status;
    }

    /* Step 6: register as live and hand the reference back to the allocating
     * site, which only now resumes. */
    g_api->register_live(g_host, object);
    g_api->release(g_host, object);
    ++g_materialize_count;
    if (out_object != NULL) *out_object = object;
    return KHU_PROC_OK;
}

#include "proc_engine.h"

#include <stddef.h>

/* Phase 0: the ABI and the pipeline skeleton exist, but no host is wired up
 * yet -- the VM that implements KhuProcHostApi arrives in Phase 3/4 and the
 * real pipeline body lands in Phase 6. */

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
    }
    return "unknown status";
}

void khu_proc_engine_init(const KhuProcHostApi* api, KhuVmHost* host) {
    g_api = api;
    g_host = host;
    g_materialize_count = 0;
    g_depth = 0;
}

void khu_proc_engine_shutdown(void) {
    g_api = NULL;
    g_host = NULL;
    g_depth = 0;
}

int khu_proc_engine_ready(void) { return g_api != NULL; }

KhuProcStatus khu_proc_materialize(uint32_t class_id, KhuStrategy strategy, uint32_t argc,
                                   KhuObject** out_object) {
    (void)class_id;
    (void)strategy;
    (void)argc;
    if (out_object != NULL) *out_object = NULL;
    if (g_api == NULL) return KHU_PROC_NOT_IMPLEMENTED;
    return KHU_PROC_NOT_IMPLEMENTED;
}

uint64_t khu_proc_materialize_count(void) { return g_materialize_count; }

uint32_t khu_proc_depth(void) { return g_depth; }

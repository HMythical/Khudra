#include "vm/proc_host.h"

#include "vm/vm.h"

namespace khu::vm {
namespace {

// The engine hands back the opaque handles it was given, so the casts are the
// inverse of the ones in install_proc_host.
Vm& as_vm(KhuVmHost* host) { return *reinterpret_cast<Vm*>(host); }
Object* as_object(KhuObject* object) { return reinterpret_cast<Object*>(object); }
KhuObject* as_handle(Object* object) { return reinterpret_cast<KhuObject*>(object); }

std::uint32_t host_chain_length(KhuVmHost* host, std::uint32_t class_id) {
    return as_vm(host).proc_chain_length(class_id);
}

std::uint32_t host_chain_at(KhuVmHost* host, std::uint32_t class_id, std::uint32_t index) {
    return as_vm(host).proc_chain_at(class_id, index);
}

KhuObject* host_allocate(KhuVmHost* host, std::uint32_t class_id, KhuStrategy strategy) {
    return as_handle(as_vm(host).proc_allocate(class_id, strategy));
}

int host_has_field_init(KhuVmHost* host, std::uint32_t class_id) {
    return as_vm(host).proc_has_field_init(class_id) ? 1 : 0;
}

int host_run_field_init(KhuVmHost* host, KhuObject* object, std::uint32_t class_id) {
    return as_vm(host).proc_run_field_init(as_object(object), class_id) ? 0 : -1;
}

int host_take_arguments(KhuVmHost* host, std::uint32_t argc) {
    return as_vm(host).proc_take_arguments(argc) ? 0 : -1;
}

void host_push_arguments(KhuVmHost* host, std::uint32_t argc) {
    as_vm(host).proc_push_arguments(argc);
}

void host_drop_arguments(KhuVmHost* host) { as_vm(host).proc_drop_arguments(); }

int host_has_procedures(KhuVmHost* host, std::uint32_t class_id) {
    return as_vm(host).proc_has_procedures(class_id) ? 1 : 0;
}

int host_run_procedures(KhuVmHost* host, KhuObject* object, std::uint32_t class_id,
                        std::uint32_t argc) {
    return as_vm(host).proc_run_procedures(as_object(object), class_id, argc) ? 0 : -1;
}

int host_has_constructor(KhuVmHost* host, std::uint32_t class_id) {
    return as_vm(host).proc_has_constructor(class_id) ? 1 : 0;
}

int host_run_constructor(KhuVmHost* host, KhuObject* object, std::uint32_t class_id,
                         std::uint32_t argc) {
    return as_vm(host).proc_run_constructor(as_object(object), class_id, argc) ? 0 : -1;
}

void host_register_live(KhuVmHost* host, KhuObject* object) {
    as_vm(host).proc_register_live(as_object(object));
}

void host_discard(KhuVmHost* host, KhuObject* object) {
    as_vm(host).proc_discard(as_object(object));
}

void host_retain(KhuVmHost* host, KhuObject* object) {
    as_vm(host).proc_retain(as_object(object));
}

void host_release(KhuVmHost* host, KhuObject* object) {
    as_vm(host).proc_release(as_object(object));
}

void host_report_error(KhuVmHost* host, const char* message) {
    as_vm(host).proc_report_error(message ? message : "materialization failed");
}

}  // namespace

const KhuProcHostApi& proc_host_api() {
    static const KhuProcHostApi api = {
        &host_chain_length,   &host_chain_at,       &host_allocate,
        &host_has_field_init, &host_run_field_init, &host_take_arguments,
        &host_push_arguments, &host_drop_arguments, &host_has_procedures,
        &host_run_procedures, &host_has_constructor, &host_run_constructor,
        &host_register_live,  &host_discard,        &host_retain,
        &host_release,        &host_report_error,
    };
    return api;
}

void install_proc_host(Vm& vm) {
    khu_proc_engine_init(&proc_host_api(), reinterpret_cast<KhuVmHost*>(&vm));
}

void uninstall_proc_host(Vm& vm) {
    if (khu_proc_engine_host() == reinterpret_cast<KhuVmHost*>(&vm)) {
        khu_proc_engine_shutdown();
    }
}

}  // namespace khu::vm

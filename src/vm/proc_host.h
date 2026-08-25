// The VM's side of the Procedure engine ABI.
//
// utils/proc_engine.c owns the materialization pipeline; this file is the
// table of C callbacks that let it drive the C++ VM without knowing anything
// about it.
#ifndef KHU_VM_PROC_HOST_H
#define KHU_VM_PROC_HOST_H

extern "C" {
#include "proc_engine.h"
}

namespace khu::vm {

class Vm;

// The callback table. Stable for the process; the host pointer varies per VM.
const KhuProcHostApi& proc_host_api();

// Installs `vm` as the engine's host, and removes it again.
void install_proc_host(Vm& vm);
void uninstall_proc_host(Vm& vm);

}  // namespace khu::vm

#endif  // KHU_VM_PROC_HOST_H

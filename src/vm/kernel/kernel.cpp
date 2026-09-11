// The kernel tier's shared implementation: the platform branch, the region
// registry and the buffer/address bridge. Everything here is portable except
// `platform()` itself, which answers the compile-time host -- and that one
// answer is what tells a program which per-OS namespace it may call, so the
// per-OS surfaces must never drift from it.
#include "vm/kernel/kernel.h"

#include "bytecode/native.h"
#include "vm/natives.h"
#include "vm/value.h"

namespace khu::vm::kernel {

void KernelServices::record(void* base, std::uint64_t length, bool owned) {
    regions.push_back(KernelRegion{reinterpret_cast<std::uint64_t>(base), length, owned});
}

void KernelServices::forget(const void* base) {
    std::uint64_t wanted = reinterpret_cast<std::uint64_t>(base);
    for (std::size_t i = 0; i < regions.size(); ++i) {
        if (regions[i].base == wanted) {
            regions.erase(regions.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
    }
}

std::uint64_t KernelServices::extent_of(const void* address) const {
    std::uint64_t addr = reinterpret_cast<std::uint64_t>(address);
    for (const KernelRegion& region : regions) {
        if (addr >= region.base && addr < region.base + region.length) {
            return region.base + region.length - addr;
        }
    }
    return 0;
}

const KernelRegion* KernelServices::find_base(const void* base) const {
    std::uint64_t wanted = reinterpret_cast<std::uint64_t>(base);
    for (const KernelRegion& region : regions) {
        if (region.base == wanted) return &region;
    }
    return nullptr;
}

std::int32_t platform() {
#if defined(_WIN32)
    return 2;
#elif defined(__linux__)
    return 1;
#elif defined(__APPLE__)
    return 3;
#else
    return 0;
#endif
}

std::string platform_name() {
    switch (platform()) {
        case 1:
            return "linux";
        case 2:
            return "windows";
        case 3:
            return "mac";
        default:
            return "unknown";
    }
}

NativeOutcome to_address(const Value& buffer) {
    if (buffer.is_null_reference() ||
        (buffer.tag == TypeTag::Ptr && buffer.as_raw == nullptr)) {
        return NativeOutcome::produced(Value::make_int(TypeTag::Int64, 0));
    }
    if (buffer.tag != TypeTag::Ptr) {
        return NativeOutcome::failed("khuAdvKernel.toAddress expects a *byte, found " +
                                     std::string(bytecode::type_tag_name(buffer.tag)));
    }
    std::uintptr_t address = reinterpret_cast<std::uintptr_t>(buffer.as_raw);
    return NativeOutcome::produced(
        Value::make_int(TypeTag::Int64, static_cast<std::int64_t>(address)));
}

NativeOutcome from_address(const Value& address, const Value& length, NativeServices& services) {
    // The length is the caller's assertion and only that: a bare address
    // carries no extent. A non-positive assertion is nonsense, and an
    // address+length that wraps is a buffer that cannot exist.
    if (length.as_int <= 0) {
        return NativeOutcome::failed("khuAdvKernel.fromAddress requires a positive length");
    }
    std::uint64_t base = static_cast<std::uint64_t>(address.as_int);
    std::uint64_t extent = static_cast<std::uint64_t>(length.as_int);
    if (address.as_int == 0) {
        // Address 0 is null, consistent with khuStdMem.isNull: nothing to
        // assert a length over.
        return NativeOutcome::produced(Value::null_pointer());
    }
    if (extent > ~std::uint64_t{0} - base) {
        return NativeOutcome::failed(
            "khuAdvKernel.fromAddress was given an address + length that wraps around");
    }
    void* pointer = reinterpret_cast<void*>(base);
    services.kernel.record(pointer, extent, false);
    return NativeOutcome::produced(Value::make_pointer(pointer));
}

NativeOutcome drop_address(const Value& region, NativeServices& services) {
    // Forgetting an extent this runtime never recorded is a no-op: the pointer
    // stays valid either way, and there is nothing to un-learn.
    if (region.tag == TypeTag::Ptr && region.as_raw != nullptr) {
        services.kernel.forget(region.as_raw);
    }
    return NativeOutcome::nothing();
}

std::string wrong_os_message(std::string_view where, std::string_view expected) {
    std::string constant;
    if (expected == "Windows") constant = "windows";
    else if (expected == "Linux") constant = "linux";
    else if (expected == "macOS") constant = "mac";
    else constant.assign(expected);
    return std::string(where) + " is a " + std::string(expected) +
           " call, but this program is running on " + platform_name() + "; guard it with " +
           "khuAdvKernel.platform() == khuAdvKernel." + constant + "()";
}

}  // namespace khu::vm::kernel
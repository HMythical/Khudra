#include "sema/builtins.h"

namespace khu::sema {

NativeBinding resolve_native_binding(std::string_view namespace_name,
                                     std::string_view member_name, std::size_t arity) {
    NativeBinding binding;

    if (namespace_name == kMathNamespace) {
        // Same-width arithmetic lowers to one instruction. There is no implicit
        // conversion to fall back on, so every width is its own declaration in
        // lib/math.khu and every one of them binds here.
        if (arity != 2) return binding;
        if (member_name == "add") binding.intrinsic = Intrinsic::Add;
        else if (member_name == "subtract") binding.intrinsic = Intrinsic::Subtract;
        else if (member_name == "multiply") binding.intrinsic = Intrinsic::Multiply;
        else if (member_name == "divide") binding.intrinsic = Intrinsic::Divide;
        else if (member_name == "remainder") binding.intrinsic = Intrinsic::Remainder;
        return binding;
    }

    if (namespace_name == kIoNamespace) {
        if (member_name == "print" && arity == 1) binding.native = Native::Print;
        else if (member_name == "printLine" && arity == 1) binding.native = Native::PrintLine;
        else if (member_name == "readLine" && arity == 0) binding.native = Native::ReadLine;
        return binding;
    }

    if (namespace_name == kRuntimeNamespace) {
        if (member_name == "stdlibLoadObject" && arity == 0) {
            binding.native = Native::StdlibLoadObject;
        } else if (member_name == "getType" && arity == 0) {
            binding.native = Native::GetType;
        } else if (member_name == "LoadRuntimeType" && arity == 0) {
            binding.native = Native::LoadRuntimeType;
        }
        return binding;
    }

    return binding;
}

}  // namespace khu::sema

#include "sema/builtins.h"

namespace khu::sema {
namespace {

struct Registrar {
    Program& program;
    TypeContext& types;
    util::Arena& arena;
    std::uint32_t next_native = static_cast<std::uint32_t>(Native::First);

    ClassSymbol* make_namespace(std::string_view name) {
        auto* symbol = arena.create<ClassSymbol>();
        symbol->name = name;
        symbol->is_namespace = true;
        symbol->class_id = static_cast<std::uint32_t>(program.namespaces.size()) | 0x80000000u;
        program.namespaces.push(symbol);
        program.class_index.insert(name, symbol);
        return symbol;
    }

    VarSymbol* make_param(std::string_view name, const Type* type) {
        auto* param = arena.create<VarSymbol>();
        param->name = name;
        param->type = type;
        param->role = VarRole::Parameter;
        param->visibility = ast::Visibility::Public;
        return param;
    }

    MethodSymbol* declare(ClassSymbol* owner, std::string_view name, const Type* return_type) {
        auto* method = arena.create<MethodSymbol>();
        method->name = name;
        method->form = ast::MethodForm::Func;
        method->visibility = ast::Visibility::Public;
        method->return_type = return_type;
        method->owner = owner;
        method->is_static = true;
        owner->methods.push(method);
        if (util::Array<MethodSymbol*>* bucket = owner->method_index.find(name)) {
            bucket->push(method);
        } else {
            util::Array<MethodSymbol*> fresh;
            fresh.push(method);
            owner->method_index.insert(name, std::move(fresh));
        }
        return method;
    }

    // A same-width binary operation: add(int32, int32) -> int32.
    void declare_binary(ClassSymbol* owner, std::string_view name, const Type* type) {
        MethodSymbol* method = declare(owner, name, type);
        method->params.push(make_param("a", type));
        method->params.push(make_param("b", type));
        method->is_native = true;
        method->native_id = 0;  // lowered to a bytecode arithmetic op, not a call
    }

    void declare_native(ClassSymbol* owner, std::string_view name, const Type* return_type,
                        Native native, const Type* argument) {
        MethodSymbol* method = declare(owner, name, return_type);
        if (argument) method->params.push(make_param("value", argument));
        method->is_native = true;
        method->native_id = static_cast<std::uint32_t>(native);
        next_native = method->native_id + 1;
    }
};

}  // namespace

void install_builtins(Program& program, TypeContext& types, util::Arena& arena) {
    Registrar registrar{program, types, arena, static_cast<std::uint32_t>(Native::First)};

    // Every numeric type, in the order the checker reports them.
    const Type* numeric[] = {
        types.signed_int(8),    types.signed_int(16),   types.signed_int(32),
        types.signed_int(64),   types.unsigned_int(8),  types.unsigned_int(16),
        types.unsigned_int(32), types.unsigned_int(64), types.float_type(32),
        types.float_type(64),
    };

    // khuStdMath: the full integer-width matrix. Khudra has no implicit
    // widening, so every operation is declared once per width and mixing widths
    // is a type error that names convertTo as the way out.
    ClassSymbol* math = registrar.make_namespace(kMathNamespace);
    for (const Type* type : numeric) {
        registrar.declare_binary(math, "add", type);
        registrar.declare_binary(math, "subtract", type);
        registrar.declare_binary(math, "multiply", type);
        registrar.declare_binary(math, "divide", type);
        if (type->is_integer()) registrar.declare_binary(math, "remainder", type);
    }

    // convertTo is an intrinsic: its first argument is a type, and it lowers to
    // a `convert` opcode rather than a call.
    MethodSymbol* convert = registrar.declare(math, "convertTo", types.error());
    convert->is_intrinsic = true;
    convert->intrinsic_id = static_cast<std::uint32_t>(Intrinsic::ConvertTo);

    // io: printing and reading.
    ClassSymbol* io = registrar.make_namespace(kIoNamespace);
    registrar.declare_native(io, "print", types.void_type(), Native::Print, types.string_type());
    registrar.declare_native(io, "printLine", types.void_type(), Native::PrintLine,
                             types.string_type());
    for (const Type* type : numeric) {
        registrar.declare_native(io, "print", types.void_type(), Native::Print, type);
        registrar.declare_native(io, "printLine", types.void_type(), Native::PrintLine, type);
    }
    registrar.declare_native(io, "print", types.void_type(), Native::Print, types.bool_type());
    registrar.declare_native(io, "printLine", types.void_type(), Native::PrintLine,
                             types.bool_type());
    registrar.declare_native(io, "readLine", types.string_type(), Native::ReadLine, nullptr);

    // khu: the runtime namespace. Phase 7 fills it in from lib/; registering it
    // now means `khu.somethingUnknown()` reports a missing member rather than an
    // unknown name.
    registrar.make_namespace(kRuntimeNamespace);
}

}  // namespace khu::sema

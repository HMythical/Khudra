#include "sema/builtins.h"

namespace khu::sema {

namespace {

NativeBinding to_intrinsic(Intrinsic which) {
    NativeBinding binding;
    binding.intrinsic = which;
    return binding;
}

NativeBinding to_native(Native which) {
    NativeBinding binding;
    binding.native = which;
    return binding;
}

}  // namespace

std::size_t intrinsic_arity(Intrinsic which) {
    switch (which) {
        case Intrinsic::Neg:
        case Intrinsic::ArraySize: return 1;
        case Intrinsic::None: return 0;
        default: return 2;
    }
}

NativeBinding bind_math(std::string_view member_name, std::size_t arity) {
    // Same-width arithmetic lowers to one instruction. There is no implicit
    // conversion to fall back on, so every width is its own declaration in
    // lib/math.khu and every one of them binds here.
    if (arity == 1 && member_name == "neg") return to_intrinsic(Intrinsic::Neg);
    if (arity == 2) {
        if (member_name == "add") return to_intrinsic(Intrinsic::Add);
        if (member_name == "subtract") return to_intrinsic(Intrinsic::Subtract);
        if (member_name == "multiply") return to_intrinsic(Intrinsic::Multiply);
        if (member_name == "divide") return to_intrinsic(Intrinsic::Divide);
        if (member_name == "remainder") return to_intrinsic(Intrinsic::Remainder);
    }

    // The utilities and the transcendentals are real calls: there is no
    // instruction for them, and no way to write them in Khudra either.
    switch (arity) {
        case 0:
            if (member_name == "pi") return to_native(Native::Pi);
            if (member_name == "e") return to_native(Native::E);
            break;
        case 1:
            if (member_name == "abs") return to_native(Native::Abs);
            if (member_name == "signum") return to_native(Native::Signum);
            if (member_name == "sqrt") return to_native(Native::Sqrt);
            if (member_name == "floor") return to_native(Native::Floor);
            if (member_name == "ceil") return to_native(Native::Ceil);
            if (member_name == "round") return to_native(Native::Round);
            if (member_name == "exp") return to_native(Native::Exp);
            if (member_name == "log") return to_native(Native::Log);
            if (member_name == "log10") return to_native(Native::Log10);
            if (member_name == "sin") return to_native(Native::Sin);
            if (member_name == "cos") return to_native(Native::Cos);
            if (member_name == "tan") return to_native(Native::Tan);
            if (member_name == "asin") return to_native(Native::Asin);
            if (member_name == "acos") return to_native(Native::Acos);
            if (member_name == "atan") return to_native(Native::Atan);
            if (member_name == "sinh") return to_native(Native::Sinh);
            if (member_name == "cosh") return to_native(Native::Cosh);
            if (member_name == "tanh") return to_native(Native::Tanh);
            break;
        case 2:
            if (member_name == "min") return to_native(Native::Min);
            if (member_name == "max") return to_native(Native::Max);
            if (member_name == "gcd") return to_native(Native::Gcd);
            if (member_name == "lcm") return to_native(Native::Lcm);
            if (member_name == "pow") return to_native(Native::Pow);
            if (member_name == "fmod") return to_native(Native::Fmod);
            if (member_name == "atan2") return to_native(Native::Atan2);
            break;
        case 3:
            if (member_name == "clamp") return to_native(Native::Clamp);
            break;
        default:
            break;
    }
    return NativeBinding{};
}

NativeBinding bind_conv(std::string_view member_name, std::size_t arity) {
    if (arity != 1) return NativeBinding{};
    if (member_name == "toString") return to_native(Native::ToString);
    if (member_name == "parseInt8") return to_native(Native::ParseInt8);
    if (member_name == "parseInt16") return to_native(Native::ParseInt16);
    if (member_name == "parseInt32") return to_native(Native::ParseInt32);
    if (member_name == "parseInt64") return to_native(Native::ParseInt64);
    if (member_name == "parseUInt8") return to_native(Native::ParseUInt8);
    if (member_name == "parseUInt16") return to_native(Native::ParseUInt16);
    if (member_name == "parseUInt32") return to_native(Native::ParseUInt32);
    if (member_name == "parseUInt64") return to_native(Native::ParseUInt64);
    if (member_name == "parseFloat") return to_native(Native::ParseFloat);
    if (member_name == "parseDFloat") return to_native(Native::ParseDFloat);
    if (member_name == "isNumeric") return to_native(Native::IsNumeric);
    if (member_name == "isDigit") return to_native(Native::IsDigit);
    if (member_name == "isLetter") return to_native(Native::IsLetter);
    if (member_name == "isWhitespace") return to_native(Native::IsWhitespace);
    if (member_name == "toUpper") return to_native(Native::ToUpper);
    if (member_name == "toLower") return to_native(Native::ToLower);
    if (member_name == "toChar") return to_native(Native::ToChar);
    if (member_name == "boolToInt") return to_native(Native::BoolToInt);
    if (member_name == "intToBool") return to_native(Native::IntToBool);
    if (member_name == "canParseInt8") return to_native(Native::CanParseInt8);
    if (member_name == "canParseInt16") return to_native(Native::CanParseInt16);
    if (member_name == "canParseInt32") return to_native(Native::CanParseInt32);
    if (member_name == "canParseInt64") return to_native(Native::CanParseInt64);
    if (member_name == "canParseUInt8") return to_native(Native::CanParseUInt8);
    if (member_name == "canParseUInt16") return to_native(Native::CanParseUInt16);
    if (member_name == "canParseUInt32") return to_native(Native::CanParseUInt32);
    if (member_name == "canParseUInt64") return to_native(Native::CanParseUInt64);
    if (member_name == "canParseFloat") return to_native(Native::CanParseFloat);
    if (member_name == "canParseDFloat") return to_native(Native::CanParseDFloat);
    return NativeBinding{};
}

NativeBinding bind_string(std::string_view member_name, std::size_t arity) {
    // `concat` is the one variadic-looking member: Khudra has no varargs, so it
    // is a handful of fixed arities that all reach the same implementation,
    // which walks its arguments.
    if (member_name == "concat" && arity >= 2 && arity <= 8) {
        return to_native(Native::StrConcat);
    }
    // `indexOf` takes an optional starting offset, which is a second arity
    // rather than a default argument.
    if (member_name == "indexOf" && (arity == 2 || arity == 3)) {
        return to_native(Native::StrIndexOf);
    }

    switch (arity) {
        case 1:
            if (member_name == "length") return to_native(Native::StrLength);
            if (member_name == "toUpperCase") return to_native(Native::StrToUpperCase);
            if (member_name == "toLowerCase") return to_native(Native::StrToLowerCase);
            if (member_name == "trim") return to_native(Native::StrTrim);
            if (member_name == "trimStart") return to_native(Native::StrTrimStart);
            if (member_name == "trimEnd") return to_native(Native::StrTrimEnd);
            if (member_name == "isEmpty") return to_native(Native::StrIsEmpty);
            if (member_name == "isBlank") return to_native(Native::StrIsBlank);
            break;
        case 2:
            if (member_name == "charAt") return to_native(Native::StrCharAt);
            if (member_name == "lastIndexOf") return to_native(Native::StrLastIndexOf);
            if (member_name == "contains") return to_native(Native::StrContains);
            if (member_name == "startsWith") return to_native(Native::StrStartsWith);
            if (member_name == "endsWith") return to_native(Native::StrEndsWith);
            if (member_name == "equals") return to_native(Native::StrEquals);
            if (member_name == "equalsIgnoreCase") {
                return to_native(Native::StrEqualsIgnoreCase);
            }
            if (member_name == "compareTo") return to_native(Native::StrCompareTo);
            if (member_name == "compareToIgnoreCase") {
                return to_native(Native::StrCompareToIgnoreCase);
            }
            if (member_name == "repeat") return to_native(Native::StrRepeat);
            break;
        case 3:
            if (member_name == "substring") return to_native(Native::StrSubstring);
            if (member_name == "replace") return to_native(Native::StrReplace);
            if (member_name == "replaceAll") return to_native(Native::StrReplaceAll);
            break;
        default:
            break;
    }
    return NativeBinding{};
}

NativeBinding bind_collection(std::string_view member_name, std::size_t arity) {
    // `arrayCreate` is not here: its first argument is a type, so it cannot be
    // declared in Khudra at all and the checker handles it directly, the way it
    // handles khuStdMath.convertTo.
    if (member_name == "arraySize" && arity == 1) return to_intrinsic(Intrinsic::ArraySize);
    // `hash` and `sameValue` are not here either: their argument is a value of
    // any type, which no Khudra signature can say, so the checker recognizes
    // them directly. They still bind to ordinary native ids.
    return NativeBinding{};
}

NativeBinding bind_errors(std::string_view member_name, std::size_t arity) {
    // `fail` is the only member here that does anything; the rest are the
    // error-code table. Constants are zero-argument functions because a
    // namespace has no const members yet (docs/roadmap.md); the values do not
    // change when that lands.
    if (member_name == "fail" && arity == 1) return to_native(Native::ErrFail);
    if (arity != 0) return NativeBinding{};
    if (member_name == "none") return to_native(Native::ErrNone);
    if (member_name == "bounds") return to_native(Native::ErrBounds);
    if (member_name == "parse") return to_native(Native::ErrParse);
    if (member_name == "nullReference") return to_native(Native::ErrNullReference);
    if (member_name == "io") return to_native(Native::ErrIo);
    if (member_name == "divideByZero") return to_native(Native::ErrDivideByZero);
    if (member_name == "notFound") return to_native(Native::ErrNotFound);
    if (member_name == "invalidArgument") return to_native(Native::ErrInvalidArgument);
    if (member_name == "unsupported") return to_native(Native::ErrUnsupported);
    if (member_name == "overflow") return to_native(Native::ErrOverflow);
    if (member_name == "empty") return to_native(Native::ErrEmpty);
    return NativeBinding{};
}

NativeBinding bind_mem(std::string_view member_name, std::size_t arity) {
    switch (arity) {
        case 0:
            if (member_name == "liveBytes") return to_native(Native::MemLiveBytes);
            if (member_name == "liveBlocks") return to_native(Native::MemLiveBlocks);
            break;
        case 1:
            if (member_name == "alloc") return to_native(Native::MemAlloc);
            if (member_name == "release") return to_native(Native::MemFree);
            if (member_name == "addressOf") return to_native(Native::MemAddressOf);
            if (member_name == "sizeOf") return to_native(Native::MemSizeOf);
            if (member_name == "isNull") return to_native(Native::MemIsNull);
            break;
        case 2:
            if (member_name == "realloc") return to_native(Native::MemRealloc);
            if (member_name == "zero") return to_native(Native::MemZero);
            if (member_name == "refEquals") return to_native(Native::MemRefEquals);
            break;
        case 3:
            if (member_name == "copy") return to_native(Native::MemCopy);
            if (member_name == "move") return to_native(Native::MemMove);
            if (member_name == "fill") return to_native(Native::MemFill);
            if (member_name == "compare") return to_native(Native::MemCompare);
            break;
        default:
            break;
    }
    return NativeBinding{};
}

NativeBinding bind_io(std::string_view member_name, std::size_t arity) {
    switch (arity) {
        case 0:
            if (member_name == "readLine") return to_native(Native::ReadLine);
            if (member_name == "flush") return to_native(Native::Flush);
            if (member_name == "readChar") return to_native(Native::ReadChar);
            if (member_name == "readByte") return to_native(Native::ReadByte);
            if (member_name == "readBool") return to_native(Native::ReadBool);
            if (member_name == "readInt8") return to_native(Native::ReadInt8);
            if (member_name == "readInt16") return to_native(Native::ReadInt16);
            if (member_name == "readInt32") return to_native(Native::ReadInt32);
            if (member_name == "readInt64") return to_native(Native::ReadInt64);
            if (member_name == "readUInt8") return to_native(Native::ReadUInt8);
            if (member_name == "readUInt16") return to_native(Native::ReadUInt16);
            if (member_name == "readUInt32") return to_native(Native::ReadUInt32);
            if (member_name == "readUInt64") return to_native(Native::ReadUInt64);
            if (member_name == "readFloat") return to_native(Native::ReadFloat);
            if (member_name == "readDFloat") return to_native(Native::ReadDFloat);
            break;
        case 1:
            if (member_name == "print") return to_native(Native::Print);
            if (member_name == "printLine") return to_native(Native::PrintLine);
            if (member_name == "writeString") return to_native(Native::WriteString);
            break;
        case 2:
            if (member_name == "writeBytes") return to_native(Native::WriteBytes);
            break;
        default:
            break;
    }
    // `describe` is not here: it takes a value of any type, which no Khudra
    // signature can say, so the checker recognizes it the way it recognizes
    // khuStdCollection.hash.
    return NativeBinding{};
}

NativeBinding bind_std_err(std::string_view member_name, std::size_t arity) {
    if (member_name == "errPrint" && arity == 1) return to_native(Native::ErrPrint);
    if (member_name == "errPrintLine" && arity == 1) return to_native(Native::ErrPrintLine);
    if (member_name == "errWriteString" && arity == 1) {
        return to_native(Native::ErrWriteString);
    }
    if (member_name == "errWriteBytes" && arity == 2) return to_native(Native::ErrWriteBytes);
    if (member_name == "flush" && arity == 0) return to_native(Native::ErrFlush);
    return NativeBinding{};
}

NativeBinding bind_random(std::string_view member_name, std::size_t arity) {
    if (member_name == "seed" && arity == 1) return to_native(Native::RandomSeed);
    // `nextInt` has a bounded form as a second arity, not a default argument.
    if (member_name == "nextInt" && (arity == 0 || arity == 1)) {
        return to_native(Native::RandomNextInt);
    }
    if (arity == 0) {
        if (member_name == "nextInt64") return to_native(Native::RandomNextInt64);
        if (member_name == "nextFloat") return to_native(Native::RandomNextFloat);
        if (member_name == "nextDouble") return to_native(Native::RandomNextDouble);
        if (member_name == "nextBool") return to_native(Native::RandomNextBool);
    }
    if (member_name == "nextBytes" && arity == 2) return to_native(Native::RandomNextBytes);
    return NativeBinding{};
}

NativeBinding bind_time(std::string_view member_name, std::size_t arity) {
    if (arity == 0) {
        if (member_name == "nowMillis") return to_native(Native::TimeNowMillis);
        if (member_name == "nowNanos") return to_native(Native::TimeNowNanos);
        if (member_name == "monotonicNanos") return to_native(Native::TimeMonotonicNanos);
    }
    if (arity == 1) {
        if (member_name == "nowString") return to_native(Native::TimeNowString);
        if (member_name == "sleep") return to_native(Native::TimeSleep);
    }
    return NativeBinding{};
}

NativeBinding bind_runtime(std::string_view member_name, std::size_t arity) {
    if (arity != 0) return NativeBinding{};
    if (member_name == "stdlibLoadObject") return to_native(Native::StdlibLoadObject);
    if (member_name == "getType") return to_native(Native::GetType);
    if (member_name == "LoadRuntimeType") return to_native(Native::LoadRuntimeType);
    return NativeBinding{};
}

NativeBinding resolve_native_binding(std::string_view namespace_name,
                                     std::string_view member_name, std::size_t arity) {
    if (namespace_name == kMathNamespace) return bind_math(member_name, arity);
    if (namespace_name == kConvNamespace) return bind_conv(member_name, arity);
    if (namespace_name == kStringNamespace) return bind_string(member_name, arity);
    if (namespace_name == kCollectionNamespace) return bind_collection(member_name, arity);
    if (namespace_name == kErrorsNamespace) return bind_errors(member_name, arity);
    if (namespace_name == kMemNamespace) return bind_mem(member_name, arity);
    if (namespace_name == kIoNamespace) return bind_io(member_name, arity);
    if (namespace_name == kStdErrNamespace) return bind_std_err(member_name, arity);
    if (namespace_name == kRandomNamespace) return bind_random(member_name, arity);
    if (namespace_name == kTimeNamespace) return bind_time(member_name, arity);
    if (namespace_name == kRuntimeNamespace) return bind_runtime(member_name, arity);
    return NativeBinding{};
}

}  // namespace khu::sema

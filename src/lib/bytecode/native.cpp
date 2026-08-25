#include "bytecode/native.h"

namespace khu::bytecode {

const char* native_name(NativeId id) {
    switch (id) {
        case NativeId::None: return "<none>";
        case NativeId::Print: return "io.print";
        case NativeId::PrintLine: return "io.printLine";
        case NativeId::ReadLine: return "io.readLine";
        case NativeId::StdlibLoadObject: return "khu.stdlibLoadObject";
        case NativeId::GetType: return "khu.getType";
        case NativeId::LoadRuntimeType: return "khu.LoadRuntimeType";
    }
    return "<native>";
}

}  // namespace khu::bytecode

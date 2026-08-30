#include "bytecode/native.h"

namespace khu::bytecode {

const char* native_name(NativeId id) {
    switch (id) {
        case NativeId::None: return "<none>";
#define KHU_NATIVE_NAME(name, value, text, results) \
    case NativeId::name: return text;
        KHU_NATIVES(KHU_NATIVE_NAME)
#undef KHU_NATIVE_NAME
    }
    return "<native>";
}

int native_result_count(NativeId id) {
    switch (id) {
        case NativeId::None: return 0;
#define KHU_NATIVE_RESULTS(name, value, text, results) \
    case NativeId::name: return results;
        KHU_NATIVES(KHU_NATIVE_RESULTS)
#undef KHU_NATIVE_RESULTS
    }
    return 0;
}

bool native_is_known(NativeId id) {
    switch (id) {
        case NativeId::None: return false;
#define KHU_NATIVE_KNOWN(name, value, text, results) \
    case NativeId::name: return true;
        KHU_NATIVES(KHU_NATIVE_KNOWN)
#undef KHU_NATIVE_KNOWN
    }
    return false;
}

}  // namespace khu::bytecode

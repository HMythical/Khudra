// The entry point of a standalone binary written by `khudra build`.
//
// There is no interpreter in this program. The lowered methods are linked in
// beside it; the embedded .kbc image supplies the class table, the constant
// pool and the line tables, exactly the way `khudra run` would have loaded them
// from disk. Entry dispatch is NativeHost::run, so a built binary and
// `khudra run` make the same decision about `main` versus root-class
// materialization.
#include <cstdio>
#include <string>

#include "bytecode/module.h"
#include "host/khu_native_abi.h"
#include "host/native_host.h"

int main() {
    std::string bytes(reinterpret_cast<const char*>(khu_native_image),
                      static_cast<std::size_t>(khu_native_image_size));

    khu::bytecode::Module module;
    std::string error;
    if (!khu::bytecode::deserialize(bytes, module, error)) {
        std::fprintf(stderr, "khudra: this program's embedded image is not valid: %s\n",
                     error.c_str());
        return 1;
    }

    khu::native::NativeHost host(module, khu_native_methods, khu_native_method_count);
    if (!host.run()) {
        std::fwrite(host.error().data(), 1, host.error().size(), stderr);
        return 1;
    }
    return 0;
}

#include "bytecode/verifier.h"

#include "util/hashmap.h"

namespace khu::bytecode {
namespace {

class MethodVerifier {
public:
    MethodVerifier(const Module& module, std::int32_t index, const MethodEntry& method,
                   util::Array<VerificationError>& errors)
        : module_(module), index_(index), method_(method), errors_(errors) {
        for (const ClassEntry& entry : module_.classes) {
            if (entry.fields.size() > widest_class_) {
                widest_class_ = static_cast<std::uint32_t>(entry.fields.size());
            }
            if (entry.vtable.size() > longest_vtable_) {
                longest_vtable_ = static_cast<std::uint32_t>(entry.vtable.size());
            }
        }
    }

    void run() {
        decode_instructions();
        check_jump_targets();
    }

private:
    void fail(std::uint32_t offset, std::string message) {
        errors_.push(VerificationError{index_, offset, std::move(message)});
    }

    std::uint16_t read_u16(std::uint32_t offset) const {
        return static_cast<std::uint16_t>(
            method_.code[offset] | (static_cast<std::uint16_t>(method_.code[offset + 1]) << 8));
    }

    std::int32_t read_i32(std::uint32_t offset) const {
        std::uint32_t bits = 0;
        for (int i = 0; i < 4; ++i) {
            bits |= static_cast<std::uint32_t>(method_.code[offset + static_cast<std::uint32_t>(i)])
                    << (i * 8);
        }
        return static_cast<std::int32_t>(bits);
    }

    bool valid_type(std::uint8_t raw) const { return raw < static_cast<std::uint8_t>(TypeTag::Count); }

    // Walks the code linearly, recording where each instruction starts so a
    // jump into the middle of one can be caught.
    void decode_instructions() {
        auto length = static_cast<std::uint32_t>(method_.code.size());
        boundaries_.resize(length + 1, false);
        boundaries_[length] = true;  // falling off the end is a valid target

        std::uint32_t offset = 0;
        while (offset < length) {
            boundaries_[offset] = true;
            std::uint8_t raw = method_.code[offset];
            if (raw >= static_cast<std::uint8_t>(Op::Count)) {
                fail(offset, "unknown opcode 0x" + std::to_string(static_cast<int>(raw)));
                return;
            }

            auto op = static_cast<Op>(raw);
            std::uint32_t size = instruction_size(op);
            if (offset + size > length) {
                fail(offset, std::string("'") + mnemonic(op) +
                                 "' runs past the end of the method");
                return;
            }
            check_operands(op, offset);
            offset += size;
        }
    }

    void check_operands(Op op, std::uint32_t offset) {
        std::uint32_t operand = offset + 1;

        switch (op) {
            case Op::LoadConst: {
                std::uint16_t index = read_u16(operand);
                if (index >= module_.constants.size()) {
                    fail(offset, "constant #" + std::to_string(index) + " does not exist");
                }
                break;
            }

            case Op::LoadLocal:
            case Op::StoreLocal: {
                std::uint16_t slot = read_u16(operand);
                if (slot >= method_.frame_size) {
                    fail(offset, "local slot " + std::to_string(slot) +
                                     " is outside a frame of " +
                                     std::to_string(method_.frame_size));
                }
                break;
            }

            case Op::GetField:
            case Op::PutField: {
                // Which class the slot belongs to is the *receiver's*, which is
                // only known at run time -- so the interpreter checks it there.
                // What is checkable here is that the value is not wild.
                std::uint16_t slot = read_u16(operand);
                if (slot >= widest_class_) {
                    fail(offset, "field slot " + std::to_string(slot) +
                                     " is beyond the widest class in this image (" +
                                     std::to_string(widest_class_) + " slots)");
                }
                break;
            }

            case Op::CallDirect: {
                std::uint16_t target = read_u16(operand);
                if (target >= module_.methods.size()) {
                    fail(offset, "call to method #" + std::to_string(target) +
                                     ", which does not exist");
                }
                break;
            }

            case Op::CallVirtual: {
                // Same reasoning as getfield: the table belongs to the receiver.
                std::uint16_t slot = read_u16(operand);
                if (slot >= longest_vtable_) {
                    fail(offset, "vtable slot " + std::to_string(slot) +
                                     " is beyond the longest vtable in this image (" +
                                     std::to_string(longest_vtable_) + " entries)");
                }
                break;
            }

            case Op::CallNative: {
                std::uint16_t native = read_u16(operand);
                if (native == 0) fail(offset, "callnative with no native id");
                break;
            }

            case Op::Materialize:
            case Op::Alloc:
            case Op::ManualAlloc: {
                std::uint16_t class_id = read_u16(operand);
                if (class_id >= module_.classes.size()) {
                    fail(offset, "class #" + std::to_string(class_id) + " does not exist");
                }
                if (op == Op::Materialize) {
                    std::uint8_t strategy = method_.code[operand + 2];
                    if (strategy > static_cast<std::uint8_t>(StrategyByte::Manual)) {
                        fail(offset, "unknown allocation strategy " + std::to_string(strategy));
                    }
                }
                break;
            }

            case Op::Convert: {
                if (!valid_type(method_.code[operand]) || !valid_type(method_.code[operand + 1])) {
                    fail(offset, "convert names a type that does not exist");
                }
                break;
            }

            default:
                if (operand_format(op) == OperandFormat::Type &&
                    !valid_type(method_.code[operand])) {
                    fail(offset, std::string("'") + mnemonic(op) +
                                     "' names a type that does not exist");
                }
                break;
        }

        if (operand_format(op) == OperandFormat::I32) {
            auto length = static_cast<std::int64_t>(method_.code.size());
            std::int64_t after = static_cast<std::int64_t>(offset) + instruction_size(op);
            std::int64_t target = after + read_i32(operand);
            if (target < 0 || target > length) {
                fail(offset, "jump target " + std::to_string(target) + " is outside the method");
            } else {
                jumps_.push(static_cast<std::uint32_t>(target));
            }
        }
    }

    void check_jump_targets() {
        for (std::uint32_t target : jumps_) {
            if (target < boundaries_.size() && boundaries_[target]) continue;
            fail(target, "jump lands in the middle of an instruction");
        }
    }

    const Module& module_;
    std::int32_t index_;
    const MethodEntry& method_;
    util::Array<VerificationError>& errors_;
    util::Array<bool> boundaries_;
    util::Array<std::uint32_t> jumps_;
    std::uint32_t widest_class_ = 0;
    std::uint32_t longest_vtable_ = 0;
};

}  // namespace

bool verify(const Module& module, util::Array<VerificationError>& errors) {
    // Class table: base links, vtable entries and the methods a class names.
    for (std::size_t i = 0; i < module.classes.size(); ++i) {
        const ClassEntry& entry = module.classes[i];
        std::string name = "class '" + std::string(module.string_at(entry.name)) + "'";

        if (entry.base >= 0 && static_cast<std::size_t>(entry.base) >= module.classes.size()) {
            errors.push(VerificationError{-1, 0, name + " names a base class that does not exist"});
        }
        if (entry.base >= 0 && static_cast<std::size_t>(entry.base) == i) {
            errors.push(VerificationError{-1, 0, name + " extends itself"});
        }
        for (std::uint32_t slot : entry.vtable) {
            if (slot >= module.methods.size()) {
                errors.push(VerificationError{-1, 0, name + " has a vtable slot naming method #" +
                                                         std::to_string(slot) +
                                                         ", which does not exist"});
            }
        }
        for (const std::int32_t index : {entry.constructor, entry.procedures, entry.field_init}) {
            if (index >= 0 && static_cast<std::size_t>(index) >= module.methods.size()) {
                errors.push(VerificationError{-1, 0, name + " names method #" +
                                                         std::to_string(index) +
                                                         ", which does not exist"});
            }
        }
        for (const FieldEntry& field : entry.fields) {
            if (field.name >= module.constants.size()) {
                errors.push(VerificationError{-1, 0, name + " has a field with no name"});
            }
            if (field.ref_kind > kRefDynamic) {
                errors.push(VerificationError{-1, 0, name + " has a field with an unknown "
                                                            "reference kind"});
            }
        }
    }

    // Entry points.
    if (module.main_method >= 0 &&
        static_cast<std::size_t>(module.main_method) >= module.methods.size()) {
        errors.push(VerificationError{-1, 0, "the entry point names a method that does not exist"});
    }
    if (module.root_class >= 0 &&
        static_cast<std::size_t>(module.root_class) >= module.classes.size()) {
        errors.push(VerificationError{-1, 0, "the root class does not exist"});
    }

    for (std::size_t i = 0; i < module.methods.size(); ++i) {
        const MethodEntry& method = module.methods[i];
        if (method.owner_class >= module.classes.size()) {
            errors.push(VerificationError{static_cast<std::int32_t>(i), 0,
                                          "belongs to a class that does not exist"});
            continue;
        }
        if (method.param_count > method.frame_size) {
            errors.push(VerificationError{static_cast<std::int32_t>(i), 0,
                                          "declares more parameters than it has frame slots"});
        }
        MethodVerifier(module, static_cast<std::int32_t>(i), method, errors).run();
    }

    return errors.empty();
}

bool verify(const Module& module, std::string& report) {
    util::Array<VerificationError> errors;
    bool ok = verify(module, errors);
    report.clear();
    for (const VerificationError& error : errors) {
        if (error.method >= 0) {
            const MethodEntry* method = module.method_at(error.method);
            report += "method #" + std::to_string(error.method);
            if (method) report += " (" + std::string(module.string_at(method->name)) + ")";
            report += " at offset " + std::to_string(error.offset) + ": ";
        }
        report += error.message;
        report += "\n";
    }
    return ok;
}

}  // namespace khu::bytecode

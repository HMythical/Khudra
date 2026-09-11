// Khudra bytecode instruction set.
//
// Stack-based. Arithmetic and comparison carry a one-byte type operand rather
// than exploding into one opcode per width -- Khudra has ten numeric types, and
// a width operand keeps the set readable while staying width-specific at run
// time (there is no implicit conversion to fall back on).
//
// The opcode list is an X-macro so the enum, the name table and the operand
// decoder cannot drift apart.
#ifndef KHU_BYTECODE_OPCODE_H
#define KHU_BYTECODE_OPCODE_H

#include <cstdint>

namespace khu::bytecode {

// The tag on a value, a constant-pool entry, a field or a return type.
enum class TypeTag : std::uint8_t {
    Void = 0,
    Int8, Int16, Int32, Int64,
    UInt8, UInt16, UInt32, UInt64,
    Float32, Float64,
    Bool,
    String,
    Array,
    Ref,     // a class instance
    Ptr,     // *T
    Memory,  // MemoryAllocationTypeObject
    Null,
    Count,
};

const char* type_tag_name(TypeTag tag);
bool is_numeric(TypeTag tag);
bool is_integer(TypeTag tag);
bool is_signed_integer(TypeTag tag);
bool is_float(TypeTag tag);
bool is_reference(TypeTag tag);
// Bit width of a numeric tag, or 0.
std::uint32_t type_width(TypeTag tag);

// Allocation strategy as encoded in a Materialize operand. Mirrors
// KhuStrategy in utils/proc_engine.h.
enum class StrategyByte : std::uint8_t {
    ClassDefault = 0,
    Gc = 1,
    Manual = 2,
};

// Shape of an instruction's immediate operands.
enum class OperandFormat : std::uint8_t {
    None,
    U8,
    U16,
    I32,
    Type,        // one TypeTag
    TypeType,    // source TypeTag, destination TypeTag
    U16U8,       // index + a byte (count, strategy, ...)
    U16U8U8,     // index + two bytes (slot, argc, "leaves a value behind")
};

// name, mnemonic, operand format
#define KHU_OPCODES(X)                                                     \
    /* stack */                                                            \
    X(Nop,          "nop",           OperandFormat::None)                  \
    X(Pop,          "pop",           OperandFormat::None)                  \
    X(Dup,          "dup",           OperandFormat::None)                  \
    /* dup_x1 inserts a copy of the top value below the one under it, which is \
       what an assignment needs to leave its value behind after putfield. */   \
    X(DupX1,        "dup_x1",        OperandFormat::None)                  \
    /* dup_x2 does the same two places down, which is what an element        \
       assignment needs: arrayset consumes an array, an index and a value,    \
       so leaving the value behind means tucking a copy under all three. */   \
    X(DupX2,        "dup_x2",        OperandFormat::None)                  \
    X(LoadConst,    "ldc",           OperandFormat::U16)                   \
    X(LoadNull,     "null",          OperandFormat::None)                  \
    X(LoadTrue,     "true",          OperandFormat::None)                  \
    X(LoadFalse,    "false",         OperandFormat::None)                  \
    /* frame slots */                                                      \
    X(LoadLocal,    "load",          OperandFormat::U16)                   \
    X(StoreLocal,   "store",         OperandFormat::U16)                   \
    X(LoadThis,     "this",          OperandFormat::None)                  \
    /* fields */                                                           \
    X(GetField,     "getfield",      OperandFormat::U16)                   \
    X(PutField,     "putfield",      OperandFormat::U16)                   \
    /* arithmetic */                                                       \
    X(Add,          "add",           OperandFormat::Type)                  \
    X(Sub,          "sub",           OperandFormat::Type)                  \
    X(Mul,          "mul",           OperandFormat::Type)                  \
    X(Div,          "div",           OperandFormat::Type)                  \
    X(Rem,          "rem",           OperandFormat::Type)                  \
    X(Neg,          "neg",           OperandFormat::Type)                  \
    /* bitwise */                                                          \
    X(BitAnd,       "and",           OperandFormat::Type)                  \
    X(BitOr,        "or",            OperandFormat::Type)                  \
    X(BitXor,       "xor",           OperandFormat::Type)                  \
    X(BitNot,       "not",           OperandFormat::Type)                  \
    X(Shl,          "shl",           OperandFormat::Type)                  \
    X(Shr,          "shr",           OperandFormat::Type)                  \
    /* comparison */                                                       \
    X(CmpEq,        "cmpeq",         OperandFormat::Type)                  \
    X(CmpNe,        "cmpne",         OperandFormat::Type)                  \
    X(CmpLt,        "cmplt",         OperandFormat::Type)                  \
    X(CmpLe,        "cmple",         OperandFormat::Type)                  \
    X(CmpGt,        "cmpgt",         OperandFormat::Type)                  \
    X(CmpGe,        "cmpge",         OperandFormat::Type)                  \
    X(RefEq,        "refeq",         OperandFormat::None)                  \
    X(RefNe,        "refne",         OperandFormat::None)                  \
    X(LogicalNot,   "lnot",          OperandFormat::None)                  \
    /* conversion */                                                       \
    X(Convert,      "convert",       OperandFormat::TypeType)              \
    /* control flow -- operands are offsets from the end of the instruction */ \
    X(Jump,         "jmp",           OperandFormat::I32)                   \
    X(JumpIfFalse,  "jmpf",          OperandFormat::I32)                   \
    X(JumpIfTrue,   "jmpt",          OperandFormat::I32)                   \
    X(Return,       "ret",           OperandFormat::None)                  \
    X(ReturnValue,  "retval",        OperandFormat::None)                  \
    /* calls. invokevirtual carries its argument count because the receiver \
       sits underneath the arguments and the slot alone does not say how many \
       to skip past to reach it. */                                          \
    X(CallDirect,   "call",          OperandFormat::U16)                   \
    /* invokevirtual's third operand says whether the call leaves a value      \
       behind. The interpreter does not need it -- it resolves the method and   \
       then knows -- but the native emitter walks the code with an abstract     \
       stack before it writes anything, and a slot number alone does not say:   \
       slot 2 is a getter in one chain and a void method in another. The front  \
       end knows at the call site, so it records it rather than leaving the      \
       backend to guess from the image. */                                     \
    X(CallVirtual,  "invokevirtual", OperandFormat::U16U8U8)               \
    X(CallNative,   "callnative",    OperandFormat::U16U8)                 \
    /* memory */                                                           \
    X(Materialize,  "materialize",   OperandFormat::U16U8)                 \
    X(Alloc,        "alloc",         OperandFormat::U16)                   \
    X(ManualAlloc,  "manualalloc",   OperandFormat::U16)                   \
    X(Free,         "free",          OperandFormat::None)                  \
    X(Pin,          "pin",           OperandFormat::None)                  \
    X(Unpin,        "unpin",         OperandFormat::None)                  \
    /* arrays. The element type is erased at run time -- every element is a  \
       tagged Value, whatever the checker knew about it -- so only arraynew  \
       carries a type, and only so a fresh Array<int32> starts at 0 rather   \
       than at null. */                                                     \
    X(ArrayNew,     "arraynew",      OperandFormat::Type)                  \
    X(ArrayLen,     "arraylen",      OperandFormat::None)                  \
    X(ArrayGet,     "arrayget",      OperandFormat::None)                  \
    X(ArraySet,     "arrayset",      OperandFormat::None)                  \
    /* raw pointers. The element type is the operand, because a `*T` carries   \
       no tag of its own: what is at an address is whatever the pointer's type  \
       said would be. Nothing bounds-checks these -- that is what "raw" means   \
       -- but a null pointer traps. */                                        \
    X(PtrGet,       "ptrget",        OperandFormat::Type)                  \
    X(PtrSet,       "ptrset",        OperandFormat::Type)                  \
/* native-only escape hatch: the operand is a constant-pool index of the   \
        raw C text. The VM traps; only the native backend executes it. */      \
    X(InlineC,      "inclinec",      OperandFormat::U16)                   \
    /* the asm sibling of inline_c: the operand names the text that becomes   \
        the guts of a `__asm__ volatile(...)` statement. */                   \
    X(InlineAsm,    "inlineasm",     OperandFormat::U16)                   \
    /* termination */                                                      \
    X(Halt,         "halt",          OperandFormat::None)

enum class Op : std::uint8_t {
#define KHU_OPCODE_ENUM(name, mnemonic, format) name,
    KHU_OPCODES(KHU_OPCODE_ENUM)
#undef KHU_OPCODE_ENUM
    Count,
};

const char* mnemonic(Op op);
OperandFormat operand_format(Op op);
// Total encoded size of `op`, including its opcode byte.
std::uint32_t instruction_size(Op op);
std::uint32_t operand_size(OperandFormat format);

}  // namespace khu::bytecode

#endif  // KHU_BYTECODE_OPCODE_H

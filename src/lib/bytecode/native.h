// Native function ids.
//
// Shared by the front end (which resolves `io.print` to an id), the VM and the
// native host (which bind the id to an implementation) and the C emitter (which
// needs to know the stack effect), so the numbering lives in one place and .kbc
// images stay stable across all of them.
//
// The list is an X-macro for the same reason KHU_OPCODES is one: the enum, the
// name table and the result-count table cannot drift apart.
//
// **Ids are frozen.** A `.kbc` image records the number, so an id that has
// shipped never changes and never gets reused. Ids are handed out in
// per-namespace blocks so a namespace can grow without disturbing its
// neighbours:
//
//   1 -  15   io and khu -- the original six, plus room for the rest of io
//  16 -  99   io extensions and khuStdErr share the low block (Phase 7)
// 100 - 299   khuStdMath          (Phase 2)
// 300 - 399   khuStdConv          (Phase 3)
// 400 - 499   khuStdString        (Phase 4)
// 500 - 599   khuStdCollection    (Phase 6)
// 600 - 699   khuErrors           (Phase 5b)
// 700 - 799   khuStdMem           (Phase 8)
// 800 - 849   khuStdRandom        (Phase 9)
// 850 - 899   khuStdTime          (Phase 9)
// 900 - 999   khuStdSystem        (Phase 9) -- tests/unit/test_natives.cpp
//             grants the namespace everything up to kNativeBlockKernel - 1.
// 1000 - 1099 khuAdvKernel        (shared) -- PLAN.md, section 6.1
// 1100 - 1199 khuAdvKernelLinux   -- PLAN.md, section 6.2
// 1200 - 1299 khuAdvKernelWindows -- PLAN.md, section 6.4
// 1300 - 1399 khuAdvKernelMac     -- PLAN.md, section 6.3
#ifndef KHU_BYTECODE_NATIVE_H
#define KHU_BYTECODE_NATIVE_H

#include <cstdint>

namespace khu::bytecode {

// The first id of each namespace's block. A new native takes the next free
// number inside its own block; nothing is ever renumbered.
enum : std::uint32_t {
    kNativeBlockIo = 1,
    kNativeBlockErr = 40,
    kNativeBlockMath = 100,
    kNativeBlockConv = 300,
    kNativeBlockString = 400,
    kNativeBlockCollection = 500,
    kNativeBlockErrors = 600,
    kNativeBlockMem = 700,
    kNativeBlockRandom = 800,
    kNativeBlockTime = 850,
    kNativeBlockSystem = 900,
    kNativeBlockKernel = 1000,
    kNativeBlockKernelLinux = 1100,
    kNativeBlockKernelWindows = 1200,
    kNativeBlockKernelMac = 1300,
    kNativeBlockEnd = 1400,
};

// name, id, display name, number of results (0 or 1)
#define KHU_NATIVES(X)                                                        \
    /* io and khu -- the original six. These ids are frozen at 1-6. */        \
    X(Print,             1,   "io.print",                 0)                  \
    X(PrintLine,         2,   "io.printLine",             0)                  \
    X(ReadLine,          3,   "io.readLine",              1)                  \
    X(StdlibLoadObject,  4,   "khu.stdlibLoadObject",     0)                  \
    X(GetType,           5,   "khu.getType",              1)                  \
    X(LoadRuntimeType,   6,   "khu.LoadRuntimeType",      1)                  \
    /* io, continued: the byte-oriented and value-oriented surface. */         \
    X(Describe,          7,   "io.describe",              1)                  \
    X(WriteString,       8,   "io.writeString",           0)                  \
    X(WriteBytes,        9,   "io.writeBytes",            0)                  \
    X(Flush,             10,  "io.flush",                 0)                  \
    X(ReadChar,          11,  "io.readChar",              1)                  \
    X(ReadByte,          12,  "io.readByte",              1)                  \
    X(ReadBool,          13,  "io.readBool",              1)                  \
    X(ReadInt8,          16,  "io.readInt8",              1)                  \
    X(ReadInt16,         17,  "io.readInt16",             1)                  \
    X(ReadInt32,         18,  "io.readInt32",             1)                  \
    X(ReadInt64,         19,  "io.readInt64",             1)                  \
    X(ReadUInt8,         20,  "io.readUInt8",             1)                  \
    X(ReadUInt16,        21,  "io.readUInt16",            1)                  \
    X(ReadUInt32,        22,  "io.readUInt32",            1)                  \
    X(ReadUInt64,        23,  "io.readUInt64",            1)                  \
    X(ReadFloat,         24,  "io.readFloat",             1)                  \
    X(ReadDFloat,        25,  "io.readDFloat",            1)                  \
    /* khuStdErr -- the standard error stream (fd 2). The same surface as io's \
       output side, pointed at the other stream. */                           \
    X(ErrPrint,          40,  "khuStdErr.errPrint",       0)                  \
    X(ErrPrintLine,      41,  "khuStdErr.errPrintLine",   0)                  \
    X(ErrWriteString,    42,  "khuStdErr.errWriteString", 0)                  \
    X(ErrWriteBytes,     43,  "khuStdErr.errWriteBytes",  0)                  \
    X(ErrFlush,          44,  "khuStdErr.flush",          0)                  \
    /* khuStdMath -- utilities and transcendentals. The arithmetic operators   \
       are intrinsics and never reach this table. One id per operation, not    \
       one per width: the binding is by name and arity, and the runtime tag    \
       says which width it got. */                                            \
    X(Abs,               100, "khuStdMath.abs",           1)                  \
    X(Min,               101, "khuStdMath.min",           1)                  \
    X(Max,               102, "khuStdMath.max",           1)                  \
    X(Clamp,             103, "khuStdMath.clamp",         1)                  \
    X(Signum,            104, "khuStdMath.signum",        1)                  \
    X(Gcd,               105, "khuStdMath.gcd",           1)                  \
    X(Lcm,               106, "khuStdMath.lcm",           1)                  \
    X(Pow,               107, "khuStdMath.pow",           1)                  \
    X(Sqrt,              108, "khuStdMath.sqrt",          1)                  \
    X(Floor,             109, "khuStdMath.floor",         1)                  \
    X(Ceil,              110, "khuStdMath.ceil",          1)                  \
    X(Round,             111, "khuStdMath.round",         1)                  \
    X(Fmod,              112, "khuStdMath.fmod",          1)                  \
    X(Exp,               113, "khuStdMath.exp",           1)                  \
    X(Log,               114, "khuStdMath.log",           1)                  \
    X(Log10,             115, "khuStdMath.log10",         1)                  \
    X(Sin,               116, "khuStdMath.sin",           1)                  \
    X(Cos,               117, "khuStdMath.cos",           1)                  \
    X(Tan,               118, "khuStdMath.tan",           1)                  \
    X(Asin,              119, "khuStdMath.asin",          1)                  \
    X(Acos,              120, "khuStdMath.acos",          1)                  \
    X(Atan,              121, "khuStdMath.atan",          1)                  \
    X(Atan2,             122, "khuStdMath.atan2",         1)                  \
    X(Sinh,              123, "khuStdMath.sinh",          1)                  \
    X(Cosh,              124, "khuStdMath.cosh",          1)                  \
    X(Tanh,              125, "khuStdMath.tanh",          1)                  \
    X(Pi,                126, "khuStdMath.pi",            1)                  \
    X(E,                 127, "khuStdMath.e",             1)                  \
    /* khuStdConv -- number, string and bool conversion. The parse functions   \
       are one id per width because they all take the same argument type, so   \
       there is nothing at run time to tell them apart. */                     \
    X(ToString,          300, "khuStdConv.toString",      1)                  \
    X(ParseInt8,         301, "khuStdConv.parseInt8",     1)                  \
    X(ParseInt16,        302, "khuStdConv.parseInt16",    1)                  \
    X(ParseInt32,        303, "khuStdConv.parseInt32",    1)                  \
    X(ParseInt64,        304, "khuStdConv.parseInt64",    1)                  \
    X(ParseUInt8,        305, "khuStdConv.parseUInt8",    1)                  \
    X(ParseUInt16,       306, "khuStdConv.parseUInt16",   1)                  \
    X(ParseUInt32,       307, "khuStdConv.parseUInt32",   1)                  \
    X(ParseUInt64,       308, "khuStdConv.parseUInt64",   1)                  \
    X(ParseFloat,        309, "khuStdConv.parseFloat",    1)                  \
    X(ParseDFloat,       310, "khuStdConv.parseDFloat",   1)                  \
    X(IsNumeric,         311, "khuStdConv.isNumeric",     1)                  \
    X(IsDigit,           320, "khuStdConv.isDigit",       1)                  \
    X(IsLetter,          321, "khuStdConv.isLetter",      1)                  \
    X(IsWhitespace,      322, "khuStdConv.isWhitespace",  1)                  \
    X(ToUpper,           323, "khuStdConv.toUpper",       1)                  \
    X(ToLower,           324, "khuStdConv.toLower",       1)                  \
    X(ToChar,            325, "khuStdConv.toChar",        1)                  \
    X(BoolToInt,         330, "khuStdConv.boolToInt",     1)                  \
    X(IntToBool,         331, "khuStdConv.intToBool",     1)                  \
    /* khuStdString -- operations over immutable byte strings. Every one that   \
       produces a string produces a fresh one. */                              \
    X(StrLength,         400, "khuStdString.length",      1)                  \
    X(StrConcat,         401, "khuStdString.concat",      1)                  \
    X(StrSubstring,      402, "khuStdString.substring",   1)                  \
    X(StrCharAt,         403, "khuStdString.charAt",      1)                  \
    X(StrIndexOf,        404, "khuStdString.indexOf",     1)                  \
    X(StrLastIndexOf,    405, "khuStdString.lastIndexOf", 1)                  \
    X(StrContains,       406, "khuStdString.contains",    1)                  \
    X(StrStartsWith,     407, "khuStdString.startsWith",  1)                  \
    X(StrEndsWith,       408, "khuStdString.endsWith",    1)                  \
    X(StrEquals,         409, "khuStdString.equals",      1)                  \
    X(StrEqualsIgnoreCase, 410, "khuStdString.equalsIgnoreCase", 1)           \
    X(StrCompareTo,      411, "khuStdString.compareTo",   1)                  \
    X(StrCompareToIgnoreCase, 412, "khuStdString.compareToIgnoreCase", 1)     \
    X(StrToUpperCase,    413, "khuStdString.toUpperCase", 1)                  \
    X(StrToLowerCase,    414, "khuStdString.toLowerCase", 1)                  \
    X(StrTrim,           415, "khuStdString.trim",        1)                  \
    X(StrTrimStart,      416, "khuStdString.trimStart",   1)                  \
    X(StrTrimEnd,        417, "khuStdString.trimEnd",     1)                  \
    X(StrReplace,        418, "khuStdString.replace",     1)                  \
    X(StrReplaceAll,     419, "khuStdString.replaceAll",  1)                  \
    X(StrRepeat,         420, "khuStdString.repeat",      1)                  \
    X(StrIsEmpty,        421, "khuStdString.isEmpty",     1)                  \
    X(StrIsBlank,        422, "khuStdString.isBlank",     1)                  \
    /* khuStdConv, continued: the per-width "would this parse" checks that let  \
       Khudra code turn a sentinel into a Result without guessing. */          \
    X(CanParseInt8,      340, "khuStdConv.canParseInt8",   1)                  \
    X(CanParseInt16,     341, "khuStdConv.canParseInt16",  1)                  \
    X(CanParseInt32,     342, "khuStdConv.canParseInt32",  1)                  \
    X(CanParseInt64,     343, "khuStdConv.canParseInt64",  1)                  \
    X(CanParseUInt8,     344, "khuStdConv.canParseUInt8",  1)                  \
    X(CanParseUInt16,    345, "khuStdConv.canParseUInt16", 1)                  \
    X(CanParseUInt32,    346, "khuStdConv.canParseUInt32", 1)                  \
    X(CanParseUInt64,    347, "khuStdConv.canParseUInt64", 1)                  \
    X(CanParseFloat,     348, "khuStdConv.canParseFloat",  1)                  \
    X(CanParseDFloat,    349, "khuStdConv.canParseDFloat", 1)                  \
    /* khuErrors -- the error-code table. Constants are zero-argument natives   \
       because a namespace has no const members yet. */                        \
    X(ErrNone,           600, "khuErrors.none",            1)                  \
    X(ErrBounds,         601, "khuErrors.bounds",          1)                  \
    X(ErrParse,          602, "khuErrors.parse",           1)                  \
    X(ErrNullReference,  603, "khuErrors.nullReference",   1)                  \
    X(ErrIo,             604, "khuErrors.io",              1)                  \
    X(ErrDivideByZero,   605, "khuErrors.divideByZero",    1)                  \
    X(ErrNotFound,       606, "khuErrors.notFound",        1)                  \
    X(ErrInvalidArgument, 607, "khuErrors.invalidArgument", 1)                 \
    X(ErrUnsupported,    608, "khuErrors.unsupported",     1)                  \
    X(ErrOverflow,       609, "khuErrors.overflow",        1)                  \
    X(ErrEmpty,          610, "khuErrors.empty",           1)                  \
    /* The one operation in khuErrors that is not a constant: it ends the      \
       program the way any other fatal runtime error does. */                  \
    X(ErrFail,           611, "khuErrors.fail",            0)                  \
    /* khuStdCollection -- the two operations that work on a value whatever    \
       its type, which is what a container over an erased element needs. */    \
    X(Hash,              500, "khuStdCollection.hash",     1)                  \
    X(SameValue,         501, "khuStdCollection.sameValue", 1)                \
    /* The one member here that is about a container rather than a value:      \
       joining a List<string>, which is what a program builds output out of    \
       once `concat` runs out of arity. */                                     \
    X(Join,              502, "khuStdCollection.join",     1)                  \
    /* khuStdMem -- manual buffers and the operations over them. */            \
    X(MemAlloc,          700, "khuStdMem.alloc",          1)                  \
    X(MemRealloc,        701, "khuStdMem.realloc",        1)                  \
    X(MemFree,           702, "khuStdMem.release",        0)                  \
    X(MemCopy,           703, "khuStdMem.copy",           0)                  \
    X(MemMove,           704, "khuStdMem.move",           0)                  \
    X(MemZero,           705, "khuStdMem.zero",           0)                  \
    X(MemFill,           706, "khuStdMem.fill",           0)                  \
    X(MemCompare,        707, "khuStdMem.compare",        1)                  \
    X(MemAddressOf,      708, "khuStdMem.addressOf",      1)                  \
    X(MemRefEquals,      709, "khuStdMem.refEquals",      1)                  \
    X(MemSizeOf,         710, "khuStdMem.sizeOf",         1)                  \
    X(MemIsNull,         711, "khuStdMem.isNull",         1)                  \
    X(MemLiveBytes,      712, "khuStdMem.liveBytes",      1)                  \
    X(MemLiveBlocks,     713, "khuStdMem.liveBlocks",     1)                  \
    /* khuStdRandom -- a deterministic PRNG, so a seeded program is            \
       reproducible across runs and across backends. */                       \
    X(RandomSeed,        800, "khuStdRandom.seed",        0)                  \
    X(RandomNextInt,     801, "khuStdRandom.nextInt",     1)                  \
    X(RandomNextInt64,   802, "khuStdRandom.nextInt64",   1)                  \
    X(RandomNextFloat,   803, "khuStdRandom.nextFloat",   1)                  \
    X(RandomNextDouble,  804, "khuStdRandom.nextDouble",  1)                  \
    X(RandomNextBool,    805, "khuStdRandom.nextBool",    1)                  \
    X(RandomNextBytes,   806, "khuStdRandom.nextBytes",   0)                  \
    /* khuStdTime -- the clock. The one part of the standard library whose      \
       answers are not reproducible, which is why nothing else depends on it. */\
    X(TimeNowMillis,     850, "khuStdTime.nowMillis",     1)                  \
    X(TimeNowNanos,      851, "khuStdTime.nowNanos",      1)                  \
    X(TimeNowString,     852, "khuStdTime.nowString",     1)                  \
    X(TimeSleep,         853, "khuStdTime.sleep",         0)                  \
    X(TimeMonotonicNanos, 854, "khuStdTime.monotonicNanos", 1)                \
    /* khuStdSystem -- the operating system: open streams, the process, the    \
       file namespace and sockets. Every member behaves the same way on Linux  \
       and on Windows; the platform differences live in src/vm/platform.cpp.   \
       A handle is an opaque `*byte` cookie, so files and sockets share one    \
       registry and one `close`. */                                           \
    X(SysOpen,           900, "khuStdSystem.open",          1)                 \
    X(SysClose,          901, "khuStdSystem.close",         0)                 \
    X(SysReadText,       902, "khuStdSystem.readText",      1)                 \
    X(SysWriteText,      903, "khuStdSystem.writeText",     0)                 \
    X(SysWriteBytes,     904, "khuStdSystem.writeBytes",    0)                 \
    X(SysIsOpen,         905, "khuStdSystem.isOpen",        1)                 \
    X(SysErrno,          906, "khuStdSystem.errno",         1)                 \
    X(SysReadByte,       907, "khuStdSystem.readByte",      1)                 \
    X(SysReadLine,       908, "khuStdSystem.readLine",      1)                 \
    X(SysSeek,           909, "khuStdSystem.seek",          1)                 \
    X(SysTell,           910, "khuStdSystem.tell",          1)                 \
    X(SysFlush,          911, "khuStdSystem.flush",         0)                 \
    X(SysAtEof,          912, "khuStdSystem.atEof",         1)                 \
    X(SysError,          913, "khuStdSystem.error",         1)                 \
    X(SysClearError,     914, "khuStdSystem.clearError",    0)                 \
    X(SysRewind,         915, "khuStdSystem.rewind",        0)                 \
    /* One id, two declarations: the binding table keys on name and arity, so   \
       fileSize(handle) and fileSize(path) arrive here together and the tag     \
       says which was written -- exactly how khuStdMem.sizeOf works. */         \
    X(SysFileSize,       916, "khuStdSystem.fileSize",      1)                 \
    /* The process surface. */                                                 \
    X(SysArgc,           920, "khuStdSystem.argc",          1)                 \
    X(SysArgv,           921, "khuStdSystem.argv",          1)                 \
    X(SysGetEnv,         922, "khuStdSystem.getEnv",        1)                 \
    X(SysHasEnv,         923, "khuStdSystem.hasEnv",        1)                 \
    X(SysEnvKeys,        924, "khuStdSystem.envKeys",       1)                 \
    X(SysExit,           925, "khuStdSystem.exit",          0)                 \
    /* Files and directories by path, and the separators a portable program     \
       builds paths out of. */                                                 \
    X(SysExists,         930, "khuStdSystem.exists",        1)                 \
    X(SysIsFile,         931, "khuStdSystem.isFile",        1)                 \
    X(SysIsDirectory,    932, "khuStdSystem.isDirectory",   1)                 \
    X(SysDeleteFile,     933, "khuStdSystem.deleteFile",    1)                 \
    X(SysRename,         934, "khuStdSystem.rename",        1)                 \
    X(SysCreateDirectory, 935, "khuStdSystem.createDirectory", 1)              \
    X(SysRemoveDirectory, 936, "khuStdSystem.removeDirectory", 1)              \
    X(SysCurrentDirectory, 937, "khuStdSystem.currentDirectory", 1)            \
    X(SysChangeDirectory, 938, "khuStdSystem.changeDirectory", 1)              \
    X(SysErrorMessage,   939, "khuStdSystem.errorMessage",  1)                 \
    X(SysPathSeparator,  940, "khuStdSystem.pathSeparator", 1)                 \
    X(SysPathListSeparator, 941, "khuStdSystem.pathListSeparator", 1)          \
    /* Sockets, on the same handle layer. All blocking: setReadTimeout is the   \
       one escape hatch a single-threaded server needs so a stalled peer cannot \
       wedge it for ever (EXPANSION-PLAN.md, section 3). */                     \
    X(SysListen,         950, "khuStdSystem.listen",        1)                 \
    X(SysAccept,         951, "khuStdSystem.accept",        1)                 \
    X(SysConnect,        952, "khuStdSystem.connect",       1)                 \
    X(SysSend,           953, "khuStdSystem.send",          1)                 \
    X(SysRecv,           954, "khuStdSystem.recv",          1)                 \
    X(SysPeerAddress,    955, "khuStdSystem.peerAddress",   1)                 \
    X(SysPeerPort,       956, "khuStdSystem.peerPort",      1)                 \
    X(SysLocalPort,      957, "khuStdSystem.localPort",     1)                 \
    X(SysSetReadTimeout, 958, "khuStdSystem.setReadTimeout", 0)                \
    X(SysResolveHost,    959, "khuStdSystem.resolveHost",   1)                 \
    X(SysShutdown,       960, "khuStdSystem.shutdown",      0)                  \
    /* khuAdvKernel -- the shared, portability-facing half of the kernel      \
       access tier. It carries what the three per-OS namespaces have in       \
       common: the platform branch, the shared error slot and the             \
       buffer/address bridge. The per-OS tiers are khuAdvKernelLinux,         \
       khuAdvKernelWindows and khuAdvKernelMac (lib/kernel.khu). */            \
    X(KernelPlatform,     1000, "khuAdvKernel.platform",     1)                \
    X(KernelPlatformLinux, 1001, "khuAdvKernel.linux",       1)                \
    X(KernelPlatformWindows, 1002, "khuAdvKernel.windows",   1)                \
    X(KernelPlatformMac,  1003, "khuAdvKernel.mac",          1)                \
    X(KernelPlatformName, 1004, "khuAdvKernel.platformName", 1)                \
    X(KernelErrno,        1005, "khuAdvKernel.errno",        1)                \
    X(KernelErrorMessage, 1006, "khuAdvKernel.errorMessage", 1)                \
    X(KernelToAddress,    1007, "khuAdvKernel.toAddress",    1)                \
    X(KernelFromAddress,  1008, "khuAdvKernel.fromAddress",  1)                \
    X(KernelDropAddress,  1009, "khuAdvKernel.dropAddress",  0)                  \
    /* khuAdvKernelLinux -- the raw and curated Linux syscall tier. The first     \
       eight ids are the raw gate: seven invoke overloads (arities 1-7) and      \
       the name-to-number lookup. The curated members arrive in Phase 3. */      \
    X(KernelLinuxInvoke1, 1100, "khuAdvKernelLinux.invoke", 1)                  \
    X(KernelLinuxInvoke2, 1101, "khuAdvKernelLinux.invoke", 1)                  \
    X(KernelLinuxInvoke3, 1102, "khuAdvKernelLinux.invoke", 1)                  \
    X(KernelLinuxInvoke4, 1103, "khuAdvKernelLinux.invoke", 1)                  \
    X(KernelLinuxInvoke5, 1104, "khuAdvKernelLinux.invoke", 1)                  \
    X(KernelLinuxInvoke6, 1105, "khuAdvKernelLinux.invoke", 1)                  \
    X(KernelLinuxInvoke7, 1106, "khuAdvKernelLinux.invoke", 1)                  \
    X(KernelLinuxNumber,  1107, "khuAdvKernelLinux.number",  1)

enum class NativeId : std::uint32_t {
    None = 0,
#define KHU_NATIVE_ENUM(name, id, text, results) name = id,
    KHU_NATIVES(KHU_NATIVE_ENUM)
#undef KHU_NATIVE_ENUM
};

// The qualified name a diagnostic or a disassembly listing shows.
const char* native_name(NativeId id);

// How many values the native leaves behind: 0 for a void one, 1 otherwise.
// The interpreter, the C emitter's abstract stack and the host all read this,
// so a native's stack effect is stated once.
int native_result_count(NativeId id);

// Whether `id` names a native this build implements.
bool native_is_known(NativeId id);

}  // namespace khu::bytecode

#endif  // KHU_BYTECODE_NATIVE_H

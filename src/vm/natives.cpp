#include "vm/natives.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <string_view>
#include <utility>

extern "C" {
#include "alloc.h"
}

#include "bytecode/opcode.h"
#include "vm/format.h"
#include "vm/object.h"

namespace khu::vm {

namespace {

// `io.print` and `io.printLine` are declared with one parameter each, but the
// loop is written for any arity: the interpreter has always accepted whatever
// the image asked for, and a shared implementation should not be the thing that
// narrows it.
std::string join_arguments(NativeServices& services, const Value* argv, std::uint32_t argc) {
    std::string text;
    for (std::uint32_t i = 0; i < argc; ++i) text += services.render(argv[i]);
    return text;
}

// --- khuStdMath ------------------------------------------------------------
//
// The binding table maps a name and an arity to one id, so every width of
// `khuStdMath.min` arrives here as the same native and the operand's tag says
// which one it was. That is deliberate: the checker has already refused a
// mixed-width call, so by the time a native runs, the arguments agree.
//
// **The float rule.** A transcendental is computed in `double` and then
// normalized back to the operand's width, so a `float` result is rounded
// through 32 bits exactly the way an arithmetic result is. It is not computed
// in `float`: doing the work at 64 bits and rounding once is both more accurate
// and, more importantly for the invariant, a single rule rather than a
// per-function choice about which `std::` overload to reach for.

bool all_float(const Value* argv, std::uint32_t argc) {
    for (std::uint32_t i = 0; i < argc; ++i) {
        if (!bytecode::is_float(argv[i].tag)) return false;
    }
    return true;
}

// The unsigned magnitude of an integer value, whatever its signedness.
std::uint64_t magnitude(const Value& value) {
    if (!bytecode::is_signed_integer(value.tag)) {
        return value.as_uint & mask_of(bytecode::type_width(value.tag));
    }
    std::int64_t signed_value = value.as_int;
    return signed_value < 0 ? (0ull - static_cast<std::uint64_t>(signed_value))
                            : static_cast<std::uint64_t>(signed_value);
}

// Ordering within one width, using the tag to pick signed, unsigned or float
// comparison -- the same three-way split khu_val_compare makes.
int order(const Value& left, const Value& right) {
    if (bytecode::is_float(left.tag)) {
        double a = left.as_float;
        double b = right.as_float;
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    if (bytecode::is_signed_integer(left.tag)) {
        std::int64_t a = left.as_int;
        std::int64_t b = right.as_int;
        return a < b ? -1 : (a > b ? 1 : 0);
    }
    std::uint64_t width = bytecode::type_width(left.tag);
    std::uint64_t a = left.as_uint & mask_of(static_cast<std::uint32_t>(width));
    std::uint64_t b = right.as_uint & mask_of(static_cast<std::uint32_t>(width));
    return a < b ? -1 : (a > b ? 1 : 0);
}

// Greatest common divisor of two magnitudes; gcd(0, 0) is 0.
std::uint64_t gcd_of(std::uint64_t a, std::uint64_t b) {
    while (b != 0) {
        std::uint64_t remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}

// base ** exponent for integers, wrapping like every other integer operation.
std::uint64_t integer_power(TypeTag tag, std::uint64_t base, std::uint64_t exponent) {
    std::uint64_t result = 1;
    std::uint64_t factor = base;
    while (exponent != 0) {
        if (exponent & 1u) result = normalize_int(tag, result * factor).as_uint;
        factor = normalize_int(tag, factor * factor).as_uint;
        exponent >>= 1;
    }
    return result;
}

// --- khuStdConv ------------------------------------------------------------
//
// **Sentinels, for now.** A parse that fails has to answer with something, and
// until `khuErrors.Result<T>` exists (PLAN.md, Phase 5b) that something is a
// documented value of the requested type rather than a trap: text arriving from
// outside the program is not a programming error, so it must not be fatal. The
// sentinel is the width's most negative value for a signed type, its largest
// value for an unsigned one, and a NaN for a float. Every one of them is also a
// value a successful parse can produce, which is exactly why this is the
// temporary answer and not the final one -- pair the call with `isNumeric` when
// the difference matters.

// The sentinel a failed parse of `tag` answers with.
Value parse_sentinel(TypeTag tag) {
    if (bytecode::is_float(tag)) {
        double zero = 0.0;
        return normalize_float(tag, zero / zero);
    }
    std::uint32_t width = bytecode::type_width(tag);
    if (bytecode::is_signed_integer(tag)) {
        return normalize_int(tag, std::uint64_t{1} << (width - 1));
    }
    return normalize_int(tag, mask_of(width));
}

// The text a string-taking native was handed. A `Value` of tag String always
// points at a constant-pool entry or a runtime string, but null is what an
// uninitialized reference slot holds, so it is checked rather than assumed.
bool text_of(const Value& value, std::string_view& out) {
    if (value.tag != TypeTag::String || !value.as_text) return false;
    out = *value.as_text;
    return true;
}

NativeOutcome parse_signed(TypeTag tag, const Value& argument) {
    std::string_view text;
    std::int64_t parsed = 0;
    if (!text_of(argument, text) ||
        !format::parse_int(text, bytecode::type_width(tag), parsed)) {
        return NativeOutcome::produced(parse_sentinel(tag));
    }
    return NativeOutcome::produced(normalize_int(tag, static_cast<std::uint64_t>(parsed)));
}

NativeOutcome parse_unsigned(TypeTag tag, const Value& argument) {
    std::string_view text;
    std::uint64_t parsed = 0;
    if (!text_of(argument, text) ||
        !format::parse_uint(text, bytecode::type_width(tag), parsed)) {
        return NativeOutcome::produced(parse_sentinel(tag));
    }
    return NativeOutcome::produced(normalize_int(tag, parsed));
}

// Whether a parse of `tag` would succeed. This is what lets Khudra code turn a
// sentinel into a `Result<T>` without having to guess whether the sentinel was
// the answer or the failure.
NativeOutcome can_parse(TypeTag tag, const Value& argument) {
    std::string_view text;
    if (!text_of(argument, text)) return NativeOutcome::produced(Value::make_bool(false));
    std::uint32_t width = bytecode::type_width(tag);
    if (bytecode::is_float(tag)) {
        double parsed = 0.0;
        return NativeOutcome::produced(
            Value::make_bool(format::parse_float(text, tag == TypeTag::Float32, parsed)));
    }
    if (bytecode::is_signed_integer(tag)) {
        std::int64_t parsed = 0;
        return NativeOutcome::produced(Value::make_bool(format::parse_int(text, width, parsed)));
    }
    std::uint64_t parsed = 0;
    return NativeOutcome::produced(Value::make_bool(format::parse_uint(text, width, parsed)));
}

NativeOutcome parse_floating(TypeTag tag, const Value& argument) {
    std::string_view text;
    double parsed = 0.0;
    if (!text_of(argument, text) ||
        !format::parse_float(text, tag == TypeTag::Float32, parsed)) {
        return NativeOutcome::produced(parse_sentinel(tag));
    }
    return NativeOutcome::produced(normalize_float(tag, parsed));
}

// --- khuStdString ----------------------------------------------------------
//
// A `string` is an immutable byte string, so every operation that produces one
// produces a *fresh* one through the backend's runtime store. None of them
// mutates its argument, and none of them can: the argument may be a constant in
// the image, shared by every frame that ever loads it.
//
// **Bytes, not code points.** `length` counts bytes, `charAt` yields a byte,
// `substring` cuts between bytes. A multi-byte UTF-8 code point is several
// positions, and cutting through the middle of one produces a string that is
// no longer valid UTF-8. That is the honest description of what the runtime
// representation is; a code-point layer belongs on top of it, not inside it.
//
// **Out of range is a value, not a trap.** An index past the end is the kind of
// thing a program computes from input, so `charAt` answers 0 and `substring`
// clamps rather than killing the program. Where "not found" is the answer,
// it is -1. These are sentinels of the same kind the parse functions use, for
// the same reason and with the same expiry: `khuErrors.Result<T>` (Phase 5b).

// Clamps `index` into [0, size]. A negative index is before the string and a
// large one is past its end; both land on the nearest edge.
std::size_t clamp_index(std::int64_t index, std::size_t size) {
    if (index <= 0) return 0;
    auto unsigned_index = static_cast<std::uint64_t>(index);
    return unsigned_index >= size ? size : static_cast<std::size_t>(unsigned_index);
}

std::string fold_case(std::string_view text, bool upper) {
    std::string folded(text);
    for (char& byte : folded) {
        auto value = static_cast<std::uint8_t>(byte);
        byte = static_cast<char>(upper ? format::to_upper_byte(value)
                                       : format::to_lower_byte(value));
    }
    return folded;
}

// Byte-wise ordering, unsigned, shorter-is-smaller on a common prefix. Reported
// as -1, 0 or 1 rather than a difference, so the answer does not depend on how
// the comparison was computed.
std::int64_t compare_bytes(std::string_view left, std::string_view right) {
    std::size_t common = left.size() < right.size() ? left.size() : right.size();
    for (std::size_t i = 0; i < common; ++i) {
        auto a = static_cast<std::uint8_t>(left[i]);
        auto b = static_cast<std::uint8_t>(right[i]);
        if (a != b) return a < b ? -1 : 1;
    }
    if (left.size() == right.size()) return 0;
    return left.size() < right.size() ? -1 : 1;
}

// The two arguments of a two-string operation, or false when either is null.
bool two_strings(const Value* argv, std::string_view& left, std::string_view& right) {
    return text_of(argv[0], left) && text_of(argv[1], right);
}

bool starts_at(std::string_view text, std::size_t at, std::string_view needle) {
    return at + needle.size() <= text.size() && text.compare(at, needle.size(), needle) == 0;
}

std::string replace_in(std::string_view text, std::string_view from, std::string_view to,
                       bool all) {
    // An empty needle matches everywhere and consumes nothing, so replacing it
    // is either a no-op or an infinite loop. It is the no-op.
    if (from.empty()) return std::string(text);
    std::string out;
    std::size_t at = 0;
    bool replaced = false;
    while (at < text.size()) {
        if ((all || !replaced) && starts_at(text, at, from)) {
            out += to;
            at += from.size();
            replaced = true;
            continue;
        }
        out += text[at];
        ++at;
    }
    return out;
}

// --- khuStdCollection ------------------------------------------------------
//
// A container over an erased element can do exactly two things to what it
// holds: ask what bucket it belongs in, and ask whether two of them are the
// same. Both have to be **deterministic** -- the same program has to lay out
// the same table on every run and under every backend, or a `Map`'s iteration
// order would depend on which one ran it, and the byte-identical invariant
// would be a matter of luck.
//
// So a reference hashes to 0 rather than to its address. An object-keyed map
// degrades to a linear scan, which is the price of an answer that does not
// depend on where the allocator happened to put things.

// FNV-1a, 64-bit, folded to 32 at the end. Chosen because it is short enough to
// read and fixed enough to write down: any program that depends on the exact
// values is depending on this comment.
std::uint64_t hash_bytes(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::int32_t hash_value(const Value& value) {
    std::uint64_t hash = 0;
    switch (value.tag) {
        case TypeTag::Null:
            return 0;
        case TypeTag::String: {
            if (!value.as_text) return 0;
            hash = hash_bytes(value.as_text->data(), value.as_text->size());
            break;
        }
        case TypeTag::Ref:
        case TypeTag::Array:
        case TypeTag::Ptr:
            // An address is not stable across runs, and a hash that is not
            // stable is a table layout that is not reproducible.
            return 0;
        default: {
            // Numbers and bools hash by their bits, which is the same thing
            // sameValue compares -- so equal values always hash equally.
            std::uint64_t bits = value.as_uint;
            hash = hash_bytes(&bits, sizeof(bits));
            break;
        }
    }
    // Folded to a non-negative int32: a bucket index is computed from this, and
    // a negative one would need a special case at every use.
    return static_cast<std::int32_t>((hash ^ (hash >> 32)) & 0x7fffffffull);
}

bool same_value(const Value& left, const Value& right) {
    if (left.is_null_reference() || right.is_null_reference()) {
        return left.is_null_reference() && right.is_null_reference();
    }
    if (left.tag == TypeTag::String || right.tag == TypeTag::String) {
        if (left.tag != right.tag) return false;
        return left.as_text && right.as_text && *left.as_text == *right.as_text;
    }
    if (left.tag == TypeTag::Ref || right.tag == TypeTag::Ref) {
        return left.tag == right.tag && left.as_ref == right.as_ref;
    }
    // Numbers and bools compare by bits, which makes this bit equality rather
    // than numeric equality: -0.0 is not 0.0, and a NaN is itself. That is the
    // rule a hash table needs -- equal keys must hash equally -- and hashing
    // the same bits is what makes it hold.
    return left.as_uint == right.as_uint;
}

// --- io and khuStdErr ------------------------------------------------------
//
// The two streams share everything but their destination: the same rendering,
// the same formatter, the same byte-oriented surface. Only the sink differs, so
// only the sink is chosen here.

// A line of input parsed at `tag`, or that width's parse sentinel
// (lib/conv.khu) when the line is not a number.
NativeOutcome read_number(NativeServices& services, TypeTag tag) {
    std::string line = services.read_line();
    std::uint32_t width = bytecode::type_width(tag);
    if (bytecode::is_float(tag)) {
        double parsed = 0.0;
        if (!format::parse_float(line, tag == TypeTag::Float32, parsed)) {
            return NativeOutcome::produced(parse_sentinel(tag));
        }
        return NativeOutcome::produced(normalize_float(tag, parsed));
    }
    if (bytecode::is_signed_integer(tag)) {
        std::int64_t parsed = 0;
        if (!format::parse_int(line, width, parsed)) {
            return NativeOutcome::produced(parse_sentinel(tag));
        }
        return NativeOutcome::produced(normalize_int(tag, static_cast<std::uint64_t>(parsed)));
    }
    std::uint64_t parsed = 0;
    if (!format::parse_uint(line, width, parsed)) {
        return NativeOutcome::produced(parse_sentinel(tag));
    }
    return NativeOutcome::produced(normalize_int(tag, parsed));
}

// Writes `count` bytes from a raw pointer to one of the streams. A null pointer
// or a negative count writes nothing; there is no allocation behind a `*byte`
// for the runtime to check the length against, which is what makes it raw.
NativeOutcome write_bytes(NativeServices& services, const Value* argv, bool to_error) {
    if (argv[0].tag != TypeTag::Ptr && !argv[0].is_null_reference()) {
        return NativeOutcome::failed("writeBytes expects a raw pointer, found " +
                                     std::string(bytecode::type_tag_name(argv[0].tag)));
    }
    std::int64_t count = argv[1].as_int;
    if (!argv[0].as_raw || count <= 0) return NativeOutcome::nothing();
    std::string text(static_cast<const char*>(argv[0].as_raw),
                     static_cast<std::size_t>(count));
    if (to_error) {
        services.write_error(text);
    } else {
        services.write_output(text);
    }
    return NativeOutcome::nothing();
}

// --- khuStdMem -------------------------------------------------------------
//
// A `*byte` from `khuStdMem.alloc` is a block from the same allocator the
// `manual` strategy uses (utils/alloc.c), so it carries a header recording its
// size and it shows up in the same leak accounting. That header is what makes
// these operations **checked**: a copy past the end of a block is a trap, not a
// corrupted heap.
//
// A pointer this runtime did not hand out has no header to read, so nothing can
// be checked about it. Today that cannot happen from Khudra source -- `alloc`
// is the only thing that produces a `*byte` -- and the operations say so by
// trapping rather than proceeding blind.

// The pointer argument, with its block size. Fails when the value is not a
// pointer, and reports whether the runtime knows how long it is.
bool pointer_of(const Value& value, void*& out, std::size_t& size) {
    if (value.is_null_reference()) {
        out = nullptr;
        size = 0;
        return true;
    }
    if (value.tag != TypeTag::Ptr) return false;
    out = value.as_raw;
    size = khu_manual_block_size(out);
    return true;
}

// Checks that [0, count) fits inside the block `pointer` names.
NativeOutcome check_range(const char* what, const Value& value, std::int64_t count,
                          void*& pointer) {
    std::size_t size = 0;
    if (!pointer_of(value, pointer, size)) {
        return NativeOutcome::failed(std::string(what) + " expects a raw pointer, found " +
                                     std::string(bytecode::type_tag_name(value.tag)));
    }
    if (count < 0) {
        return NativeOutcome::failed(std::string(what) + " was given a negative count");
    }
    if (count == 0) return NativeOutcome::nothing();
    if (!pointer) {
        return NativeOutcome::failed(std::string(what) +
                                     " through a null pointer: it is not instantiated yet");
    }
    if (size == 0) {
        return NativeOutcome::failed(
            std::string(what) +
            " on a buffer this runtime did not allocate, so its length is unknown");
    }
    if (static_cast<std::uint64_t>(count) > size) {
        return NativeOutcome::failed(std::string(what) + " of " + format::format_int(count) +
                                     " bytes runs past the end of a " +
                                     format::format_uint(size) + "-byte buffer");
    }
    return NativeOutcome::nothing();
}

// --- khuStdRandom ----------------------------------------------------------
//
// PCG32 (O'Neill, 2014): a 64-bit LCG whose output is a permuted 32-bit slice
// of the state. Chosen because it is short enough to read in full, has no
// platform-dependent step, and is exactly reproducible -- an unseeded program
// starts from the same constant under every backend, and a seeded one starts
// from the seed. There is nothing here that could differ between the VM and a
// built binary, which is the requirement; statistical quality is a bonus.
//
// It is not a cryptographic generator, and it does not pretend to be.

std::uint32_t next_bits(NativeServices& services) {
    std::uint64_t previous = services.random_state;
    services.random_state = previous * 6364136223846793005ull + services.random_increment;
    auto shifted = static_cast<std::uint32_t>(((previous >> 18u) ^ previous) >> 27u);
    auto rotation = static_cast<std::uint32_t>(previous >> 59u);
    return (shifted >> rotation) | (shifted << ((32u - rotation) & 31u));
}

std::uint64_t next_bits64(NativeServices& services) {
    std::uint64_t high = next_bits(services);
    return (high << 32) | next_bits(services);
}

// A uniform value in [0, bound), rejecting the tail that would otherwise make
// the low values likelier than the high ones.
std::uint32_t next_bounded(NativeServices& services, std::uint32_t bound) {
    std::uint32_t threshold = (0u - bound) % bound;
    while (true) {
        std::uint32_t bits = next_bits(services);
        if (bits >= threshold) return bits % bound;
    }
}

// --- khuStdTime ------------------------------------------------------------
//
// The one part of the standard library whose answers are not reproducible,
// which is why nothing else in it depends on the clock: a program that prints
// the time cannot have a golden, and a program that does not is unaffected.

std::int64_t wall_nanos() {
    auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

std::int64_t monotonic_nanos() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

// ISO 8601 to the second: `2026-08-30T14:03:07Z` in UTC, without the `Z` in
// local time. Fixed width, so a test can check the shape without knowing when
// it ran.
std::string format_now(bool utc) {
    std::time_t now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    if (utc) {
        gmtime_s(&parts, &now);
    } else {
        localtime_s(&parts, &now);
    }
#else
    if (utc) {
        gmtime_r(&now, &parts);
    } else {
        localtime_r(&now, &parts);
    }
#endif
    // Wide enough for every field at its widest, so the format is never
    // truncated even on a host whose `tm` fields are out of their usual range.
    char scratch[64];
    std::snprintf(scratch, sizeof(scratch), "%04d-%02d-%02dT%02d:%02d:%02d%s",
                  parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday, parts.tm_hour,
                  parts.tm_min, parts.tm_sec, utc ? "Z" : "");
    return scratch;
}

using Unary = double (*)(double);
using Binary = double (*)(double, double);

// A transcendental over `float` or `dfloat`. The declarations in lib/math.khu
// admit nothing else, so a non-float tag here means a malformed image.
NativeOutcome float_unary(bytecode::NativeId id, const Value* argv, std::uint32_t argc,
                          Unary function) {
    if (argc != 1 || !all_float(argv, argc)) {
        return NativeOutcome::failed(std::string(bytecode::native_name(id)) +
                                     " expects a floating point value");
    }
    return NativeOutcome::produced(
        normalize_float(argv[0].tag, function(argv[0].as_float)));
}

NativeOutcome float_binary(bytecode::NativeId id, const Value* argv, std::uint32_t argc,
                           Binary function) {
    if (argc != 2 || !all_float(argv, argc)) {
        return NativeOutcome::failed(std::string(bytecode::native_name(id)) +
                                     " expects two floating point values");
    }
    return NativeOutcome::produced(
        normalize_float(argv[0].tag, function(argv[0].as_float, argv[1].as_float)));
}

}  // namespace

NativeOutcome invoke_native(NativeServices& services, bytecode::NativeId id, const Value* argv,
                            std::uint32_t argc) {
    switch (id) {
        case bytecode::NativeId::Print:
            services.write_output(join_arguments(services, argv, argc));
            return NativeOutcome::nothing();

        case bytecode::NativeId::PrintLine:
            services.write_output(join_arguments(services, argv, argc) + "\n");
            return NativeOutcome::nothing();

        case bytecode::NativeId::ReadLine:
            return NativeOutcome::produced(
                Value::make_string(services.make_string(services.read_line())));

        case bytecode::NativeId::StdlibLoadObject:
            // The standard library is linked into every image, so there is
            // nothing to load; the call exists as an explicit step for a future
            // loader that has real work to do.
            return NativeOutcome::nothing();

        case bytecode::NativeId::GetType:
        case bytecode::NativeId::LoadRuntimeType:
            // A type descriptor is an `Array` handle, and a bare `Array` has no
            // element type, so there is nothing observable to hand back.
            // `null` -- "not instantiated yet" -- is exactly right for an
            // unresolved descriptor; reflection is a roadmap item.
            return NativeOutcome::produced(Value::make_null());

        // --- khuStdMath: utilities ---------------------------------------

        case bytecode::NativeId::Abs: {
            const Value& value = argv[0];
            if (bytecode::is_float(value.tag)) {
                return NativeOutcome::produced(
                    normalize_float(value.tag, std::fabs(value.as_float)));
            }
            // The most negative value of a width has no positive counterpart,
            // so `abs` wraps back onto it -- the same answer negation gives,
            // and the same wrapping rule the rest of the language follows.
            if (value.as_int < 0) {
                return NativeOutcome::produced(
                    normalize_int(value.tag, 0ull - value.as_uint));
            }
            return NativeOutcome::produced(value);
        }

        case bytecode::NativeId::Min:
            return NativeOutcome::produced(order(argv[0], argv[1]) <= 0 ? argv[0] : argv[1]);

        case bytecode::NativeId::Max:
            return NativeOutcome::produced(order(argv[0], argv[1]) >= 0 ? argv[0] : argv[1]);

        case bytecode::NativeId::Clamp: {
            // `low` above `high` is a caller error with no sensible answer, so
            // it is a trap rather than a silently chosen bound.
            if (order(argv[1], argv[2]) > 0) {
                return NativeOutcome::failed(
                    "khuStdMath.clamp was given a low bound above its high bound");
            }
            if (order(argv[0], argv[1]) < 0) return NativeOutcome::produced(argv[1]);
            if (order(argv[0], argv[2]) > 0) return NativeOutcome::produced(argv[2]);
            return NativeOutcome::produced(argv[0]);
        }

        case bytecode::NativeId::Signum: {
            const Value& value = argv[0];
            if (bytecode::is_float(value.tag)) {
                // A NaN has no sign to report, so it reports itself.
                if (std::isnan(value.as_float)) return NativeOutcome::produced(value);
                double sign = value.as_float > 0 ? 1.0 : (value.as_float < 0 ? -1.0 : 0.0);
                return NativeOutcome::produced(normalize_float(value.tag, sign));
            }
            std::int64_t sign = value.as_int > 0 ? 1 : (value.as_int < 0 ? -1 : 0);
            return NativeOutcome::produced(
                normalize_int(value.tag, static_cast<std::uint64_t>(sign)));
        }

        case bytecode::NativeId::Gcd:
            // Always non-negative, so it is the magnitude that matters; the
            // width still wraps on the way back, which only bites for the most
            // negative value of a signed width.
            return NativeOutcome::produced(
                normalize_int(argv[0].tag, gcd_of(magnitude(argv[0]), magnitude(argv[1]))));

        case bytecode::NativeId::Lcm: {
            std::uint64_t left = magnitude(argv[0]);
            std::uint64_t right = magnitude(argv[1]);
            if (left == 0 || right == 0) {
                return NativeOutcome::produced(normalize_int(argv[0].tag, 0));
            }
            std::uint64_t divisor = gcd_of(left, right);
            return NativeOutcome::produced(
                normalize_int(argv[0].tag, (left / divisor) * right));
        }

        case bytecode::NativeId::Pow: {
            if (bytecode::is_float(argv[0].tag)) {
                return float_binary(id, argv, argc, [](double base, double exponent) {
                    return std::pow(base, exponent);
                });
            }
            TypeTag tag = argv[0].tag;
            bool negative_exponent =
                bytecode::is_signed_integer(argv[1].tag) && argv[1].as_int < 0;
            if (!negative_exponent) {
                std::uint64_t exponent = argv[1].as_uint & mask_of(bytecode::type_width(tag));
                return NativeOutcome::produced(
                    normalize_int(tag, integer_power(tag, argv[0].as_uint, exponent)));
            }
            // A negative exponent names a fraction, and an integer result is
            // that fraction truncated toward zero: 0 for every base but the
            // three where it is not, and a division by zero for base 0.
            std::uint64_t base = argv[0].as_uint & mask_of(bytecode::type_width(tag));
            if (base == 0) return NativeOutcome::failed("division by zero");
            if (argv[0].as_int == 1) return NativeOutcome::produced(normalize_int(tag, 1));
            if (argv[0].as_int == -1) {
                bool odd = (argv[1].as_int & 1) != 0;
                return NativeOutcome::produced(normalize_int(tag, odd ? ~0ull : 1ull));
            }
            return NativeOutcome::produced(normalize_int(tag, 0));
        }

        // --- khuStdMath: transcendentals ---------------------------------

        case bytecode::NativeId::Sqrt:
            return float_unary(id, argv, argc, [](double x) { return std::sqrt(x); });
        case bytecode::NativeId::Floor:
            return float_unary(id, argv, argc, [](double x) { return std::floor(x); });
        case bytecode::NativeId::Ceil:
            return float_unary(id, argv, argc, [](double x) { return std::ceil(x); });
        case bytecode::NativeId::Round:
            // Halves go away from zero, which is C's `round` and the rule most
            // readers expect; it is not the even-rounding a printf uses.
            return float_unary(id, argv, argc, [](double x) { return std::round(x); });
        case bytecode::NativeId::Exp:
            return float_unary(id, argv, argc, [](double x) { return std::exp(x); });
        case bytecode::NativeId::Log:
            return float_unary(id, argv, argc, [](double x) { return std::log(x); });
        case bytecode::NativeId::Log10:
            return float_unary(id, argv, argc, [](double x) { return std::log10(x); });
        case bytecode::NativeId::Sin:
            return float_unary(id, argv, argc, [](double x) { return std::sin(x); });
        case bytecode::NativeId::Cos:
            return float_unary(id, argv, argc, [](double x) { return std::cos(x); });
        case bytecode::NativeId::Tan:
            return float_unary(id, argv, argc, [](double x) { return std::tan(x); });
        case bytecode::NativeId::Asin:
            return float_unary(id, argv, argc, [](double x) { return std::asin(x); });
        case bytecode::NativeId::Acos:
            return float_unary(id, argv, argc, [](double x) { return std::acos(x); });
        case bytecode::NativeId::Atan:
            return float_unary(id, argv, argc, [](double x) { return std::atan(x); });
        case bytecode::NativeId::Sinh:
            return float_unary(id, argv, argc, [](double x) { return std::sinh(x); });
        case bytecode::NativeId::Cosh:
            return float_unary(id, argv, argc, [](double x) { return std::cosh(x); });
        case bytecode::NativeId::Tanh:
            return float_unary(id, argv, argc, [](double x) { return std::tanh(x); });
        case bytecode::NativeId::Fmod:
            return float_binary(id, argv, argc,
                                [](double a, double b) { return std::fmod(a, b); });
        case bytecode::NativeId::Atan2:
            return float_binary(id, argv, argc,
                                [](double a, double b) { return std::atan2(a, b); });

        // --- khuStdMath: constants ---------------------------------------
        //
        // Functions rather than members, because a namespace has no `const`
        // members yet (PLAN.md, Phase 10). The digits are the `double` nearest
        // each constant, which is what a C library's M_PI is too.

        case bytecode::NativeId::Pi:
            return NativeOutcome::produced(
                Value::make_float(TypeTag::Float64, 3.14159265358979323846));
        case bytecode::NativeId::E:
            return NativeOutcome::produced(
                Value::make_float(TypeTag::Float64, 2.71828182845904523536));

        // --- khuStdConv: number to string --------------------------------

        case bytecode::NativeId::ToString:
            // Deliberately the same rendering `io.print` uses: a program that
            // prints a number and a program that builds a string out of one
            // must not disagree about what the number looks like.
            return NativeOutcome::produced(
                Value::make_string(services.make_string(services.render(argv[0]))));

        // --- khuStdConv: string to number --------------------------------

        case bytecode::NativeId::ParseInt8: return parse_signed(TypeTag::Int8, argv[0]);
        case bytecode::NativeId::ParseInt16: return parse_signed(TypeTag::Int16, argv[0]);
        case bytecode::NativeId::ParseInt32: return parse_signed(TypeTag::Int32, argv[0]);
        case bytecode::NativeId::ParseInt64: return parse_signed(TypeTag::Int64, argv[0]);
        case bytecode::NativeId::ParseUInt8: return parse_unsigned(TypeTag::UInt8, argv[0]);
        case bytecode::NativeId::ParseUInt16: return parse_unsigned(TypeTag::UInt16, argv[0]);
        case bytecode::NativeId::ParseUInt32: return parse_unsigned(TypeTag::UInt32, argv[0]);
        case bytecode::NativeId::ParseUInt64: return parse_unsigned(TypeTag::UInt64, argv[0]);
        case bytecode::NativeId::ParseFloat: return parse_floating(TypeTag::Float32, argv[0]);
        case bytecode::NativeId::ParseDFloat: return parse_floating(TypeTag::Float64, argv[0]);

        case bytecode::NativeId::IsNumeric: {
            std::string_view text;
            if (!text_of(argv[0], text)) return NativeOutcome::produced(Value::make_bool(false));
            return NativeOutcome::produced(Value::make_bool(format::is_numeric(text)));
        }

        // --- khuStdConv: bytes -------------------------------------------

        case bytecode::NativeId::IsDigit:
            return NativeOutcome::produced(Value::make_bool(
                format::is_digit_byte(static_cast<std::uint8_t>(argv[0].as_uint))));
        case bytecode::NativeId::IsLetter:
            return NativeOutcome::produced(Value::make_bool(
                format::is_letter_byte(static_cast<std::uint8_t>(argv[0].as_uint))));
        case bytecode::NativeId::IsWhitespace:
            return NativeOutcome::produced(Value::make_bool(
                format::is_whitespace_byte(static_cast<std::uint8_t>(argv[0].as_uint))));
        case bytecode::NativeId::ToUpper:
            return NativeOutcome::produced(normalize_int(
                TypeTag::UInt8,
                format::to_upper_byte(static_cast<std::uint8_t>(argv[0].as_uint))));
        case bytecode::NativeId::ToLower:
            return NativeOutcome::produced(normalize_int(
                TypeTag::UInt8,
                format::to_lower_byte(static_cast<std::uint8_t>(argv[0].as_uint))));

        case bytecode::NativeId::ToChar: {
            // A one-byte string. There is no other way to get from a byte back
            // to text: indexing a string yields a byte, and nothing puts one
            // back.
            std::string one(1, static_cast<char>(argv[0].as_uint & 0xffu));
            return NativeOutcome::produced(
                Value::make_string(services.make_string(std::move(one))));
        }

        // --- khuStdConv: bool --------------------------------------------

        case bytecode::NativeId::BoolToInt:
            return NativeOutcome::produced(
                normalize_int(TypeTag::Int32, argv[0].as_uint != 0 ? 1u : 0u));
        case bytecode::NativeId::IntToBool:
            // Any non-zero value is true, which is the C rule -- but note that
            // Khudra itself has no truthiness: a condition must already be a
            // bool, and this is the explicit step that makes one.
            return NativeOutcome::produced(Value::make_bool(argv[0].as_int != 0));

        // --- khuStdString ------------------------------------------------

        case bytecode::NativeId::StrLength: {
            std::string_view text;
            if (!text_of(argv[0], text)) {
                return NativeOutcome::failed(
                    "cannot read the length of null: the string is not instantiated yet");
            }
            return NativeOutcome::produced(
                normalize_int(TypeTag::Int32, static_cast<std::uint64_t>(text.size())));
        }

        case bytecode::NativeId::StrConcat: {
            std::string joined;
            for (std::uint32_t i = 0; i < argc; ++i) {
                std::string_view piece;
                if (!text_of(argv[i], piece)) {
                    return NativeOutcome::failed(
                        "cannot concatenate null: the string is not instantiated yet");
                }
                joined += piece;
            }
            return NativeOutcome::produced(
                Value::make_string(services.make_string(std::move(joined))));
        }

        case bytecode::NativeId::StrSubstring: {
            std::string_view text;
            if (!text_of(argv[0], text)) {
                return NativeOutcome::failed(
                    "cannot take a substring of null: the string is not instantiated yet");
            }
            std::size_t start = clamp_index(argv[1].as_int, text.size());
            std::size_t end = clamp_index(argv[2].as_int, text.size());
            // A reversed range names nothing rather than naming it backwards.
            if (end < start) end = start;
            return NativeOutcome::produced(Value::make_string(
                services.make_string(std::string(text.substr(start, end - start)))));
        }

        case bytecode::NativeId::StrCharAt: {
            std::string_view text;
            if (!text_of(argv[0], text)) {
                return NativeOutcome::failed(
                    "cannot read a byte of null: the string is not instantiated yet");
            }
            std::int64_t index = argv[1].as_int;
            if (index < 0 || static_cast<std::uint64_t>(index) >= text.size()) {
                return NativeOutcome::produced(normalize_int(TypeTag::UInt8, 0));
            }
            return NativeOutcome::produced(normalize_int(
                TypeTag::UInt8,
                static_cast<std::uint8_t>(text[static_cast<std::size_t>(index)])));
        }

        case bytecode::NativeId::StrIndexOf: {
            std::string_view text;
            std::string_view needle;
            if (!two_strings(argv, text, needle)) {
                return NativeOutcome::failed(
                    "cannot search null: the string is not instantiated yet");
            }
            std::size_t from = argc == 3 ? clamp_index(argv[2].as_int, text.size()) : 0;
            std::size_t at = text.find(needle, from);
            return NativeOutcome::produced(normalize_int(
                TypeTag::Int32, at == std::string_view::npos
                                    ? static_cast<std::uint64_t>(-1)
                                    : static_cast<std::uint64_t>(at)));
        }

        case bytecode::NativeId::StrLastIndexOf: {
            std::string_view text;
            std::string_view needle;
            if (!two_strings(argv, text, needle)) {
                return NativeOutcome::failed(
                    "cannot search null: the string is not instantiated yet");
            }
            std::size_t at = text.rfind(needle);
            return NativeOutcome::produced(normalize_int(
                TypeTag::Int32, at == std::string_view::npos
                                    ? static_cast<std::uint64_t>(-1)
                                    : static_cast<std::uint64_t>(at)));
        }

        case bytecode::NativeId::StrContains:
        case bytecode::NativeId::StrStartsWith:
        case bytecode::NativeId::StrEndsWith:
        case bytecode::NativeId::StrEquals:
        case bytecode::NativeId::StrEqualsIgnoreCase: {
            std::string_view text;
            std::string_view other;
            if (!two_strings(argv, text, other)) {
                return NativeOutcome::failed(
                    "cannot compare null: the string is not instantiated yet");
            }
            bool answer = false;
            switch (id) {
                case bytecode::NativeId::StrContains:
                    answer = text.find(other) != std::string_view::npos;
                    break;
                case bytecode::NativeId::StrStartsWith:
                    answer = starts_at(text, 0, other);
                    break;
                case bytecode::NativeId::StrEndsWith:
                    answer = other.size() <= text.size() &&
                             starts_at(text, text.size() - other.size(), other);
                    break;
                case bytecode::NativeId::StrEquals:
                    answer = text == other;
                    break;
                default:
                    answer = fold_case(text, false) == fold_case(other, false);
                    break;
            }
            return NativeOutcome::produced(Value::make_bool(answer));
        }

        case bytecode::NativeId::StrCompareTo:
        case bytecode::NativeId::StrCompareToIgnoreCase: {
            std::string_view text;
            std::string_view other;
            if (!two_strings(argv, text, other)) {
                return NativeOutcome::failed(
                    "cannot compare null: the string is not instantiated yet");
            }
            std::int64_t answer = 0;
            if (id == bytecode::NativeId::StrCompareTo) {
                answer = compare_bytes(text, other);
            } else {
                std::string left = fold_case(text, false);
                std::string right = fold_case(other, false);
                answer = compare_bytes(left, right);
            }
            return NativeOutcome::produced(
                normalize_int(TypeTag::Int32, static_cast<std::uint64_t>(answer)));
        }

        case bytecode::NativeId::StrToUpperCase:
        case bytecode::NativeId::StrToLowerCase: {
            std::string_view text;
            if (!text_of(argv[0], text)) {
                return NativeOutcome::failed(
                    "cannot case-fold null: the string is not instantiated yet");
            }
            return NativeOutcome::produced(Value::make_string(services.make_string(
                fold_case(text, id == bytecode::NativeId::StrToUpperCase))));
        }

        case bytecode::NativeId::StrTrim:
        case bytecode::NativeId::StrTrimStart:
        case bytecode::NativeId::StrTrimEnd: {
            std::string_view text;
            if (!text_of(argv[0], text)) {
                return NativeOutcome::failed(
                    "cannot trim null: the string is not instantiated yet");
            }
            std::size_t start = 0;
            std::size_t end = text.size();
            if (id != bytecode::NativeId::StrTrimEnd) {
                while (start < end &&
                       format::is_whitespace_byte(static_cast<std::uint8_t>(text[start]))) {
                    ++start;
                }
            }
            if (id != bytecode::NativeId::StrTrimStart) {
                while (end > start &&
                       format::is_whitespace_byte(static_cast<std::uint8_t>(text[end - 1]))) {
                    --end;
                }
            }
            return NativeOutcome::produced(Value::make_string(
                services.make_string(std::string(text.substr(start, end - start)))));
        }

        case bytecode::NativeId::StrReplace:
        case bytecode::NativeId::StrReplaceAll: {
            std::string_view text;
            std::string_view from;
            std::string_view to;
            if (!two_strings(argv, text, from) || !text_of(argv[2], to)) {
                return NativeOutcome::failed(
                    "cannot replace in null: the string is not instantiated yet");
            }
            return NativeOutcome::produced(Value::make_string(services.make_string(
                replace_in(text, from, to, id == bytecode::NativeId::StrReplaceAll))));
        }

        case bytecode::NativeId::StrRepeat: {
            std::string_view text;
            if (!text_of(argv[0], text)) {
                return NativeOutcome::failed(
                    "cannot repeat null: the string is not instantiated yet");
            }
            std::int64_t count = argv[1].as_int;
            if (count <= 0 || text.empty()) {
                return NativeOutcome::produced(
                    Value::make_string(services.make_string(std::string())));
            }
            // `length` reports an int32, so a string longer than an int32 can
            // count is one the rest of the surface could not describe.
            std::uint64_t total = static_cast<std::uint64_t>(count) * text.size();
            if (total > 2147483647ull) {
                return NativeOutcome::failed(
                    "khuStdString.repeat would produce a string longer than int32 can count");
            }
            std::string out;
            out.reserve(static_cast<std::size_t>(total));
            for (std::int64_t i = 0; i < count; ++i) out += text;
            return NativeOutcome::produced(
                Value::make_string(services.make_string(std::move(out))));
        }

        case bytecode::NativeId::StrIsEmpty:
        case bytecode::NativeId::StrIsBlank: {
            std::string_view text;
            if (!text_of(argv[0], text)) {
                return NativeOutcome::failed(
                    "cannot inspect null: the string is not instantiated yet");
            }
            if (id == bytecode::NativeId::StrIsEmpty) {
                return NativeOutcome::produced(Value::make_bool(text.empty()));
            }
            bool blank = true;
            for (char byte : text) {
                if (!format::is_whitespace_byte(static_cast<std::uint8_t>(byte))) {
                    blank = false;
                    break;
                }
            }
            return NativeOutcome::produced(Value::make_bool(blank));
        }

        case bytecode::NativeId::CanParseInt8: return can_parse(TypeTag::Int8, argv[0]);
        case bytecode::NativeId::CanParseInt16: return can_parse(TypeTag::Int16, argv[0]);
        case bytecode::NativeId::CanParseInt32: return can_parse(TypeTag::Int32, argv[0]);
        case bytecode::NativeId::CanParseInt64: return can_parse(TypeTag::Int64, argv[0]);
        case bytecode::NativeId::CanParseUInt8: return can_parse(TypeTag::UInt8, argv[0]);
        case bytecode::NativeId::CanParseUInt16: return can_parse(TypeTag::UInt16, argv[0]);
        case bytecode::NativeId::CanParseUInt32: return can_parse(TypeTag::UInt32, argv[0]);
        case bytecode::NativeId::CanParseUInt64: return can_parse(TypeTag::UInt64, argv[0]);
        case bytecode::NativeId::CanParseFloat: return can_parse(TypeTag::Float32, argv[0]);
        case bytecode::NativeId::CanParseDFloat: return can_parse(TypeTag::Float64, argv[0]);

        // --- khuErrors: the error-code table ------------------------------
        //
        // Codes, not messages. A code is what a program branches on; the
        // message beside it in a `Result` is what a person reads.

        case bytecode::NativeId::ErrNone:
            return NativeOutcome::produced(Value::make_int(TypeTag::Int32, 0));
        case bytecode::NativeId::ErrBounds:
            return NativeOutcome::produced(Value::make_int(TypeTag::Int32, 1));
        case bytecode::NativeId::ErrParse:
            return NativeOutcome::produced(Value::make_int(TypeTag::Int32, 2));
        case bytecode::NativeId::ErrNullReference:
            return NativeOutcome::produced(Value::make_int(TypeTag::Int32, 3));
        case bytecode::NativeId::ErrIo:
            return NativeOutcome::produced(Value::make_int(TypeTag::Int32, 4));
        case bytecode::NativeId::ErrDivideByZero:
            return NativeOutcome::produced(Value::make_int(TypeTag::Int32, 5));
        case bytecode::NativeId::ErrNotFound:
            return NativeOutcome::produced(Value::make_int(TypeTag::Int32, 6));
        case bytecode::NativeId::ErrInvalidArgument:
            return NativeOutcome::produced(Value::make_int(TypeTag::Int32, 7));
        case bytecode::NativeId::ErrUnsupported:
            return NativeOutcome::produced(Value::make_int(TypeTag::Int32, 8));
        case bytecode::NativeId::ErrOverflow:
            return NativeOutcome::produced(Value::make_int(TypeTag::Int32, 9));
        case bytecode::NativeId::ErrEmpty:
            return NativeOutcome::produced(Value::make_int(TypeTag::Int32, 10));

        case bytecode::NativeId::ErrFail: {
            // The one way Khudra source can raise a fatal runtime error. It is
            // how `Result.value()` refuses to invent a value it has not got:
            // generics are erased, so there is no honest default for a `T`
            // that was never written.
            std::string_view message;
            if (!text_of(argv[0], message)) return NativeOutcome::failed("failure");
            return NativeOutcome::failed(std::string(message));
        }

        // --- khuStdCollection ---------------------------------------------

        case bytecode::NativeId::Hash:
            return NativeOutcome::produced(
                Value::make_int(TypeTag::Int32, hash_value(argv[0])));

        case bytecode::NativeId::SameValue:
            return NativeOutcome::produced(Value::make_bool(same_value(argv[0], argv[1])));

        // --- io: rendering, bytes and streams -----------------------------

        case bytecode::NativeId::Describe:
            // The render rule for a value `print` has no overload for: a class
            // reference is its class name in angle brackets, an array is its
            // length. Absent a `toString` a class can override, identity is
            // the only honest thing to say about an object.
            return NativeOutcome::produced(
                Value::make_string(services.make_string(services.render(argv[0]))));

        case bytecode::NativeId::WriteString: {
            std::string_view text;
            if (!text_of(argv[0], text)) {
                return NativeOutcome::failed(
                    "cannot write null: the string is not instantiated yet");
            }
            services.write_output(std::string(text));
            return NativeOutcome::nothing();
        }

        case bytecode::NativeId::WriteBytes:
            return write_bytes(services, argv, false);

        case bytecode::NativeId::Flush:
            services.flush_output();
            return NativeOutcome::nothing();

        case bytecode::NativeId::ReadChar: {
            int byte = services.read_byte();
            return NativeOutcome::produced(
                normalize_int(TypeTag::UInt8, byte < 0 ? 0u : static_cast<std::uint64_t>(byte)));
        }

        case bytecode::NativeId::ReadByte:
            // -1 at end of input, which is the difference between this and
            // readChar: a byte cannot say "there was nothing".
            return NativeOutcome::produced(
                Value::make_int(TypeTag::Int32, services.read_byte()));

        case bytecode::NativeId::ReadBool: {
            // Exactly the two keywords, so that reading back what `io.print`
            // wrote round-trips. Anything else is false.
            std::string line = services.read_line();
            return NativeOutcome::produced(Value::make_bool(line == "true"));
        }

        case bytecode::NativeId::ReadInt8: return read_number(services, TypeTag::Int8);
        case bytecode::NativeId::ReadInt16: return read_number(services, TypeTag::Int16);
        case bytecode::NativeId::ReadInt32: return read_number(services, TypeTag::Int32);
        case bytecode::NativeId::ReadInt64: return read_number(services, TypeTag::Int64);
        case bytecode::NativeId::ReadUInt8: return read_number(services, TypeTag::UInt8);
        case bytecode::NativeId::ReadUInt16: return read_number(services, TypeTag::UInt16);
        case bytecode::NativeId::ReadUInt32: return read_number(services, TypeTag::UInt32);
        case bytecode::NativeId::ReadUInt64: return read_number(services, TypeTag::UInt64);
        case bytecode::NativeId::ReadFloat: return read_number(services, TypeTag::Float32);
        case bytecode::NativeId::ReadDFloat: return read_number(services, TypeTag::Float64);

        // --- khuStdErr: the same surface, the other stream ----------------

        case bytecode::NativeId::ErrPrint:
            services.write_error(join_arguments(services, argv, argc));
            return NativeOutcome::nothing();

        case bytecode::NativeId::ErrPrintLine:
            services.write_error(join_arguments(services, argv, argc) + "\n");
            return NativeOutcome::nothing();

        case bytecode::NativeId::ErrWriteString: {
            std::string_view text;
            if (!text_of(argv[0], text)) {
                return NativeOutcome::failed(
                    "cannot write null: the string is not instantiated yet");
            }
            services.write_error(std::string(text));
            return NativeOutcome::nothing();
        }

        case bytecode::NativeId::ErrWriteBytes:
            return write_bytes(services, argv, true);

        case bytecode::NativeId::ErrFlush:
            services.flush_error();
            return NativeOutcome::nothing();

        // --- khuStdMem ----------------------------------------------------

        case bytecode::NativeId::MemAlloc: {
            std::int64_t size = argv[0].as_int;
            if (size < 0) {
                return NativeOutcome::failed("khuStdMem.alloc was given a negative size");
            }
            void* block = khu_manual_alloc(static_cast<std::size_t>(size));
            if (!block) return NativeOutcome::failed("out of memory allocating a buffer");
            Value result;
            result.tag = TypeTag::Ptr;
            result.as_raw = block;
            return NativeOutcome::produced(result);
        }

        case bytecode::NativeId::MemRealloc: {
            void* block = nullptr;
            std::size_t size = 0;
            if (!pointer_of(argv[0], block, size)) {
                return NativeOutcome::failed("khuStdMem.realloc expects a raw pointer, found " +
                                             std::string(bytecode::type_tag_name(argv[0].tag)));
            }
            std::int64_t wanted = argv[1].as_int;
            if (wanted < 0) {
                return NativeOutcome::failed("khuStdMem.realloc was given a negative size");
            }
            if (block && size == 0) {
                return NativeOutcome::failed(
                    "khuStdMem.realloc on a buffer this runtime did not allocate");
            }
            void* fresh = khu_manual_realloc(block, static_cast<std::size_t>(wanted));
            if (!fresh && wanted > 0) {
                return NativeOutcome::failed("out of memory resizing a buffer");
            }
            Value result;
            result.tag = TypeTag::Ptr;
            result.as_raw = fresh;
            return NativeOutcome::produced(result);
        }

        case bytecode::NativeId::MemFree: {
            void* block = nullptr;
            std::size_t size = 0;
            if (!pointer_of(argv[0], block, size)) {
                return NativeOutcome::failed("khuStdMem.release expects a raw pointer, found " +
                                             std::string(bytecode::type_tag_name(argv[0].tag)));
            }
            // Releasing null is a no-op, the way it is in C. Releasing
            // something this runtime did not hand out is not: a double free
            // would look exactly like it.
            if (block && size == 0) {
                return NativeOutcome::failed(
                    "khuStdMem.release of a buffer this runtime did not allocate, or of one "
                    "that has already been released");
            }
            khu_manual_free(block);
            return NativeOutcome::nothing();
        }

        case bytecode::NativeId::MemCopy:
        case bytecode::NativeId::MemMove: {
            bool overlapping = id == bytecode::NativeId::MemMove;
            const char* what = overlapping ? "khuStdMem.move" : "khuStdMem.copy";
            std::int64_t count = argv[2].as_int;
            void* destination = nullptr;
            void* source = nullptr;
            NativeOutcome checked = check_range(what, argv[0], count, destination);
            if (!checked.ok) return checked;
            checked = check_range(what, argv[1], count, source);
            if (!checked.ok) return checked;
            if (count == 0 || !destination || !source) return NativeOutcome::nothing();
            if (overlapping) {
                std::memmove(destination, source, static_cast<std::size_t>(count));
            } else {
                // Non-overlapping is the caller's promise, so it is checked:
                // memcpy on overlapping ranges is undefined, and "undefined"
                // is not an answer a program should be able to reach.
                auto* to = static_cast<unsigned char*>(destination);
                auto* from = static_cast<unsigned char*>(source);
                std::size_t length = static_cast<std::size_t>(count);
                if (to < from + length && from < to + length) {
                    return NativeOutcome::failed(
                        "khuStdMem.copy was given overlapping ranges; use khuStdMem.move");
                }
                std::memcpy(destination, source, length);
            }
            return NativeOutcome::nothing();
        }

        case bytecode::NativeId::MemZero:
        case bytecode::NativeId::MemFill: {
            bool is_fill = id == bytecode::NativeId::MemFill;
            const char* what = is_fill ? "khuStdMem.fill" : "khuStdMem.zero";
            std::int64_t count = is_fill ? argv[2].as_int : argv[1].as_int;
            void* block = nullptr;
            NativeOutcome checked = check_range(what, argv[0], count, block);
            if (!checked.ok) return checked;
            if (count == 0 || !block) return NativeOutcome::nothing();
            int byte = is_fill ? static_cast<int>(argv[1].as_uint & 0xffu) : 0;
            std::memset(block, byte, static_cast<std::size_t>(count));
            return NativeOutcome::nothing();
        }

        case bytecode::NativeId::MemCompare: {
            std::int64_t count = argv[2].as_int;
            void* left = nullptr;
            void* right = nullptr;
            NativeOutcome checked = check_range("khuStdMem.compare", argv[0], count, left);
            if (!checked.ok) return checked;
            checked = check_range("khuStdMem.compare", argv[1], count, right);
            if (!checked.ok) return checked;
            if (count == 0 || !left || !right) {
                return NativeOutcome::produced(Value::make_int(TypeTag::Int32, 0));
            }
            // -1, 0 or 1 rather than a difference of bytes, so the answer does
            // not depend on how the comparison was computed.
            int order = std::memcmp(left, right, static_cast<std::size_t>(count));
            return NativeOutcome::produced(
                Value::make_int(TypeTag::Int32, order < 0 ? -1 : (order > 0 ? 1 : 0)));
        }

        case bytecode::NativeId::MemAddressOf: {
            void* block = nullptr;
            std::size_t size = 0;
            if (!pointer_of(argv[0], block, size)) {
                return NativeOutcome::failed("khuStdMem.addressOf expects a raw pointer, found " +
                                             std::string(bytecode::type_tag_name(argv[0].tag)));
            }
            // An address is not reproducible across runs. It is here to be
            // compared and printed while debugging, not to be depended on --
            // which is also why khuStdCollection.hash does not use one.
            return NativeOutcome::produced(
                Value::make_uint(TypeTag::UInt64, reinterpret_cast<std::uintptr_t>(block)));
        }

        case bytecode::NativeId::MemRefEquals: {
            void* left = nullptr;
            void* right = nullptr;
            std::size_t ignored = 0;
            if (!pointer_of(argv[0], left, ignored) || !pointer_of(argv[1], right, ignored)) {
                return NativeOutcome::failed("khuStdMem.refEquals expects two raw pointers");
            }
            return NativeOutcome::produced(Value::make_bool(left == right));
        }

        case bytecode::NativeId::MemSizeOf: {
            if (argv[0].tag == TypeTag::Ptr || argv[0].is_null_reference()) {
                void* block = nullptr;
                std::size_t size = 0;
                pointer_of(argv[0], block, size);
                return NativeOutcome::produced(
                    Value::make_int(TypeTag::Int32, static_cast<std::int64_t>(size)));
            }
            if (argv[0].tag == TypeTag::Ref && argv[0].as_ref && argv[0].as_ref->is_array()) {
                return NativeOutcome::produced(Value::make_int(
                    TypeTag::Int32, static_cast<std::int64_t>(argv[0].as_ref->header.size)));
            }
            return NativeOutcome::failed(
                "khuStdMem.sizeOf expects a buffer or an array, found " +
                std::string(bytecode::type_tag_name(argv[0].tag)));
        }

        case bytecode::NativeId::MemIsNull:
            return NativeOutcome::produced(Value::make_bool(argv[0].is_null_reference() ||
                                                            (argv[0].tag == TypeTag::Ptr &&
                                                             argv[0].as_raw == nullptr)));

        case bytecode::NativeId::MemLiveBytes:
            return NativeOutcome::produced(Value::make_int(
                TypeTag::Int64, static_cast<std::int64_t>(khu_manual_live_bytes())));

        case bytecode::NativeId::MemLiveBlocks:
            return NativeOutcome::produced(Value::make_int(
                TypeTag::Int64, static_cast<std::int64_t>(khu_manual_live_blocks())));

        // --- khuStdRandom -------------------------------------------------

        case bytecode::NativeId::RandomSeed:
            // The increment must be odd for the generator to reach its full
            // period, so the seed decides the stream and the constant keeps
            // it well formed.
            services.random_state = static_cast<std::uint64_t>(argv[0].as_int);
            services.random_increment = (static_cast<std::uint64_t>(argv[0].as_int) << 1) | 1ull;
            // Discard the first output: an LCG's first step barely moves, so
            // two nearby seeds would otherwise start out looking alike.
            next_bits(services);
            return NativeOutcome::nothing();

        case bytecode::NativeId::RandomNextInt: {
            if (argc == 0) {
                return NativeOutcome::produced(
                    normalize_int(TypeTag::Int32, next_bits(services)));
            }
            std::int64_t bound = argv[0].as_int;
            if (bound <= 0) {
                return NativeOutcome::failed(
                    "khuStdRandom.nextInt needs a bound above 0, found " +
                    format::format_int(bound));
            }
            return NativeOutcome::produced(normalize_int(
                TypeTag::Int32, next_bounded(services, static_cast<std::uint32_t>(bound))));
        }

        case bytecode::NativeId::RandomNextInt64:
            return NativeOutcome::produced(
                normalize_int(TypeTag::Int64, next_bits64(services)));

        case bytecode::NativeId::RandomNextFloat:
            // 24 bits is a float's whole significand, so this covers [0, 1)
            // without ever rounding up to 1.
            return NativeOutcome::produced(normalize_float(
                TypeTag::Float32,
                static_cast<double>(next_bits(services) >> 8) / 16777216.0));

        case bytecode::NativeId::RandomNextDouble:
            // The same, at a double's 53.
            return NativeOutcome::produced(normalize_float(
                TypeTag::Float64,
                static_cast<double>(next_bits64(services) >> 11) / 9007199254740992.0));

        case bytecode::NativeId::RandomNextBool:
            return NativeOutcome::produced(Value::make_bool((next_bits(services) & 1u) != 0));

        case bytecode::NativeId::RandomNextBytes: {
            std::int64_t count = argv[1].as_int;
            void* block = nullptr;
            NativeOutcome checked =
                check_range("khuStdRandom.nextBytes", argv[0], count, block);
            if (!checked.ok) return checked;
            if (count == 0 || !block) return NativeOutcome::nothing();
            auto* bytes = static_cast<unsigned char*>(block);
            for (std::int64_t i = 0; i < count; ++i) {
                bytes[i] = static_cast<unsigned char>(next_bits(services) & 0xffu);
            }
            return NativeOutcome::nothing();
        }

        // --- khuStdTime ---------------------------------------------------

        case bytecode::NativeId::TimeNowMillis:
            return NativeOutcome::produced(
                Value::make_int(TypeTag::Int64, wall_nanos() / 1000000));

        case bytecode::NativeId::TimeNowNanos:
            return NativeOutcome::produced(Value::make_int(TypeTag::Int64, wall_nanos()));

        case bytecode::NativeId::TimeMonotonicNanos:
            return NativeOutcome::produced(Value::make_int(TypeTag::Int64, monotonic_nanos()));

        case bytecode::NativeId::TimeNowString:
            return NativeOutcome::produced(Value::make_string(
                services.make_string(format_now(argv[0].as_uint != 0))));

        case bytecode::NativeId::TimeSleep: {
            std::int64_t millis = argv[0].as_int;
            if (millis > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(millis));
            }
            return NativeOutcome::nothing();
        }

        case bytecode::NativeId::None:
            break;
    }

    return NativeOutcome::failed("unknown native function id " +
                                 format::format_uint(static_cast<std::uint32_t>(id)));
}

}  // namespace khu::vm

// Number formatting and parsing, shared by both backends.
//
// The byte-identical invariant (docs/native.md, section 5) says a program's
// stdout does not depend on which backend ran it. Rendering a number is the
// easiest place for that to quietly stop being true -- one backend reaching for
// `%g` and the other for `std::to_string` is enough -- so every number that
// reaches output goes through this file, and so does every string that has to
// come back as a number.
//
// It lives in src/vm/ because that is `khu_vm_core`, which the interpreter and
// the native host both link. Neither one owns it.
//
// ## The float rule
//
// A `float` is 32-bit and a `dfloat` is 64-bit, but both are carried in a
// `double` at run time: a `float` result is rounded back through `float` at
// every operation (`normalize_float`), so its *value* is 32-bit even though its
// storage is not.
//
// Rendering does not distinguish them. Both print as C's `%g` with the default
// precision -- **six significant digits**, in whichever of `%e` and `%f` is
// shorter, with trailing zeros removed. `1.5` prints `1.5`, `1.0/3.0` prints
// `0.333333`, `1e20` prints `1e+20`.
//
// Six digits is a display format, not a round-trip format: `parse_float` of a
// formatted `dfloat` recovers the printed digits, not the original bits. That
// is a deliberate trade -- one rule for `io.print` and `khuStdConv.toString`
// alike, so the same number never renders two ways in one program.
//
// Non-finite values print `inf`, `-inf` and `nan`. NaN never prints `-nan`:
// the sign of a NaN is not observable in Khudra, so it is not printed.
#ifndef KHU_VM_FORMAT_H
#define KHU_VM_FORMAT_H

#include <cstdint>
#include <string>
#include <string_view>

namespace khu::vm::format {

// --- rendering -------------------------------------------------------------

std::string format_int(std::int64_t value);
std::string format_uint(std::uint64_t value);
// `is32` is passed for documentation and future divergence; both widths render
// identically today (see the float rule above).
std::string format_float(double value, bool is32);
std::string format_bool(bool value);

// --- parsing ---------------------------------------------------------------
//
// One grammar, no locale, no partial matches: the whole string has to be a
// number or the parse fails. Whitespace is not skipped -- `" 1"` is not a
// number -- because a parse that silently trims is a parse that silently
// accepts, and Khudra has no exceptions to report the difference with.
//
//   integer  ::= [+-]? digit+
//   float    ::= [+-]? ( digit+ ( '.' digit* )? | '.' digit+ ) ( [eE] [+-]? digit+ )?
//
// Digits are ASCII 0-9 only. An integer parse that would not fit the requested
// width fails rather than wrapping: a value out of range is not the value the
// text named, and Khudra's wrapping rule is about arithmetic, not about
// reading. `parse_int` and `parse_uint` take the width in bits (8, 16, 32, 64).

bool parse_int(std::string_view text, std::uint32_t width, std::int64_t& out);
bool parse_uint(std::string_view text, std::uint32_t width, std::uint64_t& out);
// `is32` rounds the result through `float`, so a parsed `float` is a 32-bit
// value the way an arithmetic result is.
bool parse_float(std::string_view text, bool is32, double& out);

// True when `text` would parse as either an integer or a float.
bool is_numeric(std::string_view text);

// --- bytes -----------------------------------------------------------------
//
// Byte-level character classification, ASCII only. Khudra's `string` is a byte
// string and its index yields `uint8`, so these are defined over bytes rather
// than over code points; a byte above 0x7f is not a digit, a letter or a space.

bool is_digit_byte(std::uint8_t byte);
bool is_letter_byte(std::uint8_t byte);
bool is_whitespace_byte(std::uint8_t byte);
std::uint8_t to_upper_byte(std::uint8_t byte);
std::uint8_t to_lower_byte(std::uint8_t byte);

}  // namespace khu::vm::format

#endif  // KHU_VM_FORMAT_H

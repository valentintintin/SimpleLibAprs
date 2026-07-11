/**
 * @file Aprs.cpp
 * @brief Implementation of the SimpleLibAprs encoder/decoder.
 *
 * Every write to the output buffer goes through the internal Builder helper,
 * which guarantees the destination is never overrun and stays NUL-terminated.
 * The wire formats follow the APRS Protocol Reference 1.0.1 (APRS101).
 */

#include "Aprs.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Platform abstraction
//
// WHY THIS EXISTS: on AVR-based Arduinos (Uno, Nano, ...) RAM is extremely
// scarce (2 KB on an Uno) but the flash program memory is comparatively
// plentiful (32 KB+). The AVR toolchain therefore lets a program keep string
// literals ("APRS", format strings, ...) living permanently in FLASH instead
// of being copied into RAM at startup — that's what "PROGMEM" means. Because
// flash and RAM are two separate address spaces on the classic 8-bit AVR
// (Harvard architecture), you cannot just dereference a flash pointer with an
// ordinary `char*` — you need the special `_P`-suffixed functions
// (`strlen_P`, `strncat_P`, `sscanf_P`, ...) that know how to read through
// flash instead of RAM, and the `PSTR("...")` macro that tells the compiler
// "put this literal in flash, not RAM".
//
// On every OTHER platform this library targets (desktop/native, ESP32,
// STM32, ...) there is no such split: flash and RAM are addressed the same
// way, so PROGMEM is meaningless and the "_P" functions don't exist. This
// block is the single place that papers over the difference:
//   - Compiling with -DNATIVE (used by the `pio test` / desktop build):
//     PSTR(s) simply expands to `s` (the literal itself, stored in RAM like
//     any other C++ string literal) and every "_P" function is `#define`d to
//     its ordinary <string.h>/<stdio.h> counterpart. The rest of the file can
//     then use `PSTR("foo")` and `strlen_P(...)` unconditionally, without a
//     single #ifdef anywhere else — it just happens to be a no-op here.
//   - Compiling for Arduino (AVR, ESP32, ...): <Arduino.h> is pulled in,
//     which supplies the REAL flash-aware `PSTR`/`_P` machinery via
//     <avr/pgmspace.h> (or a compatible shim on non-AVR Arduino cores, where
//     it is often ALSO a harmless passthrough since those chips don't have
//     the Harvard split either — but keeping the same macro names lets one
//     code path serve AVR, ESP32 and STM32 alike).
// ---------------------------------------------------------------------------
#ifdef NATIVE
    #define PSTR(s)       (s)
    #define snprintf_P    snprintf
    #define vsnprintf_P   vsnprintf
    #define sscanf_P      sscanf
    #define strstr_P      strstr
    #define strncat_P     strncat
    #define strncpy_P     strncpy
    #define strlen_P      strlen
#else
    #include <Arduino.h>
    #ifndef sscanf_P
        // Some Arduino cores (e.g. ESP32) don't provide a flash-aware sscanf
        // at all; falling back to the plain one is safe there because those
        // cores don't have a Harvard-style flash/RAM split in the first place.
        #define sscanf_P  sscanf
    #endif
#endif

namespace aprs {
namespace {

/// Conversion factor from miles per hour to knots (APRS compressed speed unit).
constexpr double kMphToKnots = 0.868976;
/// ln(1.08); compressed speed exponent base (speed = 1.08^s - 1).
constexpr double kSpeedLogBase = 0.07696;

/// Cell size (degrees) of each Maidenhead locator field, in decreasing precision order.
constexpr double kGridLonSize[4] = {20.0, 2.0, 2.0 / 24.0, 2.0 / 24.0 / 10.0};
constexpr double kGridLatSize[4] = {10.0, 1.0, 1.0 / 24.0, 1.0 / 24.0 / 10.0};
/// Number of cells per Maidenhead pair (18 field letters A-R, 10 digits, 24 subsquare letters a-x, 10 digits).
constexpr int kGridLonCells[4] = {18, 10, 24, 10};
constexpr int kGridLatCells[4] = {18, 10, 24, 10};

// ---------------------------------------------------------------------------
// Bounded string builder
//
// WHY THIS EXISTS: normally you'd reach for `std::string` or `snprintf` calls
// that grow a buffer as needed. On a microcontroller there is no heap to grow
// into (or worse: there IS a heap, but fragmenting it over years of uptime on
// a tracker will eventually crash the device — "no heap" is a hard design
// rule for this library, see the README). So every encode function is handed
// ONE fixed-size caller-provided buffer up front, and everything below exists
// to append to that buffer through a single choke point that (a) knows how
// much room is left and (b) never writes past the end, no matter how much
// text the caller asked to append. If a piece of text doesn't fit, we simply
// truncate it and raise the `overflow` flag instead of corrupting memory —
// the public `encode()` turns that flag into `Result::BufferTooSmall`.
// ---------------------------------------------------------------------------

/// Accumulates text into a fixed-size buffer without ever overrunning it.
struct Builder {
    char* buf = nullptr;   ///< Destination buffer.
    size_t cap = 0;        ///< Capacity in bytes (including the NUL slot).
    size_t len = 0;        ///< Current string length.
    bool overflow = false; ///< Set once a write had to be truncated.
};

void bInit(Builder& b, char* buf, size_t cap) {
    b.buf = buf;
    b.cap = cap;
    b.len = 0;
    // A zero-capacity buffer can't even hold the NUL terminator, so flag it
    // as already overflowed rather than letting every append silently no-op.
    b.overflow = (cap == 0);
    if (cap > 0) {
        buf[0] = '\0';
    }
}

/// Number of characters that can still be appended (excluding the NUL).
///
/// `cap` counts the NUL slot too, so the usable room is `cap - 1 - len` —
/// but only once `len` has grown large enough that `len + 1 < cap` is still
/// true; otherwise we're already at (or past) capacity and there is zero
/// room left. Writing it as `b.len + 1 < b.cap` rather than `b.cap - b.len
/// - 1 > 0` sidesteps a subtle trap: `cap`, `len` are unsigned (`size_t`), so
/// `cap - len - 1` would silently wrap around to a huge positive number if
/// `len >= cap` instead of going negative — comparing before subtracting
/// avoids that unsigned-underflow footgun entirely.
size_t bRemaining(const Builder& b) {
    return (b.len + 1 < b.cap) ? (b.cap - 1 - b.len) : 0;
}

/// Append a PROGMEM / string literal (no format specifiers).
///
/// `strlen_P`/`strncat_P` are the flash-aware siblings of `strlen`/`strncat`
/// described in the "Platform abstraction" block above — on real AVR
/// hardware, `pstr` lives in flash and an ordinary `strlen(pstr)` would read
/// garbage (or crash), because the CPU has no single unified address space
/// covering both flash and RAM.
void bAppendP(Builder& b, const char* pstr) {
    const size_t rem = bRemaining(b);
    if (strlen_P(pstr) > rem) {
        b.overflow = true;
    }
    if (rem == 0) {
        return;
    }
    // strncat_P always NUL-terminates and never writes more than `rem`
    // characters (plus the NUL) even if `pstr` is longer — this is exactly
    // why `overflow` above is a separate flag rather than relying on the
    // return value: the write itself is already safely truncated, we just
    // additionally want to REMEMBER that truncation happened.
    strncat_P(b.buf, pstr, rem);
    b.len = strlen(b.buf);
}

/// Append a RAM string.
void bAppend(Builder& b, const char* s) {
    const size_t rem = bRemaining(b);
    if (strlen(s) > rem) {
        b.overflow = true;
    }
    if (rem == 0) {
        return;
    }
    strncat(b.buf, s, rem);
    b.len = strlen(b.buf);
}

/// Append a single character.
void bAppendChar(Builder& b, char c) {
    if (bRemaining(b) == 0) {
        b.overflow = true;
        return;
    }
    // No strXXX call needed for a single character: just place it at the
    // current end of the string and re-terminate one byte further — this is
    // the cheapest possible append and is used a lot in the hot encode path
    // (one call per punctuation character in the wire format).
    b.buf[b.len++] = c;
    b.buf[b.len] = '\0';
}

/// Append a RAM string with leading and trailing whitespace removed.
void bAppendTrimmed(Builder& b, const char* s) {
    while (*s != '\0' && isspace((unsigned char) *s)) {
        ++s;
    }
    size_t end = strlen(s);
    while (end > 0 && isspace((unsigned char) s[end - 1])) {
        --end;
    }
    for (size_t i = 0; i < end; ++i) {
        bAppendChar(b, s[i]);
    }
}

/// printf-style append using a PROGMEM format string.
void bFormatP(Builder& b, const char* pfmt, ...) {
    const size_t rem = bRemaining(b);
    if (rem == 0) {
        b.overflow = true;
        return;
    }
    // C's variadic-argument machinery: `va_list`/`va_start`/`va_end` are how
    // a C/C++ function accepts "however many arguments the caller passed"
    // (like printf's own `...`) and hands them to another variadic function
    // (`vsnprintf_P`, the "va_list-taking" twin of `snprintf_P`). `va_start`
    // must name the LAST fixed (non-"...") parameter — here `pfmt` — so it
    // knows where the variable arguments begin on the stack; `va_end` is the
    // mandatory matching cleanup call once we're done reading them.
    va_list args;
    va_start(args, pfmt);
    // `rem + 1` tells vsnprintf_P the buffer is one byte bigger than the
    // remaining ROOM, because `rem` deliberately excludes the NUL slot (see
    // bRemaining) but snprintf-family functions want the FULL buffer size
    // including room for the terminator.
    const int n = vsnprintf_P(b.buf + b.len, rem + 1, pfmt, args);
    va_end(args);
    if (n < 0) {
        // A negative return means an encoding error in the format string
        // itself (not a capacity problem) — treat it the same as overflow
        // since there's nothing sensible left to do with a malformed result.
        b.overflow = true;
        return;
    }
    if ((size_t) n > rem) {
        // vsnprintf's contract: its return value is how many characters it
        // WOULD have written given unlimited space, even if it had to
        // truncate what it actually wrote to fit `rem + 1` bytes. So `n` can
        // be larger than `rem` even though the buffer itself was NOT
        // overrun (vsnprintf_P never writes past the size we gave it) — we
        // only use `n` here to detect and flag that truncation happened.
        b.overflow = true;
    }
    b.len = strlen(b.buf);
}

// ---------------------------------------------------------------------------
// Small string utilities
// ---------------------------------------------------------------------------

/**
 * @brief Copy at most @p maxChars characters of @p src into @p dst, always
 * NUL-terminating.
 *
 * This is the library's one and only "safe strcpy": every RX field
 * (callsigns, path, message text, ...) is filled through this function so
 * that a malformed or hostile incoming frame can never write past the end of
 * a fixed-size struct member, no matter how long the source string is.
 *
 * @p maxChars and @p dstSize serve different purposes and are easy to
 * confuse: @p maxChars is "how many characters of @p src am I semantically
 * allowed to take" (e.g. the length of a substring the caller has already
 * located with strchr/strstr), while @p dstSize is "how many bytes does the
 * destination buffer physically have" (including the NUL). The smaller of
 * the two wins — see the `limit` computation below — so this function is
 * safe to call even if @p maxChars turns out to be huge or wrong.
 */
void copyN(char* dst, const char* src, size_t maxChars, size_t dstSize) {
    if (dstSize == 0) {
        return;
    }
    // Reserve one byte for the terminating NUL: a `dstSize`-byte buffer can
    // hold at most `dstSize - 1` real characters.
    const size_t limit = (maxChars < dstSize - 1) ? maxChars : dstSize - 1;
    size_t i = 0;
    // Also stop early at an embedded NUL in `src` — `strncpy`-style
    // functions would keep going and pad the rest with zeros instead, which
    // is rarely what's wanted here and would waste time on a large buffer.
    for (; i < limit && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/**
 * @brief Shift the string so that it starts at @p from (in-place, overlap-safe).
 *
 * Used to "consume" a prefix that's already been parsed out of a working
 * buffer (see ::decodeMessage), e.g. turning "ack42 rest of message" into
 * "rest of message" after the "ack42" part has been extracted. `memmove` (as
 * opposed to `memcpy`) is required here, not just a defensive choice: @p from
 * points INTO the same buffer as @p s, a few bytes further along, so the
 * source and destination ranges being copied overlap. `memcpy`'s behaviour
 * on overlapping ranges is undefined by the C standard (it may copy forward
 * or backward depending on the platform/optimizer, silently corrupting the
 * string on some of them) — `memmove` is exactly the function the standard
 * library provides for this "copy that might overlap" case, and always
 * produces the correct result regardless of the direction of overlap.
 */
void shiftLeft(char* s, const char* from) {
    memmove(s, from, strlen(from) + 1);
}

/// Strip trailing whitespace in place by walking back from the end and
/// dropping a new NUL terminator over each trailing space/tab/etc. in turn.
void trimEnd(char* s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char) s[n - 1])) {
        s[--n] = '\0';
    }
}

/// Strip leading whitespace in place. Unlike trimEnd (which can just drop a
/// NUL earlier), removing characters from the FRONT of a C string requires
/// physically sliding the remainder left with memmove — there is no way to
/// "move the start pointer" of a fixed `char[]` array, only of a `char*`.
void trimStart(char* s) {
    const size_t n = strlen(s);
    size_t i = 0;
    while (i < n && isspace((unsigned char) s[i])) {
        ++i;
    }
    if (i > 0) {
        // +1 to also carry the terminating NUL along with the shift.
        memmove(s, s + i, n - i + 1);
    }
}

void trim(char* s) {
    trimStart(s);
    trimEnd(s);
}

void trimFirstSpace(char* s) {
    s[strcspn(s, " ")] = '\0';
}

uint8_t countChar(const char* s, size_t n, char target) {
    uint8_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (s[i] == target) {
            ++count;
        }
    }
    return count;
}

// ---------------------------------------------------------------------------
// APRS encoding helpers
// ---------------------------------------------------------------------------

/**
 * @brief Encode @p value as a Base-91 string of @p width characters.
 *
 * APRS's "compressed" formats (position, telemetry-in-position, altitude,
 * course/speed, ...) pack numbers into a small fixed number of ASCII bytes
 * using base 91 instead of base 10 or base 16, because 91 is the largest
 * base whose digits can ALL be represented as a single "safe" printable
 * ASCII character: digit values 0..90 map to the 91 consecutive characters
 * from `!` (ASCII 33) to `{` (ASCII 122), deliberately skipping everything
 * below `!` (control characters, space — space is the AX.25/APRS field
 * separator so it can never appear inside a value) and everything from `|`
 * upward. Adding 33 to a base-91 digit therefore always lands on one of
 * those safe characters — see the `+ 33` below, and `- 33` in the
 * corresponding ::base91Decode.
 *
 * THE TRICKY PART — this function produces its output "backwards" (least
 * significant base-91 digit first) but still has to leave it in the buffer
 * in the correct, human/spec-reading MOST-significant-digit-first order,
 * without knowing in advance how many decimal... er, base-91 digits `value`
 * needs (that's fixed by the caller as `width` instead — every compressed
 * field in APRS has a mandated fixed width, e.g. 4 characters for a
 * latitude). The trick: since we already know the FINAL length (`width`),
 * we can figure out where the string ENDS before we've written anything,
 * place the NUL terminator there once, and then fill the digits by walking
 * the write pointer BACKWARDS from the end towards the start — each
 * `value % 91` gives exactly the digit that belongs at the position we're
 * currently pointing at, and `value /= 91` peels it off for the next
 * (leftward) iteration. This avoids ever needing to reverse a buffer
 * afterwards, and needs no scratch memory beyond `dst` itself.
 *
 * Concretely, for `width == 4`:
 *   1. Loop init `dst += width, *dst = '\0'` (comma operator: do both, in
 *      order) moves `dst` past the last character slot and drops the NUL
 *      terminator there — e.g. for a 4-char field, `dst` now points at
 *      index 4 (one past the last valid index, 3).
 *   2. Each iteration's BODY runs `*(--dst) = ...` — decrement `dst` first
 *      (so it now points at the next slot working backwards, e.g. index 3,
 *      then 2, then 1, then 0), THEN write the least-significant remaining
 *      base-91 digit of `value` there, then divide `value` by 91 to drop
 *      that digit.
 *   3. The loop's own decrement (`--width`, run automatically after each
 *      body) counts down from `width` to 0, so the loop runs exactly
 *      `width` times — exactly enough to fill every slot from the last back
 *      to the first, and no more.
 *   4. By the time the loop condition (`width`) becomes false, `dst` has
 *      been decremented back down to the ORIGINAL start of the field — so
 *      `return dst;` conveniently returns a pointer to the very first
 *      (most-significant) character of the finished string.
 *
 * If `value` doesn't actually need all `width` digits (its true base-91
 * representation is shorter), the extra high-order digits simply come out
 * as `0 % 91 + 33` = `'!'`, i.e. base-91 zero — exactly the correct
 * zero-padding behaviour for a fixed-width field.
 */
char* base91(char* dst, uint8_t width, uint32_t value) {
    for (dst += width, *dst = '\0'; width; --width) {
        *(--dst) = (char) (value % 91 + 33);
        value /= 91;
    }
    return dst;
}

/**
 * @brief Append @p value with up to 3 decimal digits — integral values with
 * none at all (e.g. "199", not "199.000") — WITHOUT ever going through
 * printf's "%f" conversion.
 *
 * WHY NOT JUST "%.3f"/"%.0f" (which is what this function replaced): AVR's C
 * library, avr-libc, ships THREE interchangeable implementations of
 * `vfprintf` (the engine behind `printf`/`sprintf`/`vsnprintf`/...), chosen
 * at LINK time, not compile time: a minimal one, a "default" one (integers
 * only), and a full floating-point-capable one. The default — the one every
 * AVR sketch gets unless its build explicitly opts into the float variant
 * with linker flags (`-Wl,-u,vfprintf -lprintf_flt -lm`, see the README) —
 * doesn't understand "%f" at all, and silently prints '?' in its place
 * instead of a number. That's a real footgun for a LIBRARY specifically: it
 * has no control over the final application's link step, so any code path
 * relying on "%f" quietly breaks for anyone who doesn't already know to add
 * those flags — this was confirmed happening on real Arduino Uno hardware.
 * Native/ESP32/STM32 builds were never affected (their C libraries implement
 * "%f" unconditionally), which is exactly what made this so easy to miss.
 *
 * The fix: do the decimal formatting ourselves with plain integer
 * arithmetic, which every printf variant (even the most minimal one)
 * supports just fine. The technique is the same "scale up, round to the
 * nearest integer, split back apart" idea used throughout this file (see
 * e.g. ::appendPositionUncompressed's minutes rounding): multiply the
 * magnitude by 1000 and round to the nearest integer, and the low 3 digits
 * of THAT are exactly the 3 decimal places we want, with no floating-point
 * text conversion involved anywhere.
 */
void appendDouble(Builder& b, double value) {
    double integral;
    const bool hasFraction = (modf(value, &integral) != 0.0);

    if (!hasFraction) {
        bFormatP(b, PSTR("%ld"), (long) value);
        return;
    }

    const bool negative = value < 0;
    const double absValue = negative ? -value : value;
    const uint32_t scaled = (uint32_t) (absValue * 1000.0 + 0.5);  // round to nearest thousandth
    const uint32_t whole = scaled / 1000;
    const uint32_t thousandths = scaled % 1000;

    if (negative) {
        bAppendChar(b, '-');
    }
    bFormatP(b, PSTR("%lu.%03lu"), (unsigned long) whole, (unsigned long) thousandths);
}

/**
 * @brief Pack the GPS fix / NMEA source / compression origin into the
 * compression byte ("T" in the "csT" trailer of a compressed position,
 * APRS101 ch.9 table).
 *
 * BIT LAYOUT (spec-mandated, most-significant bit first):
 *   bit 5      : GPS fix age    — 0 = old, 1 = current  (::GpsFix)
 *   bits 4-3   : NMEA source    — 0..3, e.g. GGA/RMC/GLL (::NmeaSource)
 *   bits 2-0   : compression type / origin — which software/TNC produced it
 *                (::Compression)
 * Bits 7-6 are always 0 (reserved by the spec). `|=` combines each field into
 * its own slice of the byte without disturbing the others; `<<` shifts a
 * field's raw bits up to where it belongs (bit 5 for the 1-bit fix flag, bits
 * 4-3 for the 2-bit NMEA source), and the `& 0x01`/`& 0x03`/`& 0x07` masks
 * defensively clip each enum's underlying value to the number of bits it's
 * actually allowed to occupy, in case a caller ever passes a raw value.
 *
 * The caller then adds 33 to this byte before appending it (same "shift into
 * the printable ASCII range" trick as ::base91 — see there for why 33).
 */
uint8_t compressionByte(GpsFix fix, NmeaSource nmea, Compression comp) {
    uint8_t value = 0;
    value |= ((uint8_t) fix & 0x01) << 5;
    value |= ((uint8_t) nmea & 0x03) << 3;
    value |= (uint8_t) comp & 0x07;
    return value;
}

/**
 * @brief Append the compressed course/speed byte pair plus the
 * compression-type byte ("csT", APRS101 ch.9).
 *
 * Course is simply degrees/4 (the spec defines the compressed course value
 * `c` as `course = 4 * c`, i.e. one base-91 "digit" of course covers 4°,
 * giving 0-360° of resolution across the 0-90 legal range of a single
 * base-91 digit — comfortably enough since APRS course is only ever reported
 * to whole degrees anyway).
 *
 * Speed is the interesting one: the spec defines `speed_knots = 1.08^s - 1`
 * for a compressed speed digit `s` (an exponential/logarithmic scale, so
 * that one extra "digit" of resolution near walking speed corresponds to a
 * much smaller absolute change than one extra digit near highway speed — the
 * same idea as decibels or the Richter scale). To go the other way (from a
 * known speed to the digit `s` that encodes it), invert the formula:
 *     s = log_1.08(speed_knots + 1) = ln(speed_knots + 1) / ln(1.08)
 * `kSpeedLogBase` is the precomputed `ln(1.08)` denominator. The numerator
 * is computed as plain `log(1.0 + speedKnots)`. The more numerically careful
 * way to write "ln(1+x)" is the standard library's `log1p(x)`, which stays
 * accurate even when `x` is very close to 0 (where `1.0 + x` rounds so close
 * to 1.0 that `log` of it loses most of its significant digits — a classic
 * "catastrophic cancellation" trap) — but `log1p` doesn't exist at all in
 * avr-libc's `<math.h>`, one of the platforms this library targets. The
 * plain `log(1.0 + x)` form is used instead, unconditionally, because the
 * loss of precision it risks is irrelevant here in practice: the result gets
 * rounded (`+ 0.5` below) down to one of only 91 discrete base-91 digit
 * values, an amount of quantization far coarser than anything `log1p` would
 * have preserved.
 * `+ 0.5` before truncating to `uint32_t` rounds to the nearest digit
 * instead of always rounding down, per the comment below.
 */
void appendCourseSpeedBytes(Builder& b, uint32_t courseDeg, double speedKnots, NmeaSource nmea) {
    char base[2];
    base91(base, 1, courseDeg / 4);
    bAppend(b, base);
    // Round to nearest (spec encodes 36.2 kt as s=47, not 46).
    base91(base, 1, (uint32_t) (log(1.0 + speedKnots) / kSpeedLogBase + 0.5));
    bAppend(b, base);
    bAppendChar(b, (char) (compressionByte(GpsFix::Current, nmea, Compression::Compressed) + 33));
}

/**
 * @brief Append a compressed position field: /YYYYXXXX$csT.
 *
 * When @p weather is non-null the report is a compressed weather report: the
 * symbol is '_' and the cs bytes carry wind direction/speed (APRS101 ch.9/12).
 * Otherwise the cs bytes carry course/speed, or altitude when
 * Position::altitudeInComment is false.
 *
 * THE MAGIC NUMBERS BELOW, EXPLAINED — the APRS101 spec (ch.9) defines the
 * compressed coordinate values as:
 *     Y = floor(380926 * (90 - lat))        (4 base-91 digits, north to south)
 *     X = floor(190463 * (180 + lon))       (4 base-91 digits, west to east)
 * i.e. two integers obtained by scaling the (shifted-to-be-positive)
 * coordinate by a large constant. A straightforward implementation would
 * just multiply by 380926.0 / 190463.0 in floating point. This code instead:
 *   1. Shifts and scales the coordinate by 10,000,000 while it's still a
 *      `double` (`900000000 - lat*1e7` etc.) — this keeps 7 decimal digits
 *      of precision (about 1 cm at the equator) while producing a value that
 *      fits safely in a `uint32_t` once cast, sidestepping any double
 *      -precision rounding surprises from working with degree-sized floats.
 *   2. Multiplies that ×1e7 integer by 380926 / 1e7 ≈ 0.0380926 using ONLY
 *      integer division — `x/26 - x/2710 + x/15384615`. Check the arithmetic:
 *      1/26 ≈ 0.0384615, minus 1/2710 ≈ 0.0003690, plus 1/15384615 ≈ 0.
 *      0000001 — the three terms sum to ≈0.0380926, matching the spec
 *      constant to the precision that matters here. This is a classic
 *      embedded-systems trick for multiplying by an awkward fraction using
 *      nothing but a handful of cheap integer divisions instead of a
 *      floating-point multiply, guaranteeing the exact same integer result
 *      on every platform regardless of that platform's FPU/rounding
 *      behaviour (important for a wire format where decoders elsewhere must
 *      reconstruct the identical coordinate).
 *   3. Longitude reuses the EXACT SAME three-term division chain as latitude
 *      instead of needing its own (different) magic constants for 190463.
 *      This works because 190463 is precisely HALF of 380926 — so
 *      `190463 * (180 + lon)` equals `380926 * (90 + lon/2)`, and the
 *      `/ 2` has already been folded into the longitude expression above
 *      (`900000000 + lon*1e7/2`, i.e. `(90 + lon/2) * 1e7`) before the same
 *      "×0.0380926 via integer division" trick is applied to it.
 */
void appendPositionCompressed(const Position& pos, const Weather* weather, Builder& b) {
    uint32_t latitude = (uint32_t) (900000000 - pos.latitude * 10000000);
    latitude = latitude / 26 - latitude / 2710 + latitude / 15384615;

    uint32_t longitude = (uint32_t) (900000000 + pos.longitude * 10000000 / 2);
    longitude = longitude / 26 - longitude / 2710 + longitude / 15384615;

    char base[5];

    bAppendChar(b, pos.overlay);
    base91(base, 4, latitude);
    bAppend(b, base);
    base91(base, 4, longitude);
    bAppend(b, base);

    if (weather != nullptr) {
        // Compressed weather report: '_' symbol, wind in the cs bytes.
        bAppendChar(b, '_');
        if (weather->useWindDirection && weather->useWindSpeed) {
            appendCourseSpeedBytes(b, weather->windDirectionDegrees,
                                   weather->windSpeedMph * kMphToKnots, NmeaSource::Rmc);
        } else {
            // No wind data: a space in the c byte tells decoders to ignore cs/T.
            bAppendP(b, PSTR("   "));
        }
        return;
    }

    bAppendChar(b, pos.symbol);

    if (pos.altitudeInComment) {
        appendCourseSpeedBytes(b, (uint32_t) pos.courseDeg, pos.speedKnots, NmeaSource::Rmc);

        if (pos.altitudeFeet > 0) {
            long alt = (long) pos.altitudeFeet;
            if (alt > 999999L) {
                alt = 999999L;
            } else if (alt < -99999L) {
                alt = -99999L;
            }
            if (alt < 0) {
                bFormatP(b, PSTR("/A=-%05ld"), -alt);
            } else {
                bFormatP(b, PSTR("/A=%06ld"), alt);
            }
        }
    } else {
        // Compressed altitude in the cs bytes (GGA source flags it).
        //
        // APRS101 defines compressed altitude as `altitude_feet = 1.002^cs`
        // for the 2-digit base-91 value `cs` — another exponential encoding
        // like the compressed-speed one in ::appendCourseSpeedBytes, chosen
        // for the same reason: altitude can range from below sea level to
        // tens of thousands of feet, and an exponential scale lets 2 base-91
        // digits (0..8280) cover that whole range with roughly constant
        // RELATIVE precision instead of wasting resolution on a linear scale.
        // Inverting it to get `cs` from a known altitude means taking a
        // logarithm in base 1.002: `cs = log_1.002(altitude) = ln(altitude)
        // / ln(1.002)`. There's no `log1002()` in <math.h> (nor a log_b for
        // any base other than 2/e/10), so this is the standard
        // change-of-base trick — divide the natural log of the value by the
        // natural log of the desired base — computed directly with `log()`
        // rather than `log1p()` this time because altitude (unlike speed)
        // is never close to zero in a way that would need `log1p`'s
        // extra precision near 1.0.
        base91(base, 2, (uint32_t) (log(pos.altitudeFeet) / log(1.002)));
        bAppend(b, base);
        bAppendChar(b, (char) (compressionByte(GpsFix::Current, NmeaSource::Gga, Compression::Compressed) + 33));
    }
}

/**
 * @brief Blank trailing minute-digits of a coordinate just appended to @p b for
 *        position ambiguity (APRS101 ch.6).
 *
 * @p s points at the first digit (degrees) of the coordinate field, which has
 * the layout "<deg><MM>.<mm><hemisphere>". @p degWidth is 2 for latitude, 3
 * for longitude. Level 1 blanks the last decimal digit, 2 both decimal
 * digits, 3 also the minutes' units digit, 4 also the minutes' tens digit —
 * degrees are never blanked.
 *
 * A worked example makes the index arithmetic concrete. For LATITUDE
 * (`degWidth == 2`), the freshly-written field "4903.50N" is laid out at
 * these buffer offsets, relative to `s`:
 * @code
 *   index:  0  1  2  3  4  5  6  7
 *   char : '4''9''0''3''.''5''0''N'
 *          \__deg_/\min/ . \dec_/ hemi
 * @endcode
 * The four MINUTE digit positions we might need to blank, in the order the
 * spec blanks them (finest precision lost first), are: index 6 (hundredths
 * of a minute), index 5 (tenths), index 3 (units of minutes), index 2 (tens
 * of minutes) — i.e. `degWidth + {4, 3, 1, 0}`. That's exactly the `offsets`
 * table below: `offsets[0] == 4` is blanked at ambiguity level 1,
 * `offsets[1] == 3` additionally at level 2, and so on. The `.` at index 4
 * is deliberately absent from this list — it's never blanked, so "4903.  N"
 * (level 2) still reads as a coordinate, just a less precise one. For
 * LONGITUDE (`degWidth == 3`, one extra leading degree digit), every index
 * above simply shifts right by one, which is exactly what adding `degWidth`
 * to each offset achieves without needing a second, longitude-specific table.
 */
void applyAmbiguity(char* s, int degWidth, uint8_t ambiguity) {
    if (ambiguity == 0) {
        return;
    }
    if (ambiguity > 4) {
        ambiguity = 4;  // APRS101 only defines levels 0-4.
    }
    static const int8_t offsets[4] = {4, 3, 1, 0};
    // Blanking `ambiguity` of the 4 offsets, always starting from offsets[0],
    // is what makes each level a strict superset of the previous one's
    // blanked positions (level 2 blanks everything level 1 did, plus more).
    for (uint8_t i = 0; i < ambiguity; ++i) {
        s[degWidth + offsets[i]] = ' ';
    }
}

/// Append an uncompressed position: DDMM.mmN/DDDMM.mmW + symbol + extensions.
void appendPositionUncompressed(const Position& pos, const Weather* weather, Builder& b) {
    double absLat = pos.latitude < 0 ? -pos.latitude : pos.latitude;
    int latDeg = (int) absLat;
    // The fractional part of a degree, converted to minutes (x60) and then
    // to hundredths of a minute (x100), rounded to the nearest integer via
    // the classic "+0.5 then truncate" trick (there's no `round()`-to-int
    // shortcut used here, just add half a unit before the truncating cast to
    // `int` turns any 0.5-or-above remainder into a round-up).
    int latHund = (int) ((absLat - latDeg) * 6000.0 + 0.5);  // hundredths of a minute
    // ROUNDING CARRY: 6000 hundredths-of-a-minute is exactly 60.00 minutes,
    // i.e. a full degree — this can only happen because of the `+ 0.5`
    // rounding above pushing a value like 59.996 up to 60.00. Left alone,
    // that would print as the nonsensical "60.00" minutes; instead we roll
    // the extra minute over into the degrees field, exactly like carrying a
    // digit when adding decimal numbers by hand.
    if (latHund >= 6000) { latHund -= 6000; ++latDeg; }
    size_t latStart = b.len;
    bFormatP(b, PSTR("%02u%02u.%02u%c"), (unsigned) latDeg, (unsigned) (latHund / 100),
             (unsigned) (latHund % 100), pos.latitude < 0 ? 'S' : 'N');
    if (!b.overflow) {
        applyAmbiguity(b.buf + latStart, 2, pos.ambiguity);
    }

    bAppendChar(b, pos.overlay);

    double absLon = pos.longitude < 0 ? -pos.longitude : pos.longitude;
    int lonDeg = (int) absLon;
    int lonHund = (int) ((absLon - lonDeg) * 6000.0 + 0.5);
    if (lonHund >= 6000) { lonHund -= 6000; ++lonDeg; }  // Same rounding carry as latitude, above.
    size_t lonStart = b.len;
    bFormatP(b, PSTR("%03u%02u.%02u%c"), (unsigned) lonDeg, (unsigned) (lonHund / 100),
             (unsigned) (lonHund % 100), pos.longitude < 0 ? 'W' : 'E');
    if (!b.overflow) {
        applyAmbiguity(b.buf + lonStart, 3, pos.ambiguity);
    }

    if (weather != nullptr) {
        bAppendChar(b, '_');
        if (weather->useWindDirection && weather->useWindSpeed) {
            bFormatP(b, PSTR("%03u/%03u"), (unsigned) (weather->windDirectionDegrees % 360),
                     (unsigned) (weather->windSpeedMph * kMphToKnots + 0.5));
        } else {
            bAppendP(b, PSTR(".../..."));
        }
        return;
    }

    bAppendChar(b, pos.symbol);

    if (pos.courseDeg > 0 || pos.speedKnots > 0) {
        bFormatP(b, PSTR("%03u/%03u"), (unsigned) pos.courseDeg % 360, (unsigned) pos.speedKnots);
    }
    if (pos.altitudeInComment && pos.altitudeFeet > 0) {
        long alt = (long) pos.altitudeFeet;
        if (alt > 999999L) {
            alt = 999999L;
        }
        bFormatP(b, PSTR("/A=%06ld"), alt);
    }
}

/// Dispatch to the compressed or uncompressed position encoder.
void appendPosition(const Position& pos, const Weather* weather, Builder& b) {
    if (pos.compressed) {
        appendPositionCompressed(pos, weather, b);
    } else {
        appendPositionUncompressed(pos, weather, b);
    }
}

/// Append a 7-character position timestamp.
void formatTimestamp(const Timestamp& ts, Builder& b) {
    switch (ts.type) {
        case TimestampType::DhmZulu:
            bFormatP(b, PSTR("%02u%02u%02uz"), (unsigned) ts.day, (unsigned) ts.hour, (unsigned) ts.minute);
            break;
        case TimestampType::DhmLocal:
            bFormatP(b, PSTR("%02u%02u%02u/"), (unsigned) ts.day, (unsigned) ts.hour, (unsigned) ts.minute);
            break;
        case TimestampType::Hms:
            bFormatP(b, PSTR("%02u%02u%02uh"), (unsigned) ts.hour, (unsigned) ts.minute, (unsigned) ts.second);
            break;
        default:
            break;
    }
}

/// Append the position data-type identifier: '=' (no time) or '@' + timestamp.
void appendPositionDti(const Position& pos, Builder& b) {
    if (pos.timestamp.type == TimestampType::None) {
        bAppendP(b, PSTR("="));
    } else {
        bAppendP(b, PSTR("@"));
        formatTimestamp(pos.timestamp, b);
    }
}

void appendComment(const char* comment, Builder& b) {
    bAppendTrimmed(b, comment);
}

/// Append the weather data string. Wind dir/speed are carried by the compressed
/// position's cs bytes, so the data begins at the gust field (APRS101 ch.12).
void appendWeather(const Weather& w, Builder& b) {
    bAppendP(b, PSTR("g"));
    if (w.useGustSpeed) {
        bFormatP(b, PSTR("%03u"), (unsigned) w.gustSpeedMph);
    } else {
        bAppendP(b, PSTR("..."));
    }

    bAppendP(b, PSTR("t"));
    if (w.useTemperature) {
        bFormatP(b, PSTR("%03d"), (int) w.temperatureFahrenheit);
    } else {
        bAppendP(b, PSTR("..."));
    }

    bAppendP(b, PSTR("r"));
    if (w.useRain1Hour) {
        bFormatP(b, PSTR("%03u"), (unsigned) w.rain1HourHundredthsOfAnInch);
    } else {
        bAppendP(b, PSTR("..."));
    }

    bAppendP(b, PSTR("p"));
    if (w.useRain24Hour) {
        bFormatP(b, PSTR("%03u"), (unsigned) w.rain24HourHundredthsOfAnInch);
    } else {
        bAppendP(b, PSTR("..."));
    }

    bAppendP(b, PSTR("P"));
    if (w.useRainSinceMidnight) {
        bFormatP(b, PSTR("%03u"), (unsigned) w.rainSinceMidnightHundredthsOfAnInch);
    } else {
        bAppendP(b, PSTR("..."));
    }

    bAppendP(b, PSTR("h"));
    if (w.useHumidity) {
        // APRS encodes 100% humidity as "00".
        bFormatP(b, PSTR("%02u"), (unsigned) (w.humidity >= 100 ? 0 : w.humidity));
    } else {
        bAppendP(b, PSTR(".."));
    }

    bAppendP(b, PSTR("b"));
    if (w.usePressure) {
        bFormatP(b, PSTR("%05u"), (unsigned) (w.pressure * 10));
    } else {
        bAppendP(b, PSTR("....."));
    }
}

void appendTelemetry(const Packet& packet, PacketType type, Builder& b) {
    const Telemetry& t = packet.telemetry;
    char base[3];

    switch (type) {
        case PacketType::Position: {
            // Compressed (base-91) telemetry, APRS base-91 telemetry addendum.
            //
            // This variant is designed to piggy-back INSIDE a position
            // report's comment field (hence "PacketType::Position" here, not
            // "::Telemetry") to save an entire extra transmission — a
            // tracker can send its GPS fix and its sensor readings in one
            // packet instead of two. Sequence numbers only go up to 8191
            // (13 bits, 0x1FFF) in this compressed form (vs. 0-999 for the
            // plain "T#nnn" format below) simply because 2 base-91 "digits"
            // can only represent 91*91 = 8281 distinct values; anything
            // larger just wraps back to 0 rather than corrupting the field.
            const uint16_t seq = (t.sequenceNumber > 0x1FFF) ? 0 : t.sequenceNumber;

            bAppendP(b, PSTR("|"));
            base91(base, 2, seq);
            bAppend(b, base);

            for (const auto& channel : t.analog) {
                base91(base, 2, (uint16_t) fabs(channel.value));
                bAppend(b, base);
            }

            // Pack the 8 boolean channels into a single bitmask byte before
            // base-91-encoding it: `1 << i` produces a byte with only bit i
            // set (channel i's flag, at its own dedicated position), and
            // `|=` accumulates each channel's bit into `bits` without
            // touching the others — the same "one bit, one flag" idiom used
            // all over embedded C for storing several booleans compactly
            // instead of one byte (or worse, one `int`) per flag.
            uint8_t bits = 0;
            for (uint8_t i = 0; i < kMaxTelemetryBoolean; ++i) {
                if (t.boolean[i].value > 0) {
                    bits |= (uint8_t) (1 << i);
                }
            }
            base91(base, 2, bits);
            bAppend(b, base);

            bAppendP(b, PSTR("|"));
            break;
        }
        case PacketType::Telemetry: {
            // Sequence is a 3-character field (000-999) per APRS101 ch.13.
            const uint16_t seq = t.sequenceNumber % 1000;

            bFormatP(b, PSTR("T#%03u,"), (unsigned) seq);

            for (const auto& channel : t.analog) {
                if (t.legacy) {
                    // Legacy analog values are 3-digit decimals, 000-255 (APRS101 ch.13).
                    bFormatP(b, PSTR("%03u"), (unsigned) (uint8_t) fabs(channel.value));
                } else {
                    appendDouble(b, channel.value);
                }
                bAppendP(b, PSTR(","));
            }

            for (const auto& channel : t.boolean) {
                bFormatP(b, PSTR("%d"), channel.value > 0 ? 1 : 0);
            }
            break;
        }
        case PacketType::TelemetryLabel:
            // Legacy per-field max lengths (APRS101 ch.13): analog 7,6,5,5,4; bits 5,4,3,3,3,2,2,2.
            bFormatP(b, PSTR(":%-9s:PARM.%.7s,%.6s,%.5s,%.5s,%.4s,%.5s,%.4s,%.3s,%.3s,%.3s,%.2s,%.2s,%.2s"),
                     packet.source,
                     t.analog[0].name, t.analog[1].name, t.analog[2].name, t.analog[3].name, t.analog[4].name,
                     t.boolean[0].name, t.boolean[1].name, t.boolean[2].name, t.boolean[3].name,
                     t.boolean[4].name, t.boolean[5].name, t.boolean[6].name, t.boolean[7].name);
            break;
        case PacketType::TelemetryUnit:
            bFormatP(b, PSTR(":%-9s:UNIT.%.7s,%.6s,%.5s,%.5s,%.4s,%.5s,%.4s,%.3s,%.3s,%.3s,%.2s,%.2s,%.2s"),
                     packet.source,
                     t.analog[0].unit, t.analog[1].unit, t.analog[2].unit, t.analog[3].unit, t.analog[4].unit,
                     t.boolean[0].unit, t.boolean[1].unit, t.boolean[2].unit, t.boolean[3].unit,
                     t.boolean[4].unit, t.boolean[5].unit, t.boolean[6].unit, t.boolean[7].unit);
            break;
        case PacketType::TelemetryEquation:
            bFormatP(b, PSTR(":%-9s:EQNS."), packet.source);
            for (uint8_t i = 0; i < kMaxTelemetryAnalog; ++i) {
                TelemetryEquation equation = t.analog[i].equation;
                if (i > 0) {
                    bAppendP(b, PSTR(","));
                }
                if (equation.a == 0 && equation.b == 0) {
                    equation.b = 1;  // Default identity scaling.
                }
                appendDouble(b, equation.a);
                bAppendP(b, PSTR(","));
                appendDouble(b, equation.b);
                bAppendP(b, PSTR(","));
                appendDouble(b, equation.c);
            }
            break;
        case PacketType::TelemetryBitSense:
            bFormatP(b, PSTR(":%-9s:BITS."), packet.source);
            for (const auto& channel : t.boolean) {
                bAppendP(b, channel.bitSense ? PSTR("1") : PSTR("0"));
            }
            if (t.projectName[0] != '\0') {
                bFormatP(b, PSTR(",%.23s"), t.projectName);
            }
            break;
        default:
            break;
    }
}

void appendMessage(const Message& message, Builder& b) {
    bFormatP(b, PSTR(":%-9s:"), message.destination);

    // An ACK/REJ is a packet on its own: ":ADDR     :ackNN" with nothing else.
    if (message.ackToReject[0] != '\0') {
        bFormatP(b, PSTR("rej%s"), message.ackToReject);
        return;
    }
    if (message.ackToConfirm[0] != '\0') {
        bFormatP(b, PSTR("ack%s"), message.ackToConfirm);
        return;
    }

    bAppendTrimmed(b, message.message);

    if (message.ackToAsk[0] != '\0') {
        bFormatP(b, PSTR("{%s"), message.ackToAsk);
    }
}

// ---------------------------------------------------------------------------
// APRS decoding helpers (position / weather / telemetry payloads)
// ---------------------------------------------------------------------------

/**
 * @brief Decode a Base-91 string of @p width characters back to an integer.
 *
 * The mirror image of ::base91: subtracting 33 from each character undoes
 * the "shift into printable ASCII" step, recovering the original 0-90
 * digit. Reassembling the digits (most-significant character first, as they
 * appear left-to-right in the buffer — no backwards trickery needed on this
 * side) is a standard technique called Horner's method: instead of
 * computing `digit[0]*91^(width-1) + digit[1]*91^(width-2) + ... +
 * digit[width-1]*91^0` directly (which needs a separate, growing power of
 * 91 for each digit), you fold it left to right as `((digit[0]) * 91 +
 * digit[1]) * 91 + digit[2]) ...` — every step is just "multiply what I have
 * so far by the base, then add the next digit", which is exactly what the
 * loop body below does. It needs no explicit power-of-91 table and works for
 * any @p width.
 */
uint32_t base91Decode(const char* s, int width) {
    uint32_t value = 0;
    for (int i = 0; i < width; ++i) {
        value = value * 91 + (uint32_t) (uint8_t) (s[i] - 33);
    }
    return value;
}

/// Parse up to @p n leading decimal digits.
int parseDigits(const char* s, int n) {
    int value = 0;
    for (int i = 0; i < n && isdigit((unsigned char) s[i]); ++i) {
        value = value * 10 + (s[i] - '0');
    }
    return value;
}

/// Parse @p n decimal digits, treating a blanked (' ') position-ambiguity
/// digit as 0 while still preserving its place value.
int parseDigitsBlank(const char* s, int n) {
    int value = 0;
    for (int i = 0; i < n; ++i) {
        const char c = s[i];
        if (!isdigit((unsigned char) c) && c != ' ') {
            break;
        }
        value = value * 10 + (c == ' ' ? 0 : c - '0');
    }
    return value;
}

/// Detect the position ambiguity level (APRS101 ch.6) of a coordinate field
/// starting at @p s, with @p degWidth degree digits (2 for latitude, 3 for
/// longitude).
uint8_t detectAmbiguity(const char* s, int degWidth) {
    static const int8_t offsets[4] = {4, 3, 1, 0};
    uint8_t level = 0;
    for (uint8_t i = 0; i < 4; ++i) {
        if (s[degWidth + offsets[i]] != ' ') {
            break;
        }
        ++level;
    }
    return level;
}

/// Parse a 7-character timestamp field (6 digits + indicator).
void parseTimestamp7(const char* t, Timestamp& ts) {
    const char indicator = t[6];
    if (indicator == 'h') {
        ts.type = TimestampType::Hms;
        ts.hour = (uint8_t) parseDigits(t, 2);
        ts.minute = (uint8_t) parseDigits(t + 2, 2);
        ts.second = (uint8_t) parseDigits(t + 4, 2);
    } else {
        ts.type = (indicator == '/') ? TimestampType::DhmLocal : TimestampType::DhmZulu;
        ts.day = (uint8_t) parseDigits(t, 2);
        ts.hour = (uint8_t) parseDigits(t + 2, 2);
        ts.minute = (uint8_t) parseDigits(t + 4, 2);
    }
}

/// Read the data-type identifier and any timestamp; return the position-data start.
const char* parseTimestamp(const char* content, Timestamp& ts) {
    ts = Timestamp{};
    const char dti = content[0];
    if ((dti == '/' || dti == '@') && strlen(content) >= 8) {
        parseTimestamp7(content + 1, ts);
        return content + 8;
    }
    return content + 1;  // '!' or '=' : no timestamp
}

/**
 * @brief Parse a compressed position field. Returns the pointer past the
 * 13-byte field.
 *
 * The inverse of ::appendPositionCompressed's coordinate math: on the way
 * IN, degrees were multiplied by 380926/190463 using an integer-division
 * trick to avoid floating point; on the way OUT there's no such constraint
 * (we're not trying to reproduce a bit-exact wire value, just recover the
 * original coordinate as closely as possible), so a plain floating-point
 * division by the spec constants is both simpler and perfectly adequate.
 * `90.0 -` / `-180.0 +` undo the "shift to a positive range" step from the
 * encoder (`900000000 - lat*1e7` etc.) in the same way.
 *
 * The three "cs" trailer bytes (`pos[10..12]`) are only meaningful if the
 * first of them isn't a literal space — a space there is how an ENCODER
 * signals "no course/speed/altitude data available" (see the "   " written
 * in ::appendPositionCompressed for a weather report with no wind reading).
 * Each byte has 33 subtracted to undo the base-91 ASCII shift (same idea as
 * ::base91Decode, just one raw digit at a time here instead of looping).
 * `t` is the compression-TYPE byte (see ::compressionByte for its bit
 * layout): `(t >> 3) & 0x03` extracts bits 4-3 — shift right by 3 to bring
 * that field down to bit 0, then mask off everything except the 2 bits we
 * want — recovering the NMEA-source field that tells us whether `c`/`s`
 * mean "course/speed" or "altitude" (a GGA source flags altitude, per the
 * encoder's own convention). `c * 91 + s` reassembles the two altitude
 * digits with the same Horner-style combine as ::base91Decode, and
 * `pow(1.002, ...)` / `pow(1.08, s) - 1` invert the exponential encodings
 * documented in ::appendPositionCompressed / ::appendCourseSpeedBytes.
 */
const char* parsePositionCompressed(const char* pos, Position& out) {
    out.compressed = true;
    out.overlay = pos[0];
    out.latitude = 90.0 - base91Decode(pos + 1, 4) / 380926.0;
    out.longitude = -180.0 + base91Decode(pos + 5, 4) / 190463.0;
    out.symbol = pos[9];

    if (pos[10] != ' ') {
        const int c = pos[10] - 33;
        const int s = pos[11] - 33;
        const int t = pos[12] - 33;
        const int nmea = (t >> 3) & 0x03;
        if (nmea == (int) NmeaSource::Gga) {
            out.altitudeFeet = pow(1.002, c * 91 + s);
        } else {
            out.courseDeg = c * 4;
            out.speedKnots = pow(1.08, s) - 1;
        }
    }
    return pos + 13;
}

/// Parse an uncompressed position field. Returns the pointer past it (+ extension).
/**
 * @brief Parse an uncompressed position field ("DDMM.mmH"/"DDDMM.mmH" +
 * symbol, plus an optional course/speed extension). Returns the pointer past
 * it (and past the extension, if one was present).
 *
 * The coordinate itself is degrees-and-minutes (e.g. "4903.50N" means 49
 * degrees, 03.50 MINUTES — not 49.0350 degrees!), so converting it to plain
 * decimal degrees means dividing the minutes part by 60 and adding it to the
 * whole degrees (`latDeg + latMin / 60.0` below). `latMin` itself is
 * reassembled from two 2-digit groups — whole minutes, and hundredths of a
 * minute after the decimal point — via ::parseDigitsBlank rather than plain
 * ::parseDigits specifically so that a position-ambiguity frame (some of
 * those digits replaced with spaces, see ::detectAmbiguity) still parses to
 * a sensible reduced-precision coordinate instead of just stopping at the
 * first blanked space.
 */
const char* parsePositionUncompressed(const char* pos, Position& out) {
    out.compressed = false;

    const int latDeg = parseDigits(pos, 2);
    const double latMin = parseDigitsBlank(pos + 2, 2) + parseDigitsBlank(pos + 5, 2) / 100.0;
    out.latitude = latDeg + latMin / 60.0;
    if (pos[7] == 'S' || pos[7] == 's') {
        out.latitude = -out.latitude;
    }
    // Detected from the latitude field only; a spec-compliant frame blanks
    // the same number of digits in both, so this is the canonical value.
    out.ambiguity = detectAmbiguity(pos, 2);

    out.overlay = pos[8];

    const int lonDeg = parseDigits(pos + 9, 3);
    const double lonMin = parseDigitsBlank(pos + 12, 2) + parseDigitsBlank(pos + 15, 2) / 100.0;
    out.longitude = lonDeg + lonMin / 60.0;
    if (pos[17] == 'W' || pos[17] == 'w') {
        out.longitude = -out.longitude;
    }

    out.symbol = pos[18];

    const char* rest = pos + 19;
    // Optional course/speed extension "ddd/sss".
    if (strlen(rest) >= 7 && isdigit((unsigned char) rest[0]) && isdigit((unsigned char) rest[1]) &&
        isdigit((unsigned char) rest[2]) && rest[3] == '/' && isdigit((unsigned char) rest[4]) &&
        isdigit((unsigned char) rest[5]) && isdigit((unsigned char) rest[6])) {
        out.courseDeg = parseDigits(rest, 3);
        out.speedKnots = parseDigits(rest + 4, 3);
        rest += 7;
    }
    return rest;
}

/// Read exactly @p n decimal digits; returns false if any is not a digit.
bool readFixed(const char* s, int n, int* out) {
    int value = 0;
    for (int i = 0; i < n; ++i) {
        if (!isdigit((unsigned char) s[i])) {
            return false;
        }
        value = value * 10 + (s[i] - '0');
    }
    *out = value;
    return true;
}

/// Parse the weather data string (gust/temp/rain/humidity/pressure) into @p w.
void parseWeatherData(const char* s, Weather& w) {
    const size_t len = strlen(s);
    size_t i = 0;
    while (i < len) {
        const char id = s[i];
        const char* d = s + i + 1;
        const size_t avail = len - i - 1;
        int v = 0;
        switch (id) {
            case 'g':
                if (avail >= 3 && readFixed(d, 3, &v)) { w.gustSpeedMph = (uint16_t) v; w.useGustSpeed = true; }
                i += 4;
                break;
            case 't':
                if (avail >= 3 && d[0] == '-' && readFixed(d + 1, 2, &v)) {
                    w.temperatureFahrenheit = (int16_t) -v; w.useTemperature = true;
                } else if (avail >= 3 && readFixed(d, 3, &v)) {
                    w.temperatureFahrenheit = (int16_t) v; w.useTemperature = true;
                }
                i += 4;
                break;
            case 'r':
                if (avail >= 3 && readFixed(d, 3, &v)) { w.rain1HourHundredthsOfAnInch = (uint16_t) v; w.useRain1Hour = true; }
                i += 4;
                break;
            case 'p':
                if (avail >= 3 && readFixed(d, 3, &v)) { w.rain24HourHundredthsOfAnInch = (uint16_t) v; w.useRain24Hour = true; }
                i += 4;
                break;
            case 'P':
                if (avail >= 3 && readFixed(d, 3, &v)) { w.rainSinceMidnightHundredthsOfAnInch = (uint16_t) v; w.useRainSinceMidnight = true; }
                i += 4;
                break;
            case 'h':
                if (avail >= 2 && readFixed(d, 2, &v)) { w.humidity = (uint8_t) (v == 0 ? 100 : v); w.useHumidity = true; }
                i += 3;
                break;
            case 'b':
                if (avail >= 5 && readFixed(d, 5, &v)) { w.pressure = (uint16_t) (v / 10); w.usePressure = true; }
                i += 6;
                break;
            default:
                i += 1;  // skip unknown field (software type, units, luminosity, …)
                break;
        }
    }
}

/// Split @p s on ',' into @p fields (each up to 23 chars). Returns the field count.
int splitCsv(const char* s, char fields[][24], int maxFields) {
    int count = 0;
    while (count < maxFields) {
        const char* comma = strchr(s, ',');
        const size_t len = comma ? (size_t) (comma - s) : strlen(s);
        copyN(fields[count], s, len, 24);
        ++count;
        if (!comma) {
            break;
        }
        s = comma + 1;
    }
    return count;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

Result encode(const Packet& packet, char* out, size_t outSize, size_t* written) {
    if (written != nullptr) {
        *written = 0;
    }
    if (out == nullptr || outSize == 0) {
        return Result::InvalidArgument;
    }
    out[0] = '\0';
    if (packet.source[0] == '\0' || packet.destination[0] == '\0') {
        return Result::InvalidArgument;
    }

    Builder b;
    bInit(b, out, outSize);

    bFormatP(b, PSTR("%s>%s"), packet.source, packet.destination);
    if (packet.path[0] != '\0') {
        bFormatP(b, PSTR(",%s"), packet.path);
    }
    bAppendP(b, PSTR(":"));

    switch (packet.type) {
        case PacketType::Position:
            appendPositionDti(packet.position, b);
            if (packet.position.withWeather) {
                appendPosition(packet.position, &packet.weather, b);
                appendWeather(packet.weather, b);
            } else {
                appendPosition(packet.position, nullptr, b);
                if (packet.position.withTelemetry && !packet.telemetry.legacy) {
                    appendTelemetry(packet, PacketType::Position, b);
                }
            }
            appendComment(packet.comment, b);
            break;
        case PacketType::Weather:
            appendPositionDti(packet.position, b);
            appendPosition(packet.position, &packet.weather, b);
            appendWeather(packet.weather, b);
            appendComment(packet.comment, b);
            break;
        case PacketType::Message:
            appendMessage(packet.message, b);
            break;
        case PacketType::Telemetry:
        case PacketType::TelemetryLabel:
        case PacketType::TelemetryUnit:
        case PacketType::TelemetryEquation:
        case PacketType::TelemetryBitSense:
            appendTelemetry(packet, packet.type, b);
            if (packet.type == PacketType::Telemetry) {
                appendComment(packet.comment, b);
            }
            break;
        case PacketType::Status:
            bAppendP(b, PSTR(">"));
            appendComment(packet.comment, b);
            break;
        case PacketType::Object:
            // Name padded to 9 chars; HMS timestamp uses the 'h' (zulu) suffix.
            bFormatP(b, PSTR(";%-9s%c%02u%02u%02uh"),
                     packet.item.name, packet.item.active ? '*' : '_',
                     (unsigned) packet.item.utcHour, (unsigned) packet.item.utcMinute,
                     (unsigned) packet.item.utcSecond);
            appendPosition(packet.position, nullptr, b);
            appendComment(packet.comment, b);
            break;
        case PacketType::Item:
            bFormatP(b, PSTR(")%s%c"), packet.item.name, packet.item.active ? '!' : '_');
            appendPosition(packet.position, nullptr, b);
            appendComment(packet.comment, b);
            break;
        case PacketType::Raw:
            bAppend(b, packet.content);
            break;
        default:
            return Result::UnsupportedType;
    }

    trim(out);
    if (written != nullptr) {
        *written = strlen(out);
    }
    return b.overflow ? Result::BufferTooSmall : Result::Ok;
}

namespace {

/**
 * @brief Refine a position-family DTI ('=', '!', '/', '@') to Weather when
 * the embedded symbol code is '_' (APRS101 ch.12: a compressed or
 * uncompressed position report whose symbol marks it as a weather station).
 *
 * Weather doesn't get its own DTI in the spec — a weather report IS a
 * position report (same "!"/"="/"/"/"@" byte), just one whose SYMBOL happens
 * to be the weather-station glyph '_'. So telling the two apart means
 * skipping PAST the position data to find the symbol byte, whose offset
 * differs depending on THREE things stacked on top of each other:
 *   - whether there's a timestamp (`/` and `@` carry one, 7 more characters
 *     than `!`/`=` — hence `content + 8` vs `content + 1` below, to land
 *     right after the DTI and, if present, the timestamp);
 *   - whether the position itself is compressed (13 bytes: overlay + 8
 *     base-91 coordinate digits + symbol + 3 "csT" bytes) or uncompressed
 *     (19 bytes: "DDMM.mmH/DDDMM.mmH" + symbol) — `isdigit` on the first
 *     byte tells them apart, since a compressed field always starts with
 *     the overlay character (a symbol-table selector, never a digit) while
 *     an uncompressed one always starts with the tens-of-degrees digit;
 *   - and only then, the symbol byte itself sits at a fixed offset within
 *     whichever of those two layouts applies (index 9 or 18).
 * `strlen(pos) >= needed` is a bounds check guarding that offset access —
 * without it, a short/truncated frame could make `pos[18]` (or `pos[9]`)
 * read past the end of the string.
 */
PacketType refinePositionOrWeather(const char* content) {
    const char first = content[0];
    const char* pos = (first == '/' || first == '@') ? content + 8 : content + 1;
    const bool uncompressed = isdigit((unsigned char) pos[0]);
    const size_t needed = uncompressed ? 19 : 13;
    if (strlen(pos) >= needed && (uncompressed ? pos[18] : pos[9]) == '_') {
        return PacketType::Weather;
    }
    return PacketType::Position;
}

/// Refine a ':'-prefixed (message-envelope) payload: a plain message, a
/// directed query (":ADDRESSEE:?type?..."), or telemetry metadata
/// (PARM/UNIT/EQNS/BITS, APRS101 ch.13).
PacketType refineMessageFamily(const char* content) {
    const char* addrColon = strchr(content + 1, ':');
    const char* payload = (addrColon != nullptr) ? addrColon + 1 : nullptr;
    // Telemetry metadata/data keywords are anchored right after the
    // addressee (e.g. ":N0CALL-9 :PARM.Battery,..."), never just anywhere in
    // a message body — otherwise an ordinary message merely containing the
    // word "UNIT", "HABITS", or the digraph "T#" would be misclassified.
    if (payload != nullptr) {
        if (payload[0] == '?') return PacketType::Query;
        if (strstr_P(payload, PSTR("BITS.")) == payload) return PacketType::TelemetryBitSense;
        if (strstr_P(payload, PSTR("EQNS.")) == payload) return PacketType::TelemetryEquation;
        if (strstr_P(payload, PSTR("PARM.")) == payload) return PacketType::TelemetryLabel;
        if (strstr_P(payload, PSTR("UNIT.")) == payload) return PacketType::TelemetryUnit;
        if (strstr_P(payload, PSTR("T#")) == payload) return PacketType::Telemetry;  // addressed telemetry data report
    }
    return PacketType::Message;
}

/**
 * @brief Classify a payload by its Data Type Identifier (first byte), per
 * APRS101's DTI table (chapter 5, "APRS Data Formats").
 *
 * The very first byte of an APRS payload is a reserved character that
 * announces what kind of packet follows — the spec's Data Type Identifier
 * (DTI). This function is a direct transliteration of that table:
 * @code
 *   ! = / @   position (with or without timestamp, see ::parseTimestamp)
 *   _         weather (positionless)
 *   >         status
 *   ;         object
 *   )         item
 *   ?         query (general/broadcast form)
 *   :         message envelope — ALSO used for directed queries and for the
 *             PARM/UNIT/EQNS/BITS telemetry-metadata frames, all of which
 *             are addressed "messages" at the DTI level and only
 *             distinguishable by what follows the addressee — see
 *             ::refineMessageFamily
 *   T#        bare telemetry data report (not a single-byte DTI: 'T' is
 *             only meaningful together with a following '#')
 * @endcode
 * A `switch` on a single `char` compiles to a tight jump table on virtually
 * every target this library runs on, and — unlike the long `if (first ==
 * 'x') ... else if (first == 'y') ...` chain this replaced — reads as a
 * direct transcription of the spec table above, one line per DTI, with the
 * two overloaded/ambiguous cases (position-vs-weather, and the whole ':'
 * message family) clearly called out as needing a second look via the
 * refine* helpers rather than being just another `case`.
 */
PacketType classifyByDti(const char* content) {
    switch (content[0]) {
        case '=': case '!': case '/': case '@':
            return refinePositionOrWeather(content);
        case '_':
            return PacketType::Weather;
        case '>':
            return PacketType::Status;
        case ';':
            return PacketType::Object;
        case ')':
            return PacketType::Item;
        case '?':
            return PacketType::Query;  // general (unaddressed) query
        case ':':
            return refineMessageFamily(content);
        case 'T':
            return (content[1] == '#') ? PacketType::Telemetry : PacketType::Unknown;
        default:
            return PacketType::Unknown;
    }
}

/**
 * @brief Parse the header ("SRC>DST,PATH:") of @p text and classify its
 * payload type, without third-party unwrapping or digipeater bookkeeping.
 *
 * @p content is set to a pointer INTO @p text (never copied) — @p text must
 * outlive it. Shared by ::decode for the outer frame (where @p text is the
 * caller's own @c PacketLite::raw) and, without any extra buffer, for a
 * wrapped third-party payload (itself a suffix of that same @c raw).
 *
 * @return false if even the source/destination header could not be parsed;
 *         @p source/@p destination/@p path may hold partial data in that
 *         case and must be ignored by the caller.
 */
bool parseEnvelope(const char* text, char* source, char* destination, char* path,
                   const char** content, PacketType* type) {
    *type = PacketType::Unknown;
    *content = text + strlen(text);  // default: empty, if there is no ':'
    path[0] = '\0';

    // Parsed explicitly (rather than via two alternative sscanf attempts) so
    // a comma appearing in the PAYLOAD — e.g. a bare "T#005,199,..." telemetry
    // report with no digipeater path — can never be mistaken for the path
    // separator: sscanf's "%[^,]" has no notion of ':' terminating the
    // header, so it would happily scan straight through the header's ':'
    // into the payload looking for a comma, corrupting destination/path.
    const char* gt = strchr(text, '>');
    if (gt == nullptr) {
        return false;
    }
    copyN(source, text, (size_t) (gt - text), kCallsignLength + 1);

    const char* afterGt = gt + 1;
    const char* colon = strchr(afterGt, ':');
    if (colon == nullptr) {
        return false;
    }
    // A path is present only if a comma occurs strictly BEFORE the
    // header-terminating colon.
    const char* comma = strchr(afterGt, ',');
    const bool hasPath = (comma != nullptr && comma < colon);
    const char* destEnd = hasPath ? comma : colon;
    copyN(destination, afterGt, (size_t) (destEnd - afterGt), kCallsignLength + 1);
    if (hasPath) {
        copyN(path, comma + 1, (size_t) (colon - (comma + 1)), kPathLength + 1);
    }

    *content = colon + 1;
    *type = classifyByDti(*content);
    return true;
}

}  // namespace

bool decode(const char* raw, PacketLite& out) {
    reset(out);

    if (raw == nullptr) {
        return false;
    }
    copyN(out.raw, raw, kMaxPacketLength, sizeof out.raw);
    trim(out.raw);

    if (!parseEnvelope(out.raw, out.source, out.destination, out.path, &out.content, &out.type)) {
        return false;
    }
    out.digipeaterCount = lastDigipeater(out.path, out.lastDigipeaterCallsignInPath,
                                         sizeof out.lastDigipeaterCallsignInPath);

    // Third-party header (APRS101 ch.13): payload wraps a complete inner
    // frame, e.g. an I-Gate re-transmitting a packet gated from APRS-IS onto
    // RF. Unwrap one level so the caller sees the original station's data.
    // The wrapped text is a suffix of out.raw, so the inner content pointer
    // stays valid in out — no second raw-sized buffer needed.
    if (out.content[0] == '}') {
        // Save the OUTER source (the gateway's own callsign) before we
        // overwrite `out.source` with the wrapped station's callsign below —
        // once that happens there'd be no other way to recover who actually
        // relayed this frame to us.
        char gatewayCall[kCallsignLength + 1];
        copyN(gatewayCall, out.source, kCallsignLength, sizeof gatewayCall);

        // Parse the wrapped frame into throwaway local buffers FIRST, rather
        // than directly into `out.source`/`out.destination`/`out.path` — if
        // the "inner" text turns out not to be a valid frame after all
        // (malformed third-party payload), parseEnvelope() can leave these
        // partially written (see its own doc comment) and we must be able to
        // just walk away and keep the OUTER envelope intact. Committing to
        // `out` only happens in the `if` body below, once we know the parse
        // fully succeeded — a small "parse into scratch space, commit on
        // success" pattern that avoids ever leaving `out` half-updated.
        char innerSource[kCallsignLength + 1];
        char innerDestination[kCallsignLength + 1];
        char innerPath[kPathLength + 1];
        const char* innerContent = nullptr;
        PacketType innerType = PacketType::Unknown;

        if (parseEnvelope(out.content + 1, innerSource, innerDestination, innerPath,
                          &innerContent, &innerType)) {
            copyN(out.source, innerSource, kCallsignLength, sizeof out.source);
            copyN(out.destination, innerDestination, kCallsignLength, sizeof out.destination);
            copyN(out.path, innerPath, kPathLength, sizeof out.path);
            // `innerContent` is a pointer computed from `out.content + 1`,
            // which itself points inside `out.raw` — so this assignment is
            // NOT storing a pointer into the local `innerSource`/etc arrays
            // (which are about to go out of scope!), it's storing a pointer
            // that was always inside `out`'s own, long-lived `raw` buffer.
            out.content = innerContent;
            out.type = innerType;
            // Recompute the digipeater bookkeeping too: the ORIGINAL
            // station's path (now in out.path) is what matters for "who
            // relayed this on its way here", not the gateway's own outer
            // AX.25 addressing.
            out.digipeaterCount = lastDigipeater(out.path, out.lastDigipeaterCallsignInPath,
                                                 sizeof out.lastDigipeaterCallsignInPath);
            out.viaThirdParty = true;
            copyN(out.gateway, gatewayCall, kCallsignLength, sizeof out.gateway);
        }
        // If parsing failed, we simply fall through with `out` untouched
        // beyond the initial (still third-party-wrapped) envelope — the
        // caller still gets a valid, if less informative, decode().
    }

    return true;
}

/**
 * @brief Decode a message payload (":ADDRESSEE:body[{ackNN]" or an ACK/REJ
 * frame), APRS101 ch.14.
 *
 * `in.content` is a `const char*` pointing read-only into `in.raw` (see the
 * ::PacketLite::content field doc) — but parsing this format is naturally
 * expressed as a series of "consume a prefix, then work on what's left"
 * steps, which needs a buffer we're allowed to MUTATE. So the very first
 * thing this function does is take a private, mutable working copy (`work`)
 * of the content and do everything else against that instead of `in.content`
 * itself — `in` stays untouched, as a `const PacketLite&` parameter promises
 * its caller.
 *
 * The parsing itself repeats one pattern four times: find/confirm a
 * recognisable prefix, extract what follows it into the right output field,
 * then call ::shiftLeft to drop that consumed prefix and leave `msg` pointing
 * at just the remainder — so each subsequent step only ever has to look at
 * "whatever's left", never re-deriving an offset into the original string.
 *   1. Addressee, up to the second ':' (the first was the DTI already
 *      stripped by the `in.content + 1` below).
 *   2. An "ack42"/"rej42" prefix, if the (remaining) message IS one — these
 *      are how the OTHER station acknowledges/rejects a message WE sent
 *      earlier, so they only make sense as the entire remaining content, not
 *      embedded in a longer message (hence checking `ack == msg`, i.e. it
 *      must match at the very start of what's left, not merely appear
 *      somewhere in the middle of ordinary text).
 *   3. A trailing "{NN" ack-request suffix, if the sender wants US to
 *      acknowledge THIS message — cut off with `*brace = '\0'` rather than
 *      ::shiftLeft since there's nothing useful left after it to preserve.
 *   4. Whatever remains after all of the above is the plain message body.
 */
bool decodeMessage(const PacketLite& in, Message& out) {
    reset(out);

    if (in.content[0] != ':') {
        return false;
    }

    char work[kMessageLength + 1];
    copyN(work, in.content + 1, kMessageLength, sizeof work);
    char* msg = work;

    const char* colon = strchr(msg, ':');
    if (colon != nullptr) {
        copyN(out.destination, msg, (size_t) (colon - msg), sizeof out.destination);
        trimEnd(out.destination);  // Addressee is space-padded to 9 chars on the wire.
        shiftLeft(msg, colon + 1);
    }

    char* ack = strstr_P(msg, PSTR("ack"));
    if (ack == msg) {  // An ACK frame starts with "ack".
        copyN(out.ackConfirmed, ack + 3, kAckMessageLength, sizeof out.ackConfirmed);
        trimFirstSpace(out.ackConfirmed);
        shiftLeft(msg, ack + 3 + strlen(out.ackConfirmed));
    }

    char* rej = strstr_P(msg, PSTR("rej"));
    if (rej == msg) {  // A REJ frame starts with "rej".
        copyN(out.ackRejected, rej + 3, kAckMessageLength, sizeof out.ackRejected);
        trimFirstSpace(out.ackRejected);
        shiftLeft(msg, rej + 3 + strlen(out.ackRejected));
    }

    char* brace = strchr(msg, '{');
    if (brace != nullptr) {
        copyN(out.ackToConfirm, brace + 1, kAckMessageLength, sizeof out.ackToConfirm);
        *brace = '\0';  // Truncate here: everything from '{' on was just the ack-request suffix.
    }

    trim(msg);
    copyN(out.message, msg, kMessageLength, sizeof out.message);

    return true;
}

bool decodePosition(const PacketLite& in, Position& out) {
    out = Position{};
    if (in.type != PacketType::Position && in.type != PacketType::Weather) {
        return false;
    }

    const char* pos = parseTimestamp(in.content, out.timestamp);
    const size_t len = strlen(pos);
    const char* rest;
    if (isdigit((unsigned char) pos[0])) {
        if (len < 19) {
            return false;
        }
        rest = parsePositionUncompressed(pos, out);
    } else {
        if (len < 13) {
            return false;
        }
        rest = parsePositionCompressed(pos, out);
    }

    const char* alt = strstr_P(rest, PSTR("/A="));
    if (alt != nullptr && strlen(alt) >= 9) {
        out.altitudeFeet = parseDigits(alt + 3, 6);
    }
    return true;
}

bool decodeWeather(const PacketLite& in, Weather& out) {
    out = Weather{};
    if (in.type != PacketType::Weather) {
        return false;
    }

    // Positionless weather report: '_' + 8-digit MDHM + cccsss + data.
    if (in.content[0] == '_') {
        const char* s = in.content + 1;
        bool hasMdhm = strlen(s) >= 8;
        for (int i = 0; i < 8 && hasMdhm; ++i) {
            if (!isdigit((unsigned char) s[i])) {
                hasMdhm = false;
            }
        }
        if (hasMdhm) {
            s += 8;
        }
        int v = 0;
        if (s[0] == 'c') {
            if (readFixed(s + 1, 3, &v)) { out.windDirectionDegrees = (uint16_t) v; out.useWindDirection = true; }
            s += 4;
        }
        if (s[0] == 's') {
            if (readFixed(s + 1, 3, &v)) { out.windSpeedMph = (uint16_t) v; out.useWindSpeed = true; }
            s += 4;
        }
        parseWeatherData(s, out);
        return true;
    }

    Timestamp ts;
    const char* pos = parseTimestamp(in.content, ts);
    Position p;
    const size_t len = strlen(pos);
    const char* rest;
    if (isdigit((unsigned char) pos[0])) {
        if (len < 19) {
            return false;
        }
        rest = parsePositionUncompressed(pos, p);
    } else {
        if (len < 13) {
            return false;
        }
        rest = parsePositionCompressed(pos, p);
    }

    // Wind direction/speed are carried by the compressed cs bytes (or ddd/sss).
    if (p.courseDeg > 0) {
        out.windDirectionDegrees = (uint16_t) (p.courseDeg + 0.5);
        out.useWindDirection = true;
    }
    if (p.speedKnots > 0) {
        out.windSpeedMph = (uint16_t) (p.speedKnots / kMphToKnots + 0.5);
        out.useWindSpeed = true;
    }

    parseWeatherData(rest, out);
    return true;
}

bool decodeTelemetry(const PacketLite& in, Telemetry& out) {
    out = Telemetry{};
    char fields[15][24];

    // Compressed (base-91) telemetry rides inside a position report: |ss....|
    // See ::appendTelemetry's PacketType::Position case for how this was
    // built: sequence, then 5 analog channels, then 1 packed bitmask byte,
    // each as a 2-character base-91 field between the two '|' delimiters.
    const char* bar = strchr(in.content, '|');
    if (bar != nullptr && strlen(bar) >= 1 + (2 + 2 * kMaxTelemetryAnalog + 2)) {
        const char* d = bar + 1;
        out.sequenceNumber = (uint16_t) base91Decode(d, 2);
        d += 2;
        for (int i = 0; i < (int) kMaxTelemetryAnalog; ++i) {
            out.analog[i].value = base91Decode(d, 2);
            d += 2;
        }
        // Unpack the bitmask byte back into 8 individual booleans: shifting
        // right by `i` moves channel i's bit down to bit 0, and `& 0x01`
        // isolates just that bit — the inverse of the `bits |= 1 << i`
        // packing done in ::appendTelemetry.
        const uint16_t bits = (uint16_t) base91Decode(d, 2);
        for (int i = 0; i < (int) kMaxTelemetryBoolean; ++i) {
            out.boolean[i].value = (bits >> i) & 0x01;
        }
        return true;
    }

    switch (in.type) {
        case PacketType::Telemetry: {
            const char* t = strstr_P(in.content, PSTR("T#"));
            if (t == nullptr) {
                return false;
            }
            const int n = splitCsv(t + 2, fields, 7);  // sequence + 5 analog + bits
            if (n < 1) {
                return false;
            }
            out.sequenceNumber = (uint16_t) atoi(fields[0]);
            for (int i = 0; i < (int) kMaxTelemetryAnalog && i + 1 < n; ++i) {
                out.analog[i].value = atof(fields[i + 1]);
            }
            if (n >= 7) {
                const char* bits = fields[6];
                for (int i = 0; i < (int) kMaxTelemetryBoolean && bits[i]; ++i) {
                    out.boolean[i].value = (bits[i] == '1') ? 1 : 0;
                }
            }
            return true;
        }
        case PacketType::TelemetryLabel: {
            const char* p = strstr_P(in.content, PSTR("PARM."));
            if (p == nullptr) {
                return false;
            }
            const int n = splitCsv(p + 5, fields, 13);
            for (int i = 0; i < (int) kMaxTelemetryAnalog && i < n; ++i) {
                copyN(out.analog[i].name, fields[i], kTelemetryNameLength, sizeof out.analog[i].name);
            }
            for (int i = 0; i < (int) kMaxTelemetryBoolean && i + 5 < n; ++i) {
                copyN(out.boolean[i].name, fields[i + 5], kTelemetryNameLength, sizeof out.boolean[i].name);
            }
            return true;
        }
        case PacketType::TelemetryUnit: {
            const char* p = strstr_P(in.content, PSTR("UNIT."));
            if (p == nullptr) {
                return false;
            }
            const int n = splitCsv(p + 5, fields, 13);
            for (int i = 0; i < (int) kMaxTelemetryAnalog && i < n; ++i) {
                copyN(out.analog[i].unit, fields[i], kTelemetryUnitLength, sizeof out.analog[i].unit);
            }
            for (int i = 0; i < (int) kMaxTelemetryBoolean && i + 5 < n; ++i) {
                copyN(out.boolean[i].unit, fields[i + 5], kTelemetryUnitLength, sizeof out.boolean[i].unit);
            }
            return true;
        }
        case PacketType::TelemetryEquation: {
            const char* p = strstr_P(in.content, PSTR("EQNS."));
            if (p == nullptr) {
                return false;
            }
            const int n = splitCsv(p + 5, fields, 15);
            for (int i = 0; i < (int) kMaxTelemetryAnalog; ++i) {
                if (3 * i + 2 < n) {
                    out.analog[i].equation.a = atof(fields[3 * i]);
                    out.analog[i].equation.b = atof(fields[3 * i + 1]);
                    out.analog[i].equation.c = atof(fields[3 * i + 2]);
                }
            }
            return true;
        }
        case PacketType::TelemetryBitSense: {
            const char* p = strstr_P(in.content, PSTR("BITS."));
            if (p == nullptr) {
                return false;
            }
            const int n = splitCsv(p + 5, fields, 2);
            const char* bits = fields[0];
            for (int i = 0; i < (int) kMaxTelemetryBoolean && bits[i]; ++i) {
                out.boolean[i].bitSense = (bits[i] == '1');
            }
            if (n >= 2) {
                copyN(out.projectName, fields[1], kTelemetryProjectNameLength, sizeof out.projectName);
            }
            return true;
        }
        default:
            return false;
    }
}

/**
 * @brief Decode an object (PacketType::Object) or item (PacketType::Item).
 *
 * Objects and items are conceptually the same thing (a named point on the
 * map that isn't a station itself — e.g. a meeting point, a repeater, a
 * hazard) but the spec lays their names out differently on the wire, so the
 * two branches below can't share code:
 *   - An OBJECT's name is a FIXED 9 characters, space-padded (hence
 *     `trimEnd` afterwards to drop the padding) — so its end is always at a
 *     known, constant offset, and the live/killed flag ('*'/'_') and 7-byte
 *     HMS timestamp that follow are all at fixed offsets too (`c[10]`,
 *     `c + 11`, position data starting at `c + 18`). The length check up
 *     front (`1 + 9 + 1 + 7 + 1`: the ';' DTI, the 9-char name, the 1-char
 *     flag, the 7-char timestamp, and at least 1 more byte of position data)
 *     guards every one of those fixed-offset reads against a truncated frame.
 *   - An ITEM's name is VARIABLE length (3-9 characters) with no padding,
 *     so there's no fixed offset for where it ends — instead, the format
 *     itself marks the end with a live/killed flag character ('!' or '_')
 *     immediately following the name, which is why this branch has to walk
 *     the string looking for the first one of those instead of just reading
 *     a knowvn offset.
 */
bool decodeObjectItem(const PacketLite& in, ObjectItem& out, Position& position) {
    out = ObjectItem{};
    position = Position{};

    const char* c = in.content;
    const char* posData = nullptr;

    if (in.type == PacketType::Object && c[0] == ';') {
        if (strlen(c) < 1 + 9 + 1 + 7 + 1) {  // ';' + name + flag + timestamp + (some position)
            return false;
        }
        copyN(out.name, c + 1, 9, sizeof out.name);
        trimEnd(out.name);
        out.active = (c[10] == '*');
        parseTimestamp7(c + 11, out.timestamp);
        if (out.timestamp.type == TimestampType::Hms) {
            out.utcHour = out.timestamp.hour;
            out.utcMinute = out.timestamp.minute;
            out.utcSecond = out.timestamp.second;
        }
        posData = c + 18;
    } else if (in.type == PacketType::Item && c[0] == ')') {
        const char* name = c + 1;
        const char* delim = nullptr;
        for (const char* q = name; *q != '\0'; ++q) {
            if (*q == '!' || *q == '_') {
                delim = q;
                break;
            }
        }
        if (delim == nullptr) {
            return false;
        }
        copyN(out.name, name, (size_t) (delim - name), sizeof out.name);
        out.active = (*delim == '!');
        posData = delim + 1;
    } else {
        return false;
    }

    const size_t len = strlen(posData);
    const char* rest;
    if (isdigit((unsigned char) posData[0])) {
        if (len < 19) {
            return false;
        }
        rest = parsePositionUncompressed(posData, position);
    } else {
        if (len < 13) {
            return false;
        }
        rest = parsePositionCompressed(posData, position);
    }

    const char* alt = strstr_P(rest, PSTR("/A="));
    if (alt != nullptr && strlen(alt) >= 9) {
        position.altitudeFeet = parseDigits(alt + 3, 6);
    }
    return true;
}

bool decodeStatus(const PacketLite& in, char* out, size_t outSize) {
    if (outSize == 0) {
        return false;
    }
    out[0] = '\0';
    if (in.type != PacketType::Status || in.content[0] != '>') {
        return false;
    }

    const char* s = in.content + 1;  // skip '>'
    // Optional leading DHM-zulu timestamp: 6 digits + 'z'.
    if (strlen(s) >= 7 && s[6] == 'z') {
        bool digits = true;
        for (int i = 0; i < 6; ++i) {
            if (!isdigit((unsigned char) s[i])) {
                digits = false;
            }
        }
        if (digits) {
            s += 7;
        }
    }

    copyN(out, s, outSize - 1, outSize);
    trim(out);
    return true;
}

bool decodeQuery(const PacketLite& in, Query& out) {
    out = Query{};
    if (in.type != PacketType::Query) {
        return false;
    }

    const char* body = in.content;
    if (body[0] == ':') {
        // Directed query: ":ADDRESSEE:?type?...".
        const char* addrColon = strchr(body + 1, ':');
        if (addrColon == nullptr) {
            return false;
        }
        copyN(out.destination, body + 1, (size_t) (addrColon - (body + 1)), sizeof out.destination);
        trimEnd(out.destination);
        body = addrColon + 1;
    }

    if (body[0] != '?') {
        return false;
    }
    ++body;  // skip the opening '?'

    const char* closing = strchr(body, '?');
    const char* typeEnd = (closing != nullptr) ? closing : body + strlen(body);
    copyN(out.type, body, (size_t) (typeEnd - body), sizeof out.type);

    if (closing != nullptr) {
        copyN(out.argument, closing + 1, kMessageLength, sizeof out.argument);
        trim(out.argument);
    }

    return true;
}

/**
 * @brief Convert a lat/lon pair to a Maidenhead grid locator (e.g. "JN25th").
 *
 * The Maidenhead system carves the whole globe into a nested hierarchy of
 * rectangular cells, encoded as successive PAIRS of characters, each pair
 * zooming into a finer subdivision of the previous cell — like reading
 * decreasing units off a ruler (metres, then centimetres, then millimetres):
 * @code
 *   pair 0 ("field")     : 20°lon x 10°lat cell,  letters A-R  (18 values)
 *   pair 1 ("square")    :  2°lon x  1°lat cell,  digits 0-9   (10 values)
 *   pair 2 ("subsquare") : 1/12°lon x 1/24°lat,    letters a-x  (24 values)
 *   pair 3 ("extended")  : a tenth of a subsquare,  digits 0-9  (10 values)
 * @endcode
 * (kGridLonSize/kGridLatSize hold each pair's cell size in degrees;
 * kGridLonCells/kGridLatCells hold how many cells fit across that pair's
 * axis — 18/10/24/10 above — used only for the edge-case clamp below.)
 *
 * The algorithm is the same at every precision level, run `pairs` times in a
 * loop: shift the coordinate to an always-positive range (0..360°lon,
 * 0..180°lat, via the `+ 180.0` / `+ 90.0` below — Maidenhead has no notion
 * of negative coordinates), figure out which cell of the CURRENT pair's grid
 * we're in (integer-divide by that cell's size), emit the matching
 * letter/digit for that index, then subtract out the whole cells we just
 * consumed so what's LEFT is the position within that cell, ready for the
 * next (finer) pair to subdivide further. This is structurally the same
 * "keep dividing and stripping off the remainder" idea as ::base91 encoding
 * a number digit by digit — just with a different cell size (and a
 * letters-vs-digits alphabet) at each step instead of a constant base.
 */
bool encodeGridLocator(double latitude, double longitude, char* out, size_t outSize, uint8_t pairs) {
    if (out == nullptr) {
        return false;
    }
    if (pairs < 1) {
        pairs = 1;
    } else if (pairs > 4) {
        pairs = 4;
    }
    if (outSize < (size_t) pairs * 2 + 1) {
        return false;
    }
    if (latitude < -90.0 || latitude > 90.0 || longitude < -180.0 || longitude > 180.0) {
        return false;
    }

    double lon = longitude + 180.0;
    double lat = latitude + 90.0;

    for (uint8_t i = 0; i < pairs; ++i) {
        int lonIdx = (int) (lon / kGridLonSize[i]);
        int latIdx = (int) (lat / kGridLatSize[i]);
        // Clamp so the +90/+180 edge case (exactly on the last cell's far
        // boundary) lands in the last cell instead of one past it. This
        // matters because floating-point division can land EXACTLY on a
        // cell boundary for the extreme values 90°/180° (e.g. 180.0/20.0 is
        // exactly 9.0, not 8.999...), which would otherwise compute an index
        // one past the last valid letter/digit for that pair.
        if (lonIdx >= kGridLonCells[i]) { lonIdx = kGridLonCells[i] - 1; }
        if (latIdx >= kGridLatCells[i]) { latIdx = kGridLatCells[i] - 1; }
        // Field pairs (i=0) use letters A-R, square pairs (i=1,3) use digits,
        // subsquare pairs (i=2) use letters a-x. `'A' + lonIdx` is the usual
        // "offset from a known ASCII letter" idiom for turning a small
        // integer into the Nth letter of the alphabet (0 -> 'A', 1 -> 'B',
        // ...), the same trick as `'0' + digit` for turning a 0-9 integer
        // into its printable character.
        char lonChar, latChar;
        if (i == 0) {
            lonChar = (char) ('A' + lonIdx);
            latChar = (char) ('A' + latIdx);
        } else if (i == 2) {
            lonChar = (char) ('a' + lonIdx);
            latChar = (char) ('a' + latIdx);
        } else {
            lonChar = (char) ('0' + lonIdx);
            latChar = (char) ('0' + latIdx);
        }
        out[i * 2] = lonChar;
        out[i * 2 + 1] = latChar;
        // Strip off the whole cells this pair just consumed, leaving only
        // the position WITHIN that cell for the next (finer) pair to encode.
        lon -= lonIdx * kGridLonSize[i];
        lat -= latIdx * kGridLatSize[i];
    }
    out[(size_t) pairs * 2] = '\0';
    return true;
}

// decodeGridLocator() is the exact inverse of ::encodeGridLocator: for each
// pair, turn the letter/digit back into a cell index (subtracting 'A'/'a'/
// '0' instead of adding it), multiply by that pair's cell size to get back
// the ACCUMULATED offset in degrees, and add it in (rather than subtracting,
// since here we're reconstructing the coordinate instead of peeling it
// apart). A locator only pins down a CELL, not an exact point, so the best
// we can report is the centre of the smallest (last) cell resolved — hence
// the `+ kGridLonSize[pairs - 1] / 2.0` (half a cell) added once, after the
// loop, to every accumulated coordinate.
bool decodeGridLocator(const char* locator, double& latitude, double& longitude) {
    if (locator == nullptr) {
        return false;
    }
    const size_t len = strlen(locator);
    if (len < 2 || len > 8 || (len % 2) != 0) {
        return false;
    }
    const uint8_t pairs = (uint8_t) (len / 2);

    double lon = 0.0;
    double lat = 0.0;
    for (uint8_t i = 0; i < pairs; ++i) {
        const char lonC = locator[i * 2];
        const char latC = locator[i * 2 + 1];
        int lonIdx = 0;
        int latIdx = 0;
        if (i == 0) {
            const char lonU = (char) toupper((unsigned char) lonC);
            const char latU = (char) toupper((unsigned char) latC);
            if (lonU < 'A' || lonU > 'R' || latU < 'A' || latU > 'R') {
                return false;
            }
            lonIdx = lonU - 'A';
            latIdx = latU - 'A';
        } else if (i == 2) {
            const char lonL = (char) tolower((unsigned char) lonC);
            const char latL = (char) tolower((unsigned char) latC);
            if (lonL < 'a' || lonL > 'x' || latL < 'a' || latL > 'x') {
                return false;
            }
            lonIdx = lonL - 'a';
            latIdx = latL - 'a';
        } else {
            if (!isdigit((unsigned char) lonC) || !isdigit((unsigned char) latC)) {
                return false;
            }
            lonIdx = lonC - '0';
            latIdx = latC - '0';
        }
        lon += lonIdx * kGridLonSize[i];
        lat += latIdx * kGridLatSize[i];
    }
    // Centre of the smallest resolved cell.
    lon += kGridLonSize[pairs - 1] / 2.0;
    lat += kGridLatSize[pairs - 1] / 2.0;

    longitude = lon - 180.0;
    latitude = lat - 90.0;
    return true;
}

// =============================================================================
// Digipeater routing — "APRS Digipeater Algorithm" (WB2OSZ, APRS Foundation,
// 2024-2025), a.k.a. the "New N-N Paradigm" for WIDEn-N routing.
//
// THE PROBLEM THIS SOLVES: an APRS path (the comma-separated list after the
// destination, e.g. "WIDE1-1,WIDE2-1") tells digipeaters along the way how
// far to relay a frame, without the original sender needing to know in
// advance which specific stations are nearby. But naively "any digipeater
// that sees WIDE2-1 relays it" would cause every frame to be re-relayed
// forever in a busy area with several overlapping digipeaters — infinite
// echo storms. The algorithm below is the industry-standard fix, and it
// hinges on one idea: as EACH digipeater relays a frame, it doesn't just
// blindly retransmit the path — it REWRITES it, replacing the alias it just
// used with its own real callsign and marking that entry "used" (a trailing
// '*'). That turns the path into a running, self-documenting TRACE of every
// station the frame has actually passed through — e.g. after two hops,
// "WIDE2-2" might have become "WIDE2-2" -> "F4ABC-1*,WIDE2-1" (F4ABC-1
// relayed it, one hop of WIDE2 remains) -> "F4ABC-1,F4XYZ-2*" (F4XYZ-2 then
// relayed the LAST hop, replacing the alias entirely). A later digipeater
// only ever looks at the FIRST *unused* entry (the one right after the last
// '*'), so a station that's already relayed (and is now marked used, earlier
// in the path) will never be asked to relay the SAME frame again — that's
// what actually breaks the potential-infinite-loop cycle, together with the
// duplicate-suppression the CALLER is responsible for (§4.2a, see the
// aprs::canBeDigipeated doc comment in Aprs.h and the AprsDigipeat example).
//
// VOCABULARY used throughout this section, matching the PDF's own terms:
//   - "used" hop      : a path entry already carrying a trailing '*' — some
//                       earlier digipeater has already relayed through it.
//   - "unused" hop     : one that doesn't (yet) — a candidate for THIS
//                       digipeater to act on. Only the FIRST unused one is
//                       ever examined (§4.1a) — never "the nearest match
//                       anywhere in the path", which is what old, pre-2024
//                       digipeater firmware sometimes got wrong.
//   - "direct" match   : the unused hop IS literally this station's own
//                       callsign (§4.1b) — used when the sender explicitly
//                       routed the frame through us by name.
//   - "alias" match    : the unused hop is one of a configured list of extra
//                       names we also answer to (§4.1c) — e.g. a legacy
//                       "RELAY" or an EOC (Emergency Operations Center) call.
//   - "generic" match  : the unused hop follows the "XXXn-N" pattern (a
//                       configurable prefix like "WIDE", a single "role"
//                       digit `n`, and a remaining hop-count `N`) — §4.1d,
//                       handled by ::parseGeneric below.
// =============================================================================

namespace {

/// One address parsed from a digipeater path.
struct PathAddr {
    char text[kCallsignLength + 1];  ///< Address without the '*' marker.
    bool used;                       ///< Whether it carried a '*'.
};

/**
 * @brief Split @p path on ',' into @p addrs. Returns the number of addresses
 * parsed.
 *
 * A path like "F4ABC-1*,WIDE2-1" is walked one comma-separated token at a
 * time (the same "find the next comma, or the end of the string" shape used
 * throughout this file, e.g. in ::splitCsv). For each token, a trailing '*'
 * (if present) is stripped off and remembered separately in `addrs[i].used`
 * rather than kept as part of the callsign text — this is what lets the rest
 * of the algorithm treat "is this hop used" as a plain boolean check instead
 * of re-parsing the '*' out of the string every time it's needed.
 */
int parsePath(const char* path, PathAddr* addrs, int maxAddrs) {
    int count = 0;
    const char* p = path;
    while (*p != '\0' && count < maxAddrs) {
        const char* comma = strchr(p, ',');
        size_t len = comma ? (size_t) (comma - p) : strlen(p);
        bool used = false;
        if (len > 0 && p[len - 1] == '*') {
            used = true;
            --len;  // Exclude the '*' itself from the copied callsign text.
        }
        copyN(addrs[count].text, p, len, sizeof addrs[count].text);
        addrs[count].used = used;
        ++count;
        if (!comma) {
            break;  // That was the last address in the path.
        }
        p = comma + 1;
    }
    return count;
}

/**
 * @brief Match @p addr against a generic XXXn-N alias (§4.1d). Returns true
 * and fills the prefix, role digit @p n and remaining hop count @p hops when
 * it matches.
 *
 * A "generic" alias has the shape `<prefix><digit>` optionally followed by
 * `-<digit>` — e.g. "WIDE2-1": prefix "WIDE", role digit `n = 2`, remaining
 * hops `N = 1`. The `n` ("role") digit distinguishes two different generic
 * routing behaviours the PDF defines:
 *   - `n == 1` ("WIDE1-1"): a "fill-in" digipeater alias, meant to relay
 *     exactly ONE hop from a station too weak to reach a wide-area
 *     digipeater directly. `WIDE1-N` with N>=2 is therefore treated as
 *     invalid further down in ::canBeDigipeated (a fill-in relaying more
 *     than once defeats its whole purpose).
 *   - `n >= 2` ("WIDE2-2", "WIDE3-3", ...): ordinary wide-area routing, N
 *     hops remaining, decremented by one each time a digipeater relays it
 *     (§4.3.2a).
 *
 * `rest[1] == '\0'` (nothing after the role digit at all, e.g. bare "WIDE2")
 * is treated as `N == 0` — "used up", per §4.3.2(c): once every hop of a
 * generic alias has been consumed, the alias is dropped from the path
 * entirely rather than lingering with a "-0" suffix, so seeing the bare
 * prefix+digit with no "-N" at all means "there's nothing left here".
 * Anything that doesn't cleanly match "prefix + 1 digit [+ '-' + 1 digit]"
 * (extra characters, a multi-digit SSID, ...) falls through to `continue`
 * and is treated as NOT a generic match — better to reject an ambiguous or
 * malformed alias than guess.
 */
bool parseGeneric(const char* addr, const char* const* prefixes, int prefixCount,
                  char* prefixOut, size_t prefixOutSize, int* n, int* hops) {
    for (int i = 0; i < prefixCount; ++i) {
        const char* prefix = prefixes[i];
        const size_t prefixLen = strlen(prefix);
        if (strncmp(addr, prefix, prefixLen) != 0) {
            continue;  // Doesn't even start with this prefix; try the next one.
        }
        const char* rest = addr + prefixLen;
        if (!isdigit((unsigned char) rest[0])) {
            continue;  // "WIDEFOO" or similar — not a real role digit.
        }
        const int role = rest[0] - '0';
        int remaining;
        if (rest[1] == '\0') {
            remaining = 0;  // "WIDEn" with no SSID == used up.
        } else if (rest[1] == '-' && isdigit((unsigned char) rest[2]) && rest[3] == '\0') {
            remaining = rest[2] - '0';
        } else {
            continue;  // Not a clean XXXn-N.
        }
        copyN(prefixOut, prefix, prefixLen, prefixOutSize);
        *n = role;
        *hops = remaining;
        return true;
    }
    return false;
}

}  // namespace

/**
 * @brief Decide whether (and how) to relay a received frame, per the
 * eligibility test (§4.1) and path rewrite (§4.3) of the "APRS Digipeater
 * Algorithm". See the big block comment above ::PathAddr for the overall
 * picture and vocabulary; this function's own comments walk through each
 * numbered sub-section of the PDF in the order it actually executes them.
 *
 * IMPORTANT — what this function deliberately does NOT do: the PDF's full
 * decision procedure has two more steps this library leaves to the CALLER
 * (documented on the public declaration in Aprs.h), because they need state
 * or timing the library has no business owning:
 *   §4.2(a) Duplicate suppression: drop a frame already relayed in the last
 *           ~28-30 seconds (requires remembering recent frames — a
 *           fixed-size hash ring buffer in the AprsDigipeat example).
 *   §4.2(b) Never digipeat your own transmissions (requires comparing the
 *           frame's SOURCE, not its path, against your own callsign).
 *   §4.4    Once relayed, transmit promptly (no random backoff delay, unlike
 *           an ordinary station) and remember it for future §4.2(a) checks.
 * This function only ever handles §4.1 (should this frame be relayed at
 * all, and through which hop) and §4.3 (if so, how does the path change).
 */
bool canBeDigipeated(char* path, size_t pathSize, const char* myCall,
                     const DigipeaterOptions& options) {
    if (path == nullptr || myCall == nullptr || pathSize == 0) {
        return false;
    }

    // No caller-supplied generic prefixes: fall back to the one the spec
    // itself uses as the universal default, "WIDE". `prefixes` then points
    // at this single local `const char*` — safe even though `defaultPrefix`
    // is a stack variable, because it never outlives this function call (we
    // only ever read through `prefixes` before returning).
    const char* defaultPrefix = "WIDE";
    const char* const* prefixes = options.prefixes;
    int prefixCount = options.prefixCount;
    if (prefixes == nullptr || prefixCount == 0) {
        prefixes = &defaultPrefix;
        prefixCount = 1;
    }

    PathAddr addrs[kMaxPath];
    const int count = parsePath(path, addrs, (int) kMaxPath);
    if (count == 0) {
        return false;  // Empty path: nothing to digipeat through.
    }

    // §4.1(a): "Examine the first unused (not-starred) address." Scanning
    // for the LAST used entry (rather than stopping at the first UNUSED one
    // directly) is a deliberate defensive choice: it tolerates a
    // pathologically malformed path where a used ('*') marker appears AFTER
    // an unused one (which should never happen on a spec-compliant frame,
    // but a corrupted or hand-crafted one could contain it) — by always
    // trusting "one past the LAST star seen so far", we can't be tricked
    // into picking an entry earlier than every used one.
    int firstUnused = 0;
    for (int i = 0; i < count; ++i) {
        if (addrs[i].used) {
            firstUnused = i + 1;
        }
    }
    if (firstUnused >= count) {
        return false;  // Every hop is already used: nothing left to do here.
    }

    const char* addr = addrs[firstUnused].text;

    // `mode` records WHICH of §4.1(b)/(c)/(d) matched, if any — this decides
    // both whether we relay at all (§4.1e: none of them matched -> don't)
    // and, if we do, exactly how the path gets rewritten in §4.3 below (a
    // direct/alias hit is replaced outright; a generic hit may need to keep
    // a decremented remainder — see §4.3.2 further down).
    enum Mode { None, Replace, Generic };
    Mode mode = None;
    char prefix[kCallsignLength + 1] = {};
    int role = 0;
    int hops = 0;

    if (strcmp(addr, myCall) == 0) {
        // §4.1(b): the unused hop names US directly, by our own real
        // callsign — the sender explicitly routed through this station.
        mode = Replace;
    } else {
        // §4.1(c): or one of our configured ALIASES — extra names (e.g. a
        // legacy "RELAY", or a station-specific EOC callsign) this
        // digipeater is also configured to answer to, on top of its real
        // callsign.
        for (int i = 0; i < options.aliasCount; ++i) {
            if (options.aliases[i] != nullptr && strcmp(addr, options.aliases[i]) == 0) {
                mode = Replace;
                break;
            }
        }
        if (mode == None && parseGeneric(addr, prefixes, prefixCount, prefix, sizeof prefix, &role, &hops)) {
            // §4.1(d): or a GENERIC alias (WIDEn-N-shaped) whose prefix we
            // recognise. ::parseGeneric already extracted the role digit
            // and remaining hop count for us; two more checks decide whether
            // this specific generic alias is actually eligible right now:
            if (hops == 0) {
                // §4.3.2(c): "WIDEn" with no hops left (bare, no "-N" at
                // all) means this alias has already been fully consumed by
                // some earlier digipeater and should simply be gone from the
                // path — not relayed again.
                return false;
            }
            if (role == 1 && hops >= 2) {
                // A "fill-in" digipeater alias (role 1, i.e. "WIDE1-N") only
                // ever means "relay exactly one hop" by convention — seeing
                // "WIDE1-2" or higher is a malformed/non-standard path, and
                // per the algorithm a fill-in must NOT relay it.
                return false;
            }
            mode = Generic;
        }
    }
    if (mode == None) {
        // §4.1(e): none of (b)/(c)/(d) matched — this frame is not for us
        // to relay. Note the path is left completely untouched in this
        // case: the caller can safely inspect it afterwards, or try again
        // with different DigipeaterOptions, without needing to restore it.
        return false;
    }

    // §4.3: we ARE relaying — rebuild the path from scratch into a local
    // buffer (never partially mutate the caller's `path` in place, in case
    // something below were to fail; only the final ::copyN commits the
    // result). The general shape mirrors how a real digipeater rewrites the
    // path so it becomes a running trace of who has relayed the frame so
    // far (see the "THE PROBLEM THIS SOLVES" block comment above):
    char buffer[kPathLength + 1];
    Builder b;
    bInit(b, buffer, sizeof buffer);

    // 1. Every hop BEFORE the one we just matched is already used (that's
    //    what made `firstUnused` what it is) and is carried over verbatim,
    //    '*' markers and all — those stations already did their job, their
    //    place in the trace doesn't change.
    for (int i = 0; i < firstUnused; ++i) {
        if (b.len) {
            bAppendChar(b, ',');
        }
        bAppend(b, addrs[i].text);
    }

    // 2. OUR OWN callsign is inserted here, now marked used ('*') — this is
    //    the actual "trace" step: from now on, anyone looking at this path
    //    can see that WE relayed it, replacing whatever alias/direct-match
    //    got us here (§4.1b/c) or standing in as the new last-used hop
    //    ahead of a decremented generic alias (§4.1d, see step 3).
    if (b.len) {
        bAppendChar(b, ',');
    }
    bAppend(b, myCall);
    bAppendChar(b, '*');

    if (mode == Generic && hops >= 2) {
        // §4.3.2(a): "WIDEn-N" with N>=2 remaining keeps the alias in the
        // path (so the NEXT digipeater still knows there's wide-area
        // routing left to do) but with its hop count decremented by one —
        // and, critically, it is NOT marked used ('*'): it's still an
        // available hop for whichever digipeater picks up the frame next.
        // (When hops == 1 instead, this branch is skipped entirely — the
        // alias is fully consumed and simply disappears from the path,
        // replaced outright by our callsign from step 2 above; that's
        // §4.3.2(b), and it needs no extra code here because "don't add
        // anything more" IS the correct behaviour for that case.)
        bAppendChar(b, ',');
        bFormatP(b, PSTR("%s%d-%d"), prefix, role, hops - 1);
    }

    // 3. Every hop AFTER the one we matched is untouched — those are future
    //    routing instructions for later digipeaters, none of our business.
    for (int i = firstUnused + 1; i < count; ++i) {
        bAppendChar(b, ',');
        bAppend(b, addrs[i].text);
    }

    copyN(path, buffer, pathSize - 1, pathSize);
    return true;
}

/**
 * @brief Extract the last station that relayed a frame and count the used
 * hops.
 *
 * Thanks to the path-rewriting rules in ::canBeDigipeated, the LAST '*'
 * marker in the path always belongs to whichever station relayed the frame
 * most recently — this is a direct consequence of the algorithm always
 * rewriting the path to move the trailing '*' forward one hop at a time
 * (§4.3's "keep only the new last hop marked with '*'" — see the digipeater
 * block comment further up for the full picture). So finding "the last
 * digipeater" is just: find the last '*' (`strrchr` searches from the END of
 * the string, exactly what we want here — vs. `strchr` which finds the
 * FIRST occurrence), then take everything between the comma before it (if
 * any — there might be no earlier hop at all) and the '*' itself.
 *
 * The hop COUNT is simply "how many commas appear before that last '*',
 * plus one" — one comma separates the first and second hop, two commas
 * separate three hops, and so on, so N commas always means N+1 addresses.
 */
uint8_t lastDigipeater(const char* path, char* out, size_t outSize) {
    if (out == nullptr || outSize == 0) {
        return 0;
    }
    out[0] = '\0';
    if (path == nullptr) {
        return 0;
    }

    const char* lastStar = strrchr(path, '*');
    if (lastStar == nullptr) {
        return 0;  // No hop has been used yet — nothing has relayed this frame.
    }

    const char* lastComma = strrchr(path, ',');
    if (lastComma != nullptr && lastComma < lastStar) {
        // There's at least one earlier hop before the used one: the
        // callsign we want sits strictly between that last comma and the
        // star (e.g. in "F4ABC-1,F4XYZ-2*", between the comma and the '*').
        copyN(out, lastComma + 1, (size_t) (lastStar - lastComma - 1), outSize);
        return (uint8_t) (countChar(path, (size_t) (lastStar - path), ',') + 1);
    }

    // No comma before the star at all (e.g. "F4ABC-1*" alone, or a star
    // that happens to sit before every comma) — the used hop IS the very
    // first address in the path, running from its start up to the star.
    copyN(out, path, (size_t) (lastStar - path), outSize);
    return 1;
}

/**
 * @brief Copy constructor — delegates to the default constructor, then reuses
 * ::operator= instead of duplicating its logic.
 *
 * WHY THESE EXIST AT ALL: `PacketLite::content` is a `const char*` that
 * points INTO this same object's `raw` array (see the field's doc comment in
 * Aprs.h) rather than owning a separate copy of the payload text — that's
 * what saves ~240 bytes per instance compared to an earlier version of this
 * struct. But a plain, COMPILER-GENERATED copy (the default C++ would give
 * us if we declared nothing here) just copies each member's raw bits,
 * including the `content` POINTER's numeric value — which would leave the
 * copy's `content` still pointing into the ORIGINAL object's `raw` buffer,
 * not its own. That "works" for as long as the original object is still
 * alive and unchanged, and then silently reads garbage (or a dangling
 * pointer, if the original has since been destroyed — e.g. it was a local
 * variable in a function that already returned) the moment it isn't
 * anymore. Any code that stores decoded packets in a buffer or queue for
 * later processing — a very natural thing to do for a RECEIVER — would hit
 * this. Defining a custom copy constructor and assignment operator below is
 * what fixes it, by re-deriving `content`'s pointer relative to the NEW
 * object's own `raw` on every copy.
 *
 * `: PacketLite()` is a "delegating constructor" (C++11): rather than
 * duplicating the "start from a properly zero-initialized object" logic,
 * this constructor first calls the ORDINARY default constructor (the
 * compiler-generated one, from the `= default` in Aprs.h, which runs every
 * member's in-class initializer — `raw[...]{}`, `content = ""`, etc.) to
 * bring `*this` to a known-good empty state, and only THEN calls
 * `operator=` to actually copy `other`'s data over it. Without the
 * delegation, this constructor would have to either repeat every member's
 * default initializer by hand, or risk running `operator=` against
 * whatever indeterminate garbage happened to be in freshly-allocated,
 * never-initialized memory.
 */
PacketLite::PacketLite(const PacketLite& other) : PacketLite() {
    *this = other;
}

PacketLite& PacketLite::operator=(const PacketLite& other) {
    // Guard against self-assignment (`p = p;`, or more realistically `a = b`
    // where `a` and `b` happen to be references to the same object) — without
    // this check we'd read from `other.raw` with `memcpy` while potentially
    // still writing into the very same memory earlier in this function,
    // which is at best redundant and at worst (depending on how the
    // compiler orders the memcpy calls relative to the pointer-offset
    // computation below) could compute a bogus offset from a half-copied
    // buffer.
    if (this == &other) {
        return *this;
    }

    // `content` points into `other.raw` (or, before any ::decode() call, at
    // the static "" literal). Recompute it relative to OUR OWN raw so a copy
    // never ends up aliasing another instance's buffer — otherwise this
    // object would read garbage (or dangle) the moment `other` is reset,
    // reused, or destroyed.
    //
    // A SUBTLE C++ TRAP hiding here: you might expect to just always compute
    // `other.content - other.raw` and reapply that same offset to our own
    // `raw`. But pointer SUBTRACTION (and even ordering comparisons like `<`,
    // `<=`) between two pointers is only well-defined by the C++ standard
    // when both pointers point INTO THE SAME ARRAY (or one-past its end).
    // Before any ::decode() call has ever run on `other`, `other.content`
    // still holds its default value — a pointer to the static string literal
    // `""`, which lives in a completely unrelated piece of memory from
    // `other.raw`. Computing `other.content - other.raw` in that situation
    // would be undefined behaviour: not just "a meaningless number", but a
    // computation the compiler is allowed to assume never happens and
    // optimize around however it likes, potentially in surprising ways. The
    // `contentInRaw` check below exists specifically to avoid ever performing
    // that subtraction (or the `<=` comparison next to it) unless we've first
    // confirmed `content` really is inside `raw`'s bounds — only then is the
    // pointer arithmetic actually well-defined. When it ISN'T (the default
    // "" case), we just copy `other.content`'s pointer value as-is: aliasing
    // a shared, immutable, program-lifetime string literal across any number
    // of copies is perfectly safe, unlike aliasing another instance's `raw`.
    const bool contentInRaw = other.content >= other.raw && other.content <= other.raw + kMaxPacketLength;
    const ptrdiff_t contentOffset = contentInRaw ? (other.content - other.raw) : 0;

    // `memcpy` (not simple `=`) for every fixed-size array member: C arrays
    // aren't assignable with `=` in C++ (unlike, say, `std::array`), so
    // copying their contents always means an explicit byte-for-byte copy.
    memcpy(raw, other.raw, sizeof raw);
    // Re-point `content` using the offset computed above, relative to OUR OWN
    // `raw` (which we just finished filling, right above this line) instead
    // of blindly copying `other.content`'s pointer value.
    content = contentInRaw ? (raw + contentOffset) : other.content;
    memcpy(source, other.source, sizeof source);
    memcpy(destination, other.destination, sizeof destination);
    memcpy(path, other.path, sizeof path);
    memcpy(lastDigipeaterCallsignInPath, other.lastDigipeaterCallsignInPath, sizeof lastDigipeaterCallsignInPath);
    digipeaterCount = other.digipeaterCount;
    viaThirdParty = other.viaThirdParty;
    memcpy(gateway, other.gateway, sizeof gateway);
    type = other.type;

    return *this;
}

void reset(Packet& packet) {
    packet = Packet{};
}

void reset(PacketLite& packet) {
    packet = PacketLite{};
}

void reset(Message& message) {
    message = Message{};
}

}  // namespace aprs

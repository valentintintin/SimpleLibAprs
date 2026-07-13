# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and this project adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Position ambiguity (`Position::ambiguity`, 0-4, APRS101 ch.6): blanks
  trailing minute digits on the uncompressed position format to signal
  reduced precision; detected automatically on decode.
- Maidenhead grid locator conversion: `aprs::encodeGridLocator()` and
  `aprs::decodeGridLocator()`, supporting the 2/4/6/8-character (field /
  square / subsquare / extended square) forms.
- Third-party header decoding (APRS101 ch.13): `::decode()` now unwraps a
  leading `}` payload (e.g. a frame gated from APRS-IS onto RF by an Internet
  Gateway) so the typed decoders see the original station's packet
  transparently. `PacketLite::viaThirdParty` / `PacketLite::gateway` expose
  that a frame was relayed and by whom.
- Query request decoding (APRS101 ch.15): `PacketType::Query` classification
  (general and directed) plus `aprs::decodeQuery()`, extracting the addressee
  (directed queries), the type keyword (e.g. "APRS", "APRSD", "WX") and any
  trailing argument text verbatim. Deciding whether/how to respond is left to
  the caller — no state, no response logic in the library.

### Changed
- **Breaking:** the monolithic `Packet` struct and `aprs::encode(packet, ...)`
  are gone, replaced by one function per payload kind — `encodePosition`,
  `encodeMessage`, `encodeTelemetryData`/`Label`/`Unit`/`Equation`/`BitSense`,
  `encodeObjectItem`, `encodeStatus`, `encodeRaw` — each taking only the source
  callsign, destination, path and the ONE payload struct it actually needs,
  mirroring the `decode()` + typed-decoder pattern already used on the RX
  side. `Packet` bundled a `Position` + `Message` + `Telemetry` + `Weather` +
  `ObjectItem` side by side so every encode call paid for the SUM of all five
  regardless of which one was used: on an ATmega328P Uno (2 KB of RAM total),
  `sizeof(Packet)` was 1244 bytes (61% of all RAM) for a single call, even
  when only encoding a `Position` (30 bytes on its own). `Position::withWeather`
  / `Position::withTelemetry` are also gone — `encodePosition` takes an
  optional `Weather*`/`Telemetry*` instead, so there's no separate flag to
  keep in sync with the pointer. All call sites (examples, tests) have been
  updated; see the README's "Encode a position" section for the new pattern.
- **Breaking:** `PacketLite::content` is now a `const char*` pointing into
  `PacketLite::raw` instead of its own `kMaxPacketLength+1` buffer, cutting
  `sizeof(PacketLite)` from 654 to 416 bytes. All decoders only ever read
  `content`, so this is a drop-in replacement for existing call sites; only
  code that wrote into `content` or relied on `sizeof(content)` is affected.
  `PacketLite` now has a custom copy constructor/assignment operator that
  re-points `content` into the copy's own `raw`, so storing decoded packets
  by value (a buffer/queue of received frames) remains safe — a naive
  pointer-into-`raw` design would otherwise have the copy silently alias the
  original's buffer.
- `::decode()`'s payload classification is now split into focused helpers
  (`classifyByDti`, `refinePositionOrWeather`, `refineMessageFamily`) instead
  of one growing if/else chain. No behaviour change.
- **Breaking:** `Telemetry::analog`/`Telemetry::boolean` are now distinct
  types, `AnalogChannel` and `BooleanChannel`, instead of both sharing
  `TelemetryChannel` (removed). A boolean channel never carried a conversion
  equation or a floating-point value, so it no longer pays for `equation`
  (3 doubles) and a `double value` it never needed — `value` is now a `bool`.
  Field names (`name`, `value`, `unit`, `bitSense`) are unchanged, so typical
  usage (`telemetry.boolean[i].value`, `.name`, ...) is unaffected; only code
  naming `TelemetryChannel` explicitly needs updating.
  `sizeof(Telemetry)`: 968 → 512 bytes; `sizeof(Packet)`: 1896 → 1440 bytes.

### Fixed
- **Telemetry encoding no longer depends on `"%f"`.** Non-legacy analog
  values (`T#`) and EQNS equation coefficients used to be formatted via
  `snprintf(..., "%.3f"/"%.0f", value)`. AVR's default (non-float) `vfprintf`
  doesn't implement `%f` at all and silently prints `?` in its place unless
  the application explicitly links avr-libc's floating-point printf variant
  — a real footgun for a library, which has no control over its caller's
  final link step. Confirmed happening on real Arduino Uno hardware.
  Replaced with `appendDouble()`, a hand-rolled decimal formatter using only
  integer arithmetic (scale by 1000, round, split into whole/thousandths —
  the same technique used elsewhere in this file, e.g. for minutes
  rounding), producing byte-for-byte identical output to the old `%f` calls
  but working on every printf variant, on every platform, unconditionally.
  Confirmed fixed on the same real Uno hardware (the previously-`?` EQNS
  line now prints real numbers). The corresponding README caveat has been
  removed since it no longer applies.
- `::decode()` no longer misclassifies an ordinary message merely containing
  the word "UNIT", "PARM", "PARMESAN", "HABITS", "T#", etc. as telemetry
  metadata/data: the PARM/UNIT/EQNS/BITS/T# keywords are now matched anchored
  right after the addressee (as the wire format actually places them), not as
  a substring search over the whole message body.
- `::decode()` no longer corrupts `PacketLite::destination`/`PacketLite::path`
  for a path-less frame whose payload contains a comma before any other
  colon (e.g. a bare `T#005,199,...` telemetry report, or any DTI followed by
  comma-separated data): the header is now parsed explicitly (`>`, then an
  optional `,`, then the terminating `:`) instead of via two alternative
  `sscanf` patterns, one of which could scan straight through the header's
  `:` into the payload looking for a comma. Present since the original
  1.0.0 release; only surfaced now that an example started printing
  `destination` for such a frame.
- **Real AVR (Arduino Uno) compile failure, present since 1.0.0 and never
  actually verified until now:** `Aprs.h` included `<cstddef>`/`<cstdint>`,
  the C++ wrapper headers — but the AVR toolchain PlatformIO bundles
  (`toolchain-atmelavr`) ships avr-libc (a C library) with no libstdc++ at
  all, so those headers don't exist there and including them was a hard
  compile error on real Uno hardware. Switched to the plain C headers
  (`<stddef.h>`/`<stdint.h>`), which exist everywhere this library targets;
  `size_t`/`uint8_t`/etc. land in the global namespace either way, matching
  how this header already used them (never `std::size_t`).
- Also on AVR: `appendCourseSpeedBytes()` called `log1p()`, which doesn't
  exist in avr-libc's `<math.h>` either. Replaced with plain
  `log(1.0 + speedKnots)` — the precision `log1p` protects near zero is
  irrelevant here since the result is rounded down to one of only 91
  discrete base-91 digit values regardless.
- `env:uno` had never actually been compiled in this repo's history before
  the two fixes above — only `env:esp32dev` (which, being newlib/GCC-based,
  happened to have both `<cstddef>` and `log1p`) had ever been checked, so
  this AVR-only failure went unnoticed. `Aprs.cpp`/`Aprs.h` now compile
  cleanly on real AVR hardware too. Both `env:uno` and `env:esp32dev` still
  fail to *link* into a full firmware (`undefined reference to setup()/
  loop()`) because neither environment points at an actual sketch — see the
  comment in `platformio.ini`; this is a pre-existing project-configuration
  gap, not a library compile error, and is unrelated to the two fixes above.

### Examples
- `AprsEncode` and `AprsDecode` now cover EVERY `PacketType` the library
  supports, one canned frame and one handled branch each: position
  (compressed, uncompressed, timestamped, reduced-precision/ambiguity, with
  embedded telemetry), weather, message (plain, ack-request, ACK reply, REJ
  reply), telemetry (data report + PARM/UNIT/EQNS/BITS definition frames),
  object, item, status, query (general and directed), raw, third-party
  (gated-from-APRS-IS) frames, and the standalone Maidenhead grid locator
  conversion. (`PacketType::Query` has no encoder — see the library's design
  note on why — so it only appears in `AprsDecode`.)
- **`AprsEncode` fixed after testing on real Arduino Uno hardware** (a first
  for this repo — building/uploading it surfaced three more real, previously
  invisible problems):
  - The 13 `aprs::Packet` locals were all flat in `setup()`'s scope with no
    block boundaries between them. On AVR (`sizeof(Packet)` ≈ 1.2 KB there)
    that let the compiler's stack allocation balloon far past the Uno's 2 KB
    of RAM — nothing printed at all. Every block is now wrapped in its own
    `{ }` scope so each `Packet`'s stack space is freed before the next one
    is declared.
  - Every frame's label was a plain string literal (`const char*`), which
    AVR copies into RAM at boot and keeps there permanently; ~20 labels was
    the single largest consumer of this sketch's static RAM (~900 of ~2048
    bytes). Switched to flash-resident labels (`const __FlashStringHelper*`,
    every call site wrapped in `F(...)`), cutting static RAM to ~400 bytes.
  - Printing all 22 frames back-to-back with no pause corrupted characters
    on real hardware (through a CH340 USB-serial adapter) even once the RAM
    issues above were fixed. Added a small `delay(10)` + `Serial.flush()`
    after each frame.
  - After all three fixes, `AprsEncode` runs correctly end-to-end on a real
    Uno — every line matches the native-build output exactly, confirming
    the library itself works correctly on real AVR hardware. One line,
    "Telemetry equations (EQNS)", still prints `?` placeholders instead of
    numbers on real AVR: this is the *already-documented* README caveat
    that AVR's default printf doesn't implement `%f` unless the
    floating-point variant is explicitly linked — not a new bug, just the
    first time it was actually observed on hardware.
- **`AprsDecode` fixed the same way, also confirmed on real Uno hardware:**
  the ~19 canned test frames were plain string literals passed straight to
  `handleFrame(const char*)`, so AVR copied all of them into RAM at boot
  (~1074 of 2048 bytes of static RAM — the single largest consumer). Since
  `::decode()` genuinely needs an ordinary RAM buffer (it isn't flash-aware),
  each frame is now kept in flash and copied into one small shared buffer
  right before the call (`handleFlashFrame()` + `strcpy_P`), rather than
  living in RAM permanently — down to ~320 bytes. The same `delay(10)` +
  `Serial.flush()` fix as `AprsEncode` was applied for reliable back-to-back
  printing. Every line now matches the native-build output exactly on real
  hardware (down to expected float-precision differences, e.g. `-72.75001`
  vs `-72.75000` from AVR's 32-bit `double`) — no `%f` issue on this side,
  since decoding uses `atof()` and printing goes through `Serial.print()`
  (Arduino's own `dtostrf`-based float printing), neither of which touches
  avr-libc's `vfprintf`.
- `AprsDigipeat` was already close to fine on real Uno hardware (36.8%
  static RAM, well within budget — `dedup[]` and `lineBuffer` are legitimate,
  necessary globals, not waste), but its 5 canned demonstration frames got
  the same flash-residency treatment as the other two examples' canned
  data, for consistency (~140 bytes recovered). Tested on real hardware both
  ways: the boot-time canned demo (relay, dedup, own-source rejection,
  already-used-path rejection, alias routing — every line matches the
  native-build output) AND, more importantly, the actual interactive
  `loop()` serial-reading path, by sending fresh frames over the wire live
  and confirming correct relay, duplicate suppression and own-source
  rejection in real time — the first time this project's digipeater example
  has been exercised as a live, running digipeater rather than just a
  one-shot boot demo.

## [1.0.0] - 2026-06-20

First packaged, spec-verified release. This is a substantial rework of the
original library into a publishable form.

### Added
- Modern C++ API in the `aprs::` namespace with scoped `enum class` types,
  `constexpr` limits and Doxygen documentation.
- `aprs::Result` return codes and a fully bounds-checked output builder
  (the destination buffer is never overrun).
- Two-stage decoding: `decode()` parses the envelope (header, routing, type) and
  typed decoders extract a payload on demand — `decodeMessage()`,
  `decodePosition()` (compressed & uncompressed, with timestamp),
  `decodeWeather()` (positioned and positionless), `decodeTelemetry()` (T#,
  PARM/UNIT/EQNS/BITS and the compressed base-91 form), `decodeObjectItem()` and
  `decodeStatus()`.
- Uncompressed position encoding (`Position::compressed = false`) and position
  timestamps (`Position::timestamp`, emitting `@` + `DDHHMMz` / `DDHHMM/` / `HHMMSSh`).
- Spec-accurate digipeater algorithm (`canBeDigipeated`) following the
  *APRS Digipeater Algorithm* (WB2OSZ, 2024-2025): first-unused-address rule,
  `*` marker movement, alias and configurable generic prefixes via
  `DigipeaterOptions`.
- Packaging for Arduino (`library.properties`) and PlatformIO (`library.json`,
  `platformio.ini`).
- Assertion-based test suite including the worked examples from the APRS Protocol
  Reference and the digipeater algorithm document, runnable with `pio test`
  (custom test runner) on the `native` environment.
- `examples/` sketches for encoding and decoding.

### Fixed
- Weather reports now use the spec-compliant compressed form (wind direction/
  speed in the compressed `cs` bytes, weather data starting at the gust field).
  The previous output mixed a compressed position with an uncompressed
  `ddd/sss` block, which was malformed.
- Object timestamps now use the `h` (HMS zulu) suffix instead of `z`, which is
  reserved for the day/hour/minute format.
- ACK/REJ frames are now emitted as standalone packets (`:ADDR     :ackNN`)
  with no trailing space or message body.
- Telemetry sequence number is a 3-digit field (`T#nnn`); legacy analog values
  are zero-padded to 3 digits (`000`-`255`).
- Compressed speed is rounded to nearest (matches the spec worked example).
- Weather rain-since-midnight field used the 1-hour rain value.
- `reset()` now restores every field (weather `use*` flags, routing fields, …).

### Changed
- Digipeater path matching now only considers the first unused address, instead
  of searching the whole path. `WIDE1-N` with N≥2 is treated as invalid (a
  fill-in relays a single hop) and is not digipeated.
- Files reorganised into `src/`, `examples/`, `test/`.

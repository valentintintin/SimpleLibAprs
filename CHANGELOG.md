# Changelog

All notable changes to this project are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and this project adheres
to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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

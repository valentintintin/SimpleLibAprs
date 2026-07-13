# SimpleLibAprs

A lightweight **APRS** (Automatic Packet Reporting System) encoder/decoder for
embedded systems.

SimpleLibAprs builds and parses APRS frames **without any dynamic memory
allocation** — every buffer is caller-provided or fixed-size — which makes it
suitable for microcontrollers with very little RAM (AVR, ESP32, RP2040,
STM32, …). The same code compiles natively on a desktop for testing.

Wire formats follow the *APRS Protocol Reference 1.0.1* and the digipeater
behaviour follows the *APRS Digipeater Algorithm* (WB2OSZ, APRS Foundation,
2024-2025).

## Features

- **Encode**: position reports — **compressed and uncompressed**, with or
  without a **timestamp**, course/speed or altitude, **position ambiguity**
  (APRS101 ch.6) — text messages with ACK/REJ handling, weather reports,
  telemetry (data + `PARM`/`UNIT`/`EQNS`/`BITS` definitions, plus compressed
  base-91 telemetry in a position), objects, items, status and raw frames.
- **Decode**: a lightweight envelope parser (header, routing, packet type) plus
  on-demand typed decoders for **messages, positions** (compressed & uncompressed,
  with timestamp), **weather** (positioned & positionless), **telemetry** (T# data,
  the PARM/UNIT/EQNS/BITS definitions and the compressed base-91 form),
  **objects/items**, **status** and **query requests** (general or directed,
  `?type?argument`). You only allocate the payload you actually need.
  **Third-party headers** (`}`, e.g. a frame gated from APRS-IS onto RF by an
  Internet Gateway) are unwrapped automatically, so the typed decoders see the
  original station's packet transparently.
- **Digipeater routing**: spec-accurate `WIDEn-N` handling, callsign/alias
  matching on the *first unused* address, and correct `*` marker movement.
- **Maidenhead grid locator**: convert a latitude/longitude pair to/from a
  2/4/6/8-character grid locator (`encodeGridLocator`/`decodeGridLocator`).
- No heap, no exceptions, bounded writes (the output buffer is never overrun).

## Installation

### PlatformIO

Add to `platformio.ini`:

```ini
lib_deps = https://github.com/F4HVV/SimpleLibAprs.git
```

### Arduino IDE

Copy this folder into your Arduino `libraries/` directory, then
`#include <Aprs.h>`.

## Quick start

### Encode a position

There is no single "packet" struct bundling every payload kind together —
each `encodeXxx()` function takes only the ONE payload it needs (here a
`Position`), so you only ever pay, in stack RAM, for what you're actually
sending:

```cpp
#include <Aprs.h>

char frame[aprs::kMaxPacketLength + 1];

aprs::Position position;
position.latitude = 45.325776;
position.longitude = 5.636580;
position.symbol = '>';            // car
position.altitudeFeet = 2722;

size_t written = 0;
aprs::Result r = aprs::encodePosition("N0CALL-9", "APRS", "WIDE1-1", position,
                                      nullptr, nullptr, nullptr, frame, sizeof frame, &written);
if (r == aprs::Result::Ok) {
    // frame -> "N0CALL-9>APRS,WIDE1-1:=/..."
}
```

The two `nullptr`s are an optional `Weather*` (pass one to turn this into a
weather report — APRS doesn't have a separate "weather" frame, it's just a
position report with the weather-station symbol) and an optional
`Telemetry*` (piggy-backs compressed telemetry onto the position); the last
one is the optional free-text comment. `encodeMessage`,
`encodeTelemetryData`/`Label`/`Unit`/`Equation`/`BitSense`, `encodeObjectItem`,
`encodeStatus` and `encodeRaw` follow the same pattern for the other payload
kinds — see [Aprs.h](src/Aprs.h) for the full, documented signatures.

### Decode a frame

`decode()` fills the envelope and classifies the type; you then call a typed
decoder for the payload you care about:

```cpp
aprs::PacketLite pkt;
if (aprs::decode("F4HVV-10>APDR16,WIDE1-1,F4HVV-9*::N0CALL-9 :Hi{42", pkt)) {
    // pkt.source, pkt.destination, pkt.path
    // pkt.lastDigipeaterCallsignInPath, pkt.digipeaterCount
    if (pkt.type == aprs::PacketType::Message) {
        aprs::Message msg;
        aprs::decodeMessage(pkt, msg);   // msg.destination, msg.message, msg.ackToConfirm ...
    }
}

// A frame gated from APRS-IS onto RF carries a third-party header ('}');
// it is unwrapped automatically — pkt.source/destination/path/content already
// describe the ORIGINAL station, and pkt.gateway records who relayed it.
if (aprs::decode("F4HVV-10>APRS,TCPIP*:}N0CALL-9>APDR16:=4519.92N/00537.15E>", pkt)) {
    if (pkt.viaThirdParty) {
        // pkt.gateway -> "F4HVV-10", pkt.source -> "N0CALL-9"
    }
}
```

### Digipeat a frame

```cpp
char path[aprs::kPathLength + 1];
strcpy(path, "WIDE2-2");
if (aprs::canBeDigipeated(path, sizeof path, "WB2OSZ")) {
    // path -> "WB2OSZ*,WIDE2-1"
}
```

Aliases and extra generic prefixes are configurable:

```cpp
const char* aliases[]  = {"EOC-1", "TEST"};
const char* prefixes[] = {"WIDE", "MD"};
aprs::DigipeaterOptions opt;
opt.aliases = aliases;   opt.aliasCount = 2;
opt.prefixes = prefixes; opt.prefixCount = 2;
aprs::canBeDigipeated(path, sizeof path, "KB1MKZ", opt);
```

> The caller is responsible for the stateful parts of the digipeater algorithm:
> not repeating frames whose source is your own callsign, and 30-second
> duplicate suppression.

## API overview

| Function | Purpose |
|----------|---------|
| `aprs::encodePosition(source, dest, path, position, weather, telemetry, comment, out, outSize, &written)` | Build a position report (optionally weather and/or piggy-backed telemetry). |
| `aprs::encodeMessage(source, dest, path, message, out, outSize, &written)` | Build a text message, ACK or REJ. |
| `aprs::encodeTelemetryData(source, dest, path, telemetry, comment, out, outSize, &written)` | Build a telemetry data report (`T#`). |
| `aprs::encodeTelemetryLabel`/`Unit`/`Equation`/`BitSense(source, dest, path, telemetry, out, outSize, &written)` | Build a telemetry `PARM`/`UNIT`/`EQNS`/`BITS` metadata message. |
| `aprs::encodeObjectItem(source, dest, path, type, item, position, comment, out, outSize, &written)` | Build an object or item report. |
| `aprs::encodeStatus(source, dest, path, status, out, outSize, &written)` | Build a status report. |
| `aprs::encodeRaw(source, dest, path, content, out, outSize, &written)` | Emit caller-provided content verbatim after the header. |
| `aprs::decode(raw, lite)` | Parse the envelope and classify the type. |
| `aprs::decodeMessage(lite, message)` | Extract a message payload on demand. |
| `aprs::decodePosition(lite, position)` | Decode lat/lon, symbol, course/speed/altitude, timestamp. |
| `aprs::decodeWeather(lite, weather)` | Decode wind and the weather data fields (positioned or positionless). |
| `aprs::decodeTelemetry(lite, telemetry)` | Decode T#, PARM/UNIT/EQNS/BITS or compressed telemetry. |
| `aprs::decodeObjectItem(lite, item, position)` | Decode an object/item: name, live/kill, timestamp, position. |
| `aprs::decodeStatus(lite, out, outSize)` | Decode the status text (timestamp stripped). |
| `aprs::decodeQuery(lite, query)` | Extract a query's addressee (if directed), type keyword and argument. |
| `aprs::canBeDigipeated(path, size, myCall, opt)` | Apply the digipeater path algorithm. |
| `aprs::lastDigipeater(path, out, size)` | Last station that relayed + hop count. |
| `aprs::encodeGridLocator(lat, lon, out, size, pairs)` | Convert lat/lon to a Maidenhead grid locator. |
| `aprs::decodeGridLocator(locator, &lat, &lon)` | Convert a Maidenhead grid locator back to lat/lon. |
| `aprs::reset(...)` | Reset a `PacketLite` / `Message`. |

All sizes and limits are exposed as `constexpr` constants (`aprs::kMaxPacketLength`,
`aprs::kCallsignLength`, `aprs::kPathLength`, …).

## Building & testing

The library is built with PlatformIO. The `native` environment runs the test
suite on the host machine; the hardware environments compile-check the library
and examples on real MCUs:

```bash
pio test -e native      # run the test suite on the host
pio run -e esp32dev     # compile-check on ESP32 (or -e uno)
```

The test suite includes the worked examples from the APRS Protocol Reference and
the APRS Digipeater Algorithm document.

## Notes

- There is no monolithic "packet" struct bundling every payload kind
  together — each `encodeXxx()` function takes only the ONE payload struct it
  needs. This matters on the tightest targets: on an ATmega328P Uno (2 KB of
  RAM total), a `Position` is 30 bytes and a `Telemetry` (5 analog + 8 boolean
  channels) is 358 bytes, versus the 1244 bytes a single struct holding all
  five payload kinds side by side would cost for every encode call regardless
  of which one you actually use.
- Weather reports are emitted in the **compressed** form: wind direction/speed
  are carried in the compressed-position `cs` bytes and the weather data starts
  at the gust field, as required by the spec.
- `decodeQuery()` only extracts the query's fields. Like the digipeater
  algorithm's duplicate suppression, deciding **whether and how to respond**
  (e.g. which heard stations to list for `APRSD`, whether to answer a general
  `APRS` query, rate-limiting) is application logic and stays out of the
  library.

## License

MIT — see [LICENSE](LICENSE).

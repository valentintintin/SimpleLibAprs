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
  without a **timestamp**, course/speed or altitude — text messages with
  ACK/REJ handling, weather reports, telemetry (data + `PARM`/`UNIT`/`EQNS`/`BITS`
  definitions, plus compressed base-91 telemetry in a position), objects, items,
  status and raw frames.
- **Decode**: a lightweight envelope parser (header, routing, packet type) plus
  on-demand typed decoders for **messages, positions** (compressed & uncompressed,
  with timestamp), **weather** (positioned & positionless), **telemetry** (T# data,
  the PARM/UNIT/EQNS/BITS definitions and the compressed base-91 form),
  **objects/items** and **status**. You only allocate the payload you actually need.
- **Digipeater routing**: spec-accurate `WIDEn-N` handling, callsign/alias
  matching on the *first unused* address, and correct `*` marker movement.
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

```cpp
#include <Aprs.h>

char frame[aprs::kMaxPacketLength + 1];

aprs::Packet p;
strcpy(p.source, "N0CALL-9");
strcpy(p.destination, "APRS");
strcpy(p.path, "WIDE1-1");
p.type = aprs::PacketType::Position;
p.position.latitude = 45.325776;
p.position.longitude = 5.636580;
p.position.symbol = '>';            // car
p.position.altitudeFeet = 2722;

size_t written = 0;
if (aprs::encode(p, frame, sizeof frame, &written) == aprs::Result::Ok) {
    // frame -> "N0CALL-9>APRS,WIDE1-1:=/..."
}
```

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
| `aprs::encode(packet, out, outSize, &written)` | Build a frame. Returns `aprs::Result`. |
| `aprs::decode(raw, lite)` | Parse the envelope and classify the type. |
| `aprs::decodeMessage(lite, message)` | Extract a message payload on demand. |
| `aprs::decodePosition(lite, position)` | Decode lat/lon, symbol, course/speed/altitude, timestamp. |
| `aprs::decodeWeather(lite, weather)` | Decode wind and the weather data fields (positioned or positionless). |
| `aprs::decodeTelemetry(lite, telemetry)` | Decode T#, PARM/UNIT/EQNS/BITS or compressed telemetry. |
| `aprs::decodeObjectItem(lite, item, position)` | Decode an object/item: name, live/kill, timestamp, position. |
| `aprs::decodeStatus(lite, out, outSize)` | Decode the status text (timestamp stripped). |
| `aprs::canBeDigipeated(path, size, myCall, opt)` | Apply the digipeater path algorithm. |
| `aprs::lastDigipeater(path, out, size)` | Last station that relayed + hop count. |
| `aprs::reset(...)` | Reset a `Packet` / `PacketLite` / `Message`. |

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

- Floating-point telemetry/equation values use `printf("%f")`. On AVR this
  requires the floating-point `printf` variant to be linked; on ESP32/STM32/native
  it works out of the box.
- Weather reports are emitted in the **compressed** form: wind direction/speed
  are carried in the compressed-position `cs` bytes and the weather data starts
  at the gust field, as required by the spec.

## License

MIT — see [LICENSE](LICENSE).

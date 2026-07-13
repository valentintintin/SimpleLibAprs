/*
 * SimpleLibAprs - Encode example
 *
 * Builds one frame for every APRS payload kind the library can encode, and
 * prints the resulting text to the serial port. Feed these frames to a TNC /
 * radio modem (e.g. via KISS) to actually transmit them.
 *
 * Queries have no encoder (see README: deciding whether/how to respond to a
 * query is application logic, so the library only decodes them) and are
 * therefore not shown here — see the AprsDecode example instead.
 *
 * IMPORTANT (AVR/Uno RAM): there is no single "packet" struct bundling every
 * payload kind together — each `encodeXxx()` function below takes only the
 * ONE payload struct it actually needs (e.g. `encodePosition` takes a
 * `Position`, `encodeMessage` a `Message`), so a block only ever pays for
 * what it declares. `Telemetry` (5 analog + 8 boolean channels, ~512 bytes on
 * AVR) is still by far the biggest single struct in the library, which is why
 * each block below is wrapped in its own { } scope: that local goes out of
 * scope — and its stack space is freed for reuse — before the NEXT block's
 * locals are declared, so at most one Telemetry (or Message, or Position)
 * worth of stack is ever live at a time. An Uno only has 2 KB of RAM total,
 * so flattening every block into one function would keep all of them live
 * simultaneously and reliably overflow the stack, silently corrupting RAM.
 *
 * ALSO on AVR: every label is passed through F(...) (flash-resident) rather
 * than as a plain string literal — a plain literal is copied into RAM at
 * boot and stays there, and ~20 labels of ~30 bytes each was, before this
 * change, the single largest consumer of this sketch's static RAM (confirmed
 * on real hardware: ~900 bytes of RAM dropped to ~400 just from this).
 *
 * The small delay(10) after each frame gives a slow USB-serial adapter time
 * to drain its TX buffer before the next burst — without it, sustained
 * back-to-back printing was observed to corrupt characters on real Uno
 * hardware (through a CH340 USB-serial adapter specifically; your mileage
 * may vary with a different adapter, but the delay is harmless everywhere).
 *
 * One further AVR caveat this example does NOT work around: avr-libc's
 * default printf doesn't implement "%f" unless the floating-point variant is
 * explicitly linked (see the README's "Notes" section) — without it, the
 * "Telemetry equations (EQNS)" line below will print "?" instead of the
 * actual coefficients on real AVR hardware, even though everything else
 * works. Native/ESP32/STM32 builds are unaffected.
 */

#include <Aprs.h>

// `label` is a `__FlashStringHelper*` (i.e. every call site wraps its literal
// in F(...)) rather than a plain `const char*`: an ordinary string literal
// used as `const char*` gets copied into RAM at boot and stays there for the
// program's whole lifetime, while F(...) keeps it in flash and only copies
// each byte out transiently as Serial.print reads it. With ~20 labels of
// ~30 bytes each, that difference is the ~600 bytes that used to be the
// single largest consumer of this sketch's static RAM on AVR.
void printFrame(const __FlashStringHelper* label, aprs::Result result, const char* frame) {
  Serial.print(label);
  if (result == aprs::Result::Ok) {
    Serial.println(frame);
  } else {
    Serial.println(F("(encode failed)"));
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    ;  // wait for the USB serial port (Leonardo/Micro)
  }

  char frame[aprs::kMaxPacketLength + 1];

  // === Position reports ======================================================

  // --- Compressed position, with course/speed/altitude ----------------------
  {
    aprs::Position position;
    position.latitude = 45.325776;
    position.longitude = 5.636580;
    position.symbol = '>';      // car
    position.overlay = '/';
    position.courseDeg = 90;
    position.speedKnots = 25;
    position.altitudeFeet = 2722;
    aprs::Result r = aprs::encodePosition("N0CALL-9", "APRS", "WIDE1-1", position,
                                          nullptr, nullptr, "SimpleLibAprs", frame, sizeof frame);
    printFrame(F("Position (compressed):        "), r, frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // --- Uncompressed position with a timestamp --------------------------------
  {
    aprs::Position position;
    position.compressed = false;
    position.latitude = 45.325776;
    position.longitude = 5.636580;
    position.symbol = '>';
    position.timestamp.type = aprs::TimestampType::DhmZulu;
    position.timestamp.day = 11;
    position.timestamp.hour = 14;
    position.timestamp.minute = 30;
    aprs::Result r = aprs::encodePosition("N0CALL-9", "APRS", nullptr, position,
                                          nullptr, nullptr, nullptr, frame, sizeof frame);
    printFrame(F("Position (timestamped):       "), r, frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // --- Uncompressed position with reduced precision (ambiguity) -------------
  // Useful when the GPS fix is coarse, or to intentionally publish a less
  // precise location. Ambiguity only applies to the uncompressed format.
  {
    aprs::Position position;
    position.compressed = false;
    position.latitude = 45.325776;
    position.longitude = 5.636580;
    position.symbol = '>';
    position.ambiguity = 2;  // ~1 mile accuracy instead of ~60 feet
    aprs::Result r = aprs::encodePosition("N0CALL-9", "APRS", nullptr, position,
                                          nullptr, nullptr, nullptr, frame, sizeof frame);
    printFrame(F("Position (reduced precision): "), r, frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // --- Position with compressed telemetry riding along -----------------------
  // Saves an entire extra transmission: the GPS fix and a telemetry snapshot
  // travel together in one frame instead of two (see the standalone Telemetry
  // data (T#) block below for the separate form).
  {
    aprs::Position position;
    position.latitude = 45.325776;
    position.longitude = 5.636580;
    position.symbol = '>';
    aprs::Telemetry telemetry;
    telemetry.sequenceNumber = 5;
    telemetry.analog[0].value = 199;
    telemetry.analog[1].value = 42;
    telemetry.boolean[1].value = true;
    aprs::Result r = aprs::encodePosition("N0CALL-9", "APRS", nullptr, position,
                                          nullptr, &telemetry, nullptr, frame, sizeof frame);
    printFrame(F("Position + telemetry:         "), r, frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // === Weather ================================================================
  // A weather report is just a position report whose symbol happens to be the
  // weather-station glyph — passing a Weather to encodePosition is what turns
  // it into one, there's no separate "weather" frame to build.

  {
    aprs::Position position;
    position.latitude = 45.325776;
    position.longitude = 5.636580;
    aprs::Weather weather;
    weather.useTemperature = true;
    weather.temperatureFahrenheit = 72;
    weather.useHumidity = true;
    weather.humidity = 55;
    weather.useWindDirection = true;
    weather.windDirectionDegrees = 270;
    weather.useWindSpeed = true;
    weather.windSpeedMph = 12;
    weather.useGustSpeed = true;
    weather.gustSpeedMph = 18;
    aprs::Result r = aprs::encodePosition("N0CALL-9", "APRS", nullptr, position,
                                          &weather, nullptr, nullptr, frame, sizeof frame);
    printFrame(F("Weather:                      "), r, frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // === Messages ================================================================

  // --- Plain message, asking the recipient to acknowledge line 001 ----------
  {
    aprs::Message message;
    strcpy(message.destination, "F4HVV-15");
    strcpy(message.message, "Hello from Arduino!");
    strcpy(message.ackToAsk, "001");
    aprs::Result r = aprs::encodeMessage("N0CALL-9", "APRS", nullptr, message, frame, sizeof frame);
    printFrame(F("Message:                      "), r, frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // --- ACK / REJ replies ------------------------------------------------------
  // Sent back to the ORIGINAL sender of a message that asked for an ack (the
  // "{NN" suffix) — see AprsDecode's handling of Message::ackToConfirm for the
  // RX side of this same field. An ack/rej is a standalone packet: no body,
  // no trailing space (see appendMessage in Aprs.cpp for why).
  {
    aprs::Message message;
    strcpy(message.destination, "F4HVV-15");
    strcpy(message.ackToConfirm, "009");
    aprs::Result r = aprs::encodeMessage("N0CALL-9", "APRS", nullptr, message, frame, sizeof frame);
    printFrame(F("Message ACK:                  "), r, frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  {
    aprs::Message message;
    strcpy(message.destination, "F4HVV-15");
    strcpy(message.ackToReject, "016");
    aprs::Result r = aprs::encodeMessage("N0CALL-9", "APRS", nullptr, message, frame, sizeof frame);
    printFrame(F("Message REJ:                  "), r, frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // === Telemetry ================================================================
  // One Telemetry struct, reused across all five telemetry frame types: the
  // DATA report (T#) carries the live values, while PARM/UNIT/EQNS/BITS are
  // separate "definition" frames (sent occasionally, not on every beacon)
  // that tell the receiving station what those values MEAN and how to
  // convert/display them.
  {
    aprs::Telemetry telemetry;
    telemetry.sequenceNumber = 5;
    telemetry.legacy = true;  // 3-digit legacy analog format (000-255)
    telemetry.analog[0].value = 199;  // e.g. battery voltage, raw ADC count
    telemetry.analog[1].value = 0;
    telemetry.analog[2].value = 255;
    telemetry.analog[3].value = 73;
    telemetry.analog[4].value = 123;
    telemetry.boolean[1].value = true;  // e.g. "door open"
    strcpy(telemetry.analog[0].name, "Battery");
    strcpy(telemetry.analog[0].unit, "V");
    telemetry.analog[0].equation.a = 0;
    telemetry.analog[0].equation.b = 0.1;  // raw ADC count * 0.1 = volts
    telemetry.analog[0].equation.c = 0;
    strcpy(telemetry.boolean[1].name, "Door");
    // PARM/UNIT truncate each field to a legacy per-position max length defined
    // by APRS101 ch.13 (analog: 7,6,5,5,4 chars; bits: 5,4,3,3,3,2,2,2 chars) —
    // boolean channel index 1 only gets 4 characters, so keep this short.
    strcpy(telemetry.boolean[1].unit, "On");
    telemetry.boolean[0].bitSense = false;  // show a mix of 0/1 in BITS below
    strcpy(telemetry.projectName, "Demo");

    aprs::Result r = aprs::encodeTelemetryData("N0CALL-9", "APRS", nullptr, telemetry, nullptr, frame, sizeof frame);
    printFrame(F("Telemetry data (T#):          "), r, frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst

    r = aprs::encodeTelemetryLabel("N0CALL-9", "APRS", nullptr, telemetry, frame, sizeof frame);
    printFrame(F("Telemetry names (PARM):       "), r, frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst

    r = aprs::encodeTelemetryUnit("N0CALL-9", "APRS", nullptr, telemetry, frame, sizeof frame);
    printFrame(F("Telemetry units (UNIT):       "), r, frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst

    r = aprs::encodeTelemetryEquation("N0CALL-9", "APRS", nullptr, telemetry, frame, sizeof frame);
    printFrame(F("Telemetry equations (EQNS):   "), r, frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst

    r = aprs::encodeTelemetryBitSense("N0CALL-9", "APRS", nullptr, telemetry, frame, sizeof frame);
    printFrame(F("Telemetry bit sense (BITS):   "), r, frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // === Object / Item ================================================================

  {
    aprs::ObjectItem item;
    strcpy(item.name, "LEADER");
    item.active = true;
    item.utcHour = 9;
    item.utcMinute = 23;
    item.utcSecond = 45;
    aprs::Position position;
    position.latitude = 49.058333;
    position.longitude = -72.029167;
    position.symbol = '>';
    aprs::Result r = aprs::encodeObjectItem("N0CALL-9", "APRS", nullptr, aprs::PacketType::Object,
                                            item, position, nullptr, frame, sizeof frame);
    printFrame(F("Object:                       "), r, frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  {
    aprs::ObjectItem item;
    strcpy(item.name, "ABC");
    item.active = true;
    aprs::Position position;
    position.latitude = 49.058333;
    position.longitude = -72.029167;
    position.symbol = '>';
    aprs::Result r = aprs::encodeObjectItem("N0CALL-9", "APRS", nullptr, aprs::PacketType::Item,
                                            item, position, nullptr, frame, sizeof frame);
    printFrame(F("Item:                         "), r, frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // === Status ================================================================

  {
    aprs::Result r = aprs::encodeStatus("N0CALL-9", "APRS", nullptr, "Net control tonight", frame, sizeof frame);
    printFrame(F("Status:                       "), r, frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // === Raw (caller-provided content, emitted verbatim) ==========================

  {
    aprs::Result r = aprs::encodeRaw("N0CALL-9", "APRS", nullptr, "anything goes here", frame, sizeof frame);
    printFrame(F("Raw:                          "), r, frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // === Maidenhead grid locator (standalone utility) ==============================
  char locator[7];  // 6 characters + NUL
  if (aprs::encodeGridLocator(45.325776, 5.636580, locator, sizeof locator)) {
    Serial.print(F("Grid locator:                  "));
    Serial.println(locator);

    // Round-trip: decode it back to the centre of that grid cell (useful for
    // a fixed station without GPS that only knows its own locator).
    double lat = 0, lon = 0;
    if (aprs::decodeGridLocator(locator, lat, lon)) {
      Serial.print(F("  -> back to lat/lon: "));
      Serial.print(lat, 4);
      Serial.print(F(", "));
      Serial.println(lon, 4);
    }
  }
}

void loop() {
}

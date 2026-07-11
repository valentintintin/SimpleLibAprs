/*
 * SimpleLibAprs - Encode example
 *
 * Builds one frame for every aprs::PacketType the library can encode, and
 * prints the resulting text to the serial port. Feed these frames to a TNC /
 * radio modem (e.g. via KISS) to actually transmit them.
 *
 * PacketType::Query has no encoder (see README: deciding whether/how to
 * respond to a query is application logic, so the library only decodes
 * them) and is therefore not shown here — see the AprsDecode example instead.
 *
 * IMPORTANT (AVR/Uno RAM): each block below is wrapped in its own { } scope
 * so its aprs::Packet local goes out of scope — and its stack space is freed
 * for reuse — before the NEXT block's Packet is declared. aprs::Packet is
 * ~1.2 KB on AVR (it bundles a Position + Message + Telemetry + Weather +
 * ObjectItem side by side, see the struct's doc comment in Aprs.h); an Uno
 * only has 2 KB of RAM total, so declaring more than one or two of these at
 * once in the SAME scope reliably overflows the stack and silently corrupts
 * RAM (confirmed on real Uno hardware: with all ~13 Packets flattened into
 * one function, nothing is printed at all — the scoping below is not just
 * tidiness, it is required for this example to run on an Uno).
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
// single largest consumer of this sketch's static RAM on AVR — worth far
// more than the couple of Packet-sized stack blocks the { } scoping saves.
void printFrame(const __FlashStringHelper* label, const aprs::Packet& packet, char* frame, size_t frameSize) {
  if (aprs::encode(packet, frame, frameSize) == aprs::Result::Ok) {
    Serial.print(label);
    Serial.println(frame);
  } else {
    Serial.print(label);
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
    aprs::Packet beacon;
    strcpy(beacon.source, "N0CALL-9");
    strcpy(beacon.destination, "APRS");
    strcpy(beacon.path, "WIDE1-1");
    strcpy(beacon.comment, "SimpleLibAprs");
    beacon.type = aprs::PacketType::Position;
    beacon.position.latitude = 45.325776;
    beacon.position.longitude = 5.636580;
    beacon.position.symbol = '>';      // car
    beacon.position.overlay = '/';
    beacon.position.courseDeg = 90;
    beacon.position.speedKnots = 25;
    beacon.position.altitudeFeet = 2722;
    printFrame(F("Position (compressed):        "), beacon, frame, sizeof frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // --- Uncompressed position with a timestamp --------------------------------
  {
    aprs::Packet timed;
    strcpy(timed.source, "N0CALL-9");
    strcpy(timed.destination, "APRS");
    timed.type = aprs::PacketType::Position;
    timed.position.compressed = false;
    timed.position.latitude = 45.325776;
    timed.position.longitude = 5.636580;
    timed.position.symbol = '>';
    timed.position.timestamp.type = aprs::TimestampType::DhmZulu;
    timed.position.timestamp.day = 11;
    timed.position.timestamp.hour = 14;
    timed.position.timestamp.minute = 30;
    printFrame(F("Position (timestamped):       "), timed, frame, sizeof frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // --- Uncompressed position with reduced precision (ambiguity) -------------
  // Useful when the GPS fix is coarse, or to intentionally publish a less
  // precise location. Ambiguity only applies to the uncompressed format.
  {
    aprs::Packet approx;
    strcpy(approx.source, "N0CALL-9");
    strcpy(approx.destination, "APRS");
    approx.type = aprs::PacketType::Position;
    approx.position.compressed = false;
    approx.position.latitude = 45.325776;
    approx.position.longitude = 5.636580;
    approx.position.symbol = '>';
    approx.position.ambiguity = 2;  // ~1 mile accuracy instead of ~60 feet
    printFrame(F("Position (reduced precision): "), approx, frame, sizeof frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // --- Position with compressed telemetry riding along -----------------------
  // Saves an entire extra transmission: the GPS fix and a telemetry snapshot
  // travel together in one frame instead of two (see PacketType::Telemetry
  // below for the standalone form).
  {
    aprs::Packet posTelemetry;
    strcpy(posTelemetry.source, "N0CALL-9");
    strcpy(posTelemetry.destination, "APRS");
    posTelemetry.type = aprs::PacketType::Position;
    posTelemetry.position.latitude = 45.325776;
    posTelemetry.position.longitude = 5.636580;
    posTelemetry.position.symbol = '>';
    posTelemetry.position.withTelemetry = true;
    posTelemetry.telemetry.sequenceNumber = 5;
    posTelemetry.telemetry.analog[0].value = 199;
    posTelemetry.telemetry.analog[1].value = 42;
    posTelemetry.telemetry.boolean[1].value = true;
    printFrame(F("Position + telemetry:         "), posTelemetry, frame, sizeof frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // === Weather ================================================================

  {
    aprs::Packet weather;
    strcpy(weather.source, "N0CALL-9");
    strcpy(weather.destination, "APRS");
    weather.type = aprs::PacketType::Weather;
    weather.position.latitude = 45.325776;
    weather.position.longitude = 5.636580;
    weather.weather.useTemperature = true;
    weather.weather.temperatureFahrenheit = 72;
    weather.weather.useHumidity = true;
    weather.weather.humidity = 55;
    weather.weather.useWindDirection = true;
    weather.weather.windDirectionDegrees = 270;
    weather.weather.useWindSpeed = true;
    weather.weather.windSpeedMph = 12;
    weather.weather.useGustSpeed = true;
    weather.weather.gustSpeedMph = 18;
    printFrame(F("Weather:                      "), weather, frame, sizeof frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // === Messages ================================================================

  // --- Plain message, asking the recipient to acknowledge line 001 ----------
  {
    aprs::Packet message;
    strcpy(message.source, "N0CALL-9");
    strcpy(message.destination, "APRS");
    strcpy(message.message.destination, "F4HVV-15");
    strcpy(message.message.message, "Hello from Arduino!");
    strcpy(message.message.ackToAsk, "001");
    message.type = aprs::PacketType::Message;
    printFrame(F("Message:                      "), message, frame, sizeof frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // --- ACK / REJ replies ------------------------------------------------------
  // Sent back to the ORIGINAL sender of a message that asked for an ack (the
  // "{NN" suffix) — see AprsDecode's handling of Message::ackToConfirm for the
  // RX side of this same field. An ack/rej is a standalone packet: no body,
  // no trailing space (see appendMessage in Aprs.cpp for why).
  {
    aprs::Packet ackReply;
    strcpy(ackReply.source, "N0CALL-9");
    strcpy(ackReply.destination, "APRS");
    ackReply.type = aprs::PacketType::Message;
    strcpy(ackReply.message.destination, "F4HVV-15");
    strcpy(ackReply.message.ackToConfirm, "009");
    printFrame(F("Message ACK:                  "), ackReply, frame, sizeof frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  {
    aprs::Packet rejReply;
    strcpy(rejReply.source, "N0CALL-9");
    strcpy(rejReply.destination, "APRS");
    rejReply.type = aprs::PacketType::Message;
    strcpy(rejReply.message.destination, "F4HVV-15");
    strcpy(rejReply.message.ackToReject, "016");
    printFrame(F("Message REJ:                  "), rejReply, frame, sizeof frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // === Telemetry ================================================================
  // One Telemetry struct, reused across all five telemetry frame types: the
  // DATA report (T#) carries the live values, while PARM/UNIT/EQNS/BITS are
  // separate "definition" frames (sent occasionally, not on every beacon)
  // that tell the receiving station what those values MEAN and how to
  // convert/display them.
  {
    aprs::Packet telemetry;
    strcpy(telemetry.source, "N0CALL-9");
    strcpy(telemetry.destination, "APRS");
    telemetry.telemetry.sequenceNumber = 5;
    telemetry.telemetry.legacy = true;  // 3-digit legacy analog format (000-255)
    telemetry.telemetry.analog[0].value = 199;  // e.g. battery voltage, raw ADC count
    telemetry.telemetry.analog[1].value = 0;
    telemetry.telemetry.analog[2].value = 255;
    telemetry.telemetry.analog[3].value = 73;
    telemetry.telemetry.analog[4].value = 123;
    telemetry.telemetry.boolean[1].value = true;  // e.g. "door open"
    strcpy(telemetry.telemetry.analog[0].name, "Battery");
    strcpy(telemetry.telemetry.analog[0].unit, "V");
    telemetry.telemetry.analog[0].equation.a = 0;
    telemetry.telemetry.analog[0].equation.b = 0.1;  // raw ADC count * 0.1 = volts
    telemetry.telemetry.analog[0].equation.c = 0;
    strcpy(telemetry.telemetry.boolean[1].name, "Door");
    // PARM/UNIT truncate each field to a legacy per-position max length defined
    // by APRS101 ch.13 (analog: 7,6,5,5,4 chars; bits: 5,4,3,3,3,2,2,2 chars) —
    // boolean channel index 1 only gets 4 characters, so keep this short.
    strcpy(telemetry.telemetry.boolean[1].unit, "On");
    telemetry.telemetry.boolean[0].bitSense = false;  // show a mix of 0/1 in BITS below
    strcpy(telemetry.telemetry.projectName, "Demo");

    telemetry.type = aprs::PacketType::Telemetry;
    printFrame(F("Telemetry data (T#):          "), telemetry, frame, sizeof frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst

    telemetry.type = aprs::PacketType::TelemetryLabel;
    printFrame(F("Telemetry names (PARM):       "), telemetry, frame, sizeof frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst

    telemetry.type = aprs::PacketType::TelemetryUnit;
    printFrame(F("Telemetry units (UNIT):       "), telemetry, frame, sizeof frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst

    telemetry.type = aprs::PacketType::TelemetryEquation;
    printFrame(F("Telemetry equations (EQNS):   "), telemetry, frame, sizeof frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst

    telemetry.type = aprs::PacketType::TelemetryBitSense;
    printFrame(F("Telemetry bit sense (BITS):   "), telemetry, frame, sizeof frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // === Object / Item ================================================================

  {
    aprs::Packet object;
    strcpy(object.source, "N0CALL-9");
    strcpy(object.destination, "APRS");
    object.type = aprs::PacketType::Object;
    strcpy(object.item.name, "LEADER");
    object.item.active = true;
    object.item.utcHour = 9;
    object.item.utcMinute = 23;
    object.item.utcSecond = 45;
    object.position.latitude = 49.058333;
    object.position.longitude = -72.029167;
    object.position.symbol = '>';
    printFrame(F("Object:                       "), object, frame, sizeof frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  {
    aprs::Packet item;
    strcpy(item.source, "N0CALL-9");
    strcpy(item.destination, "APRS");
    item.type = aprs::PacketType::Item;
    strcpy(item.item.name, "ABC");
    item.item.active = true;
    item.position.latitude = 49.058333;
    item.position.longitude = -72.029167;
    item.position.symbol = '>';
    printFrame(F("Item:                         "), item, frame, sizeof frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // === Status ================================================================

  {
    aprs::Packet status;
    strcpy(status.source, "N0CALL-9");
    strcpy(status.destination, "APRS");
    status.type = aprs::PacketType::Status;
    strcpy(status.comment, "Net control tonight");
    printFrame(F("Status:                       "), status, frame, sizeof frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // === Raw (caller-provided content, emitted verbatim) ==========================

  {
    aprs::Packet raw;
    strcpy(raw.source, "N0CALL-9");
    strcpy(raw.destination, "APRS");
    raw.type = aprs::PacketType::Raw;
    strcpy(raw.content, "anything goes here");
    printFrame(F("Raw:                          "), raw, frame, sizeof frame);
    delay(10);  // let a slow USB-serial adapter drain the TX buffer before the next burst
  }

  // === Maidenhead grid locator (standalone utility, no Packet involved) ==========
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

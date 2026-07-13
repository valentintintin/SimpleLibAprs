/**
 * @file test.cpp
 * @brief Assertion-based test suite for SimpleLibAprs (native build).
 *
 * Build & run with CMake (see top-level CMakeLists.txt) or directly:
 *   c++ -std=c++17 -DNATIVE -I../src test.cpp ../src/Aprs.cpp -o app && ./app
 */

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "Aprs.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void reportPass(const char* what) {
    ++g_checks;
    std::printf("  \033[32mok\033[0m   %s\n", what);
}

void reportFail(const char* what, const char* detail) {
    ++g_checks;
    ++g_failures;
    std::printf("  \033[31mFAIL\033[0m %s\n       %s\n", what, detail);
}

void checkBool(bool cond, const char* what) {
    if (cond) {
        reportPass(what);
    } else {
        reportFail(what, "expected true");
    }
}

void checkStr(const char* actual, const char* expected, const char* what) {
    if (std::strcmp(actual, expected) == 0) {
        reportPass(what);
    } else {
        char detail[600];
        std::snprintf(detail, sizeof detail, "expected [%s]\n            actual   [%s]", expected, actual);
        reportFail(what, detail);
    }
}

void checkContains(const char* haystack, const char* needle, const char* what) {
    if (std::strstr(haystack, needle) != nullptr) {
        reportPass(what);
    } else {
        char detail[600];
        std::snprintf(detail, sizeof detail, "expected to contain [%s]\n            in       [%s]", needle, haystack);
        reportFail(what, detail);
    }
}

void checkInt(long actual, long expected, const char* what) {
    if (actual == expected) {
        reportPass(what);
    } else {
        char detail[120];
        std::snprintf(detail, sizeof detail, "expected %ld, actual %ld", expected, actual);
        reportFail(what, detail);
    }
}

void checkNear(double actual, double expected, double tol, const char* what) {
    double d = actual - expected;
    if (d < 0) d = -d;
    if (d <= tol) {
        reportPass(what);
    } else {
        char detail[120];
        std::snprintf(detail, sizeof detail, "expected %.5f (+/-%.5f), actual %.5f", expected, tol, actual);
        reportFail(what, detail);
    }
}

void section(const char* title) {
    std::printf("\n== %s ==\n", title);
}

// ---------------------------------------------------------------------------

void testCompressedPosition() {
    section("Compressed position (spec example 49.5N, 72.75W)");

    aprs::Position position;
    position.overlay = '/';
    position.symbol = '>';
    position.latitude = 49.5;
    position.longitude = -72.75;
    position.courseDeg = 0;
    position.speedKnots = 0;
    position.altitudeFeet = 0;
    position.altitudeInComment = true;

    char out[aprs::kMaxPacketLength + 1];
    aprs::encodePosition("TEST", "APRS", nullptr, position, nullptr, nullptr, nullptr, out, sizeof out);

    // Spec APRS101 ch.9 worked example: lat -> "5L!!", lon -> "<*e7".
    checkContains(out, "5L!!<*e7", "latitude/longitude base-91 match the spec example");
    checkStr(out, "TEST>APRS:=/5L!!<*e7>!!Y", "full compressed position frame");
}

void testPositionWithAltitude() {
    section("Position with /A= altitude");

    aprs::Position position;
    position.latitude = 45.325776;
    position.longitude = 5.636580;
    position.altitudeFeet = 2722;  // ~830 m
    position.altitudeInComment = true;

    char out[aprs::kMaxPacketLength + 1];
    aprs::encodePosition("F4HVV-15", "APRS", nullptr, position, nullptr, nullptr, nullptr, out, sizeof out);

    checkContains(out, "/A=002722", "altitude rendered as /A=002722");
}

void testWeather() {
    section("Compressed weather report (spec-compliant)");

    aprs::Position position;
    position.latitude = 49.5;
    position.longitude = -72.75;
    aprs::Weather weather;
    weather.useGustSpeed = true;
    weather.gustSpeedMph = 5;
    weather.useTemperature = true;
    weather.temperatureFahrenheit = 77;
    weather.useHumidity = true;
    weather.humidity = 50;
    weather.usePressure = true;
    weather.pressure = 990;

    char out[aprs::kMaxPacketLength + 1];
    aprs::encodePosition("F4HVV-15", "APRS", nullptr, position, &weather, nullptr, nullptr, out, sizeof out);

    // '_' weather symbol, then wind in cs bytes, then weather data from gust.
    checkContains(out, "5L!!<*e7_", "weather uses compressed position with '_' symbol");
    checkContains(out, "g005t077", "weather data starts at gust (no ddd/sss text)");
    checkContains(out, "h50b09900", "humidity and pressure encoded");
    checkBool(std::strstr(out, "/005g") == nullptr, "no uncompressed ddd/sss wind block");
}

void testMessageAndAck() {
    section("Message, ACK and REJ");

    aprs::Message message;
    std::strcpy(message.destination, "NOCALL-4");
    std::strcpy(message.message, "Hello, world!");

    char out[aprs::kMaxPacketLength + 1];

    aprs::encodeMessage("N0CALL-9", "APRS", nullptr, message, out, sizeof out);
    checkStr(out, "N0CALL-9>APRS::NOCALL-4 :Hello, world!", "plain message frame");

    std::strcpy(message.ackToAsk, "001");
    aprs::encodeMessage("N0CALL-9", "APRS", nullptr, message, out, sizeof out);
    checkStr(out, "N0CALL-9>APRS::NOCALL-4 :Hello, world!{001", "message with line number {001");

    aprs::Message ackMsg;
    std::strcpy(ackMsg.destination, "NOCALL-4");
    std::strcpy(ackMsg.ackToConfirm, "009");
    std::strcpy(ackMsg.message, "this body must be ignored");
    aprs::encodeMessage("N0CALL-9", "APRS", nullptr, ackMsg, out, sizeof out);
    checkStr(out, "N0CALL-9>APRS::NOCALL-4 :ack009", "ACK is a pure packet (no body, no trailing space)");

    std::strcpy(ackMsg.ackToConfirm, "");
    std::strcpy(ackMsg.ackToReject, "016");
    aprs::encodeMessage("N0CALL-9", "APRS", nullptr, ackMsg, out, sizeof out);
    checkStr(out, "N0CALL-9>APRS::NOCALL-4 :rej016", "REJ is a pure packet");
}

void testTelemetry() {
    section("Telemetry");

    aprs::Telemetry telemetry;
    telemetry.sequenceNumber = 7544;  // must wrap into a 3-digit field
    telemetry.analog[0].value = 1472;
    telemetry.analog[1].value = 1564;
    std::strcpy(telemetry.analog[0].name, "A1");
    std::strcpy(telemetry.analog[1].name, "A2");
    std::strcpy(telemetry.analog[0].unit, "V");
    telemetry.boolean[0].value = 1;

    char out[aprs::kMaxPacketLength + 1];

    aprs::encodeTelemetryData("N0CALL-9", "APRS", nullptr, telemetry, nullptr, out, sizeof out);
    checkContains(out, ":T#544,", "T# sequence wrapped to 3 digits (544)");
    checkContains(out, "T#544,1472,1564,", "analog values present");

    aprs::encodeTelemetryLabel("N0CALL-9", "APRS", nullptr, telemetry, out, sizeof out);
    checkStr(out, "N0CALL-9>APRS::N0CALL-9 :PARM.A1,A2,,,,,,,,,,,", "PARM frame with 9-char addressee");

    aprs::encodeTelemetryUnit("N0CALL-9", "APRS", nullptr, telemetry, out, sizeof out);
    checkContains(out, ":N0CALL-9 :UNIT.V,", "UNIT frame");

    std::strcpy(telemetry.projectName, "Demo");
    aprs::encodeTelemetryBitSense("N0CALL-9", "APRS", nullptr, telemetry, out, sizeof out);
    checkContains(out, ":N0CALL-9 :BITS.", "BITS frame");
    checkContains(out, ",Demo", "BITS project name appended");
}

void testObjectAndItem() {
    section("Object and Item");

    aprs::ObjectItem item;
    std::strcpy(item.name, "OBJITM");
    item.active = true;
    item.utcHour = 12;
    item.utcMinute = 34;
    item.utcSecond = 56;
    aprs::Position position;
    position.latitude = 49.5;
    position.longitude = -72.75;

    char out[aprs::kMaxPacketLength + 1];

    aprs::encodeObjectItem("F4HVV-15", "APRS", nullptr, aprs::PacketType::Object, item, position, nullptr, out, sizeof out);
    checkContains(out, ";OBJITM   *123456h", "Object: 9-char name, '*' live, HMS 'h' suffix");

    aprs::encodeObjectItem("F4HVV-15", "APRS", nullptr, aprs::PacketType::Item, item, position, nullptr, out, sizeof out);
    checkContains(out, ")OBJITM!", "Item: variable name, '!' live");
}

void testStatusAndRaw() {
    section("Status and Raw");

    char out[aprs::kMaxPacketLength + 1];

    aprs::encodeStatus("F4HVV-15", "APRS", nullptr, "I'm good !", out, sizeof out);
    checkStr(out, "F4HVV-15>APRS:>I'm good !", "Status frame");

    aprs::encodeRaw("F4HVV-15", "APRS", nullptr, "anything goes here", out, sizeof out);
    checkStr(out, "F4HVV-15>APRS:anything goes here", "Raw content passed through");
}

void testEncodeErrors() {
    section("Encode error handling");

    aprs::Position position;  // missing source/destination
    char out[aprs::kMaxPacketLength + 1];
    checkInt((long) aprs::encodePosition("", "", nullptr, position, nullptr, nullptr, nullptr, out, sizeof out),
             (long) aprs::Result::InvalidArgument, "missing callsign -> InvalidArgument");

    char tiny[8];
    aprs::Result r = aprs::encodePosition("TEST", "APRS", nullptr, position, nullptr, nullptr, nullptr, tiny, sizeof tiny);
    checkInt((long) r, (long) aprs::Result::BufferTooSmall, "tiny buffer -> BufferTooSmall");
    checkBool(std::strlen(tiny) < sizeof tiny, "tiny buffer stays within bounds and NUL-terminated");
}

void testDecode() {
    section("Decode (envelope) + decodeMessage");

    aprs::PacketLite lite;
    bool ok = aprs::decode("F4HVV-10>APDR16,WIDE1-1,F4HVV-9*::F4HVV-15 :ack1 hello{2", lite);
    checkBool(ok, "decode returns true");
    checkStr(lite.source, "F4HVV-10", "decoded source");
    checkStr(lite.destination, "APDR16", "decoded destination");
    checkStr(lite.path, "WIDE1-1,F4HVV-9*", "decoded path");
    checkStr(lite.lastDigipeaterCallsignInPath, "F4HVV-9", "last digipeater");
    checkInt(lite.digipeaterCount, 2, "digipeater count");
    checkInt((long) lite.type, (long) aprs::PacketType::Message, "type Message");

    aprs::Message msg;
    bool mok = aprs::decodeMessage(lite, msg);
    checkBool(mok, "decodeMessage returns true");
    checkStr(msg.destination, "F4HVV-15", "message addressee");
    checkStr(msg.message, "hello", "message body");
    checkStr(msg.ackConfirmed, "1", "ackConfirmed");
    checkStr(msg.ackToConfirm, "2", "line number to confirm");

    aprs::PacketLite pos;
    aprs::decode("F4HVV-9>APDR16,WIDE1-1:=4519.92N/00537.15E[", pos);
    checkStr(pos.source, "F4HVV-9", "position: source");
    checkStr(pos.path, "WIDE1-1", "position: path");
    checkInt((long) pos.type, (long) aprs::PacketType::Position, "position: type");
    checkInt(pos.digipeaterCount, 0, "position: no digipeater used");

    aprs::PacketLite nopath;
    aprs::decode("F4HVV-9>APDR16:=4519.92N/00537.15E[", nopath);
    checkStr(nopath.source, "F4HVV-9", "no-path: source");
    checkStr(nopath.destination, "APDR16", "no-path: destination");

    // No path, but the PAYLOAD itself contains a comma right after the
    // header colon (a bare T# telemetry report). The comma must not be
    // mistaken for a path separator — the header ends at the first ':',
    // full stop, regardless of what commas follow it.
    aprs::PacketLite noPathCommaPayload;
    aprs::decode("N0QBF>APRS:T#005,199,000,255,073,123,01101001", noPathCommaPayload);
    checkStr(noPathCommaPayload.source, "N0QBF", "no-path+comma-payload: source");
    checkStr(noPathCommaPayload.destination, "APRS", "no-path+comma-payload: destination not swallowed");
    checkStr(noPathCommaPayload.path, "", "no-path+comma-payload: path stays empty");
    checkStr(noPathCommaPayload.content, "T#005,199,000,255,073,123,01101001",
             "no-path+comma-payload: content unaffected");

    // Same, but WITH a real path this time: the payload's commas must not
    // leak into the path either.
    aprs::PacketLite pathCommaPayload;
    aprs::decode("N0QBF>APRS,WIDE1-1:T#005,199,000,255,073,123,01101001", pathCommaPayload);
    checkStr(pathCommaPayload.destination, "APRS", "path+comma-payload: destination");
    checkStr(pathCommaPayload.path, "WIDE1-1", "path+comma-payload: path not extended into payload");
}

void testThirdPartyHeader() {
    section("Third-party header (RF frame gated from APRS-IS)");

    // An I-Gate ("F4HVV-10-IGATE") re-transmits a position report that
    // originated from N0CALL-9 on APRS-IS.
    aprs::PacketLite lite;
    bool ok = aprs::decode(
        "F4HVV-10>APRS,TCPIP*:}N0CALL-9>APDR16,WIDE1-1:=4519.92N/00537.15E>088/036", lite);
    checkBool(ok, "decode returns true");
    checkBool(lite.viaThirdParty, "flagged as third-party");
    checkStr(lite.gateway, "F4HVV-10", "gateway callsign captured");
    checkStr(lite.source, "N0CALL-9", "unwrapped: original source");
    checkStr(lite.destination, "APDR16", "unwrapped: original destination");
    checkStr(lite.path, "WIDE1-1", "unwrapped: original path");
    checkInt((long) lite.type, (long) aprs::PacketType::Position, "unwrapped: type Position");

    // Typed decoders must work transparently on the unwrapped packet.
    aprs::Position pos;
    checkBool(aprs::decodePosition(lite, pos), "decodePosition works on unwrapped packet");
    checkNear(pos.latitude, 45.332, 0.001, "unwrapped: latitude");
    checkNear(pos.longitude, 5.6192, 0.001, "unwrapped: longitude");

    // A message wrapped the same way.
    aprs::decode("F4HVV-10>APRS,TCPIP*:}N0CALL-9>APDR16::F4HVV-15 :hello{2", lite);
    checkBool(lite.viaThirdParty, "message: flagged as third-party");
    checkStr(lite.source, "N0CALL-9", "message: unwrapped source");
    checkInt((long) lite.type, (long) aprs::PacketType::Message, "message: unwrapped type");
    aprs::Message msg;
    checkBool(aprs::decodeMessage(lite, msg), "decodeMessage works on unwrapped packet");
    checkStr(msg.message, "hello", "message: unwrapped body");

    // A normal (non-gated) frame must NOT be flagged.
    aprs::decode("F4HVV-9>APDR16,WIDE1-1:=4519.92N/00537.15E[", lite);
    checkBool(!lite.viaThirdParty, "plain frame: not flagged as third-party");
    checkStr(lite.gateway, "", "plain frame: gateway left empty");

    // Malformed inner frame: falls back to the raw (still-wrapped) content.
    aprs::decode("F4HVV-10>APRS,TCPIP*:}garbage-no-colon", lite);
    checkBool(!lite.viaThirdParty, "malformed wrapper: not flagged");
    checkStr(lite.source, "F4HVV-10", "malformed wrapper: outer source kept");
}

void testPacketLiteCopySafety() {
    section("PacketLite copy/assignment safety (content must not alias another instance)");

    aprs::PacketLite a;
    aprs::decode("F4HVV-9>APDR16:=4519.92N/00537.15E>088/036", a);

    aprs::PacketLite b = a;  // copy-construct
    checkBool(b.content >= b.raw && b.content < b.raw + sizeof(b.raw),
             "copy-construct: content re-points into the copy's own raw");
    checkStr(b.content, "=4519.92N/00537.15E>088/036", "copy-construct: content value preserved");

    aprs::reset(a);
    checkStr(b.content, "=4519.92N/00537.15E>088/036",
            "copy-construct: content survives the original being reset");

    aprs::PacketLite c;
    c = a;  // copy-assign a reset (default-state) packet
    checkStr(c.content, "", "copy-assign: default/reset state copies the empty content correctly");

    // Copy-assigning a third-party-unwrapped packet must also re-point
    // correctly and preserve the third-party-specific fields.
    aprs::PacketLite d;
    aprs::decode("F4HVV-10>APRS,TCPIP*:}N0CALL-9>APDR16:=4519.92N/00537.15E>088/036", d);
    aprs::PacketLite e;
    e = d;
    checkBool(e.content >= e.raw && e.content < e.raw + sizeof(e.raw),
             "copy-assign third-party: content re-points into the copy's own raw");
    checkBool(e.viaThirdParty, "copy-assign third-party: viaThirdParty preserved");
    checkStr(e.gateway, "F4HVV-10", "copy-assign third-party: gateway preserved");
    checkStr(e.source, "N0CALL-9", "copy-assign third-party: unwrapped source preserved");
}

void testUncompressedEncode() {
    section("Uncompressed position encoding (TX)");

    aprs::Position position;
    position.compressed = false;
    position.overlay = '/';
    position.symbol = '>';
    position.latitude = 45.332;
    position.longitude = 5.6192;
    position.courseDeg = 88;
    position.speedKnots = 36;

    char out[aprs::kMaxPacketLength + 1];
    aprs::encodePosition("TEST", "APRS", nullptr, position, nullptr, nullptr, nullptr, out, sizeof out);
    checkStr(out, "TEST>APRS:=4519.92N/00537.15E>088/036", "uncompressed position with course/speed");

    position.courseDeg = 0;
    position.speedKnots = 0;
    position.altitudeFeet = 2722;
    aprs::encodePosition("TEST", "APRS", nullptr, position, nullptr, nullptr, nullptr, out, sizeof out);
    checkContains(out, "=4519.92N/00537.15E>/A=002722", "uncompressed position with altitude");
}

void testTimestamp() {
    section("Position timestamp (TX + RX)");

    aprs::Position position;
    position.latitude = 49.5;
    position.longitude = -72.75;
    position.timestamp.type = aprs::TimestampType::Hms;
    position.timestamp.hour = 12;
    position.timestamp.minute = 34;
    position.timestamp.second = 56;

    char out[aprs::kMaxPacketLength + 1];
    aprs::encodePosition("TEST", "APRS", nullptr, position, nullptr, nullptr, nullptr, out, sizeof out);
    checkContains(out, ":@123456h/5L!!<*e7", "timestamped position uses @ + HHMMSSh");

    aprs::PacketLite lite;
    aprs::decode(out, lite);
    aprs::Position decoded;
    aprs::decodePosition(lite, decoded);
    checkInt((long) decoded.timestamp.type, (long) aprs::TimestampType::Hms, "decoded timestamp type Hms");
    checkInt(decoded.timestamp.hour, 12, "decoded timestamp hour");
    checkInt(decoded.timestamp.minute, 34, "decoded timestamp minute");
    checkInt(decoded.timestamp.second, 56, "decoded timestamp second");
}

void testDecodePosition() {
    section("Position decoding (RX)");

    // Compressed (spec example): 49.5N, 72.75W, course 88, speed ~36.2 kt.
    aprs::PacketLite lite;
    aprs::decode("F4HVV-9>APDR16:=/5L!!<*e7>7P[", lite);
    checkInt((long) lite.type, (long) aprs::PacketType::Position, "compressed: type Position");
    aprs::Position cp;
    checkBool(aprs::decodePosition(lite, cp), "compressed: decodePosition ok");
    checkNear(cp.latitude, 49.5, 0.001, "compressed: latitude");
    checkNear(cp.longitude, -72.75, 0.001, "compressed: longitude");
    checkInt(cp.symbol, '>', "compressed: symbol");
    checkNear(cp.courseDeg, 88, 1, "compressed: course");
    checkNear(cp.speedKnots, 36.2, 1.5, "compressed: speed");

    // Uncompressed.
    aprs::decode("F4HVV-9>APDR16:=4519.92N/00537.15E>088/036", lite);
    checkInt((long) lite.type, (long) aprs::PacketType::Position, "uncompressed: type Position");
    aprs::Position up;
    checkBool(aprs::decodePosition(lite, up), "uncompressed: decodePosition ok");
    checkNear(up.latitude, 45.332, 0.001, "uncompressed: latitude");
    checkNear(up.longitude, 5.6192, 0.001, "uncompressed: longitude");
    checkInt(up.symbol, '>', "uncompressed: symbol");
    checkNear(up.courseDeg, 88, 0.5, "uncompressed: course");
    checkNear(up.speedKnots, 36, 0.5, "uncompressed: speed");

    // Southern / western hemisphere.
    aprs::decode("VK1ABC>APRS:=3357.00S/15112.00E-", lite);
    aprs::Position south;
    aprs::decodePosition(lite, south);
    checkNear(south.latitude, -33.95, 0.001, "southern latitude is negative");
    checkNear(south.longitude, 151.20, 0.001, "eastern longitude");
}

void testPositionAmbiguity() {
    section("Position ambiguity (APRS101 ch.6 worked example, TX + RX)");

    aprs::Position position;
    position.compressed = false;
    position.symbol = '>';
    position.latitude = 49.05833;    // 49 03.50' N
    position.longitude = -72.02917;  // 072 01.75' W

    char out[aprs::kMaxPacketLength + 1];

    position.ambiguity = 1;
    aprs::encodePosition("TEST", "APRS", nullptr, position, nullptr, nullptr, nullptr, out, sizeof out);
    checkContains(out, "=4903.5 N/07201.7 W>", "ambiguity 1: last decimal digit blanked");

    position.ambiguity = 2;
    aprs::encodePosition("TEST", "APRS", nullptr, position, nullptr, nullptr, nullptr, out, sizeof out);
    checkContains(out, "=4903.  N/07201.  W>", "ambiguity 2: both decimal digits blanked");

    position.ambiguity = 3;
    aprs::encodePosition("TEST", "APRS", nullptr, position, nullptr, nullptr, nullptr, out, sizeof out);
    checkContains(out, "=490 .  N/0720 .  W>", "ambiguity 3: minutes units digit also blanked");

    position.ambiguity = 4;
    aprs::encodePosition("TEST", "APRS", nullptr, position, nullptr, nullptr, nullptr, out, sizeof out);
    checkContains(out, "=49  .  N/072  .  W>", "ambiguity 4: minutes tens digit also blanked");

    aprs::PacketLite lite;
    aprs::decode(out, lite);
    aprs::Position decoded;
    checkBool(aprs::decodePosition(lite, decoded), "ambiguity 4: decodePosition ok");
    checkInt(decoded.ambiguity, 4, "ambiguity 4: detected level");
    checkNear(decoded.latitude, 49.0, 0.001, "ambiguity 4: latitude rounds to degrees");
    checkNear(decoded.longitude, -72.0, 0.001, "ambiguity 4: longitude rounds to degrees");

    // No ambiguity: round-trips exactly as before.
    position.ambiguity = 0;
    aprs::encodePosition("TEST", "APRS", nullptr, position, nullptr, nullptr, nullptr, out, sizeof out);
    checkContains(out, "=4903.50N/07201.75W>", "ambiguity 0: unchanged");
}

void testGridLocator() {
    section("Maidenhead grid locator (encode/decode)");

    char loc[9];
    checkBool(aprs::encodeGridLocator(51.5, -1.0, loc, sizeof loc, 2), "encode 4-char locator ok");
    checkStr(loc, "IO91", "encode: known reference IO91 (UK)");

    double lat = 0, lon = 0;
    checkBool(aprs::decodeGridLocator("IO91", lat, lon), "decode 4-char locator ok");
    checkNear(lat, 51.5, 0.001, "decode: IO91 latitude (cell centre)");
    checkNear(lon, -1.0, 0.001, "decode: IO91 longitude (cell centre)");

    checkBool(aprs::decodeGridLocator("io91", lat, lon), "decode is case-insensitive");
    checkNear(lat, 51.5, 0.001, "decode lowercase: latitude");

    // 6-char round trip: precision should be within half a subsquare cell.
    checkBool(aprs::encodeGridLocator(45.19, 5.72, loc, sizeof loc, 3), "encode 6-char locator ok");
    checkBool(aprs::decodeGridLocator(loc, lat, lon), "decode 6-char locator ok");
    checkNear(lat, 45.19, 1.0 / 24.0, "6-char round trip: latitude within subsquare");
    checkNear(lon, 5.72, 2.0 / 24.0, "6-char round trip: longitude within subsquare");

    checkBool(!aprs::decodeGridLocator("ZZ99", lat, lon), "decode rejects out-of-range field");
    checkBool(!aprs::decodeGridLocator("ABC", lat, lon), "decode rejects odd-length input");
    checkBool(!aprs::encodeGridLocator(91.0, 0.0, loc, sizeof loc), "encode rejects out-of-range latitude");
    checkBool(!aprs::encodeGridLocator(0.0, 0.0, loc, 3), "encode rejects buffer too small");

    // Exact +90/+180 edge: must clamp into the last cell, not overflow it.
    checkBool(aprs::encodeGridLocator(90.0, 180.0, loc, sizeof loc, 4), "encode north-east pole corner ok");
    checkStr(loc, "RR99xx99", "north-east corner clamps to the last cell of every pair");
    checkBool(aprs::encodeGridLocator(-90.0, -180.0, loc, sizeof loc, 4), "encode south-west pole corner ok");
    checkStr(loc, "AA00aa00", "south-west corner sits in the first cell of every pair");
}

void testDecodeWeather() {
    section("Weather decoding (RX, round-trip)");

    aprs::Position position;
    position.latitude = 49.5;
    position.longitude = -72.75;
    aprs::Weather weather;
    weather.useGustSpeed = true;       weather.gustSpeedMph = 5;
    weather.useTemperature = true;     weather.temperatureFahrenheit = 77;
    weather.useRain1Hour = true;       weather.rain1HourHundredthsOfAnInch = 12;
    weather.useHumidity = true;        weather.humidity = 50;
    weather.usePressure = true;        weather.pressure = 990;

    char out[aprs::kMaxPacketLength + 1];
    aprs::encodePosition("F4HVV-15", "APRS", nullptr, position, &weather, nullptr, nullptr, out, sizeof out);

    aprs::PacketLite lite;
    aprs::decode(out, lite);
    checkInt((long) lite.type, (long) aprs::PacketType::Weather, "weather: type Weather");
    aprs::Weather w;
    checkBool(aprs::decodeWeather(lite, w), "weather: decodeWeather ok");
    checkInt(w.gustSpeedMph, 5, "weather: gust");
    checkInt(w.temperatureFahrenheit, 77, "weather: temperature");
    checkInt(w.rain1HourHundredthsOfAnInch, 12, "weather: rain 1h");
    checkInt(w.humidity, 50, "weather: humidity");
    checkInt(w.pressure, 990, "weather: pressure");

    // Positionless weather report ('_' + MDHM + cccsss + data).
    aprs::PacketLite pw;
    aprs::decode("N0CALL>APRS:_10090556c220s004g005t077h50b09900", pw);
    checkInt((long) pw.type, (long) aprs::PacketType::Weather, "positionless: type Weather");
    aprs::Weather w2;
    checkBool(aprs::decodeWeather(pw, w2), "positionless: decode ok");
    checkInt(w2.windDirectionDegrees, 220, "positionless: wind direction");
    checkInt(w2.windSpeedMph, 4, "positionless: wind speed");
    checkInt(w2.gustSpeedMph, 5, "positionless: gust");
    checkInt(w2.temperatureFahrenheit, 77, "positionless: temperature");
    checkInt(w2.humidity, 50, "positionless: humidity");
    checkInt(w2.pressure, 990, "positionless: pressure");
}

void testDecodeObjectStatus() {
    section("Object / Item / Status decoding (RX)");

    aprs::PacketLite lite;

    // Object with a DHM-zulu timestamp and a position.
    aprs::decode("N0CALL>APRS:;LEADER   *092345z4903.50N/07201.75W>", lite);
    checkInt((long) lite.type, (long) aprs::PacketType::Object, "object: type Object");
    aprs::ObjectItem obj;
    aprs::Position op;
    checkBool(aprs::decodeObjectItem(lite, obj, op), "object: decode ok");
    checkStr(obj.name, "LEADER", "object: name (trimmed)");
    checkBool(obj.active, "object: live");
    checkInt((long) obj.timestamp.type, (long) aprs::TimestampType::DhmZulu, "object: timestamp type");
    checkInt(obj.timestamp.day, 9, "object: timestamp day");
    checkInt(obj.timestamp.hour, 23, "object: timestamp hour");
    checkNear(op.latitude, 49.0583, 0.001, "object: latitude");
    checkNear(op.longitude, -72.0292, 0.001, "object: longitude");
    checkInt(op.symbol, '>', "object: symbol");

    // Killed object.
    aprs::decode("N0CALL>APRS:;LEADER   _092345z4903.50N/07201.75W>", lite);
    aprs::ObjectItem dead;
    aprs::Position dp;
    aprs::decodeObjectItem(lite, dead, dp);
    checkBool(!dead.active, "object: killed -> inactive");

    // Item.
    aprs::decode("N0CALL>APRS:)ABC!4903.50N/07201.75W>", lite);
    checkInt((long) lite.type, (long) aprs::PacketType::Item, "item: type Item");
    aprs::ObjectItem item;
    aprs::Position ip;
    checkBool(aprs::decodeObjectItem(lite, item, ip), "item: decode ok");
    checkStr(item.name, "ABC", "item: name");
    checkBool(item.active, "item: live");
    checkNear(ip.latitude, 49.0583, 0.001, "item: latitude");

    // Status, plain text.
    aprs::decode("N0CALL>APRS:>Net control tonight", lite);
    checkInt((long) lite.type, (long) aprs::PacketType::Status, "status: type Status");
    char status[64];
    checkBool(aprs::decodeStatus(lite, status, sizeof status), "status: decode ok");
    checkStr(status, "Net control tonight", "status: text");

    // Status with a leading timestamp.
    aprs::decode("N0CALL>APRS:>092345zNet is up", lite);
    char status2[64];
    aprs::decodeStatus(lite, status2, sizeof status2);
    checkStr(status2, "Net is up", "status: timestamp stripped");
}

void testDecodeQuery() {
    section("Query decoding (RX)");

    aprs::PacketLite lite;
    aprs::Query q;

    // General (broadcast) query, no argument.
    aprs::decode("N0CALL>APRS:?APRS?", lite);
    checkInt((long) lite.type, (long) aprs::PacketType::Query, "general: type Query");
    checkBool(aprs::decodeQuery(lite, q), "general: decode ok");
    checkStr(q.destination, "", "general: no addressee");
    checkStr(q.type, "APRS", "general: query type");
    checkStr(q.argument, "", "general: no argument");

    // General query with a non-standard trailing argument.
    aprs::decode("N0CALL>APRS:?WX? 38400", lite);
    checkBool(aprs::decodeQuery(lite, q), "general with argument: decode ok");
    checkStr(q.type, "WX", "general with argument: query type");
    checkStr(q.argument, "38400", "general with argument: opaque argument text");

    // Directed query (message envelope).
    aprs::decode("N0CALL>APRS::F4HVV-9  :?APRSD?", lite);
    checkInt((long) lite.type, (long) aprs::PacketType::Query, "directed: type Query");
    checkBool(aprs::decodeQuery(lite, q), "directed: decode ok");
    checkStr(q.destination, "F4HVV-9", "directed: addressee (trimmed)");
    checkStr(q.type, "APRSD", "directed: query type");
    checkStr(q.argument, "", "directed: no argument");

    // A plain message containing '?' must NOT be misclassified as a query.
    aprs::decode("N0CALL>APRS::F4HVV-9  :are you there?", lite);
    checkInt((long) lite.type, (long) aprs::PacketType::Message, "plain message with '?' stays Message");

    // A plain message merely containing a telemetry keyword must NOT be
    // misclassified as telemetry metadata (the keyword must be anchored right
    // after the addressee, followed by '.').
    aprs::decode("N0CALL>APRS::F4HVV-9  :check UNIT 5 please", lite);
    checkInt((long) lite.type, (long) aprs::PacketType::Message, "message containing 'UNIT' stays Message");
    aprs::decode("N0CALL>APRS::F4HVV-9  :bad HABITS today", lite);
    checkInt((long) lite.type, (long) aprs::PacketType::Message, "message containing 'HABITS' stays Message");
    aprs::decode("N0CALL>APRS::F4HVV-9  :see report T#42 please", lite);
    checkInt((long) lite.type, (long) aprs::PacketType::Message, "message containing 'T#' stays Message");
}

void testDecodeTelemetry() {
    section("Telemetry decoding (RX)");

    // ch.13 data report example.
    aprs::PacketLite lite;
    aprs::decode("N0QBF>APRS:T#005,199,000,255,073,123,01101001", lite);
    checkInt((long) lite.type, (long) aprs::PacketType::Telemetry, "T#: type Telemetry");
    aprs::Telemetry t;
    checkBool(aprs::decodeTelemetry(lite, t), "T#: decodeTelemetry ok");
    checkInt(t.sequenceNumber, 5, "T#: sequence");
    checkNear(t.analog[0].value, 199, 0.001, "T#: analog 1");
    checkNear(t.analog[4].value, 123, 0.001, "T#: analog 5");
    checkInt(t.boolean[1].value != 0, 1, "T#: bit 2 set");
    checkInt(t.boolean[0].value != 0, 0, "T#: bit 1 clear");
    checkInt(t.boolean[7].value != 0, 1, "T#: bit 8 set");

    // PARM definition.
    aprs::decode("N0QBF>APRS::N0QBF-11 :PARM.Battery,Btemp,ATemp,Pres,Alt,Camra,Chut,Sun", lite);
    checkInt((long) lite.type, (long) aprs::PacketType::TelemetryLabel, "PARM: type TelemetryLabel");
    aprs::Telemetry parm;
    checkBool(aprs::decodeTelemetry(lite, parm), "PARM: decodeTelemetry ok");
    checkStr(parm.analog[0].name, "Battery", "PARM: analog 1 name");
    checkStr(parm.boolean[0].name, "Camra", "PARM: bit 1 name");

    // EQNS definition.
    aprs::decode("N0QBF>APRS::N0QBF-11 :EQNS.0,5.2,0,0,.53,-32,3,4.39,49,-32,3,18,1,2,3", lite);
    aprs::Telemetry eqns;
    aprs::decodeTelemetry(lite, eqns);
    checkNear(eqns.analog[0].equation.b, 5.2, 0.001, "EQNS: analog 1 coefficient b");
    checkNear(eqns.analog[1].equation.c, -32, 0.001, "EQNS: analog 2 coefficient c");

    // Compressed (base-91) telemetry carried inside a position report (round-trip).
    aprs::Position position;
    position.latitude = 49.5;
    position.longitude = -72.75;
    aprs::Telemetry telemetry;
    telemetry.sequenceNumber = 3;
    telemetry.analog[0].value = 123;
    telemetry.analog[1].value = 456;
    telemetry.boolean[0].value = 1;
    telemetry.boolean[2].value = 1;
    char out[aprs::kMaxPacketLength + 1];
    aprs::encodePosition("N0CALL-9", "APRS", nullptr, position, nullptr, &telemetry, nullptr, out, sizeof out);
    checkContains(out, "|", "compressed telemetry block present in position");

    aprs::PacketLite litp;
    aprs::decode(out, litp);
    aprs::Telemetry ct;
    checkBool(aprs::decodeTelemetry(litp, ct), "compressed telemetry: decode ok");
    checkInt(ct.sequenceNumber, 3, "compressed telemetry: sequence");
    checkNear(ct.analog[0].value, 123, 0.001, "compressed telemetry: analog 1");
    checkNear(ct.analog[1].value, 456, 0.001, "compressed telemetry: analog 2");
    checkInt(ct.boolean[0].value != 0, 1, "compressed telemetry: bit 1 set");
    checkInt(ct.boolean[1].value != 0, 0, "compressed telemetry: bit 2 clear");
    checkInt(ct.boolean[2].value != 0, 1, "compressed telemetry: bit 3 set");
}

// Cases taken verbatim from the APRS Protocol Reference 1.0.1 (APRS101).
void testSpecExamples() {
    section("APRS101 spec examples");

    // ch.9 compressed position example: 49.5N, 72.75W, course 88, speed 36.2 kt,
    // car symbol '>'. Spec field: /5L!!<*e7>7P[ (we vary only the trailing
    // compression-origin byte, which is implementation-defined).
    aprs::Position position;
    position.overlay = '/';
    position.symbol = '>';
    position.latitude = 49.5;
    position.longitude = -72.75;
    position.courseDeg = 88;
    position.speedKnots = 36.2;
    position.altitudeFeet = 0;
    char out[aprs::kMaxPacketLength + 1];
    aprs::encodePosition("TEST", "APRS", nullptr, position, nullptr, nullptr, nullptr, out, sizeof out);
    checkContains(out, "/5L!!<*e7>7P", "ch.9 compressed position/course/speed example");

    // ch.13 telemetry data example: T#005,199,000,255,073,123,01101001
    aprs::Telemetry telemetry;
    telemetry.legacy = true;
    telemetry.sequenceNumber = 5;
    const double analog[5] = {199, 0, 255, 73, 123};
    for (int i = 0; i < 5; ++i) telemetry.analog[i].value = analog[i];
    const int bits[8] = {0, 1, 1, 0, 1, 0, 0, 1};  // -> "01101001"
    for (int i = 0; i < 8; ++i) telemetry.boolean[i].value = bits[i];
    aprs::encodeTelemetryData("N0QBF-11", "APRS", nullptr, telemetry, nullptr, out, sizeof out);
    checkContains(out, "T#005,199,000,255,073,123,01101001", "ch.13 telemetry data example");

    // ch.13 telemetry definition messages, addressee padded to 9 chars.
    aprs::PacketLite lite;

    aprs::decode("N0QBF>APRS::N0QBF-11 :PARM.Battery,Btemp,ATemp,Pres,Alt,Camra,Chut,Sun", lite);
    checkInt((long) lite.type, (long) aprs::PacketType::TelemetryLabel, "PARM classified as TelemetryLabel");

    aprs::decode("N0QBF>APRS::N0QBF-11 :BITS.10110000,N0QBF's Big Balloon", lite);
    checkInt((long) lite.type, (long) aprs::PacketType::TelemetryBitSense, "BITS classified as TelemetryBitSense");

    // ch.14 message examples.
    aprs::decode("WU2Z>APRS::WU2Z     :Testing{003", lite);
    checkInt((long) lite.type, (long) aprs::PacketType::Message, "message classified as Message");
    aprs::Message m;
    aprs::decodeMessage(lite, m);
    checkStr(m.destination, "WU2Z", "ch.14 message addressee (trimmed)");
    checkStr(m.message, "Testing", "ch.14 message body");
    checkStr(m.ackToConfirm, "003", "ch.14 message line number {003");

    // ch.14 ACK example: :KB2ICI-14:ack003
    aprs::decode("WU2Z>APRS::KB2ICI-14:ack003", lite);
    aprs::Message ack;
    aprs::decodeMessage(lite, ack);
    checkStr(ack.destination, "KB2ICI-14", "ch.14 ACK addressee");
    checkStr(ack.ackConfirmed, "003", "ch.14 ACK number");
    checkStr(ack.message, "", "ch.14 ACK has no body");
}

struct DigiCase {
    const char* path;
    const char* myCall;
    const char* expected;
};

void testDigipeating() {
    section("canBeDigipeated");

    const DigiCase cases[] = {
        {"", "F4HVV-9", ""},
        {"WIDE1-1", "F4HVV-9", "F4HVV-9*"},
        {"WIDE1-1,RFONLY", "F4HVV-9", "F4HVV-9*,RFONLY"},
        {"F4HVV-10*,WIDE1-1", "F4HVV-9", "F4HVV-10,F4HVV-9*"},
        {"F4HVV-10*,WIDE2-2", "F4HVV-9", "F4HVV-10,F4HVV-9*,WIDE2-1"},
        {"F4HVV-10*,WIDE2-1", "F4HVV-9", "F4HVV-10,F4HVV-9*"},
        {"WIDE1-2", "F4HVV-9", "WIDE1-2"},        // fill-in N>=2 is invalid -> not digipeated
        {"WIDE2-2", "F4HVV-9", "F4HVV-9*,WIDE2-1"},
        {"WIDE2-1", "F4HVV-9", "F4HVV-9*"},
        {"F4HVV-10*,WIDE1-1,WIDE2-1", "F4HVV-9", "F4HVV-10,F4HVV-9*,WIDE2-1"},
        {"F4HVV-10*,WIDE1-1,WIDE2-2", "F4HVV-9", "F4HVV-10,F4HVV-9*,WIDE2-2"},
        {"F4HVV-9", "F4HVV-9", "F4HVV-9*"},
        {"F4HVV-9*", "F4HVV-9", "F4HVV-9*"},
        {"F4HVV-9*,F4HVV-10", "F4HVV-9", "F4HVV-9*,F4HVV-10"},
        {"F4HVV-10*,F4HVV-9", "F4HVV-9", "F4HVV-10,F4HVV-9*"},
        {"WIDE3-3", "F4HVV-9", "F4HVV-9*,WIDE3-2"},
        {"TCPIP*", "F4HVV-9", "TCPIP*"},          // already used up -> unchanged
        {"WIDE2", "F4HVV-9", "WIDE2"},            // N=0 -> not digipeated
        {"FOO-1,WIDE1-1", "F4HVV-9", "FOO-1,WIDE1-1"},  // first unused is not us
    };

    char path[aprs::kPathLength + 1];
    char label[160];
    for (const auto& c : cases) {
        std::strcpy(path, c.path);
        aprs::canBeDigipeated(path, sizeof path, c.myCall);
        std::snprintf(label, sizeof label, "digi [%s] by %s", c.path, c.myCall);
        checkStr(path, c.expected, label);
    }
}

// Examples taken verbatim from "APRS Digipeater Algorithm" (WB2OSZ, 2024-2025).
void testDigipeaterSpec() {
    section("Digipeater algorithm — spec examples (WB2OSZ)");

    char path[aprs::kPathLength + 1];

    // §2.1 manual routing: the '*' marker moves to the last used hop.
    std::strcpy(path, "N2GH,W2UB");
    aprs::canBeDigipeated(path, sizeof path, "N2GH");
    checkStr(path, "N2GH*,W2UB", "manual routing: N2GH relays");

    std::strcpy(path, "N2GH*,W2UB");
    aprs::canBeDigipeated(path, sizeof path, "W2UB");
    checkStr(path, "N2GH,W2UB*", "manual routing: W2UB relays, marker moves");

    // §3.1 WIDE3-3 chain decrement.
    std::strcpy(path, "WIDE3-3");
    aprs::canBeDigipeated(path, sizeof path, "WW1ABC");
    checkStr(path, "WW1ABC*,WIDE3-2", "WIDE3-3 -> WW1ABC*,WIDE3-2");

    std::strcpy(path, "WW1ABC*,WIDE3-2");
    aprs::canBeDigipeated(path, sizeof path, "WW2DEF");
    checkStr(path, "WW1ABC,WW2DEF*,WIDE3-1", "WIDE3-2 -> insert WW2DEF*, decrement");

    std::strcpy(path, "WW2DEF*,WIDE3-1");
    aprs::canBeDigipeated(path, sizeof path, "W3GHI");
    checkStr(path, "WW2DEF,W3GHI*", "WIDE3-1 -> replaced by W3GHI*");

    // §4.3.2 examples.
    std::strcpy(path, "WIDE2-2");
    aprs::canBeDigipeated(path, sizeof path, "WB2OSZ");
    checkStr(path, "WB2OSZ*,WIDE2-1", "§4.3.2(a) WIDE2-2 -> WB2OSZ*,WIDE2-1");

    std::strcpy(path, "WIDE2-1");
    aprs::canBeDigipeated(path, sizeof path, "WB2OSZ");
    checkStr(path, "WB2OSZ*", "§4.3.2(b) WIDE2-1 -> WB2OSZ*");

    // §2.2 alias: the alias is replaced by the digipeater callsign.
    const char* aliases[] = {"EOC-1", "TEST"};
    aprs::DigipeaterOptions opt;
    opt.aliases = aliases;
    opt.aliasCount = 2;
    std::strcpy(path, "EOC-1");
    bool relayed = aprs::canBeDigipeated(path, sizeof path, "KB1MKZ", opt);
    checkBool(relayed, "alias EOC-1 is eligible");
    checkStr(path, "KB1MKZ*", "alias EOC-1 replaced by KB1MKZ*");

    // §2.3 / Golden Packet: a non-WIDE generic prefix (HOP).
    const char* prefixes[] = {"HOP"};
    aprs::DigipeaterOptions hop;
    hop.prefixes = prefixes;
    hop.prefixCount = 1;
    std::strcpy(path, "HOP7-7");
    aprs::canBeDigipeated(path, sizeof path, "WB2OSZ", hop);
    checkStr(path, "WB2OSZ*,HOP7-6", "generic HOP7-7 -> WB2OSZ*,HOP7-6");

    // A HOP path is NOT matched by the default (WIDE-only) configuration.
    std::strcpy(path, "HOP7-7");
    relayed = aprs::canBeDigipeated(path, sizeof path, "WB2OSZ");
    checkBool(!relayed, "HOP not relayed without a HOP prefix configured");
}

struct HeardCase {
    const char* path;
    const char* expected;
    uint8_t count;
};

void testLastHeard() {
    section("lastDigipeater");

    const HeardCase cases[] = {
        {"", "", 0},
        {"WIDE1-1", "", 0},
        {"F4HVV-10*,WIDE1-1", "F4HVV-10", 1},
        {"F4HVV-9*", "F4HVV-9", 1},
        {"F4HVV-9*,F4HVV-10", "F4HVV-9", 1},
        {"F4HVV-9,F4HVV-10*", "F4HVV-10", 2},
        {"F4HVV-9,F4HVV-10,F4HVV-11*", "F4HVV-11", 3},
    };

    char last[aprs::kCallsignLength + 1];
    char label[160];
    for (const auto& c : cases) {
        uint8_t n = aprs::lastDigipeater(c.path, last, sizeof last);
        std::snprintf(label, sizeof label, "last heard [%s]", c.path);
        checkStr(last, c.expected, label);
        std::snprintf(label, sizeof label, "hop count [%s]", c.path);
        checkInt(n, c.count, label);
    }
}

}  // namespace

int main() {
    std::printf("SimpleLibAprs test suite\n");

    testCompressedPosition();
    testPositionWithAltitude();
    testWeather();
    testMessageAndAck();
    testTelemetry();
    testObjectAndItem();
    testStatusAndRaw();
    testEncodeErrors();
    testUncompressedEncode();
    testTimestamp();
    testSpecExamples();
    testDecode();
    testThirdPartyHeader();
    testPacketLiteCopySafety();
    testDecodePosition();
    testPositionAmbiguity();
    testGridLocator();
    testDecodeWeather();
    testDecodeTelemetry();
    testDecodeObjectStatus();
    testDecodeQuery();
    testDigipeating();
    testDigipeaterSpec();
    testLastHeard();

    std::printf("\n----------------------------------------\n");
    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

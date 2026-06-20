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

// Encode helper returning the produced string by value-ish (into caller buffer).
aprs::Result encode(aprs::Packet& p, char* out) {
    return aprs::encode(p, out, aprs::kMaxPacketLength + 1);
}

// ---------------------------------------------------------------------------

void testCompressedPosition() {
    section("Compressed position (spec example 49.5N, 72.75W)");

    aprs::Packet p;
    std::strcpy(p.source, "TEST");
    std::strcpy(p.destination, "APRS");
    p.type = aprs::PacketType::Position;
    p.position.overlay = '/';
    p.position.symbol = '>';
    p.position.latitude = 49.5;
    p.position.longitude = -72.75;
    p.position.courseDeg = 0;
    p.position.speedKnots = 0;
    p.position.altitudeFeet = 0;
    p.position.altitudeInComment = true;

    char out[aprs::kMaxPacketLength + 1];
    encode(p, out);

    // Spec APRS101 ch.9 worked example: lat -> "5L!!", lon -> "<*e7".
    checkContains(out, "5L!!<*e7", "latitude/longitude base-91 match the spec example");
    checkStr(out, "TEST>APRS:=/5L!!<*e7>!!Y", "full compressed position frame");
}

void testPositionWithAltitude() {
    section("Position with /A= altitude");

    aprs::Packet p;
    std::strcpy(p.source, "F4HVV-15");
    std::strcpy(p.destination, "APRS");
    p.type = aprs::PacketType::Position;
    p.position.latitude = 45.325776;
    p.position.longitude = 5.636580;
    p.position.altitudeFeet = 2722;  // ~830 m
    p.position.altitudeInComment = true;

    char out[aprs::kMaxPacketLength + 1];
    encode(p, out);

    checkContains(out, "/A=002722", "altitude rendered as /A=002722");
}

void testWeather() {
    section("Compressed weather report (spec-compliant)");

    aprs::Packet p;
    std::strcpy(p.source, "F4HVV-15");
    std::strcpy(p.destination, "APRS");
    p.type = aprs::PacketType::Weather;
    p.position.latitude = 49.5;
    p.position.longitude = -72.75;
    p.weather.useGustSpeed = true;
    p.weather.gustSpeedMph = 5;
    p.weather.useTemperature = true;
    p.weather.temperatureFahrenheit = 77;
    p.weather.useHumidity = true;
    p.weather.humidity = 50;
    p.weather.usePressure = true;
    p.weather.pressure = 990;

    char out[aprs::kMaxPacketLength + 1];
    encode(p, out);

    // '_' weather symbol, then wind in cs bytes, then weather data from gust.
    checkContains(out, "5L!!<*e7_", "weather uses compressed position with '_' symbol");
    checkContains(out, "g005t077", "weather data starts at gust (no ddd/sss text)");
    checkContains(out, "h50b09900", "humidity and pressure encoded");
    checkBool(std::strstr(out, "/005g") == nullptr, "no uncompressed ddd/sss wind block");
}

void testMessageAndAck() {
    section("Message, ACK and REJ");

    aprs::Packet p;
    std::strcpy(p.source, "N0CALL-9");
    std::strcpy(p.destination, "APRS");
    std::strcpy(p.message.destination, "NOCALL-4");
    std::strcpy(p.message.message, "Hello, world!");
    p.type = aprs::PacketType::Message;

    char out[aprs::kMaxPacketLength + 1];

    encode(p, out);
    checkStr(out, "N0CALL-9>APRS::NOCALL-4 :Hello, world!", "plain message frame");

    std::strcpy(p.message.ackToAsk, "001");
    encode(p, out);
    checkStr(out, "N0CALL-9>APRS::NOCALL-4 :Hello, world!{001", "message with line number {001");

    aprs::Packet ackPkt;
    std::strcpy(ackPkt.source, "N0CALL-9");
    std::strcpy(ackPkt.destination, "APRS");
    std::strcpy(ackPkt.message.destination, "NOCALL-4");
    std::strcpy(ackPkt.message.ackToConfirm, "009");
    std::strcpy(ackPkt.message.message, "this body must be ignored");
    ackPkt.type = aprs::PacketType::Message;
    encode(ackPkt, out);
    checkStr(out, "N0CALL-9>APRS::NOCALL-4 :ack009", "ACK is a pure packet (no body, no trailing space)");

    std::strcpy(ackPkt.message.ackToConfirm, "");
    std::strcpy(ackPkt.message.ackToReject, "016");
    encode(ackPkt, out);
    checkStr(out, "N0CALL-9>APRS::NOCALL-4 :rej016", "REJ is a pure packet");
}

void testTelemetry() {
    section("Telemetry");

    aprs::Packet p;
    std::strcpy(p.source, "N0CALL-9");
    std::strcpy(p.destination, "APRS");
    p.telemetry.sequenceNumber = 7544;  // must wrap into a 3-digit field
    p.telemetry.analog[0].value = 1472;
    p.telemetry.analog[1].value = 1564;
    std::strcpy(p.telemetry.analog[0].name, "A1");
    std::strcpy(p.telemetry.analog[1].name, "A2");
    std::strcpy(p.telemetry.analog[0].unit, "V");
    p.telemetry.boolean[0].value = 1;

    char out[aprs::kMaxPacketLength + 1];

    p.type = aprs::PacketType::Telemetry;
    encode(p, out);
    checkContains(out, ":T#544,", "T# sequence wrapped to 3 digits (544)");
    checkContains(out, "T#544,1472,1564,", "analog values present");

    p.type = aprs::PacketType::TelemetryLabel;
    encode(p, out);
    checkStr(out, "N0CALL-9>APRS::N0CALL-9 :PARM.A1,A2,,,,,,,,,,,", "PARM frame with 9-char addressee");

    p.type = aprs::PacketType::TelemetryUnit;
    encode(p, out);
    checkContains(out, ":N0CALL-9 :UNIT.V,", "UNIT frame");

    p.type = aprs::PacketType::TelemetryBitSense;
    std::strcpy(p.telemetry.projectName, "Demo");
    encode(p, out);
    checkContains(out, ":N0CALL-9 :BITS.", "BITS frame");
    checkContains(out, ",Demo", "BITS project name appended");
}

void testObjectAndItem() {
    section("Object and Item");

    aprs::Packet p;
    std::strcpy(p.source, "F4HVV-15");
    std::strcpy(p.destination, "APRS");
    std::strcpy(p.item.name, "OBJITM");
    p.item.active = true;
    p.item.utcHour = 12;
    p.item.utcMinute = 34;
    p.item.utcSecond = 56;
    p.position.latitude = 49.5;
    p.position.longitude = -72.75;

    char out[aprs::kMaxPacketLength + 1];

    p.type = aprs::PacketType::Object;
    encode(p, out);
    checkContains(out, ";OBJITM   *123456h", "Object: 9-char name, '*' live, HMS 'h' suffix");

    p.type = aprs::PacketType::Item;
    encode(p, out);
    checkContains(out, ")OBJITM!", "Item: variable name, '!' live");
}

void testStatusAndRaw() {
    section("Status and Raw");

    aprs::Packet p;
    std::strcpy(p.source, "F4HVV-15");
    std::strcpy(p.destination, "APRS");
    std::strcpy(p.comment, "I'm good !");

    char out[aprs::kMaxPacketLength + 1];

    p.type = aprs::PacketType::Status;
    encode(p, out);
    checkStr(out, "F4HVV-15>APRS:>I'm good !", "Status frame");

    p.type = aprs::PacketType::Raw;
    std::strcpy(p.content, "anything goes here");
    encode(p, out);
    checkStr(out, "F4HVV-15>APRS:anything goes here", "Raw content passed through");
}

void testEncodeErrors() {
    section("Encode error handling");

    aprs::Packet p;
    p.type = aprs::PacketType::Position;  // missing source/destination
    char out[aprs::kMaxPacketLength + 1];
    checkInt((long) aprs::encode(p, out, sizeof out), (long) aprs::Result::InvalidArgument,
             "missing callsign -> InvalidArgument");

    std::strcpy(p.source, "TEST");
    std::strcpy(p.destination, "APRS");
    char tiny[8];
    aprs::Result r = aprs::encode(p, tiny, sizeof tiny);
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
}

void testUncompressedEncode() {
    section("Uncompressed position encoding (TX)");

    aprs::Packet p;
    std::strcpy(p.source, "TEST");
    std::strcpy(p.destination, "APRS");
    p.type = aprs::PacketType::Position;
    p.position.compressed = false;
    p.position.overlay = '/';
    p.position.symbol = '>';
    p.position.latitude = 45.332;
    p.position.longitude = 5.6192;
    p.position.courseDeg = 88;
    p.position.speedKnots = 36;

    char out[aprs::kMaxPacketLength + 1];
    encode(p, out);
    checkStr(out, "TEST>APRS:=4519.92N/00537.15E>088/036", "uncompressed position with course/speed");

    p.position.courseDeg = 0;
    p.position.speedKnots = 0;
    p.position.altitudeFeet = 2722;
    encode(p, out);
    checkContains(out, "=4519.92N/00537.15E>/A=002722", "uncompressed position with altitude");
}

void testTimestamp() {
    section("Position timestamp (TX + RX)");

    aprs::Packet p;
    std::strcpy(p.source, "TEST");
    std::strcpy(p.destination, "APRS");
    p.type = aprs::PacketType::Position;
    p.position.latitude = 49.5;
    p.position.longitude = -72.75;
    p.position.timestamp.type = aprs::TimestampType::Hms;
    p.position.timestamp.hour = 12;
    p.position.timestamp.minute = 34;
    p.position.timestamp.second = 56;

    char out[aprs::kMaxPacketLength + 1];
    encode(p, out);
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

void testDecodeWeather() {
    section("Weather decoding (RX, round-trip)");

    aprs::Packet p;
    std::strcpy(p.source, "F4HVV-15");
    std::strcpy(p.destination, "APRS");
    p.type = aprs::PacketType::Weather;
    p.position.latitude = 49.5;
    p.position.longitude = -72.75;
    p.weather.useGustSpeed = true;       p.weather.gustSpeedMph = 5;
    p.weather.useTemperature = true;     p.weather.temperatureFahrenheit = 77;
    p.weather.useRain1Hour = true;       p.weather.rain1HourHundredthsOfAnInch = 12;
    p.weather.useHumidity = true;        p.weather.humidity = 50;
    p.weather.usePressure = true;        p.weather.pressure = 990;

    char out[aprs::kMaxPacketLength + 1];
    encode(p, out);

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
    aprs::Packet pp;
    std::strcpy(pp.source, "N0CALL-9");
    std::strcpy(pp.destination, "APRS");
    pp.type = aprs::PacketType::Position;
    pp.position.latitude = 49.5;
    pp.position.longitude = -72.75;
    pp.position.withTelemetry = true;
    pp.telemetry.sequenceNumber = 3;
    pp.telemetry.analog[0].value = 123;
    pp.telemetry.analog[1].value = 456;
    pp.telemetry.boolean[0].value = 1;
    pp.telemetry.boolean[2].value = 1;
    char out[aprs::kMaxPacketLength + 1];
    encode(pp, out);
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
    aprs::Packet p;
    std::strcpy(p.source, "TEST");
    std::strcpy(p.destination, "APRS");
    p.type = aprs::PacketType::Position;
    p.position.overlay = '/';
    p.position.symbol = '>';
    p.position.latitude = 49.5;
    p.position.longitude = -72.75;
    p.position.courseDeg = 88;
    p.position.speedKnots = 36.2;
    p.position.altitudeFeet = 0;
    char out[aprs::kMaxPacketLength + 1];
    encode(p, out);
    checkContains(out, "/5L!!<*e7>7P", "ch.9 compressed position/course/speed example");

    // ch.13 telemetry data example: T#005,199,000,255,073,123,01101001
    aprs::Packet t;
    std::strcpy(t.source, "N0QBF-11");
    std::strcpy(t.destination, "APRS");
    t.type = aprs::PacketType::Telemetry;
    t.telemetry.legacy = true;
    t.telemetry.sequenceNumber = 5;
    const double analog[5] = {199, 0, 255, 73, 123};
    for (int i = 0; i < 5; ++i) t.telemetry.analog[i].value = analog[i];
    const int bits[8] = {0, 1, 1, 0, 1, 0, 0, 1};  // -> "01101001"
    for (int i = 0; i < 8; ++i) t.telemetry.boolean[i].value = bits[i];
    encode(t, out);
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
    testDecodePosition();
    testDecodeWeather();
    testDecodeTelemetry();
    testDecodeObjectStatus();
    testDigipeating();
    testDigipeaterSpec();
    testLastHeard();

    std::printf("\n----------------------------------------\n");
    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

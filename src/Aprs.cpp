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
// On a desktop build (compile with -DNATIVE) the PROGMEM helpers fall back to
// their standard library counterparts. On Arduino they come from <Arduino.h>.
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
        #define sscanf_P  sscanf
    #endif
#endif

namespace aprs {
namespace {

/// Conversion factor from miles per hour to knots (APRS compressed speed unit).
constexpr double kMphToKnots = 0.868976;
/// ln(1.08); compressed speed exponent base (speed = 1.08^s - 1).
constexpr double kSpeedLogBase = 0.07696;

// ---------------------------------------------------------------------------
// Bounded string builder
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
    b.overflow = (cap == 0);
    if (cap > 0) {
        buf[0] = '\0';
    }
}

/// Number of characters that can still be appended (excluding the NUL).
size_t bRemaining(const Builder& b) {
    return (b.len + 1 < b.cap) ? (b.cap - 1 - b.len) : 0;
}

/// Append a PROGMEM / string literal (no format specifiers).
void bAppendP(Builder& b, const char* pstr) {
    const size_t rem = bRemaining(b);
    if (strlen_P(pstr) > rem) {
        b.overflow = true;
    }
    if (rem == 0) {
        return;
    }
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
    va_list args;
    va_start(args, pfmt);
    const int n = vsnprintf_P(b.buf + b.len, rem + 1, pfmt, args);
    va_end(args);
    if (n < 0) {
        b.overflow = true;
        return;
    }
    if ((size_t) n > rem) {
        b.overflow = true;
    }
    b.len = strlen(b.buf);
}

// ---------------------------------------------------------------------------
// Small string utilities
// ---------------------------------------------------------------------------

/// Copy at most @p maxChars characters of @p src into @p dst, always NUL-terminating.
void copyN(char* dst, const char* src, size_t maxChars, size_t dstSize) {
    if (dstSize == 0) {
        return;
    }
    const size_t limit = (maxChars < dstSize - 1) ? maxChars : dstSize - 1;
    size_t i = 0;
    for (; i < limit && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/// Shift the string so that it starts at @p from (in-place, overlap-safe).
void shiftLeft(char* s, const char* from) {
    memmove(s, from, strlen(from) + 1);
}

void trimEnd(char* s) {
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char) s[n - 1])) {
        s[--n] = '\0';
    }
}

void trimStart(char* s) {
    const size_t n = strlen(s);
    size_t i = 0;
    while (i < n && isspace((unsigned char) s[i])) {
        ++i;
    }
    if (i > 0) {
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

/// Encode @p value as a Base-91 string of @p width characters.
char* base91(char* dst, uint8_t width, uint32_t value) {
    for (dst += width, *dst = '\0'; width; --width) {
        *(--dst) = (char) (value % 91 + 33);
        value /= 91;
    }
    return dst;
}

/// Pick the printf format for a double: integral values drop their decimals.
const char* doubleFormat(double value) {
    double integral;
    if (modf(value, &integral) != 0.0) {
        return PSTR("%.3f");
    }
    return PSTR("%.0f");
}

/// Pack the GPS fix / NMEA source / compression origin into the compression byte.
uint8_t compressionByte(GpsFix fix, NmeaSource nmea, Compression comp) {
    uint8_t value = 0;
    value |= ((uint8_t) fix & 0x01) << 5;
    value |= ((uint8_t) nmea & 0x03) << 3;
    value |= (uint8_t) comp & 0x07;
    return value;
}

/// Append the compressed course/speed byte pair plus the compression-type byte.
void appendCourseSpeedBytes(Builder& b, uint32_t courseDeg, double speedKnots, NmeaSource nmea) {
    char base[2];
    base91(base, 1, courseDeg / 4);
    bAppend(b, base);
    // Round to nearest (spec encodes 36.2 kt as s=47, not 46).
    base91(base, 1, (uint32_t) (log1p(speedKnots) / kSpeedLogBase + 0.5));
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
        base91(base, 2, (uint32_t) (log(pos.altitudeFeet) / log(1.002)));
        bAppend(b, base);
        bAppendChar(b, (char) (compressionByte(GpsFix::Current, NmeaSource::Gga, Compression::Compressed) + 33));
    }
}

/// Append an uncompressed position: DDMM.mmN/DDDMM.mmW + symbol + extensions.
void appendPositionUncompressed(const Position& pos, const Weather* weather, Builder& b) {
    double absLat = pos.latitude < 0 ? -pos.latitude : pos.latitude;
    int latDeg = (int) absLat;
    int latHund = (int) ((absLat - latDeg) * 6000.0 + 0.5);  // hundredths of a minute
    if (latHund >= 6000) { latHund -= 6000; ++latDeg; }
    bFormatP(b, PSTR("%02u%02u.%02u%c"), (unsigned) latDeg, (unsigned) (latHund / 100),
             (unsigned) (latHund % 100), pos.latitude < 0 ? 'S' : 'N');

    bAppendChar(b, pos.overlay);

    double absLon = pos.longitude < 0 ? -pos.longitude : pos.longitude;
    int lonDeg = (int) absLon;
    int lonHund = (int) ((absLon - lonDeg) * 6000.0 + 0.5);
    if (lonHund >= 6000) { lonHund -= 6000; ++lonDeg; }
    bFormatP(b, PSTR("%03u%02u.%02u%c"), (unsigned) lonDeg, (unsigned) (lonHund / 100),
             (unsigned) (lonHund % 100), pos.longitude < 0 ? 'W' : 'E');

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
            const uint16_t seq = (t.sequenceNumber > 0x1FFF) ? 0 : t.sequenceNumber;

            bAppendP(b, PSTR("|"));
            base91(base, 2, seq);
            bAppend(b, base);

            for (const auto& channel : t.analog) {
                base91(base, 2, (uint16_t) fabs(channel.value));
                bAppend(b, base);
            }

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
                    bFormatP(b, doubleFormat(channel.value), channel.value);
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
                bFormatP(b, doubleFormat(equation.a), equation.a);
                bAppendP(b, PSTR(","));
                bFormatP(b, doubleFormat(equation.b), equation.b);
                bAppendP(b, PSTR(","));
                bFormatP(b, doubleFormat(equation.c), equation.c);
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

/// Decode a Base-91 string of @p width characters back to an integer.
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

/// Parse a compressed position field. Returns the pointer past the 13-byte field.
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
const char* parsePositionUncompressed(const char* pos, Position& out) {
    out.compressed = false;

    const int latDeg = parseDigits(pos, 2);
    const double latMin = parseDigits(pos + 2, 2) + parseDigits(pos + 5, 2) / 100.0;
    out.latitude = latDeg + latMin / 60.0;
    if (pos[7] == 'S' || pos[7] == 's') {
        out.latitude = -out.latitude;
    }

    out.overlay = pos[8];

    const int lonDeg = parseDigits(pos + 9, 3);
    const double lonMin = parseDigits(pos + 12, 2) + parseDigits(pos + 15, 2) / 100.0;
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

bool decode(const char* raw, PacketLite& out) {
    reset(out);

    if (raw == nullptr) {
        return false;
    }

    if (sscanf_P(raw, PSTR("%11[^>]>%11[^,],%88[^:]:"),
                 out.source, out.destination, out.path) != 3) {
        if (sscanf_P(raw, PSTR("%11[^>]>%11[^:]:"),
                     out.source, out.destination) != 2) {
            return false;
        }
    }

    copyN(out.raw, raw, kMaxPacketLength, sizeof out.raw);
    trim(out.raw);

    out.digipeaterCount = lastDigipeater(out.path, out.lastDigipeaterCallsignInPath,
                                         sizeof out.lastDigipeaterCallsignInPath);

    const char* colon = strchr(raw, ':');
    if (colon == nullptr) {
        return true;
    }

    copyN(out.content, colon + 1, kMaxPacketLength, sizeof out.content);

    const char first = out.content[0];
    if (first == '=' || first == '!' || first == '/' || first == '@') {
        out.type = PacketType::Position;
        // Locate the symbol code; a '_' there means it is a weather report.
        const char* pos = (first == '/' || first == '@') ? out.content + 8 : out.content + 1;
        const bool uncompressed = isdigit((unsigned char) pos[0]);
        const size_t needed = uncompressed ? 19 : 13;
        if (strlen(pos) >= needed && (uncompressed ? pos[18] : pos[9]) == '_') {
            out.type = PacketType::Weather;
        }
    } else if (first == '_') {
        out.type = PacketType::Weather;
    } else if (first == '>') {
        out.type = PacketType::Status;
    } else if (first == ';') {
        out.type = PacketType::Object;
    } else if (first == ')') {
        out.type = PacketType::Item;
    } else if (first == 'T' && out.content[1] == '#') {
        out.type = PacketType::Telemetry;  // bare "T#..." data report
    } else if (first == ':') {
        // Message-format payload: distinguish a plain message from telemetry metadata.
        out.type = PacketType::Message;
        const char* body = out.content;
        if (strstr_P(body, PSTR("BITS")) != nullptr) out.type = PacketType::TelemetryBitSense;
        else if (strstr_P(body, PSTR("EQNS")) != nullptr) out.type = PacketType::TelemetryEquation;
        else if (strstr_P(body, PSTR("PARM")) != nullptr) out.type = PacketType::TelemetryLabel;
        else if (strstr_P(body, PSTR("UNIT")) != nullptr) out.type = PacketType::TelemetryUnit;
        else if (strstr_P(body, PSTR("T#")) != nullptr) out.type = PacketType::Telemetry;
    }

    return true;
}

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
        trimEnd(out.destination);
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
        *brace = '\0';
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
    const char* bar = strchr(in.content, '|');
    if (bar != nullptr && strlen(bar) >= 1 + (2 + 2 * kMaxTelemetryAnalog + 2)) {
        const char* d = bar + 1;
        out.sequenceNumber = (uint16_t) base91Decode(d, 2);
        d += 2;
        for (int i = 0; i < (int) kMaxTelemetryAnalog; ++i) {
            out.analog[i].value = base91Decode(d, 2);
            d += 2;
        }
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

namespace {

/// One address parsed from a digipeater path.
struct PathAddr {
    char text[kCallsignLength + 1];  ///< Address without the '*' marker.
    bool used;                       ///< Whether it carried a '*'.
};

/// Split @p path on ',' into @p addrs. Returns the number of addresses parsed.
int parsePath(const char* path, PathAddr* addrs, int maxAddrs) {
    int count = 0;
    const char* p = path;
    while (*p != '\0' && count < maxAddrs) {
        const char* comma = strchr(p, ',');
        size_t len = comma ? (size_t) (comma - p) : strlen(p);
        bool used = false;
        if (len > 0 && p[len - 1] == '*') {
            used = true;
            --len;
        }
        copyN(addrs[count].text, p, len, sizeof addrs[count].text);
        addrs[count].used = used;
        ++count;
        if (!comma) {
            break;
        }
        p = comma + 1;
    }
    return count;
}

/// Match @p addr against a generic XXXn-N alias. Returns true and fills the
/// prefix, role digit @p n and remaining hop count @p hops when it matches.
bool parseGeneric(const char* addr, const char* const* prefixes, int prefixCount,
                  char* prefixOut, size_t prefixOutSize, int* n, int* hops) {
    for (int i = 0; i < prefixCount; ++i) {
        const char* prefix = prefixes[i];
        const size_t prefixLen = strlen(prefix);
        if (strncmp(addr, prefix, prefixLen) != 0) {
            continue;
        }
        const char* rest = addr + prefixLen;
        if (!isdigit((unsigned char) rest[0])) {
            continue;
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

bool canBeDigipeated(char* path, size_t pathSize, const char* myCall,
                     const DigipeaterOptions& options) {
    if (path == nullptr || myCall == nullptr || pathSize == 0) {
        return false;
    }

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
        return false;
    }

    // §4.1(a): the first unused address sits right after the last used one.
    int firstUnused = 0;
    for (int i = 0; i < count; ++i) {
        if (addrs[i].used) {
            firstUnused = i + 1;
        }
    }
    if (firstUnused >= count) {
        return false;  // No unused address left.
    }

    const char* addr = addrs[firstUnused].text;

    enum Mode { None, Replace, Generic };
    Mode mode = None;
    char prefix[kCallsignLength + 1] = {};
    int role = 0;
    int hops = 0;

    if (strcmp(addr, myCall) == 0) {  // §4.1(b)
        mode = Replace;
    } else {                          // §4.1(c) aliases
        for (int i = 0; i < options.aliasCount; ++i) {
            if (options.aliases[i] != nullptr && strcmp(addr, options.aliases[i]) == 0) {
                mode = Replace;
                break;
            }
        }
        if (mode == None && parseGeneric(addr, prefixes, prefixCount,  // §4.1(d)
                                         prefix, sizeof prefix, &role, &hops)) {
            if (hops == 0) {
                return false;  // §4.3.2(c): used up.
            }
            if (role == 1 && hops >= 2) {
                return false;  // A "fill-in" (XXX1) relays a single hop; XXX1-N (N>=2) is invalid.
            }
            mode = Generic;
        }
    }
    if (mode == None) {
        return false;  // §4.1(e)
    }

    // §4.3: rebuild the path, keeping only the new last hop marked with '*'.
    char buffer[kPathLength + 1];
    Builder b;
    bInit(b, buffer, sizeof buffer);

    for (int i = 0; i < firstUnused; ++i) {  // earlier hops, markers cleared
        if (b.len) {
            bAppendChar(b, ',');
        }
        bAppend(b, addrs[i].text);
    }

    if (b.len) {
        bAppendChar(b, ',');
    }
    bAppend(b, myCall);
    bAppendChar(b, '*');

    if (mode == Generic && hops >= 2) {  // §4.3.2(a): keep alias, decrement N
        bAppendChar(b, ',');
        bFormatP(b, PSTR("%s%d-%d"), prefix, role, hops - 1);
    }

    for (int i = firstUnused + 1; i < count; ++i) {  // remaining unused hops
        bAppendChar(b, ',');
        bAppend(b, addrs[i].text);
    }

    copyN(path, buffer, pathSize - 1, pathSize);
    return true;
}

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
        return 0;
    }

    const char* lastComma = strrchr(path, ',');
    if (lastComma != nullptr && lastComma < lastStar) {
        copyN(out, lastComma + 1, (size_t) (lastStar - lastComma - 1), outSize);
        return (uint8_t) (countChar(path, (size_t) (lastStar - path), ',') + 1);
    }

    copyN(out, path, (size_t) (lastStar - path), outSize);
    return 1;
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

/**
 * @file Aprs.h
 * @brief Lightweight APRS (Automatic Packet Reporting System) encoder/decoder
 *        for embedded systems.
 *
 * SimpleLibAprs builds and parses APRS packets without any dynamic memory
 * allocation: every buffer is caller-provided or fixed-size, which makes the
 * library suitable for microcontrollers with very little RAM (AVR, ESP, RP2040,
 * STM32, ...). It also compiles natively (define @c NATIVE) for desktop testing.
 *
 * The public API lives in the @ref aprs namespace.
 *
 * @copyright MIT License — see the LICENSE file.
 */

#ifndef SIMPLELIBAPRS_APRS_H
#define SIMPLELIBAPRS_APRS_H

// The plain C headers (<stddef.h>/<stdint.h>), not their C++ "cstddef"/
// "cstdint" wrappers: the AVR toolchain bundled with PlatformIO's atmelavr
// platform ships avr-libc (a C library) but no libstdc++ at all, so
// <cstddef>/<cstdint> don't exist there and including them is a hard compile
// error on real Arduino Uno hardware. size_t/uint8_t/uint16_t/... land in the
// global namespace either way, which is what this header uses throughout
// (never std::size_t), so the plain C headers are a strict improvement here:
// available on every target this library supports, AVR included.
#include <stddef.h>
#include <stdint.h>

namespace aprs {

// ---------------------------------------------------------------------------
// Compile-time limits
//
// Buffers declared with these constants always reserve one extra byte for the
// terminating NUL (e.g. a callsign buffer is `char[kCallsignLength + 1]`).
// ---------------------------------------------------------------------------

/// Maximum length of an encoded APRS frame (excluding the terminating NUL).
constexpr size_t kMaxPacketLength = 256;
/// Maximum length of a callsign including its SSID, e.g. "N0CALL-15".
constexpr size_t kCallsignLength = 11;
/// Maximum number of digipeater hops handled in a path.
constexpr size_t kMaxPath = 8;
/// Maximum length of a full digipeater path string.
constexpr size_t kPathLength = kCallsignLength * kMaxPath;
/// Number of analog telemetry channels defined by the APRS specification.
constexpr size_t kMaxTelemetryAnalog = 5;
/// Number of boolean (bit) telemetry channels defined by the APRS specification.
constexpr size_t kMaxTelemetryBoolean = 8;
/// Maximum length of a telemetry channel name.
constexpr size_t kTelemetryNameLength = 8;
/// Maximum length of a telemetry channel unit.
constexpr size_t kTelemetryUnitLength = 8;
/// Maximum length of a telemetry project name.
constexpr size_t kTelemetryProjectNameLength = 24;
/// Maximum length of a message body.
constexpr size_t kMessageLength = 200;
/// Length of an APRS message acknowledgement identifier.
constexpr size_t kAckMessageLength = 4;
/// Maximum length of a query type keyword, e.g. "APRSD", "IGATE", "WX".
constexpr size_t kQueryTypeLength = 8;

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

/// Kind of APRS payload carried by a packet.
enum class PacketType : uint8_t {
    Unknown,            ///< Not recognised / not set.
    Position,           ///< Position report (optionally with weather/telemetry).
    Message,            ///< Text message, possibly with ACK handling.
    Telemetry,          ///< Telemetry data report (T# frame).
    TelemetryUnit,      ///< Telemetry UNIT metadata frame.
    TelemetryLabel,     ///< Telemetry PARM (parameter name) metadata frame.
    TelemetryEquation,  ///< Telemetry EQNS (equation) metadata frame.
    TelemetryBitSense,  ///< Telemetry BITS (bit sense) metadata frame.
    Weather,            ///< Weather report.
    Item,               ///< APRS item.
    Object,             ///< APRS object.
    Status,             ///< Status text.
    Query,              ///< Query request (general or directed), APRS101 ch.15.
    Raw                 ///< Caller-provided raw content, emitted verbatim.
};

/// Age of the GPS fix encoded in the compression byte.
enum class GpsFix : uint8_t {
    Old = 0,
    Current = 1
};

/// Origin of the position data encoded in the compression byte.
enum class NmeaSource : uint8_t {
    Other = 0,
    Gll = 1,
    Gga = 2,
    Rmc = 3
};

/// Compression origin encoded in the compression byte.
enum class Compression : uint8_t {
    Compressed = 0,
    TncBText = 1,
    Software = 2,
    Tbd1 = 3,
    Kpc3 = 4,
    Pico = 5,
    OtherTrackerTbd2 = 6,
    DigipeaterConversion = 7
};

/// Result of an encode operation.
enum class Result : uint8_t {
    Ok = 0,             ///< Operation succeeded.
    InvalidArgument,    ///< A null pointer or empty mandatory field was supplied.
    BufferTooSmall,     ///< The output buffer was too small for the frame.
    UnsupportedType     ///< The packet type cannot be encoded.
};

// ---------------------------------------------------------------------------
// Payload structures
// ---------------------------------------------------------------------------

/// Weather observation. Each field is emitted only when its `useX` flag is set.
struct Weather {
    uint16_t windDirectionDegrees = 0;                  ///< Wind direction in degrees.
    uint16_t windSpeedMph = 0;                          ///< Sustained wind speed in mph.
    uint16_t gustSpeedMph = 0;                          ///< Gust speed in mph.
    int16_t temperatureFahrenheit = 0;                  ///< Temperature in °F.
    uint16_t rain1HourHundredthsOfAnInch = 0;           ///< Rain over the last hour (1/100 inch).
    uint16_t rain24HourHundredthsOfAnInch = 0;          ///< Rain over the last 24 h (1/100 inch).
    uint16_t rainSinceMidnightHundredthsOfAnInch = 0;   ///< Rain since midnight (1/100 inch).
    uint8_t humidity = 0;                               ///< Relative humidity (0-99 %).
    uint16_t pressure = 0;                              ///< Barometric pressure in mbar (encoded x10).

    bool useWindDirection = false;
    bool useWindSpeed = false;
    bool useGustSpeed = false;
    bool useTemperature = false;
    bool useRain1Hour = false;
    bool useRain24Hour = false;
    bool useRainSinceMidnight = false;
    bool useHumidity = false;
    bool usePressure = false;
};

/// Quadratic conversion applied to an analog telemetry value: a·x² + b·x + c.
struct TelemetryEquation {
    double a = 0;
    double b = 0;
    double c = 0;
};

/// An analog telemetry channel: value, name/unit labels, and the quadratic
/// conversion applied to it (APRS101 ch.13: PARM/UNIT/EQNS).
struct AnalogChannel {
    char name[kTelemetryNameLength + 1]{};  ///< Channel name (PARM).
    double value = 0;                       ///< Current value.
    char unit[kTelemetryUnitLength + 1]{};  ///< Channel unit (UNIT).
    TelemetryEquation equation{};           ///< Conversion equation (EQNS).
};

/// A boolean (bit) telemetry channel: state, name/unit labels, and its
/// active sense (APRS101 ch.13: PARM/UNIT/BITS). Unlike an analog channel it
/// carries no numeric value beyond on/off and no conversion equation.
struct BooleanChannel {
    char name[kTelemetryNameLength + 1]{};  ///< Channel name (PARM).
    char unit[kTelemetryUnitLength + 1]{};  ///< Channel unit/label (UNIT).
    bool value = false;                     ///< Current state.
    bool bitSense = true;                   ///< Active sense (BITS).
};

/// Full set of telemetry channels plus framing metadata.
struct Telemetry {
    AnalogChannel analog[kMaxTelemetryAnalog]{};
    BooleanChannel boolean[kMaxTelemetryBoolean]{};
    uint16_t sequenceNumber = 0;                            ///< Frame sequence counter.
    char projectName[kTelemetryProjectNameLength + 1]{};    ///< Optional project name (BITS frame).
    bool legacy = false;                                    ///< Use legacy integer telemetry format.
};

/// APRS timestamp format (the trailing indicator character).
enum class TimestampType : uint8_t {
    None,       ///< No timestamp.
    DhmZulu,    ///< Day/Hours/Minutes, zulu — "DDHHMMz".
    DhmLocal,   ///< Day/Hours/Minutes, local — "DDHHMM/".
    Hms         ///< Hours/Minutes/Seconds, zulu — "HHMMSSh".
};

/// A position-report timestamp.
struct Timestamp {
    TimestampType type = TimestampType::None;
    uint8_t day = 0;     ///< Day of month (DHM formats).
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;  ///< Seconds (HMS format only).
};

/// Position report and movement vector.
struct Position {
    char symbol = '!';              ///< APRS symbol code.
    char overlay = '/';             ///< Symbol table / overlay character.
    double latitude = 0;            ///< Latitude in decimal degrees (north positive).
    double longitude = 0;           ///< Longitude in decimal degrees (east positive).
    double courseDeg = 0;           ///< Course over ground in degrees.
    double speedKnots = 0;          ///< Speed over ground in knots.
    double altitudeFeet = 0;        ///< Altitude in feet.
    bool compressed = true;         ///< Emit/expect the compressed position format.
    bool altitudeInComment = true;  ///< Emit altitude as /A= text instead of compressed.
    bool withWeather = false;       ///< Append a weather report to the position.
    bool withTelemetry = false;     ///< Append compressed telemetry to the position.
    Timestamp timestamp{};          ///< Optional timestamp (TX and RX).
    /// Position ambiguity level, 0-4 (APRS101 ch.6): blanks trailing minute
    /// digits to indicate reduced precision, e.g. level 2 turns "4903.50N"
    /// into "4903.  N". Uncompressed format only; ignored when @ref compressed
    /// is true.
    uint8_t ambiguity = 0;
};

/// Outgoing text message with acknowledgement handling.
struct Message {
    char destination[kCallsignLength + 1]{};     ///< Addressee callsign.
    char message[kMessageLength + 1]{};          ///< Message body.
    char ackToConfirm[kAckMessageLength + 1]{};  ///< ACK id to confirm a received message (RX).
    char ackToReject[kAckMessageLength + 1]{};   ///< REJ id to reject a received message (RX).
    char ackToAsk[kAckMessageLength + 1]{};      ///< ACK id requested for this message (TX).
    char ackConfirmed[kAckMessageLength + 1]{};  ///< ACK id confirmed by the peer (RX after TX).
    char ackRejected[kAckMessageLength + 1]{};   ///< REJ id received from the peer (RX after TX).
};

/// Object / item descriptor.
struct ObjectItem {
    char name[kTelemetryNameLength + 1]{};  ///< Object / item name.
    bool active = true;                     ///< Whether the object/item is live or killed.
    uint8_t utcHour = 0;                    ///< Used by ::encode (HMS timestamp).
    uint8_t utcMinute = 0;
    uint8_t utcSecond = 0;
    Timestamp timestamp{};                  ///< Filled by ::decodeObjectItem.
};

/**
 * @brief A query request (APRS101 ch.15): "?type?" or "?type?argument".
 *
 * A query is either general (broadcast, no @ref destination — any station
 * that understands @ref type may respond) or directed (addressed to one
 * station via the message envelope ":ADDRESSEE:?type?..."). @ref type is the
 * query keyword (e.g. "APRS", "APRSD", "APRST", "IGATE", "WX", "PING");
 * @ref argument is whatever trailing text follows it, verbatim — the base
 * spec does not define a standard argument grammar, so this is opaque,
 * application-defined data. Decoding only extracts these fields; deciding
 * whether and how to respond is entirely up to the caller.
 */
struct Query {
    char destination[kCallsignLength + 1]{};  ///< Addressee for a directed query; empty for a general query.
    char type[kQueryTypeLength + 1]{};        ///< Query keyword.
    char argument[kMessageLength + 1]{};      ///< Optional trailing text after the keyword, verbatim.
};

/**
 * @brief Complete packet to be encoded with ::encode.
 *
 * All five payload sub-structs (@ref position, @ref message, @ref telemetry,
 * @ref weather, @ref item) are plain members that ALWAYS exist side by side
 * in memory — only @ref type says which one is actually meaningful for a
 * given `Packet` instance; the others just sit there unused. This is a
 * deliberate simplicity-over-size tradeoff: a tagged union would let a
 * `Packet` be only as big as its LARGEST payload (`Telemetry`, currently)
 * instead of the SUM of all of them, but unions holding non-trivial types
 * (these structs all have in-class member initializers) need careful manual
 * construction/destruction bookkeeping in C++, which cuts against this
 * library's "keep it simple and obviously correct" design goal. On the
 * tightest targets (e.g. an ATmega328P Uno with 2 KB of RAM total), keep this
 * in mind: `sizeof(Packet)` is dominated by `Telemetry` (its 5 analog + 8
 * boolean channels) even when you're only ever encoding a `Position`.
 */
struct Packet {
    char source[kCallsignLength + 1]{};         ///< Source callsign (mandatory).
    char destination[kCallsignLength + 1]{};    ///< Destination callsign (mandatory).
    char path[kPathLength + 1]{};               ///< Optional digipeater path.
    char comment[kMessageLength + 1]{};         ///< Optional free-text comment.
    char content[kMaxPacketLength + 1]{};       ///< Raw content (used by PacketType::Raw).

    Position position{};
    Message message{};
    Telemetry telemetry{};
    Weather weather{};
    ObjectItem item{};

    PacketType type = PacketType::Unknown;
};

/**
 * @brief Envelope produced by ::decode.
 *
 * decode() is deliberately lightweight: it fills the AX.25 header, the routing
 * information and classifies the @ref type, leaving @ref content as the raw
 * payload. To extract a specific payload, call the matching typed decoder
 * (e.g. ::decodeMessage) once you have inspected @ref type. This keeps the RX
 * path small in RAM — the caller only allocates the payload it actually needs.
 */
struct PacketLite {
    char raw[kMaxPacketLength + 1]{};                         ///< Frame as received, trimmed (always the OUTER frame, even when @ref viaThirdParty unwraps it).
    /// Payload after the ':' separator. Points INTO @ref raw (no separate
    /// copy) — valid as long as this @ref PacketLite instance is alive and
    /// @ref raw is not overwritten. Copying/assigning a @ref PacketLite
    /// re-points @ref content into the COPY's own @ref raw (see the custom
    /// copy constructor/assignment below), so storing decoded packets in a
    /// buffer or queue is safe.
    const char* content = "";
    char source[kCallsignLength + 1]{};
    char destination[kCallsignLength + 1]{};
    char path[kPathLength + 1]{};
    char lastDigipeaterCallsignInPath[kCallsignLength + 1]{}; ///< Last station that relayed the frame.
    uint8_t digipeaterCount = 0;                              ///< Number of hops already used.

    /// True if the frame carried a third-party header (payload starting with
    /// '}', APRS101 ch.13): typically an Internet Gateway re-transmitting a
    /// packet from APRS-IS onto RF, or vice-versa. When true, @ref source,
    /// @ref destination, @ref path and @ref content have already been
    /// replaced by the WRAPPED (original) packet's fields, so the typed
    /// decoders (::decodeMessage, ::decodePosition, ...) work transparently;
    /// @ref gateway keeps the callsign that actually transmitted this frame.
    bool viaThirdParty = false;
    char gateway[kCallsignLength + 1]{};  ///< Outer AX.25 source when @ref viaThirdParty is true.

    PacketType type = PacketType::Unknown;

    PacketLite() = default;
    /// Deep-copies @p other and re-points @ref content into this object's own
    /// @ref raw, so the copy never aliases @p other's buffer.
    PacketLite(const PacketLite& other);
    PacketLite& operator=(const PacketLite& other);
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/**
 * @brief Encode a packet into a textual APRS frame.
 *
 * The function never writes past @p outSize bytes and always NUL-terminates
 * @p out (provided @p outSize > 0).
 *
 * @param packet   Packet to encode. Not modified.
 * @param out      Destination buffer.
 * @param outSize  Size of @p out in bytes (including room for the NUL).
 * @param written  Optional; receives the number of characters written.
 * @return Result::Ok on success, otherwise an error code.
 */
Result encode(const Packet& packet, char* out, size_t outSize, size_t* written = nullptr);

/**
 * @brief Parse the envelope of a textual APRS frame.
 *
 * Fills the header (source/destination/path), the routing information
 * (last digipeater, hop count), the raw payload (@ref PacketLite::content) and
 * classifies @ref PacketLite::type. It does NOT parse the payload itself — use
 * a typed decoder such as ::decodeMessage afterwards.
 *
 * When the payload carries a third-party header (a leading '}', e.g. a frame
 * gated from APRS-IS onto RF), it is unwrapped automatically: the envelope
 * fields describe the ORIGINAL (wrapped) packet and @ref PacketLite::gateway
 * / @ref PacketLite::viaThirdParty record that it was relayed and by whom.
 *
 * @param raw  NUL-terminated frame to parse.
 * @param out  Structure that receives the parsed envelope (reset beforehand).
 * @return true if at least the source/destination header could be parsed.
 */
bool decode(const char* raw, PacketLite& out);

/**
 * @brief Extract the fields of a message payload previously classified by ::decode.
 *
 * Call this when @ref PacketLite::type is PacketType::Message (or one of the
 * telemetry-definition message types). On success @p out receives the addressee,
 * the message body and any acknowledgement information found:
 *  - @ref Message::ackToConfirm   the line number we are asked to acknowledge ("{nn");
 *  - @ref Message::ackConfirmed   a received "ackNN";
 *  - @ref Message::ackRejected    a received "rejNN".
 *
 * @param in   Envelope filled by ::decode.
 * @param out  Message structure to populate (reset beforehand).
 * @return true if the payload was a message and could be parsed.
 */
bool decodeMessage(const PacketLite& in, Message& out);

/**
 * @brief Decode a position payload (PacketType::Position or PacketType::Weather).
 *
 * Handles both the compressed and uncompressed formats, with or without a
 * timestamp. Fills latitude/longitude, symbol/overlay, course/speed/altitude
 * (when present) and @ref Position::timestamp. @ref Position::compressed is set
 * to reflect the format that was decoded.
 *
 * @param in   Envelope filled by ::decode.
 * @param out  Position structure to populate (reset beforehand).
 * @return true if a position could be parsed.
 */
bool decodePosition(const PacketLite& in, Position& out);

/**
 * @brief Decode a weather payload (PacketType::Weather).
 *
 * Parses the wind (from the compressed cs bytes or the uncompressed ddd/sss
 * field) and the weather data string (gust, temperature, rain, humidity,
 * pressure). Each field's `useX` flag is set when the field was present.
 *
 * @param in   Envelope filled by ::decode.
 * @param out  Weather structure to populate (reset beforehand).
 * @return true if weather data could be parsed.
 */
bool decodeWeather(const PacketLite& in, Weather& out);

/**
 * @brief Decode a telemetry payload.
 *
 * Depending on @ref PacketLite::type, parses a T# data report (sequence, five
 * analog values, eight bits) or a PARM/UNIT/EQNS/BITS definition message into
 * the matching fields of @p out.
 *
 * @param in   Envelope filled by ::decode.
 * @param out  Telemetry structure to populate (reset beforehand).
 * @return true if telemetry data could be parsed.
 */
bool decodeTelemetry(const PacketLite& in, Telemetry& out);

/**
 * @brief Decode an object (PacketType::Object) or item (PacketType::Item).
 *
 * Fills @p out with the name, live/killed state and (objects only) the
 * timestamp, and @p position with the embedded position report.
 *
 * @param in        Envelope filled by ::decode.
 * @param out       Object/item descriptor to populate (reset beforehand).
 * @param position  Position structure to populate (reset beforehand).
 * @return true if an object/item could be parsed.
 */
bool decodeObjectItem(const PacketLite& in, ObjectItem& out, Position& position);

/**
 * @brief Decode a status report (PacketType::Status).
 *
 * Copies the status text into @p out, stripping a leading DHM-zulu timestamp
 * when present.
 *
 * @param in       Envelope filled by ::decode.
 * @param out      Destination buffer for the status text.
 * @param outSize  Size of @p out in bytes.
 * @return true if a status text could be extracted.
 */
bool decodeStatus(const PacketLite& in, char* out, size_t outSize);

/**
 * @brief Decode a query request (PacketType::Query).
 *
 * Fills @p out with the addressee (directed queries only), the query
 * keyword and any trailing argument text. This only extracts the fields —
 * it does not decide whether or how to respond, which is application logic
 * (e.g. which stations to list for "APRSD", whether to answer a general
 * "APRS" query, rate-limiting).
 *
 * @param in   Envelope filled by ::decode.
 * @param out  Query structure to populate (reset beforehand).
 * @return true if a query could be parsed.
 */
bool decodeQuery(const PacketLite& in, Query& out);

/**
 * @brief Optional digipeater configuration: aliases and generic routing prefixes.
 *
 * Both lists are arrays of NUL-terminated strings owned by the caller. When
 * @ref prefixes is empty the single default prefix "WIDE" is used.
 */
struct DigipeaterOptions {
    const char* const* aliases = nullptr;  ///< Extra names (incl. SSID) to respond to.
    uint8_t aliasCount = 0;
    const char* const* prefixes = nullptr; ///< Generic XXXn-N prefixes (default: {"WIDE"}).
    uint8_t prefixCount = 0;
};

/**
 * @brief Apply the APRS digipeater path algorithm to a received frame.
 *
 * Implements the eligibility decision (§4.1) and the path rewrite (§4.3) of the
 * "APRS Digipeater Algorithm" (WB2OSZ, APRS Foundation, 2024-2025):
 *  - only the FIRST UNUSED address (the one right after the last '*') is examined;
 *  - it is relayed if it equals @p myCall, one of @p options.aliases, or a
 *    generic @c XXXn-N alias whose prefix is configured (default "WIDE");
 *  - aliases and used-up @c XXXn-1 are replaced by @p myCall; @c XXXn-N (N>=2)
 *    keeps the alias with N decremented and inserts @p myCall before it;
 *  - the previous '*' marker is moved so that only the last used hop carries it.
 *
 * The caller remains responsible for the parts that require state or I/O:
 * not digipeating a frame whose source is @p myCall (§4.2b) and 30-second
 * duplicate suppression (§4.2a). See the AprsDigipeat example for a working
 * implementation of both.
 *
 * For a full, section-by-section walkthrough of WHY the algorithm works this
 * way (the "New N-N Paradigm", how relaying rewrites the path into a
 * self-documenting trace of every station a frame passed through, and how
 * that prevents routing loops), see the block comment above @c PathAddr and
 * the inline @c §-numbered comments throughout @c canBeDigipeated's
 * implementation in Aprs.cpp.
 *
 * @param path      Path string, rewritten in place when the frame is relayed.
 * @param pathSize  Size of the @p path buffer in bytes.
 * @param myCall    Local station callsign.
 * @param options   Optional aliases and generic prefixes.
 * @return true if the frame should be relayed (path was updated).
 */
bool canBeDigipeated(char* path, size_t pathSize, const char* myCall,
                     const DigipeaterOptions& options = {});

/**
 * @brief Extract the last station that relayed a frame and count the used hops.
 *
 * @param path     Path string to inspect.
 * @param out      Buffer receiving the last digipeater callsign.
 * @param outSize  Size of @p out in bytes.
 * @return Number of digipeater hops already used (0 if none).
 */
uint8_t lastDigipeater(const char* path, char* out, size_t outSize);

/**
 * @brief Convert a latitude/longitude pair into a Maidenhead grid locator.
 *
 * @param latitude   Latitude in decimal degrees (-90..90).
 * @param longitude  Longitude in decimal degrees (-180..180).
 * @param out        Destination buffer.
 * @param outSize    Size of @p out in bytes (at least @p pairs * 2 + 1).
 * @param pairs      Number of character pairs to emit: 1 (field, e.g. "JN"),
 *                   2 (square, "JN18"), 3 (subsquare, "JN18dc", the common ham
 *                   radio precision) or 4 (extended square, "JN18dc12").
 *                   Clamped to [1, 4]. Defaults to 3.
 * @return true on success, false if the coordinates are out of range or
 *         @p out is too small.
 */
bool encodeGridLocator(double latitude, double longitude, char* out, size_t outSize, uint8_t pairs = 3);

/**
 * @brief Convert a Maidenhead grid locator back to a latitude/longitude pair.
 *
 * The result is the centre of the smallest cell the locator resolves to
 * (e.g. a 4-character locator such as "JN18" resolves to the centre of that
 * 1°x2° square). Accepts 2, 4, 6 or 8-character locators, case-insensitive.
 *
 * @param locator    NUL-terminated locator string.
 * @param latitude   Receives the decoded latitude in decimal degrees.
 * @param longitude  Receives the decoded longitude in decimal degrees.
 * @return true if @p locator was well-formed.
 */
bool decodeGridLocator(const char* locator, double& latitude, double& longitude);

/// Reset a Packet back to its default state.
void reset(Packet& packet);

/// Reset a PacketLite back to its default state.
void reset(PacketLite& packet);

/// Reset a Message back to its default state.
void reset(Message& message);

}  // namespace aprs

#endif  // SIMPLELIBAPRS_APRS_H

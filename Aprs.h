#ifndef CUBECELL_MONITORING_APRS_H
#define CUBECELL_MONITORING_APRS_H

#define MAX_PACKET_LENGTH 256
#define CALLSIGN_LENGTH 11
#define MAX_PATH 8
#define PATH_LENGTH CALLSIGN_LENGTH * MAX_PATH
#define MAX_TELEMETRY_ANALOG 5
#define MAX_TELEMETRY_BOOLEAN 8
#define TELEMETRY_NAME_LENGTH 8
#define TELEMETRY_UNIT_LENGTH 8
#define TELEMETRY_PROJECT_NAME_LENGTH 24
#define MESSAGE_LENGTH 200
#define ACK_MESSAGE_LENGTH 4
#define WEATHER_DEVICE_LENGTH 4

#include <cstdint>

enum AprsPacketType { Unknown, Position, Message, Telemetry, TelemetryUnit, TelemetryLabel, TelemetryEquation, TelemetryBitSense, Weather, Item, Object, Status, RawContent };

enum GPSFix {
    OLD = 0,
    CURRENT = 1
};

enum NMEA {
    OTHER = 0,
    GLL = 1,
    GGA = 2,
    RMC = 3
};

enum Compression {
    COMPRESSED = 0,
    TNC_BTEXT = 1,
    SOFTWARE = 2,
    TBD_1 = 3,
    KPC3 = 4,
    PICO = 5,
    OTHER_TRACKER_TBD_2 = 6,
    DIGIPEATER_CONVERSION = 7
};

typedef struct {
    uint16_t windDirectionDegress = 0;
    uint16_t windSpeedMph = 0;
    uint16_t gustSpeedMph = 0;
    int16_t temperatureFahrenheit = 0;
    uint16_t rain1HourHundredthsOfAnInch = 0;
    uint16_t rain24HourHundredthsOfAnInch = 0;
    uint16_t rainSinceMidnightHundredthsOfAnInch = 0;
    uint8_t humidity = 0;
    uint16_t pressure = 0;
    bool useWindDirection = false;
    bool useWindSpeed = false;
    bool useGustSpeed = false;
    bool useTemperature = false;
    bool useRain1Hour = false;
    bool useRain24Hour = false;
    bool useRainSinceMidnight = false;
    bool useHumidity = false;
    bool usePressure = false;
//    char device[WEATHER_DEVICE_LENGTH]{};
} AprsWeather;

typedef struct {
    // a x value^2 + b x value + c
    double a = 0;
    double b = 0;
    double c = 0;
} AprsTelemetryEquation;

typedef struct {
    char name[TELEMETRY_NAME_LENGTH]{};
    double value = 0;
    char unit[TELEMETRY_UNIT_LENGTH]{};
    AprsTelemetryEquation equation{};
    bool bitSense = true;
} AprsTelemetry;

typedef struct {
    AprsTelemetry telemetriesAnalog[MAX_TELEMETRY_ANALOG]{};
    AprsTelemetry telemetriesBoolean[MAX_TELEMETRY_BOOLEAN]{};
    uint16_t telemetrySequenceNumber = 0;
    char projectName[TELEMETRY_PROJECT_NAME_LENGTH]{};
    bool legacy = false;
} AprsTelemetries;

typedef struct {
    char symbol = '!';
    char overlay = '/';
    double latitude = 0;
    double longitude = 0;
    double courseDeg = 0;
    double speedKnots = 0;
    double altitudeFeet = 0;
    bool altitudeInComment = true;
    bool withWeather = false;
    bool withTelemetry = false;
} AprsPosition;

typedef struct {
    char destination[CALLSIGN_LENGTH]{};
    char message[MESSAGE_LENGTH]{};
    char ackToConfirm[ACK_MESSAGE_LENGTH]{}; // when RX
    char ackToReject[ACK_MESSAGE_LENGTH]{}; // when RX
    char ackToAsk[ACK_MESSAGE_LENGTH]{}; // when TX
    char ackConfirmed[ACK_MESSAGE_LENGTH]{}; // when RX after TX
    char ackRejected[ACK_MESSAGE_LENGTH]{}; // when RX after TX
} AprsMessage;

typedef struct {
    char name[TELEMETRY_NAME_LENGTH];
    bool active;
    uint8_t utcHour;
    uint8_t utcMinute;
    uint8_t utcSecond;
} AprsObjectItem;

typedef struct {
    char destination[CALLSIGN_LENGTH]{};
    char message[MESSAGE_LENGTH]{};
    char ackToConfirm[ACK_MESSAGE_LENGTH]{}; // when RX
    char ackConfirmed[ACK_MESSAGE_LENGTH]{}; // when RX after TX
    char ackRejected[ACK_MESSAGE_LENGTH]{}; // when RX after TX
} AprsMessageLite;

typedef struct {
    char content[MAX_PACKET_LENGTH]{};
    char source[CALLSIGN_LENGTH]{};
    char destination[CALLSIGN_LENGTH]{};
    char path[CALLSIGN_LENGTH * MAX_PATH]{};
    char comment[MESSAGE_LENGTH]{};
    AprsPosition position;
    AprsMessage message;
    AprsTelemetries telemetries;
    AprsWeather weather;
    AprsObjectItem item;
    AprsPacketType type = Unknown;
} AprsPacket;

typedef struct {
    char raw[MAX_PACKET_LENGTH]{};
    char content[MAX_PACKET_LENGTH]{};
    char source[CALLSIGN_LENGTH]{};
    char destination[CALLSIGN_LENGTH]{};
    char path[CALLSIGN_LENGTH * MAX_PATH]{};
    char lastDigipeaterCallsignInPath[CALLSIGN_LENGTH]{};
    uint8_t digipeaterCount;
    AprsMessageLite message;
    AprsPacketType type = Unknown;
} AprsPacketLite;

class Aprs {
public:
    static uint8_t encode(AprsPacket* aprsPacket, char* aprsResult);
    static bool decode(const char* aprs, AprsPacketLite* aprsPacket);
    static bool canBeDigipeated(char* path, const char* myCall);
    static uint8_t getLastDigipeater(const char* path, char* lastDigipeaterCallsign);
    static void reset(AprsPacket* aprsPacket);
    static void reset(AprsPacketLite* aprsPacket);
private:
    static void appendPosition(const AprsPosition* position, char* aprsResult);
    static void appendComment(AprsPacket *aprsPacket, char* aprsResult);
    static void appendTelemetries(AprsPacket *aprsPacket, char* aprsResult);
    static void appendMessage(AprsMessage *message, char* aprsResult);
    static void appendWeather(const AprsWeather *weather, char* aprsResult);
    static char *ax25Base91Enc(char *destination, uint8_t width, uint32_t value);
    static const char *formatDouble(double value);
    static void trim(char *string);
    static void trimStart(char *string);
    static void trimEnd(char *string);
    static void trimFirstSpace(char *string);
    static uint8_t getCompressionType(GPSFix gpsFix, NMEA nmeaSource, Compression compressionType);
    static uint8_t countCharOccurrences(const char *str, size_t n, char target);
};

#endif //CUBECELL_MONITORING_APRS_H

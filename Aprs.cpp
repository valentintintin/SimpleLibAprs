#include <cstdio>
#include <cstring>
#include <cctype>
#include <cmath>
#include "Aprs.h"

#ifdef NATIVE
#define strcpy_P     strcpy
#define sprintf_P    sprintf
#define sscanf_P     sscanf
#define strstr_P     strstr
#define strcat_P     strcat
#define PSTR(s)      s
#define min         std::min
#define max         std::max
#else
#include "Arduino.h"
#define sscanf_P     sscanf
#endif

uint8_t Aprs::encode(AprsPacket* aprsPacket, char* aprsResult) {
    sprintf_P(aprsResult, PSTR("%s>%s"), aprsPacket->source, aprsPacket->destination);

    if (strlen(aprsPacket->path)) {
        sprintf_P(&aprsResult[strlen(aprsResult)], PSTR(",%s"), aprsPacket->path);
    }

    strcat_P(aprsResult, PSTR(":"));

    switch (aprsPacket->type) {
        case Position:
            strcat_P(aprsResult, PSTR("="));
            appendPosition(&aprsPacket->position, aprsResult);
            if (aprsPacket->position.withWeather) {
                appendWeather(&aprsPacket->weather, aprsResult);
            } else if (aprsPacket->position.withTelemetry && !aprsPacket->telemetries.legacy) {
                appendTelemetries(aprsPacket, aprsResult);
            }
            appendComment(aprsPacket, aprsResult);
            break;
        case Message:
            appendMessage(&aprsPacket->message, aprsResult);
            break;
        case Telemetry:
        case TelemetryLabel:
        case TelemetryUnit:
        case TelemetryEquation:
        case TelemetryBitSense:
            appendTelemetries(aprsPacket, aprsResult);
            if (aprsPacket->type == Telemetry) {
                appendComment(aprsPacket, aprsResult);
            }
            break;
        case Status:
            strcat_P(aprsResult, PSTR(">"));
            appendComment(aprsPacket, aprsResult);
            break;
        case Object:
            sprintf_P(&aprsResult[strlen(aprsResult)], PSTR(";%s%c%d%d%dz"), aprsPacket->item.name, aprsPacket->item.active ? '*' : '_', aprsPacket->item.utcHour, aprsPacket->item.utcMinute, aprsPacket->item.utcSecond);
            aprsPacket->position.withWeather = false;
            appendPosition(&aprsPacket->position, aprsResult);
            appendComment(aprsPacket, aprsResult);
            break;
        case Item:
            sprintf_P(&aprsResult[strlen(aprsResult)], PSTR(")%s%c"), aprsPacket->item.name, aprsPacket->item.active ? '!' : '_');
            aprsPacket->position.withWeather = false;
            appendPosition(&aprsPacket->position, aprsResult);
            appendComment(aprsPacket, aprsResult);
            break;
        case RawContent:
            strcpy(&aprsResult[strlen(aprsResult)], aprsPacket->content);
            break;
        default:
            return 0;
    }

    trim(aprsResult);

    return strlen(aprsResult);
}

bool Aprs::decode(const char* aprs, AprsPacketLite* aprsPacket) {
    reset(aprsPacket);

    if (sscanf_P(aprs, PSTR("%9[^>]>%9[^,],%19[^:]:"), aprsPacket->source, aprsPacket->destination, aprsPacket->path) != 3) {
        if (sscanf_P(aprs, PSTR("%9[^>]>%9[^:]:"), aprsPacket->source, aprsPacket->destination) != 2) {
            return false;
        }
    }

    strcpy(aprsPacket->raw, aprs);
    trim(aprsPacket->raw);

    char *point = strchr(aprs, ':');
    if (point != nullptr) {
        strcpy(aprsPacket->content, point + 1);

        if (aprsPacket->content[0] == '=' || aprsPacket->content[0] == '!' || aprsPacket->content[0] == '/' || aprsPacket->content[0] == '@') {
            aprsPacket->type = Position;

            char* find = strchr(aprsPacket->message.message, '_');
            if (find != nullptr) {
                aprsPacket->type = Weather;
            }
        } else if (aprsPacket->content[0] == '_') {
            aprsPacket->type = Weather;
        } else if (aprsPacket->content[0] == '>') {
            aprsPacket->type = Status;
        } else if (aprsPacket->content[0] == ';') {
            aprsPacket->type = Object;
        } else if (aprsPacket->content[0] == ')') {
            aprsPacket->type = Item;
        }
        else if (aprsPacket->content[0] == ':') {
            strcpy(aprsPacket->message.message, aprsPacket->content + 1);

            char* find = strchr(aprsPacket->message.message, ':');
            if (find != nullptr) {
                aprsPacket->type = Message;
                strncpy(aprsPacket->message.destination, aprsPacket->message.message, find - aprsPacket->message.message);
                strcpy(aprsPacket->message.message, find + 1);
            }

            find = strstr_P(aprsPacket->message.message, PSTR("ack"));
            if (find != nullptr) {
                aprsPacket->type = Message;
                strcpy(aprsPacket->message.ackConfirmed, find + 3);
                trimFirstSpace(aprsPacket->message.ackConfirmed);
                strcpy(aprsPacket->message.message, find + 3 + strlen(aprsPacket->message.ackConfirmed));
            }

            find = strstr_P(aprsPacket->message.message, PSTR("rej"));
            if (find != nullptr) {
                aprsPacket->type = Message;
                strcpy(aprsPacket->message.ackRejected, find + 3);
                trimFirstSpace(aprsPacket->message.ackRejected);
                strcpy(aprsPacket->message.message, find + 3 + strlen(aprsPacket->message.ackRejected));
            }

            find = strchr(aprsPacket->message.message, '{');
            if (find != nullptr) {
                aprsPacket->type = Message;
                strcpy(aprsPacket->message.ackToConfirm, find + 1);
                aprsPacket->message.message[find - aprsPacket->message.message] = '\0'; // remove '{'
            }

            find = strstr_P(aprsPacket->message.message, PSTR("BITS"));
            if (find != nullptr) {
                aprsPacket->type = TelemetryBitSense;
            }

            find = strstr_P(aprsPacket->message.message, PSTR("EQNS"));
            if (find != nullptr) {
                aprsPacket->type = TelemetryEquation;
            }

            find = strstr_P(aprsPacket->message.message, PSTR("PARM"));
            if (find != nullptr) {
                aprsPacket->type = TelemetryLabel;
            }

            find = strstr_P(aprsPacket->message.message, PSTR("UNIT"));
            if (find != nullptr) {
                aprsPacket->type = TelemetryUnit;
            }

            find = strstr_P(aprsPacket->message.message, PSTR("T#"));
            if (find != nullptr) {
                aprsPacket->type = Telemetry;
            }

            trim(aprsPacket->message.message);
        }
    }

    return true;
}

void Aprs::appendPosition(AprsPosition* position, char* aprsResult) {
    uint32_t latitude, longitude;
    latitude = 900000000 - position->latitude * 10000000;
    latitude = latitude / 26 - latitude / 2710 + latitude / 15384615;
    longitude = 900000000 + position->longitude * 10000000 / 2;
    longitude = longitude / 26 - longitude / 2710 + longitude / 15384615;

    sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("%c"), position->overlay);

    char bufferBase91[] = {"0000\0" };
    ax25Base91Enc(bufferBase91, 4, latitude);
    strcat(aprsResult, bufferBase91);

    ax25Base91Enc(bufferBase91, 4, longitude);
    strcat(aprsResult, bufferBase91);

    if (position->withWeather) {
        strcat_P(aprsResult, PSTR("_"));
    } else {
        strncat(aprsResult, &position->symbol, 1);
    }

    if (position->altitudeInComment) {
        ax25Base91Enc(bufferBase91, 1, (uint32_t) position->courseDeg / 4);
        strcpy(&aprsResult[strlen(aprsResult)], bufferBase91);
        ax25Base91Enc(bufferBase91, 1, (uint32_t) (log1p(position->speedKnots) / 0.07696));
        strcpy(&aprsResult[strlen(aprsResult)], bufferBase91);

        sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("%c"), char(getCompressionType(CURRENT, RMC, COMPRESSED)+33));

        if (position->altitudeFeet > 0) {
            int alt_int = max(-99999, min(999999, (int) position->altitudeFeet));
            if (alt_int < 0) {
                sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("/A=-%05d"), alt_int * -1);
            } else {
                sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("/A=%06d"), alt_int);
            }
        }
    } else {
        ax25Base91Enc(bufferBase91, 2, (uint32_t) (log(position->altitudeFeet) / log(1.002)));
        strcpy(&aprsResult[strlen(aprsResult)], bufferBase91);

        sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("%c"), char(getCompressionType(CURRENT, GGA, COMPRESSED)+33));
    }
}

void Aprs::appendComment(AprsPacket *aprsPacket, char* aprsResult) {
    trim(aprsPacket->comment);

    if (strlen(aprsPacket->comment) == 0) {
        return;
    }

    strcat(aprsResult, aprsPacket->comment);
}

void Aprs::appendTelemetries(AprsPacket *aprsPacket, char* aprsResult) {
    char bufferBase91[] = {"00\0"};
    uint8_t boolTelemetry = 0;

    switch (aprsPacket->type) {
        case Position:
            if (aprsPacket->telemetries.telemetrySequenceNumber > 0x1FFF) {
                aprsPacket->telemetries.telemetrySequenceNumber = 0;
            }

            strcat_P(aprsResult, PSTR("|"));

            ax25Base91Enc(bufferBase91, 2, aprsPacket->telemetries.telemetrySequenceNumber);
            strcpy(&aprsResult[strlen(aprsResult)], bufferBase91);

            for (auto telemetry : aprsPacket->telemetries.telemetriesAnalog) {
                ax25Base91Enc(bufferBase91, 2, (uint16_t) abs(telemetry.value));
                strcpy(&aprsResult[strlen(aprsResult)], bufferBase91);
            }

            for (uint8_t i = 0; i < 8; i++) {
                auto telemetry = aprsPacket->telemetries.telemetriesBoolean[i];
                if (telemetry.value > 0) {
                    boolTelemetry += 1 << i;
                }
            }

            ax25Base91Enc(bufferBase91, 2, boolTelemetry);
            strcpy(&aprsResult[strlen(aprsResult)], bufferBase91);

            strcat_P(aprsResult, PSTR("|"));
            break;
        case Telemetry:
            if (aprsPacket->telemetries.legacy && aprsPacket->telemetries.telemetrySequenceNumber > 999) {
                aprsPacket->telemetries.telemetrySequenceNumber = 0;
            }

            sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("T#%d,"), aprsPacket->telemetries.telemetrySequenceNumber);

            for (auto telemetry : aprsPacket->telemetries.telemetriesAnalog) {
                if (aprsPacket->telemetries.legacy) {
                    sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("%d"), (uint8_t) abs(telemetry.value));
                } else {
                    sprintf_P(&aprsResult[strlen(aprsResult)], formatDouble(telemetry.value), telemetry.value);
                }
                strcat_P(aprsResult, PSTR(","));
            }

            for (auto telemetry : aprsPacket->telemetries.telemetriesBoolean) {
                sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("%d"), telemetry.value > 0 ? 1 : 0);
            }
            break;
        case TelemetryLabel:
            sprintf_P(&aprsResult[strlen(aprsResult)], PSTR(":%-9s:PARM.%-1.7s,%-1.6s,%-1.5s,%-1.5s,%-1.4s,%-1.5s,%-1.4s,%-1.4s,%-1.4s,%-1.4s,%-1.3s,%-1.3s,%-1.3s"),
                      aprsPacket->source,
                      aprsPacket->telemetries.telemetriesAnalog[0].name,
                      aprsPacket->telemetries.telemetriesAnalog[1].name,
                      aprsPacket->telemetries.telemetriesAnalog[2].name,
                      aprsPacket->telemetries.telemetriesAnalog[3].name,
                      aprsPacket->telemetries.telemetriesAnalog[4].name,
                      aprsPacket->telemetries.telemetriesBoolean[0].name,
                      aprsPacket->telemetries.telemetriesBoolean[1].name,
                      aprsPacket->telemetries.telemetriesBoolean[2].name,
                      aprsPacket->telemetries.telemetriesBoolean[3].name,
                      aprsPacket->telemetries.telemetriesBoolean[4].name,
                      aprsPacket->telemetries.telemetriesBoolean[5].name,
                      aprsPacket->telemetries.telemetriesBoolean[6].name,
                      aprsPacket->telemetries.telemetriesBoolean[7].name
            );
            break;
        case TelemetryUnit:
            sprintf_P(&aprsResult[strlen(aprsResult)], PSTR(":%-9s:UNIT.%-1.7s,%-1.6s,%-1.5s,%-1.5s,%-1.4s,%-1.5s,%-1.4s,%-1.4s,%-1.4s,%-1.4s,%-1.3s,%-1.3s,%-1.3s"),
                      aprsPacket->source,
                      aprsPacket->telemetries.telemetriesAnalog[0].unit,
                      aprsPacket->telemetries.telemetriesAnalog[1].unit,
                      aprsPacket->telemetries.telemetriesAnalog[2].unit,
                      aprsPacket->telemetries.telemetriesAnalog[3].unit,
                      aprsPacket->telemetries.telemetriesAnalog[4].unit,
                      aprsPacket->telemetries.telemetriesBoolean[0].unit,
                      aprsPacket->telemetries.telemetriesBoolean[1].unit,
                      aprsPacket->telemetries.telemetriesBoolean[2].unit,
                      aprsPacket->telemetries.telemetriesBoolean[3].unit,
                      aprsPacket->telemetries.telemetriesBoolean[4].unit,
                      aprsPacket->telemetries.telemetriesBoolean[5].unit,
                      aprsPacket->telemetries.telemetriesBoolean[6].unit,
                      aprsPacket->telemetries.telemetriesBoolean[7].unit
            );
            break;
        case TelemetryEquation:
            sprintf_P(&aprsResult[strlen(aprsResult)], PSTR(":%-9s:EQNS."), aprsPacket->source);

            for (uint8_t i = 0; i < MAX_TELEMETRY_ANALOG; i++) {
                AprsTelemetryEquation equation = aprsPacket->telemetries.telemetriesAnalog[i].equation;

                if (i > 0) {
                    strcat_P(aprsResult, PSTR(","));
                }

                if (equation.a == 0 && equation.b == 0) {
                    equation.b = 1;
                }

                sprintf_P(&aprsResult[strlen(aprsResult)], formatDouble(equation.a), equation.a);
                strcat_P(aprsResult, PSTR(","));
                sprintf_P(&aprsResult[strlen(aprsResult)], formatDouble(equation.b), equation.b);
                strcat_P(aprsResult, PSTR(","));
                sprintf_P(&aprsResult[strlen(aprsResult)], formatDouble(equation.c), equation.c);
            }

            break;
        case TelemetryBitSense:
            sprintf_P(&aprsResult[strlen(aprsResult)], PSTR(":%-9s:BITS."), aprsPacket->source);

            for (auto telemetry : aprsPacket->telemetries.telemetriesBoolean) {
                strcat_P(aprsResult, telemetry.bitSense ? PSTR("1") : PSTR("0"));
            }

            if (strlen(aprsPacket->telemetries.projectName)) {
                sprintf_P(&aprsResult[strlen(aprsResult)], PSTR(",%.23s"), aprsPacket->telemetries.projectName);
            }
            break;
        default:
            return;
    }
}

void Aprs::appendMessage(AprsMessage *message, char* aprsResult) {
    sprintf_P(&aprsResult[strlen(aprsResult)], PSTR(":%-9s:"), message->destination);

    if (strlen(message->ackToReject)) {
        sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("rej%s "), message->ackToReject);
    } else if (strlen(message->ackToConfirm)) {
        sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("ack%s "), message->ackToConfirm);
    }

    trim(message->message);

    if (strlen(message->message)) {
        strcat(aprsResult, message->message);
    }

    if (strlen(message->ackToAsk)) {
        sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("{%s"), message->ackToAsk);
    }
}

void Aprs::appendWeather(AprsWeather *weather, char* aprsResult) {
    if (weather->useWindDirection) {
        sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("%03d"), weather->windDirectionDegress);
    } else {
        strcat_P(aprsResult, PSTR("..."));
    }

    strcat_P(aprsResult, PSTR("/"));
    if (weather->useWindSpeed) {
        sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("%03d"), weather->windSpeedMph);
    } else {
        strcat_P(aprsResult, PSTR("..."));
    }

    strcat_P(aprsResult, PSTR("g"));
    if (weather->useGustSpeed) {
        sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("%03d"), weather->gustSpeedMph);
    } else {
        strcat_P(aprsResult, PSTR("..."));
    }

    strcat_P(aprsResult, PSTR("t"));
    if (weather->useTemperature) {
        sprintf_P(&aprsResult[strlen(aprsResult)], weather->temperatureFahrenheit > 0 ? PSTR("%03d") : PSTR("%02d"), weather->temperatureFahrenheit);
    } else {
        strcat_P(aprsResult, PSTR("..."));
    }

    strcat_P(aprsResult, PSTR("r"));
    if (weather->useRain1Hour) {
        sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("%03d"), weather->rain1HourHundredthsOfAnInch);
    } else {
        strcat_P(aprsResult, PSTR("..."));
    }
    strcat_P(aprsResult, PSTR("p"));
    if (weather->useRain24Hour) {
        sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("%03d"), weather->rain24HourHundredthsOfAnInch);
    } else {
        strcat_P(aprsResult, PSTR("..."));
    }

    strcat_P(aprsResult, PSTR("P"));
    if (weather->useRainSinceMidnight) {
        sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("%03d"), weather->rain1HourHundredthsOfAnInch);
    } else {
        strcat_P(aprsResult, PSTR("..."));
    }

    strcat_P(aprsResult, PSTR("h"));
    if (weather->useHumidity) {
        sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("%02d"), weather->humidity);
    } else {
        strcat_P(aprsResult, PSTR(".."));
    }

    strcat_P(aprsResult, PSTR("b"));
    if (weather->usePressure) {
        sprintf_P(&aprsResult[strlen(aprsResult)], PSTR("%05d"), weather->pressure * 10);
    } else {
        strcat_P(aprsResult, PSTR("....."));
    }

//    strcat_P(aprsResult, weather->device);
}

char* Aprs::ax25Base91Enc(char *destination, uint8_t width, uint32_t value) {
    /* Creates a Base-91 representation of the value in v in the string */
    /* pointed to by s, n-characters long. String length should be n+1. */

    for(destination += width, *destination = '\0'; width; width--) {
        *(--destination) = value % 91 + 33;
        value /= 91;
    }

    return(destination);
}

const char *Aprs::formatDouble(double value) {
    double integral;
    double fractional = modf(value, &integral);

    if (fractional != 0) {
        return PSTR("%.3f");
    }

    return PSTR("%.0f");
}

void Aprs::trim(char *string) {
    trimStart(string);
    trimEnd(string);
}

void Aprs::trimEnd(char *string) {
    size_t len = strlen(string);
    while (len > 0 && (isspace(string[len - 1]) || string[len - 1] == '\n' || string[len - 1] == '\r')) {
        string[--len] = '\0';
    }
}

void Aprs::trimStart(char *string) {
    size_t len = strlen(string);
    int start = 0;

    while (isspace(string[start]) || string[start] == '\n' || string[start] == '\r') {
        start++;
    }

    if (start > 0) {
        memmove(string, string + start, len - start + 1);
    }
}

void Aprs::trimFirstSpace(char *string) {
    string[strcspn(string, " ")] = '\0';
}

uint8_t Aprs::getCompressionType(enum GPSFix gpsFix, enum NMEA nmeaSource, enum Compression compressionType) {
    uint8_t value = 0;
    value |= (gpsFix & 0x01) << 5;
    value |= (nmeaSource & 0x03) << 3;
    value |= (compressionType & 0x07);
    return value;
}

void Aprs::reset(AprsPacket *aprsPacket) {
    aprsPacket->source[0] = '\0';
    aprsPacket->destination[0] = '\0';
    aprsPacket->path[0] = '\0';

    aprsPacket->content[0] = '\0';

    aprsPacket->comment[0] = '\0';

    aprsPacket->message.message[0] = '\0';
    aprsPacket->message.ackConfirmed[0] = '\0';
    aprsPacket->message.ackRejected[0] = '\0';
    aprsPacket->message.ackToAsk[0] = '\0';
    aprsPacket->message.ackToConfirm[0] = '\0';
    aprsPacket->message.ackToReject[0] = '\0';
    aprsPacket->message.destination[0] = '\0';

//    aprsPacket->weather.device[0] = '\0';
    aprsPacket->weather.temperatureFahrenheit = 0;
    aprsPacket->weather.humidity = 0;
    aprsPacket->weather.pressure = 0;
    aprsPacket->weather.rain1HourHundredthsOfAnInch = 0;
    aprsPacket->weather.rain24HourHundredthsOfAnInch = 0;
    aprsPacket->weather.rainSinceMidnightHundredthsOfAnInch = 0;
    aprsPacket->weather.windDirectionDegress = 0;
    aprsPacket->weather.windSpeedMph = 0;
    aprsPacket->weather.gustSpeedMph = 0;

    aprsPacket->telemetries.telemetrySequenceNumber = 0;
    aprsPacket->telemetries.projectName[0] = '\0';
    for (auto & i : aprsPacket->telemetries.telemetriesBoolean) {
        i.name[0] = '\0';
        i.unit[0] = '\0';
        i.value = 0;
        i.bitSense = false;
    }
    for (auto & i : aprsPacket->telemetries.telemetriesAnalog) {
        i.name[0] = '\0';
        i.unit[0] = '\0';
        i.value = 0;
        i.equation.a = 0;
        i.equation.b = 1;
        i.equation.c = 0;
    }

    aprsPacket->item.active = true;
    aprsPacket->item.name[0] = '\0';
    aprsPacket->item.utcHour = 0;
    aprsPacket->item.utcMinute = 0;
    aprsPacket->item.utcSecond = 0;

    aprsPacket->type = Unknown;
}

void Aprs::reset(AprsPacketLite *aprsPacket) {
    aprsPacket->source[0] = '\0';
    aprsPacket->destination[0] = '\0';
    aprsPacket->path[0] = '\0';

    aprsPacket->content[0] = '\0';

    aprsPacket->message.message[0] = '\0';
    aprsPacket->message.ackConfirmed[0] = '\0';
    aprsPacket->message.ackRejected[0] = '\0';
    aprsPacket->message.ackToConfirm[0] = '\0';
    aprsPacket->message.destination[0] = '\0';

    aprsPacket->type = Unknown;
}

bool Aprs::canBeDigipeated(char* path, const char* myCall) {
    if (strstr_P(path, PSTR("TCPIP")) != nullptr) {
        return false;
    }

    char *pathStart = path;
    char buffer[CALLSIGN_LENGTH * MAX_PATH] = {'\0'};

    char *hasStar = strstr_P(path, PSTR("*")); // Search for a star
    if (hasStar != nullptr) { // We got one so we set the start of or path to it
        strncpy(buffer, path, hasStar - path);
        strcat_P(buffer, PSTR(","));

        pathStart = hasStar;
    }

    const char *hasWide = strstr_P(pathStart, PSTR("WIDE")); // Do we have a WIDE path after star ?
    if (hasWide != nullptr) {
        int wideN = atoi(hasWide + 3 + 1); // WIDE_  // Take the first number of WIDE
        int wideN2 = atoi(hasWide + 3 + 1 + 2); // WIDE_-_  // Take the second one
        const char *rest = hasWide + 3 + 1 + 2 + 1; // We take the rest of the path (possible NULL)

        if (wideN2 == 1) { // If we have a WIDEN-1 : last of the chain
            sprintf_P(&buffer[strlen(buffer)], PSTR("%s*"), myCall); // We remove WIDE
        } else { // We have a WIDEN-N : keep it but decrease N
            sprintf_P(&buffer[strlen(buffer)], PSTR("%s*,WIDE%d-%d"), myCall, wideN, wideN2 - 1);
        }

        if (rest[0] != '\0') {
            strcat(buffer, rest);
        }

        strcpy(path, buffer);

        return true;
    }

    const char *hasCallsign = strstr(pathStart, myCall); // Search our callsign after star
    if (hasCallsign != nullptr) {
        const char *rest = hasCallsign + strlen(myCall); // We take the rest of the path (possible NULL)

        sprintf_P(&buffer[strlen(buffer)], PSTR("%s*"), myCall);

        if (rest[0] != '\0') {
            strcat(buffer, rest);
        }

        strcpy(path, buffer);

        return true;
    }

    return false;
}

#include <cstring>
#include <cstdio>
#include "../Aprs.h"

bool testDigi(const char* pathToTest, const char* callsign, const char* result);
void testOneFrame() {
    AprsPacket packet;
    char encoded_packet[MAX_PACKET_LENGTH];

    strcpy(packet.source, "F4HVV-15");
    strcpy(packet.destination, "TEST");
    strcpy(packet.comment, "TEST");
    packet.position = {
            '#',
            'L',
            45.325776,
            5.6365808,
            0,
            0,
            830 * 3.28,
            false,
            true,
            false
    };
    packet.telemetries.telemetriesAnalog[0].value = 123;
    packet.telemetries.telemetriesAnalog[1].value = 456;
    packet.telemetries.telemetriesAnalog[2].value = 789;
    packet.telemetries.telemetriesAnalog[3].value = 789;
    packet.telemetries.telemetriesBoolean[0].value = true;
    packet.telemetries.telemetriesBoolean[1].value = false;
    packet.telemetries.telemetriesBoolean[2].value = true;
    packet.telemetries.telemetrySequenceNumber = 3;
    packet.weather.temperatureFahrenheit = 55;
    packet.weather.pressure = 555;
    packet.type = Position;

    Aprs::encode(&packet, encoded_packet);
    printf("%s\n", encoded_packet);

    strcpy(packet.item.name, "MSH");
    packet.item.active = false;
    packet.position.symbol = '#';
    packet.position.overlay = '\\';
    strcpy(packet.comment, "Meshtastic LongModerate 868.4625");
    packet.type = Item;
    Aprs::encode(&packet, encoded_packet);
    printf("%s\n", encoded_packet);
}

int main() {
    testOneFrame();

    AprsPacket packet;
    char encoded_packet[MAX_PACKET_LENGTH];

    strcpy(packet.source, "N0CALL-9");
    strcpy(packet.destination, "APRS");
    strcpy(packet.path, "WIDE1-1,WIDE2-1");
    strcpy(packet.comment, "    \n  Comment to trim   \n   ");
    strcpy(packet.message.message, "Hello, world!");
    strcpy(packet.message.destination, "NOCALL-4");
    packet.position = {
            '!',
            '/',
            40.12345,
            5.12345,
            123,
            45, // 83 Km/h
            678 // 206 m
    };
    packet.telemetries.telemetrySequenceNumber = 7544;
    strcpy(packet.telemetries.projectName, "Test project");
    packet.telemetries.telemetriesAnalog[0] = {
            "A1",
            1472,
            "VS"
    };
    packet.telemetries.telemetriesAnalog[1] = {
            "A2",
            1564,
            "",
            {
                0, 0.001, 0
            }
    };
    packet.telemetries.telemetriesAnalog[2] = {
            "",
            -1656.45,
            "",
            {
                1, -2.0987, 34.5
            }
    };
    packet.telemetries.telemetriesAnalog[3] = {
            "",
            1748,
    };
    packet.telemetries.telemetriesAnalog[4] = {
            "",
            1840,
    };
    packet.telemetries.telemetriesBoolean[0] = {
            "Bool1",
            1,
            "on"
    };
    packet.telemetries.telemetriesBoolean[1] = {
            "B2",
            0,
            "yes"
    };
    packet.telemetries.telemetriesBoolean[2] = {
            "",
            0,
            "",
            {},
            false
    };
    packet.telemetries.telemetriesBoolean[2] = {
            "",
            0,
            "",
            {},
            false
    };
    packet.telemetries.telemetriesBoolean[3] = {
            "",
            0,
    };
    packet.telemetries.telemetriesBoolean[4] = {
            "",
            0,
    };
    packet.telemetries.telemetriesBoolean[5] = {
            "",
            0,
    };
    packet.telemetries.telemetriesBoolean[6] = {
            "",
            0,
    };
    packet.telemetries.telemetriesBoolean[7] = {
            "",
            1,
    };
    strcpy(packet.item.name, "OBJITM");
    packet.item.active = true;
    packet.item.utcHour = 12;
    packet.item.utcMinute = 34;
    packet.item.utcSecond = 56;

    packet.type = Position;
    uint8_t size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Position Size : %d\n%s\n", size, encoded_packet);

    packet.position.altitudeInComment = false;
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Position with altitude compressed Size : %d\n%s\n", size, encoded_packet);

    packet.position.withTelemetry = true;
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Position with telemetry Size : %d\n%s\n", size, encoded_packet);

    packet.type = Message;
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Message Size : %d\n%s\n", size, encoded_packet);

    strcpy(packet.message.ackToAsk, "001");
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Message ack Size : %d\n%s\n", size, encoded_packet);

    strcpy(packet.message.ackToReject, "016");
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Message ack reject Size : %d\n%s\n", size, encoded_packet);

    strcpy(packet.message.ackToConfirm, "009");
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Message ack confirm Size : %d\n%s\n", size, encoded_packet);

    strcpy(packet.message.ackToAsk, "010");
    strcpy(packet.message.ackToConfirm, "009");
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Message ack and confirm Size : %d\n%s\n", size, encoded_packet);

    packet.type = Telemetry;
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Telemetry Size : %d\n%s\n", size, encoded_packet);

    packet.type = TelemetryLabel;
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Telemetry label Size : %d\n%s\n", size, encoded_packet);

    packet.type = TelemetryUnit;
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Telemetry unit Size : %d\n%s\n", size, encoded_packet);

    packet.type = TelemetryEquation;
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Telemetry equations Size : %d\n%s\n", size, encoded_packet);

    packet.type = TelemetryBitSense;
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Telemetry bits Size : %d\n%s\n", size, encoded_packet);

    packet.type = Weather;
    packet.weather.windSpeedMph = 5.2;
    packet.weather.windDirectionDegress = 123;
    packet.weather.gustSpeedMph = 20.2;
    packet.weather.temperatureFahrenheit = 23.56;
    packet.weather.humidity = 45.78;
    packet.weather.rain1HourHundredthsOfAnInch = 1.23;
    packet.weather.rain24HourHundredthsOfAnInch = 12.23;
    packet.weather.rainSinceMidnightHundredthsOfAnInch = 6.32;
    packet.weather.pressure = 1123;
    packet.weather.useWindSpeed = true;
    packet.weather.useWindDirection = true;
    packet.weather.useGustSpeed = true;
    packet.weather.useTemperature = true;
    packet.weather.useHumidity = true;
    packet.weather.useRain1Hour = true;
    packet.weather.useRain24Hour = true;
    packet.weather.useRainSinceMidnight = true;
    packet.weather.usePressure = true;
    packet.position.withWeather = true;
    packet.position.withTelemetry = false;
//    strcpy(packet.weather.device, "Test");
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Weather Size : %d\n%s\n", size, encoded_packet);

    packet.type = Position;
    packet.position.withTelemetry = true;
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Weather and Telemetry Size : %d\n%s\n", size, encoded_packet);

    packet.type = Status;
    strcpy(packet.comment, "I'm good !");
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Status size : %d\n%s\n", size, encoded_packet);

    packet.type = Object;
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Object size : %d\n%s\n", size, encoded_packet);

    packet.type = Item;
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Item size : %d\n%s\n", size, encoded_packet);

    packet.type = RawContent;
    strcpy(packet.content, "Test test test");
    size = Aprs::encode(&packet, encoded_packet);
    printf("\nAPRS Raw Content : %d\n%s\n", size, encoded_packet);

    printf("\n");

    AprsPacketLite packet2;
    if (!Aprs::decode("F4HVV-10>APDR16,WIDE1-1*,F4HVV-9::F4HVV-15 :ack1 hello{2", &packet2)) {
        printf("Decode error for %s\n\n", encoded_packet);
        return 1;
    } else {
        printf("Received type %d from %s to %s by %s --> %s\nMessage length %ld to %s with ack %s and confirmed %s --> %s\n\n",
               packet2.type,
               packet2.source, packet2.destination, packet2.path, packet2.content,
               strlen(packet2.message.message), packet2.message.destination, packet2.message.ackToConfirm, packet2.message.ackConfirmed,
               packet2.message.message);
    }

    AprsPacketLite packet3;
    if (!Aprs::decode("F4HVV-9>APDR16,WIDE1-1:=4519.92N/00537.15E[", &packet3)) {
        printf("Decode error for %s\n\n", encoded_packet);
        return 1;
    } else {
        printf("Received type %d from %s to %s by %s --> %s\n\n",
               packet3.type,
               packet3.source, packet3.destination, packet3.path, packet3.content);
    }

    AprsPacketLite packet4;
    if (!Aprs::decode("F4HVV-9>APDR16:=4519.92N/00537.15E[", &packet4)) {
        printf("Decode error for %s\n\n", encoded_packet);
        return 1;
    } else {
        printf("Received type %d from %s to %s --> %s\n\n",
               packet4.type,
               packet4.source, packet4.destination, packet4.content);
    }

    if (!testDigi("", "F4HVV-9", "")) {
        printf("Error");
        return 1;
    }
    if (!testDigi("WIDE1-1", "F4HVV-9", "F4HVV-9*")) {
        printf("Error");
        return 1;
    }
    if (!testDigi("WIDE1-1,RFONLY", "F4HVV-9", "F4HVV-9*,RFONLY")) {
        printf("Error");
        return 1;
    }
    if (!testDigi("F4HVV-10*,WIDE1-1", "F4HVV-9", "F4HVV-10,F4HVV-9*")) {
        printf("Error");
        return 1;
    }
    if (!testDigi("F4HVV-10*,WIDE1-2", "F4HVV-9", "F4HVV-10,F4HVV-9*,WIDE1-1")) {
        printf("Error");
        return 1;
    }
    if (!testDigi("WIDE1-2", "F4HVV-9", "F4HVV-9*,WIDE1-1")) {
        printf("Error");
        return 1;
    }
    if (!testDigi("WIDE2-2", "F4HVV-9", "F4HVV-9*,WIDE2-1")) {
        printf("Error");
        return 1;
    }
    if (!testDigi("WIDE2-1", "F4HVV-9", "F4HVV-9*")) {
        printf("Error");
        return 1;
    }
    if (!testDigi("F4HVV-10*,WIDE2-1,WIDE1-1", "F4HVV-9", "F4HVV-10,F4HVV-9*,WIDE1-1")) {
        printf("Error");
        return 1;
    }
    if (!testDigi("F4HVV-10*,WIDE2-2,WIDE1-1", "F4HVV-9", "F4HVV-10,F4HVV-9*,WIDE2-1,WIDE1-1")) {
        printf("Error");
        return 1;
    }
    if (!testDigi("F4HVV-9", "F4HVV-9", "F4HVV-9*")) {
        printf("Error");
        return 1;
    }
    if (!testDigi("F4HVV-9*", "F4HVV-9", "F4HVV-9*")) {
        printf("Error");
        return 1;
    }
    if (!testDigi("F4HVV-9*,F4HVV-10", "F4HVV-9", "F4HVV-9*,F4HVV-10")) {
        printf("Error");
        return 1;
    }
    if (!testDigi("F4HVV-10*,F4HVV-9", "F4HVV-9", "F4HVV-10,F4HVV-9*")) {
        printf("Error");
        return 1;
    }
    if (!testDigi("WIDE3-3", "F4HVV-9", "F4HVV-9*,WIDE3-2")) {
        printf("Error");
        return 1;
    }

    return 0;
}

bool testDigi(const char* pathToTest, const char* callsign, const char* result) {
    char path[CALLSIGN_LENGTH * MAX_PATH];
    strcpy(path, pathToTest);
    printf("Can be digi %s ?\t", path);
    Aprs::canBeDigipeated(path, callsign);
    printf("%s\n\n", path);
    return strcmp(path, result) == 0;
}
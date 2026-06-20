/*
 * SimpleLibAprs - Decode example
 *
 * Parses the envelope of incoming APRS frames (here hard-coded strings; in a
 * real application they would come from a TNC/KISS modem), classifies the
 * packet type, and decodes the message payload on demand.
 */

#include <Aprs.h>

void handleFrame(const char* raw) {
  aprs::PacketLite packet;
  if (!aprs::decode(raw, packet)) {
    Serial.print(F("Could not parse: "));
    Serial.println(raw);
    return;
  }

  Serial.print(F("From "));
  Serial.print(packet.source);
  Serial.print(F(" to "));
  Serial.print(packet.destination);
  if (packet.digipeaterCount > 0) {
    Serial.print(F(" via "));
    Serial.print(packet.lastDigipeaterCallsignInPath);
    Serial.print(F(" ("));
    Serial.print(packet.digipeaterCount);
    Serial.print(F(" hops)"));
  }
  Serial.println();

  // Decode the payload on demand, based on the classified type.
  if (packet.type == aprs::PacketType::Message) {
    aprs::Message message;
    if (aprs::decodeMessage(packet, message)) {
      Serial.print(F("  Message for "));
      Serial.print(message.destination);
      Serial.print(F(": "));
      Serial.println(message.message);
      if (message.ackToConfirm[0]) {
        Serial.print(F("  -> please acknowledge line "));
        Serial.println(message.ackToConfirm);
      }
    }
  } else if (packet.type == aprs::PacketType::Position) {
    aprs::Position position;
    if (aprs::decodePosition(packet, position)) {
      Serial.print(F("  Position: "));
      Serial.print(position.latitude, 5);
      Serial.print(F(", "));
      Serial.print(position.longitude, 5);
      Serial.print(F("  symbol "));
      Serial.println(position.symbol);
    }
  } else if (packet.type == aprs::PacketType::Weather) {
    aprs::Weather weather;
    if (aprs::decodeWeather(packet, weather)) {
      Serial.print(F("  Weather: "));
      if (weather.useTemperature) {
        Serial.print(weather.temperatureFahrenheit);
        Serial.print(F("F "));
      }
      if (weather.useHumidity) {
        Serial.print(weather.humidity);
        Serial.print(F("% "));
      }
      Serial.println();
    }
  } else if (packet.type == aprs::PacketType::Object || packet.type == aprs::PacketType::Item) {
    aprs::ObjectItem object;
    aprs::Position position;
    if (aprs::decodeObjectItem(packet, object, position)) {
      Serial.print(F("  Object/Item "));
      Serial.print(object.name);
      Serial.print(object.active ? F(" (live) at ") : F(" (killed) at "));
      Serial.print(position.latitude, 5);
      Serial.print(F(", "));
      Serial.println(position.longitude, 5);
    }
  } else if (packet.type == aprs::PacketType::Status) {
    char status[64];
    if (aprs::decodeStatus(packet, status, sizeof status)) {
      Serial.print(F("  Status: "));
      Serial.println(status);
    }
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    ;
  }

  handleFrame("F4HVV-10>APDR16,WIDE1-1,F4HVV-9*::N0CALL-9 :Hi there{42");
  handleFrame("F4HVV-9>APDR16,WIDE1-1:=4519.92N/00537.15E[");
  handleFrame("F4HVV-9>APDR16:=/5L!!<*e7>7P[");
  handleFrame("N0CALL>APRS:;LEADER   *092345z4903.50N/07201.75W>");
  handleFrame("N0CALL>APRS:>Net control tonight");
}

void loop() {
}

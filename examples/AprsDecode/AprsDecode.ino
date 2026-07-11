/*
 * SimpleLibAprs - Decode example
 *
 * Parses the envelope of incoming APRS frames (here hard-coded strings; in a
 * real application they would come from a TNC/KISS modem), classifies the
 * packet type, and decodes the payload on demand — one canned frame and one
 * handled branch for every aprs::PacketType the library can classify.
 */

#include <Aprs.h>

// decodeTelemetry() always fills the same aprs::Telemetry struct, but which
// FIELDS it actually populates depends on which of the 5 telemetry frame
// types was decoded (a "definition" frame like PARM only ever carries
// names, never values) — so print only what that specific type provides,
// rather than the whole struct every time (which would show a lot of
// misleading zeros for the fields THIS frame didn't touch).
void printTelemetry(aprs::PacketType type, const aprs::Telemetry& t) {
  switch (type) {
    case aprs::PacketType::Telemetry:
      Serial.print(F("  Telemetry data: seq="));
      Serial.print(t.sequenceNumber);
      for (uint8_t i = 0; i < aprs::kMaxTelemetryAnalog; i++) {
        Serial.print(F(" "));
        Serial.print(t.analog[i].value);
      }
      Serial.print(F(" bits="));
      for (uint8_t i = 0; i < aprs::kMaxTelemetryBoolean; i++) {
        Serial.print(t.boolean[i].value ? '1' : '0');
      }
      Serial.println();
      break;
    case aprs::PacketType::TelemetryLabel:
      Serial.print(F("  Telemetry names (PARM): "));
      for (uint8_t i = 0; i < aprs::kMaxTelemetryAnalog; i++) {
        Serial.print(t.analog[i].name);
        Serial.print(' ');
      }
      Serial.println();
      break;
    case aprs::PacketType::TelemetryUnit:
      Serial.print(F("  Telemetry units (UNIT): "));
      for (uint8_t i = 0; i < aprs::kMaxTelemetryAnalog; i++) {
        Serial.print(t.analog[i].unit);
        Serial.print(' ');
      }
      Serial.println();
      break;
    case aprs::PacketType::TelemetryEquation:
      Serial.print(F("  Telemetry equation (EQNS), channel 1: a="));
      Serial.print(t.analog[0].equation.a);
      Serial.print(F(" b="));
      Serial.print(t.analog[0].equation.b);
      Serial.print(F(" c="));
      Serial.println(t.analog[0].equation.c);
      break;
    case aprs::PacketType::TelemetryBitSense:
      Serial.print(F("  Telemetry bit sense (BITS): "));
      for (uint8_t i = 0; i < aprs::kMaxTelemetryBoolean; i++) {
        Serial.print(t.boolean[i].bitSense ? '1' : '0');
      }
      Serial.print(F("  project: "));
      Serial.println(t.projectName);
      break;
    default:
      break;
  }
}

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
  if (packet.viaThirdParty) {
    // Gated from APRS-IS onto RF (or vice-versa): source/destination/path
    // already describe the ORIGINAL station, packet.gateway is who relayed it.
    Serial.print(F(" (gated via "));
    Serial.print(packet.gateway);
    Serial.print(F(")"));
  }
  if (packet.digipeaterCount > 0) {
    Serial.print(F(" via "));
    Serial.print(packet.lastDigipeaterCallsignInPath);
    Serial.print(F(" ("));
    Serial.print(packet.digipeaterCount);
    Serial.print(F(" hops)"));
  }
  Serial.println();

  // Decode the payload on demand, based on the classified type.
  switch (packet.type) {
    case aprs::PacketType::Message: {
      aprs::Message message;
      if (aprs::decodeMessage(packet, message)) {
        if (message.ackConfirmed[0]) {
          Serial.print(F("  ACK for message "));
          Serial.println(message.ackConfirmed);
        } else if (message.ackRejected[0]) {
          Serial.print(F("  REJ for message "));
          Serial.println(message.ackRejected);
        } else {
          Serial.print(F("  Message for "));
          Serial.print(message.destination);
          Serial.print(F(": "));
          Serial.println(message.message);
          if (message.ackToConfirm[0]) {
            Serial.print(F("  -> please acknowledge line "));
            Serial.println(message.ackToConfirm);
          }
        }
      }
      break;
    }

    case aprs::PacketType::Position: {
      aprs::Position position;
      if (aprs::decodePosition(packet, position)) {
        Serial.print(F("  Position: "));
        Serial.print(position.latitude, 5);
        Serial.print(F(", "));
        Serial.print(position.longitude, 5);
        Serial.print(F("  symbol "));
        Serial.print(position.symbol);
        if (position.ambiguity > 0) {
          Serial.print(F("  (ambiguity level "));
          Serial.print(position.ambiguity);
          Serial.print(F(")"));
        }
        Serial.println();
      }
      break;
    }

    case aprs::PacketType::Weather: {
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
        if (weather.useGustSpeed) {
          Serial.print(F("gust "));
          Serial.print(weather.gustSpeedMph);
          Serial.print(F("mph "));
        }
        Serial.println();
      }
      break;
    }

    // All five telemetry frame types share the same decoder: the DATA
    // report (T#) carries live values, PARM/UNIT/EQNS/BITS are "definition"
    // frames describing what those values mean.
    case aprs::PacketType::Telemetry:
    case aprs::PacketType::TelemetryLabel:
    case aprs::PacketType::TelemetryUnit:
    case aprs::PacketType::TelemetryEquation:
    case aprs::PacketType::TelemetryBitSense: {
      aprs::Telemetry telemetry;
      if (aprs::decodeTelemetry(packet, telemetry)) {
        printTelemetry(packet.type, telemetry);
      }
      break;
    }

    case aprs::PacketType::Object:
    case aprs::PacketType::Item: {
      aprs::ObjectItem object;
      aprs::Position position;
      if (aprs::decodeObjectItem(packet, object, position)) {
        Serial.print(F("  "));
        Serial.print(packet.type == aprs::PacketType::Object ? F("Object ") : F("Item "));
        Serial.print(object.name);
        Serial.print(object.active ? F(" (live) at ") : F(" (killed) at "));
        Serial.print(position.latitude, 5);
        Serial.print(F(", "));
        Serial.println(position.longitude, 5);
      }
      break;
    }

    case aprs::PacketType::Status: {
      char status[64];
      if (aprs::decodeStatus(packet, status, sizeof status)) {
        Serial.print(F("  Status: "));
        Serial.println(status);
      }
      break;
    }

    case aprs::PacketType::Query: {
      aprs::Query query;
      if (aprs::decodeQuery(packet, query)) {
        Serial.print(F("  Query "));
        Serial.print(query.type);
        if (query.destination[0]) {
          Serial.print(F(" directed to "));
          Serial.print(query.destination);
        } else {
          Serial.print(F(" (general)"));
        }
        if (query.argument[0]) {
          Serial.print(F("  argument: "));
          Serial.print(query.argument);
        }
        Serial.println();
        // Deciding whether and how to respond (e.g. list heard stations for
        // "APRSD", answer a general "APRS" query) is up to the application —
        // the library only extracts the request.
      }
      break;
    }

    case aprs::PacketType::Unknown:
    case aprs::PacketType::Raw:
      // Raw is a TX-only convenience (decode() never classifies a frame as
      // Raw); Unknown means none of the known Data Type Identifiers matched.
      break;
  }

  // Give a slow USB-serial adapter time to drain its TX buffer before the
  // next frame's burst of prints — see AprsEncode's top-of-file comment for
  // why (observed to matter on real hardware through a CH340 adapter).
  delay(10);
  Serial.flush();
}

// The canned frames below are only ever needed transiently (copy in, decode,
// move on) so they are kept in FLASH (F(...)) and copied one at a time into
// this single shared buffer, rather than each living permanently in RAM as a
// plain string literal would — the same "flash instead of RAM" idea as
// AprsEncode's frame labels, just applied to input data instead of output
// labels here. aprs::decode() itself still only ever sees an ordinary RAM
// `const char*`, exactly like it would reading a line out of a UART buffer.
char lineBuf[80];

void handleFlashFrame(const __FlashStringHelper* raw) {
  strcpy_P(lineBuf, (PGM_P) raw);
  handleFrame(lineBuf);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    ;
  }

  // --- Messages ---------------------------------------------------------
  handleFlashFrame(F("F4HVV-10>APDR16,WIDE1-1,F4HVV-9*::N0CALL-9 :Hi there{42"));
  handleFlashFrame(F("N0CALL-9>APRS::NOCALL-4 :ack009"));
  handleFlashFrame(F("N0CALL-9>APRS::NOCALL-4 :rej016"));

  // --- Positions ----------------------------------------------------------
  handleFlashFrame(F("F4HVV-9>APDR16,WIDE1-1:=4519.92N/00537.15E["));     // uncompressed
  handleFlashFrame(F("F4HVV-9>APDR16:=/5L!!<*e7>7P["));                   // compressed
  handleFlashFrame(F("N0CALL>APRS:=4903.  N/07201.  W>"));                // reduced precision (ambiguity)

  // --- Weather --------------------------------------------------------------
  handleFlashFrame(F("F4HVV-15>APRS:=/5L!!<*e7_   g005t077r...p...P...h50b....."));

  // --- Telemetry --------------------------------------------------------------
  handleFlashFrame(F("N0QBF>APRS:T#005,199,000,255,073,123,01101001"));                              // data
  handleFlashFrame(F("N0QBF>APRS::N0QBF-11 :PARM.Battery,Btemp,ATemp,Pres,Alt,Camra,Chut,Sun"));        // names
  handleFlashFrame(F("N0QBF-11>APRS::N0QBF-11 :UNIT.Volts,DegF,DegF,PSI,Ft,Eng,Chut,Sun,,,,,"));        // units
  handleFlashFrame(F("N0QBF>APRS::N0QBF-11 :EQNS.0,5.2,0,0,.53,-32,3,4.39,49,-32,3,18,1,2,3"));         // equations
  handleFlashFrame(F("N0QBF>APRS::N0QBF-11 :BITS.10110000,N0QBF's Big Balloon"));                       // bit sense

  // --- Object / Item ----------------------------------------------------------
  handleFlashFrame(F("N0CALL>APRS:;LEADER   *092345z4903.50N/07201.75W>"));   // object
  handleFlashFrame(F("N0CALL>APRS:)ABC!4903.50N/07201.75W>"));                // item

  // --- Status -----------------------------------------------------------------
  handleFlashFrame(F("N0CALL>APRS:>Net control tonight"));

  // --- Queries ------------------------------------------------------------
  handleFlashFrame(F("N0CALL>APRS:?APRS?"));             // general (broadcast)
  handleFlashFrame(F("N0CALL>APRS::F4HVV-9  :?APRSD?")); // directed

  // --- Third-party header (frame gated from APRS-IS onto RF) -------------
  handleFlashFrame(F("F4HVV-10>APRS,TCPIP*:}N0CALL-9>APDR16:=4519.92N/00537.15E>088/036"));
}

void loop() {
}

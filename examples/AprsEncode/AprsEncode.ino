/*
 * SimpleLibAprs - Encode example
 *
 * Builds a compressed APRS position report and a short message, and prints the
 * resulting frames to the serial port. Feed these frames to a TNC / radio modem
 * (e.g. via KISS) to actually transmit them.
 */

#include <Aprs.h>

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    ;  // wait for the USB serial port (Leonardo/Micro)
  }

  char frame[aprs::kMaxPacketLength + 1];

  // --- Position report -----------------------------------------------------
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

  size_t written = 0;
  if (aprs::encode(beacon, frame, sizeof frame, &written) == aprs::Result::Ok) {
    Serial.print(F("Position: "));
    Serial.println(frame);
  } else {
    Serial.println(F("Position: encode failed"));
  }

  // --- Text message with a line number to be acknowledged ------------------
  aprs::Packet message;
  strcpy(message.source, "N0CALL-9");
  strcpy(message.destination, "APRS");
  strcpy(message.message.destination, "F4HVV-15");
  strcpy(message.message.message, "Hello from Arduino!");
  strcpy(message.message.ackToAsk, "001");
  message.type = aprs::PacketType::Message;

  if (aprs::encode(message, frame, sizeof frame) == aprs::Result::Ok) {
    Serial.print(F("Message:  "));
    Serial.println(frame);
  }
}

void loop() {
}

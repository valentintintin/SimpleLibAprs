/*
 * SimpleLibAprs - Digipeater example
 *
 * A minimal but spec-correct APRS digipeater, following the four steps of the
 * "APRS Digipeater Algorithm" (WB2OSZ, APRS Foundation, 2024-2025):
 *
 *   4.1  Eligibility + 4.3 path rewrite ....... aprs::canBeDigipeated()
 *   4.2b Don't repeat your own packets ........ source-callsign check (below)
 *   4.2a Duplicate suppression (~30 s) ........ small hash ring buffer (below)
 *   4.4  Transmit and remember ................ sendFrame() + remember()
 *
 * Here frames are read from / written to the serial port (one frame per line)
 * to keep the example self-contained. In a real digipeater you would read and
 * write AX.25 UI frames through a KISS TNC or a radio modem instead.
 */

#include <Aprs.h>

// --- Digipeater configuration ----------------------------------------------
static const char* MY_CALL = "F4HVV-1";

static const char* ALIASES[]  = {"EOC", "TEST"};   // extra names we answer to
static const char* PREFIXES[] = {"WIDE"};          // generic XXXn-N prefixes

static aprs::DigipeaterOptions makeOptions() {
  aprs::DigipeaterOptions opt;
  opt.aliases = ALIASES;
  opt.aliasCount = sizeof(ALIASES) / sizeof(ALIASES[0]);
  opt.prefixes = PREFIXES;
  opt.prefixCount = sizeof(PREFIXES) / sizeof(PREFIXES[0]);
  return opt;
}

// --- §4.2a Duplicate suppression -------------------------------------------
// Remember a hash of (source + destination-without-SSID + info) for ~30 s.
static const uint8_t  DEDUP_SLOTS = 16;
static const uint32_t DEDUP_WINDOW_MS = 30000;

struct DedupEntry {
  uint32_t hash;
  uint32_t timeMs;
};
static DedupEntry dedup[DEDUP_SLOTS];

// Copy a callsign without its SSID ("F4HVV-9" -> "F4HVV").
static void stripSsid(const char* call, char* out, size_t outSize) {
  size_t i = 0;
  for (; call[i] && call[i] != '-' && i < outSize - 1; i++) {
    out[i] = call[i];
  }
  out[i] = '\0';
}

// djb2 hash, fed the dedup-relevant fields.
static uint32_t frameHash(const aprs::PacketLite& p) {
  char destNoSsid[aprs::kCallsignLength + 1];
  stripSsid(p.destination, destNoSsid, sizeof destNoSsid);

  uint32_t h = 5381;
  for (const char* s = p.source;   *s; s++) h = (h * 33) ^ (uint8_t) *s;
  for (const char* s = destNoSsid; *s; s++) h = (h * 33) ^ (uint8_t) *s;
  for (const char* s = p.content;  *s; s++) h = (h * 33) ^ (uint8_t) *s;
  return h;
}

static bool isDuplicate(uint32_t hash, uint32_t now) {
  for (uint8_t i = 0; i < DEDUP_SLOTS; i++) {
    if (dedup[i].hash == hash && (now - dedup[i].timeMs) < DEDUP_WINDOW_MS) {
      return true;
    }
  }
  return false;
}

static void remember(uint32_t hash, uint32_t now) {
  uint8_t oldest = 0;
  for (uint8_t i = 1; i < DEDUP_SLOTS; i++) {
    if (dedup[i].timeMs < dedup[oldest].timeMs) {
      oldest = i;
    }
  }
  dedup[oldest].hash = hash;
  dedup[oldest].timeMs = now;
}

// --- Transmit (stand-in: print to serial) ----------------------------------
static void sendFrame(const char* frame) {
  // §4.4(a): a real digipeater transmits immediately once the channel is clear,
  // WITHOUT the usual random delay.
  Serial.print(F("TX: "));
  Serial.println(frame);
}

// Rebuild a full TNC-2 frame "SOURCE>DEST[,PATH]:INFO".
static bool buildFrame(const aprs::PacketLite& p, char* out, size_t outSize) {
  int n = snprintf(out, outSize, "%s>%s", p.source, p.destination);
  if (n < 0 || (size_t) n >= outSize) return false;
  if (p.path[0]) {
    strncat(out, ",", outSize - strlen(out) - 1);
    strncat(out, p.path, outSize - strlen(out) - 1);
  }
  strncat(out, ":", outSize - strlen(out) - 1);
  strncat(out, p.content, outSize - strlen(out) - 1);
  return true;
}

// --- Core: handle one received frame ---------------------------------------
static void onFrameReceived(const char* raw) {
  aprs::PacketLite pkt;
  if (!aprs::decode(raw, pkt)) {
    return;  // not a valid frame
  }

  // §4.2(b): never digipeat a packet that we originated.
  if (strcmp(pkt.source, MY_CALL) == 0) {
    return;
  }

  // §4.1 + §4.3: are we the first unused hop? If so, rewrite the path.
  if (!aprs::canBeDigipeated(pkt.path, sizeof pkt.path, MY_CALL, makeOptions())) {
    return;
  }

  // §4.2(a): drop duplicates heard within the last 30 seconds.
  const uint32_t now = millis();
  const uint32_t hash = frameHash(pkt);
  if (isDuplicate(hash, now)) {
    return;
  }

  // §4.4: transmit the modified frame and remember it.
  char frame[aprs::kMaxPacketLength + 1];
  if (buildFrame(pkt, frame, sizeof frame)) {
    sendFrame(frame);
    remember(hash, now);
  }
}

// --- Serial line reader (one frame per line) -------------------------------
static char lineBuffer[aprs::kMaxPacketLength + 1];
static size_t lineLength = 0;

// The canned demonstration frames only exist transiently (decode, evaluate,
// move on), so they are kept in flash (F(...)) and copied one at a time into
// lineBuffer (already needed for the live serial reader below) rather than
// each living permanently in RAM as a plain string literal would.
static void onFlashFrameReceived(const __FlashStringHelper* raw) {
  strcpy_P(lineBuffer, (PGM_P) raw);
  onFrameReceived(lineBuffer);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    ;
  }
  Serial.print(F("APRS digipeater ready as "));
  Serial.println(MY_CALL);

  // Demonstration with a few canned frames:
  onFlashFrameReceived(F("N0CALL-9>APRS,WIDE2-2:=4519.92N/00537.15E>test"));   // relayed
  onFlashFrameReceived(F("N0CALL-9>APRS,WIDE2-2:=4519.92N/00537.15E>test"));   // dropped (dup)
  onFlashFrameReceived(F("F4HVV-1>APRS,WIDE2-2:hello"));                        // dropped (our own)
  onFlashFrameReceived(F("N0CALL-9>APRS,F4ABC-1*:already used up"));            // dropped (no unused hop)
  onFlashFrameReceived(F("N0CALL-9>APRS,EOC:emergency"));                       // relayed via alias
}

void loop() {
  // Read frames line-by-line from the serial port and digipeat them.
  while (Serial.available() > 0) {
    char c = (char) Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLength > 0) {
        lineBuffer[lineLength] = '\0';
        onFrameReceived(lineBuffer);
        lineLength = 0;
      }
    } else if (lineLength < sizeof(lineBuffer) - 1) {
      lineBuffer[lineLength++] = c;
    }
  }
}

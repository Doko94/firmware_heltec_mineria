#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <RadioLib.h>
#include <SPI.h>
#include <SSD1306Wire.h>
#include <WiFi.h>
#include <mbedtls/aes.h>
#include <cmath>
#include <cstring>

namespace {

// RX-02 funciona como puente autonomo entre la malla Meshtastic del T1000-E
// y el portal que RX-01 publica dentro de MINA-LOCAL.
constexpr char WIFI_SSID[] = "MINA-LOCAL";
constexpr uint8_t WIFI_CHANNEL = 6;
constexpr char RX01_GPS_ENDPOINT[] = "http://192.168.4.1/api/gps/actualizar";
constexpr char READER_TOKEN[] = "mina-local-rx-2026";

constexpr char TRACKER_NODE_ID[] = "!f72ad896";
constexpr uint32_t TRACKER_NODE_NUM = 4146780310UL;
constexpr char TRACKER_BEACON_ID[] = "TAG-001";

// Parametros leidos del T1000-E (firmware Meshtastic 2.7.15):
// region ANZ, preset LONG_FAST, canal primario ALERTA.
constexpr float MESH_FREQUENCY_MHZ = 919.625F;
constexpr float MESH_BANDWIDTH_KHZ = 250.0F;
constexpr uint8_t MESH_SPREADING_FACTOR = 11;
constexpr uint8_t MESH_CODING_RATE = 5;
constexpr uint8_t MESH_SYNC_WORD = 0x2B;
constexpr uint16_t MESH_PREAMBLE_LENGTH = 16;
constexpr int8_t RECEIVE_POWER_DBM = 14;
constexpr uint8_t MESH_CHANNEL_HASH = 0x00;
constexpr uint8_t POSITION_APP = 3;
constexpr size_t MESH_HEADER_LENGTH = 16;

// Clave AES-256 del canal ALERTA. No se imprime ni se expone por la red.
constexpr uint8_t MESH_CHANNEL_KEY[32] = {
    0x89, 0xCA, 0xFC, 0x73, 0xD7, 0x70, 0x65, 0x4B,
    0xB9, 0x32, 0x8B, 0x06, 0x21, 0x69, 0xB1, 0xF7,
    0x72, 0xFE, 0x32, 0x35, 0xD8, 0xD7, 0x3D, 0x77,
    0x05, 0x1A, 0x8A, 0xB9, 0xC0, 0x17, 0x8F, 0xF8,
};

constexpr uint32_t WIFI_RETRY_MS = 5000;
constexpr uint32_t POST_RETRY_MS = 5000;
constexpr uint32_t OLED_REFRESH_MS = 1000;

struct __attribute__((packed)) MeshHeader {
  uint32_t to;
  uint32_t from;
  uint32_t id;
  uint8_t flags;
  uint8_t channel;
  uint8_t nextHop;
  uint8_t relayNode;
};

static_assert(sizeof(MeshHeader) == MESH_HEADER_LENGTH,
              "El encabezado Meshtastic debe ocupar 16 bytes");

struct PositionFix {
  int32_t latitudeI = 0;
  int32_t longitudeI = 0;
  int32_t altitude = 0;
  uint32_t timestamp = 0;
  uint32_t precisionBits = 0;
  bool hasLatitude = false;
  bool hasLongitude = false;
};

struct ProtoReader {
  const uint8_t* data;
  size_t length;
  size_t position;

  ProtoReader(const uint8_t* source, size_t sourceLength)
      : data(source), length(sourceLength), position(0) {}

  bool readVarint(uint64_t& value) {
    value = 0;
    for (uint8_t shift = 0; shift < 64 && position < length; shift += 7) {
      const uint8_t current = data[position++];
      value |= static_cast<uint64_t>(current & 0x7F) << shift;
      if ((current & 0x80) == 0) return true;
    }
    return false;
  }

  bool readFixed32(uint32_t& value) {
    if (position + 4 > length) return false;
    value = static_cast<uint32_t>(data[position]) |
            (static_cast<uint32_t>(data[position + 1]) << 8) |
            (static_cast<uint32_t>(data[position + 2]) << 16) |
            (static_cast<uint32_t>(data[position + 3]) << 24);
    position += 4;
    return true;
  }

  bool readBytes(const uint8_t*& value, size_t& valueLength) {
    uint64_t encodedLength = 0;
    if (!readVarint(encodedLength) || encodedLength > length - position) return false;
    value = data + position;
    valueLength = static_cast<size_t>(encodedLength);
    position += valueLength;
    return true;
  }

  bool skip(uint8_t wireType) {
    uint64_t ignored = 0;
    const uint8_t* ignoredBytes = nullptr;
    size_t ignoredLength = 0;
    switch (wireType) {
      case 0: return readVarint(ignored);
      case 1:
        if (position + 8 > length) return false;
        position += 8;
        return true;
      case 2: return readBytes(ignoredBytes, ignoredLength);
      case 5:
        if (position + 4 > length) return false;
        position += 4;
        return true;
      default: return false;
    }
  }
};

SX1262 radio = new Module(SS, DIO0, RST_LoRa, BUSY_LoRa);
SSD1306Wire display(0x3C, SDA_OLED, SCL_OLED, GEOMETRY_128_64,
                   I2C_ONE, 400000);

volatile bool packetReady = false;
bool radioReady = false;
bool displayReady = false;
bool pendingPost = false;
PositionFix pendingFix;
float pendingRssi = 0.0F;
float pendingSnr = 0.0F;
uint32_t packetCount = 0;
uint32_t positionCount = 0;
uint32_t lastWifiAttempt = 0;
uint32_t lastPostAttempt = 0;
uint32_t lastDisplayRefresh = 0;
uint32_t lastAcceptedPacketId = 0;
uint32_t lastPostedTimestamp = 0;
String lastGatewayState = "Iniciando";

void IRAM_ATTR onRadioPacket() {
  packetReady = true;
}

bool decodeDataEnvelope(const uint8_t* plaintext, size_t plaintextLength,
                        const uint8_t*& positionPayload,
                        size_t& positionPayloadLength) {
  ProtoReader reader{plaintext, plaintextLength};
  uint32_t portNumber = 0;
  positionPayload = nullptr;
  positionPayloadLength = 0;

  while (reader.position < reader.length) {
    uint64_t key = 0;
    if (!reader.readVarint(key) || key == 0) return false;
    const uint32_t field = static_cast<uint32_t>(key >> 3);
    const uint8_t wire = static_cast<uint8_t>(key & 0x07);
    if (field == 1 && wire == 0) {
      uint64_t value = 0;
      if (!reader.readVarint(value)) return false;
      portNumber = static_cast<uint32_t>(value);
    } else if (field == 2 && wire == 2) {
      if (!reader.readBytes(positionPayload, positionPayloadLength)) return false;
    } else if (!reader.skip(wire)) {
      return false;
    }
  }
  return portNumber == POSITION_APP && positionPayload != nullptr &&
         positionPayloadLength > 0;
}

bool decodePosition(const uint8_t* payload, size_t payloadLength,
                    PositionFix& fix) {
  ProtoReader reader{payload, payloadLength};
  while (reader.position < reader.length) {
    uint64_t key = 0;
    if (!reader.readVarint(key) || key == 0) return false;
    const uint32_t field = static_cast<uint32_t>(key >> 3);
    const uint8_t wire = static_cast<uint8_t>(key & 0x07);
    if ((field == 1 || field == 2 || field == 4 || field == 7) && wire == 5) {
      uint32_t value = 0;
      if (!reader.readFixed32(value)) return false;
      if (field == 1) {
        fix.latitudeI = static_cast<int32_t>(value);
        fix.hasLatitude = true;
      } else if (field == 2) {
        fix.longitudeI = static_cast<int32_t>(value);
        fix.hasLongitude = true;
      } else if (field == 7 || (field == 4 && fix.timestamp == 0)) {
        fix.timestamp = value;
      }
    } else if ((field == 3 || field == 23) && wire == 0) {
      uint64_t value = 0;
      if (!reader.readVarint(value)) return false;
      if (field == 3) fix.altitude = static_cast<int32_t>(value);
      else fix.precisionBits = static_cast<uint32_t>(value);
    } else if (!reader.skip(wire)) {
      return false;
    }
  }

  const double latitude = static_cast<double>(fix.latitudeI) / 10000000.0;
  const double longitude = static_cast<double>(fix.longitudeI) / 10000000.0;
  return fix.hasLatitude && fix.hasLongitude && fix.timestamp != 0 &&
         std::isfinite(latitude) && std::isfinite(longitude) &&
         latitude >= -90.0 && latitude <= 90.0 &&
         longitude >= -180.0 && longitude <= 180.0;
}

bool decryptChannelPayload(const MeshHeader& header, const uint8_t* encrypted,
                           size_t encryptedLength, uint8_t* plaintext) {
  if (encryptedLength == 0 || encryptedLength > 239) return false;
  uint8_t nonceCounter[16] = {};
  const uint64_t packetId = header.id;
  memcpy(nonceCounter, &packetId, sizeof(packetId));
  memcpy(nonceCounter + sizeof(packetId), &header.from, sizeof(header.from));

  uint8_t streamBlock[16] = {};
  size_t nonceOffset = 0;
  mbedtls_aes_context context;
  mbedtls_aes_init(&context);
  const int keyState = mbedtls_aes_setkey_enc(&context, MESH_CHANNEL_KEY, 256);
  const int cryptState = keyState == 0
      ? mbedtls_aes_crypt_ctr(&context, encryptedLength, &nonceOffset,
                             nonceCounter, streamBlock, encrypted, plaintext)
      : keyState;
  mbedtls_aes_free(&context);
  return cryptState == 0;
}

void renderStatus() {
  if (!displayReady) return;
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setFont(ArialMT_Plain_16);
  display.drawString(64, 0, "RX-02 GW");
  display.setFont(ArialMT_Plain_10);
  display.drawString(64, 18, "T1000-E -> RX-01");
  display.drawString(64, 29, String("LoRa: ") +
                              (radioReady ? "919.625 OK" : "ERROR"));
  display.drawString(64, 40, String("WiFi: ") +
                              (WiFi.status() == WL_CONNECTED ? "MINA-LOCAL" : "buscando"));
  const String footer = pendingPost
      ? String("GPS pendiente: ") + positionCount
      : (lastPostedTimestamp != 0
             ? String("GPS enviados: ") + positionCount
             : String("Esperando GPS"));
  display.drawString(64, 51, footer);
  display.display();
}

void startDisplay() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  pinMode(RST_OLED, OUTPUT);
  digitalWrite(RST_OLED, LOW);
  delay(20);
  digitalWrite(RST_OLED, HIGH);
  delay(20);
  display.init();
  display.flipScreenVertically();
  display.setBrightness(128);
  displayReady = true;
  renderStatus();
}

void startRadio() {
  SPI.begin(SCK, MISO, MOSI, SS);
  const int16_t state = radio.begin(
      MESH_FREQUENCY_MHZ, MESH_BANDWIDTH_KHZ, MESH_SPREADING_FACTOR,
      MESH_CODING_RATE, MESH_SYNC_WORD, RECEIVE_POWER_DBM,
      MESH_PREAMBLE_LENGTH, 1.8F, false);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[GATEWAY] Error al iniciar SX1262: %d\n", state);
    lastGatewayState = "Error LoRa";
    renderStatus();
    return;
  }
  radio.setPacketReceivedAction(onRadioPacket);
  const int16_t receiveState = radio.startReceive();
  radioReady = receiveState == RADIOLIB_ERR_NONE;
  lastGatewayState = radioReady ? "Escuchando T1000-E" : "Error RX LoRa";
  Serial.printf("[GATEWAY] Meshtastic RX %.3f MHz, BW %.0f, SF%u, CR 4/%u: %d\n",
                MESH_FREQUENCY_MHZ, MESH_BANDWIDTH_KHZ,
                MESH_SPREADING_FACTOR, MESH_CODING_RATE, receiveState);
  renderStatus();
}

void startWifi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, nullptr, WIFI_CHANNEL);
  lastWifiAttempt = millis();
  Serial.println("[GATEWAY] Buscando RX-01 en MINA-LOCAL...");
}

void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  const uint32_t now = millis();
  if (now - lastWifiAttempt < WIFI_RETRY_MS) return;
  lastWifiAttempt = now;
  WiFi.disconnect(false, false);
  WiFi.begin(WIFI_SSID, nullptr, WIFI_CHANNEL);
  Serial.println("[GATEWAY] Reintentando enlace Wi-Fi con RX-01");
}

void receiveMeshPacket() {
  if (!radioReady || !packetReady) return;
  packetReady = false;

  const size_t packetLength = radio.getPacketLength();
  uint8_t packet[256] = {};
  int16_t state = RADIOLIB_ERR_PACKET_TOO_LONG;
  if (packetLength >= MESH_HEADER_LENGTH + 1 && packetLength <= sizeof(packet)) {
    state = radio.readData(packet, packetLength);
  }
  const float rssi = radio.getRSSI();
  const float snr = radio.getSNR();
  radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[GATEWAY] Paquete LoRa descartado: %d\n", state);
    return;
  }
  ++packetCount;

  MeshHeader header{};
  memcpy(&header, packet, sizeof(header));
  if (header.from != TRACKER_NODE_NUM || header.channel != MESH_CHANNEL_HASH) return;
  if (header.id == lastAcceptedPacketId) return;

  const size_t encryptedLength = packetLength - MESH_HEADER_LENGTH;
  uint8_t plaintext[240] = {};
  if (!decryptChannelPayload(header, packet + MESH_HEADER_LENGTH,
                             encryptedLength, plaintext)) {
    Serial.println("[GATEWAY] No fue posible descifrar el paquete ALERTA");
    return;
  }

  const uint8_t* positionPayload = nullptr;
  size_t positionPayloadLength = 0;
  if (!decodeDataEnvelope(plaintext, encryptedLength, positionPayload,
                          positionPayloadLength)) return;

  PositionFix fix;
  if (!decodePosition(positionPayload, positionPayloadLength, fix)) {
    Serial.println("[GATEWAY] Position_APP recibido sin coordenada valida");
    return;
  }

  lastAcceptedPacketId = header.id;
  pendingFix = fix;
  pendingRssi = rssi;
  pendingSnr = snr;
  pendingPost = true;
  ++positionCount;
  lastGatewayState = "GPS recibido";
  Serial.printf("[GATEWAY] GPS %.7f, %.7f alt=%ld t=%lu precision=%lu RSSI=%.0f SNR=%.1f\n",
                static_cast<double>(fix.latitudeI) / 10000000.0,
                static_cast<double>(fix.longitudeI) / 10000000.0,
                static_cast<long>(fix.altitude),
                static_cast<unsigned long>(fix.timestamp),
                static_cast<unsigned long>(fix.precisionBits), rssi, snr);
  renderStatus();
}

void postPendingPosition() {
  if (!pendingPost || WiFi.status() != WL_CONNECTED) return;
  const uint32_t now = millis();
  if (lastPostAttempt != 0 && now - lastPostAttempt < POST_RETRY_MS) return;
  lastPostAttempt = now;

  JsonDocument document;
  document["token"] = READER_TOKEN;
  document["node_id"] = TRACKER_NODE_ID;
  document["node_num"] = TRACKER_NODE_NUM;
  document["beacon_id"] = TRACKER_BEACON_ID;
  JsonObject position = document["position"].to<JsonObject>();
  position["latitude"] = serialized(String(
      static_cast<double>(pendingFix.latitudeI) / 10000000.0, 7));
  position["longitude"] = serialized(String(
      static_cast<double>(pendingFix.longitudeI) / 10000000.0, 7));
  position["altitude"] = pendingFix.altitude;
  position["timestamp"] = pendingFix.timestamp;
  position["precision_bits"] = pendingFix.precisionBits;
  position["source"] = "rx02_meshtastic_lora";
  position["gateway_rssi"] = serialized(String(pendingRssi, 1));
  position["gateway_snr"] = serialized(String(pendingSnr, 1));

  String payload;
  serializeJson(document, payload);
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(1200);
  http.setTimeout(1800);
  if (!http.begin(client, RX01_GPS_ENDPOINT)) return;
  http.addHeader("Content-Type", "application/json");
  const int status = http.POST(payload);
  http.end();

  if (status >= 200 && status < 300) {
    pendingPost = false;
    lastPostedTimestamp = pendingFix.timestamp;
    lastGatewayState = "GPS enviado a RX-01";
    Serial.printf("[GATEWAY] Punto GPS entregado a RX-01: HTTP %d\n", status);
  } else {
    lastGatewayState = String("Error HTTP ") + status;
    Serial.printf("[GATEWAY] RX-01 no acepto el punto: HTTP %d\n", status);
  }
  renderStatus();
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println();
  Serial.println("[GATEWAY] RX-02 Meshtastic -> MINA-LOCAL");
  startDisplay();
  startWifi();
  startRadio();
}

void loop() {
  maintainWifi();
  receiveMeshPacket();
  postPendingPosition();
  if (millis() - lastDisplayRefresh >= OLED_REFRESH_MS) {
    lastDisplayRefresh = millis();
    renderStatus();
  }
  delay(2);
}

#include <Arduino.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include <RadioLib.h>
#include <SPI.h>
#include <SSD1306Wire.h>
#include <WebServer.h>
#include <WiFi.h>
#include <cmath>

namespace {

#ifndef MINA_READER_NUMBER
#define MINA_READER_NUMBER 1
#endif

#if 0
#if MINA_READER_NUMBER == 3
constexpr char AP_SSID[] = "MINA-LOCAL";
constexpr char READER_ID[] = "RX-03";
constexpr char READER_NAME[] = "ESP32 Reader 2 RX-03";
constexpr char READER_SECTOR[] = "Frente de trabajo";
// MAC AP de RX-02. Su MAC de estación es 68:FE:71:89:00:94 y el ESP32
// reserva la siguiente dirección para la interfaz SoftAP.
constexpr uint8_t PRIMARY_AP_BSSID[6] = {0x68, 0xFE, 0x71, 0x89, 0x00, 0x95};
constexpr int READER_X = 82;
constexpr int READER_Y = 24;
constexpr int READER_Z = -100;
#else
constexpr char AP_SSID[] = "MINA-LOCAL";
constexpr char READER_ID[] = "RX-02";
constexpr char READER_NAME[] = "ESP32 Reader 1 RX-02";
constexpr char READER_SECTOR[] = "Rampa";
constexpr int READER_X = 48;
constexpr int READER_Y = 50;
constexpr int READER_Z = -50;
#endif
// Ambos readers usan el mismo canal. Esto acelera el enlace RX-03 -> RX-02 y
// permite que el teléfono vuelva a asociarse al respaldo sin buscar otro canal.
#endif

#if MINA_READER_NUMBER < 1 || MINA_READER_NUMBER > 3
#error "MINA_READER_NUMBER debe estar entre 1 y 3"
#endif

struct ReaderConfig {
  const char* id;
  const char* name;
  const char* sector;
  int x;
  int y;
  int z;
};

constexpr ReaderConfig READERS[] = {
    {"RX-01", "Heltec Reader 1 RX-01", "Portal y sala de control", 20, 78, 0},
    {"RX-02", "Heltec Reader 2 RX-02", "Rampa", 48, 50, -50},
    {"RX-03", "Heltec Reader 3 RX-03", "Frente de trabajo", 82, 24, -100},
};
constexpr size_t READER_COUNT = sizeof(READERS) / sizeof(READERS[0]);
constexpr size_t LOCAL_READER_INDEX = MINA_READER_NUMBER - 1;
constexpr const ReaderConfig& LOCAL_READER = READERS[LOCAL_READER_INDEX];
constexpr const char* READER_ID = LOCAL_READER.id;
constexpr const char* READER_NAME = LOCAL_READER.name;
constexpr const char* READER_SECTOR = LOCAL_READER.sector;
constexpr int READER_X = LOCAL_READER.x;
constexpr int READER_Y = LOCAL_READER.y;
constexpr int READER_Z = LOCAL_READER.z;
constexpr char AP_SSID[] = "MINA-LOCAL";
constexpr uint8_t AP_CHANNEL = 1;
constexpr char TARGET_UUID[] = "E2C56DB5DFFB48D2B060D0F5A71096E0";
constexpr char SUPERVISOR_PIN[] = "123456";
constexpr char ADMIN_PIN[] = "12345";
constexpr uint8_t DNS_PORT = 53;
constexpr uint32_t TAG_TIMEOUT_MS = 12000;
constexpr uint32_t STATE_REFRESH_MS = 500;
constexpr uint32_t READER_HEARTBEAT_TIMEOUT_MS = 9000;
// Primero se intenta el BSSID conocido de RX-02. Si no responde, RX-03
// publica MINA-LOCAL rápidamente, sin efectuar un barrido completo de canales.
constexpr uint32_t EMERGENCY_AP_DELAY_MS = 1200;
constexpr uint32_t STARTUP_CONNECT_TIMEOUT_MS = 2200;
constexpr uint32_t STA_CONNECT_TIMEOUT_MS = 3000;
constexpr uint32_t STA_RETRY_INTERVAL_MS = 4000;
constexpr uint32_t EMERGENCY_RECOVERY_PROBE_MS = 6000;
constexpr float READER_SWITCH_FACTOR = 0.75F;
constexpr char READER_TOKEN[] = "mina-local-rx-2026";
// El teléfono ve un único nombre de red, independientemente del reader activo.
constexpr char EMERGENCY_SSID[] = "MINA-LOCAL";
constexpr size_t MAX_EVENTS = 30;
constexpr size_t MAX_MESSAGES = 16;
constexpr uint32_t LORA_TX_INTERVAL_MS = 1800;
constexpr float LORA_FREQUENCY_MHZ = 915.0F;
constexpr float LORA_BANDWIDTH_KHZ = 125.0F;
constexpr uint8_t LORA_SPREADING_FACTOR = 7;
constexpr uint8_t LORA_CODING_RATE = 5;
constexpr int8_t LORA_POWER_DBM = 14;

struct ReaderObservation {
  int rssi = -127;
  int txPower = -59;
  float filteredRssi = -127.0F;
  float distance = 0.0F;
  uint32_t lastSeen = 0;
};

struct TagState {
  const char* id;
  const char* name;
  const char* type;
  const char* category;
  const char* owner;
  const char* installation;
  uint16_t major;
  uint16_t minor;
  int rssi = -127;
  int txPower = -59;
  float filteredRssi = -127.0F;
  float distance = 0.0F;
  float previousDistance = 0.0F;
  uint32_t lastSeen = 0;
  String readerId;
  ReaderObservation observations[READER_COUNT];
  String status = "sin_senal";
  String trend = "sin_datos";
  uint32_t dangerCount = 0;
  uint32_t nearCount = 0;
  uint32_t offlineCount = 0;

  TagState(const char* tagId, const char* tagName, const char* tagType,
           const char* tagCategory, const char* tagOwner,
           const char* tagInstallation, uint16_t tagMajor, uint16_t tagMinor)
      : id(tagId), name(tagName), type(tagType), category(tagCategory),
        owner(tagOwner), installation(tagInstallation), major(tagMajor),
        minor(tagMinor) {}
};

struct EventRecord {
  String id;
  String tagId;
  String name;
  String previous;
  String current;
  float distance = 0.0F;
  int rssi = -127;
  uint64_t timestamp = 0;
};

struct SupervisorMessage {
  bool used = false;
  String id;
  String target;
  String level;
  String title;
  String body;
  String author;
  uint64_t timestamp = 0;
  bool active = true;
  String confirmedBy;
  uint64_t confirmedAt = 0;
};

TagState tags[] = {
    {"TAG-001", "Casco minero 01", "TAG de persona", "persona", "Operador A",
     "Montado en casco o lámpara minera", 0, 0},
    {"TAG-002", "Casco minero 02", "TAG de persona", "persona", "Operador B",
     "Montado en casco o lámpara minera", 1, 0},
    {"TAG-003", "Scoop LHD 01", "TAG de maquinaria", "maquinaria", "Equipo móvil",
     "Fijado en cabina o estructura protegida", 2, 0},
};
constexpr size_t TAG_COUNT = sizeof(tags) / sizeof(tags[0]);

EventRecord events[MAX_EVENTS];
size_t eventCount = 0;
SupervisorMessage messages[MAX_MESSAGES];
size_t messageCount = 0;

WebServer server(80);
DNSServer dnsServer;
NimBLEScan* scanner = nullptr;
SX1262 radio = new Module(SS, DIO0, RST_LoRa, BUSY_LoRa);
SSD1306Wire oledDisplay(0x3C, SDA_OLED, SCL_OLED, GEOMETRY_128_64,
                        I2C_ONE, 400000);
String supervisorName;
bool adminAuthenticated = false;
uint64_t clockEpochBase = 0;
uint32_t clockMillisBase = 0;
uint32_t lastStateRefresh = 0;
uint32_t lastLoRaTx = 0;
uint32_t readerLastSeen[READER_COUNT] = {0, 0, 0};
uint16_t loRaSequence = 0;
volatile bool loRaPacketReady = false;
bool loRaReady = false;
bool oledReady = false;

uint64_t epochNow() {
  if (clockEpochBase == 0) return 0;
  return clockEpochBase + static_cast<uint64_t>(millis() - clockMillisBase);
}

String activeCoordinatorId() {
  return "RX-01";
}

String localCoordinatorRole() {
  return LOCAL_READER_INDEX == 0 ? "coordinador_preferido" : "reader_respaldo";
}

bool peerReaderOnline() {
  const uint32_t now = millis();
  for (size_t index = 0; index < READER_COUNT; ++index) {
    if (index == LOCAL_READER_INDEX) continue;
    if (readerLastSeen[index] != 0 && now - readerLastSeen[index] <= READER_HEARTBEAT_TIMEOUT_MS) return true;
  }
  return false;
}

void addTimestamp(JsonObject object, const char* key, uint64_t value) {
  if (value == 0) object[key] = nullptr;
  else object[key] = value;
}

String uuidFromBytes(const uint8_t* bytes) {
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  String result;
  result.reserve(32);
  for (size_t index = 0; index < 16; ++index) {
    result += HEX_DIGITS[(bytes[index] >> 4) & 0x0F];
    result += HEX_DIGITS[bytes[index] & 0x0F];
  }
  return result;
}

float estimateDistance(float rssi, int txPower) {
  return constrain(powf(10.0F, (static_cast<float>(txPower) - rssi) / 22.0F), 0.1F, 1000.0F);
}

String classifyDistance(float distance) {
  if (distance <= 2.0F) return "peligro";
  if (distance <= 5.0F) return "precaucion";
  if (distance <= 12.0F) return "proximo";
  return "seguro";
}

void addEvent(TagState& tag, const String& previous, const String& current) {
  for (size_t index = min(eventCount, MAX_EVENTS - 1); index > 0; --index) {
    events[index] = events[index - 1];
  }
  if (eventCount < MAX_EVENTS) ++eventCount;
  EventRecord& item = events[0];
  item.id = String(millis()) + "-" + tag.id;
  item.tagId = tag.id;
  item.name = tag.name;
  item.previous = previous;
  item.current = current;
  item.distance = tag.distance;
  item.rssi = static_cast<int>(roundf(tag.filteredRssi));
  item.timestamp = epochNow();
  if (current == "peligro") ++tag.dangerCount;
  if (current == "proximo") ++tag.nearCount;
  if (current == "sin_senal") ++tag.offlineCount;
}

void transitionTag(TagState& tag, const String& next) {
  if (tag.status == next) return;
  const String previous = tag.status;
  tag.status = next;
  addEvent(tag, previous, next);
}

int readerIndexFor(const String& readerId) {
  for (size_t index = 0; index < READER_COUNT; ++index) {
    if (readerId == READERS[index].id) return static_cast<int>(index);
  }
  return -1;
}

ReaderObservation& observationFor(TagState& tag, const String& readerId) {
  const int index = readerIndexFor(readerId);
  return tag.observations[index < 0 ? LOCAL_READER_INDEX : static_cast<size_t>(index)];
}

bool observationVisible(const ReaderObservation& observation, uint32_t now) {
  return observation.lastSeen != 0 && now - observation.lastSeen <= TAG_TIMEOUT_MS;
}

void selectBestReader(TagState& tag) {
  const uint32_t now = millis();
  ReaderObservation* selected = nullptr;
  String selectedId;

  int currentIndex = readerIndexFor(tag.readerId);
  if (currentIndex >= 0 && observationVisible(tag.observations[currentIndex], now)) {
    selected = &tag.observations[currentIndex];
    selectedId = READERS[currentIndex].id;
  }

  for (size_t index = 0; index < READER_COUNT; ++index) {
    ReaderObservation& candidate = tag.observations[index];
    if (!observationVisible(candidate, now)) continue;
    if (selected == nullptr || candidate.distance < selected->distance * READER_SWITCH_FACTOR) {
      selected = &candidate;
      selectedId = READERS[index].id;
    }
  }

  if (selected == nullptr) {
    transitionTag(tag, "sin_senal");
    tag.trend = "sin_datos";
    tag.readerId = "";
    return;
  }

  tag.previousDistance = tag.distance;
  tag.readerId = selectedId;
  tag.rssi = selected->rssi;
  tag.txPower = selected->txPower;
  tag.filteredRssi = selected->filteredRssi;
  tag.distance = selected->distance;
  tag.lastSeen = selected->lastSeen;
  if (tag.previousDistance == 0.0F || fabsf(tag.distance - tag.previousDistance) < 0.4F) tag.trend = "estable";
  else tag.trend = tag.distance < tag.previousDistance ? "acercandose" : "alejandose";
  transitionTag(tag, classifyDistance(tag.distance));
}

void updateExpiredTags() {
  for (TagState& tag : tags) selectBestReader(tag);
}

void ingestTag(TagState& tag, int rssi, int advertisedTxPower, const String& readerId) {
  ReaderObservation& observation = observationFor(tag, readerId);
  observation.rssi = rssi;
  if (advertisedTxPower > -100 && advertisedTxPower < -20) observation.txPower = advertisedTxPower;
  if (observation.filteredRssi < -120.0F) observation.filteredRssi = static_cast<float>(rssi);
  else observation.filteredRssi = 0.35F * static_cast<float>(rssi) + 0.65F * observation.filteredRssi;
  observation.distance = estimateDistance(observation.filteredRssi, observation.txPower);
  observation.lastSeen = millis();
  selectBestReader(tag);
}

class BeaconCallbacks final : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* device) override {
    const uint8_t count = device->getManufacturerDataCount();
    for (uint8_t frame = 0; frame < count; ++frame) {
      const std::string data = device->getManufacturerData(frame);
      const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data.data());
      for (size_t marker = 0; marker + 23 <= data.size(); ++marker) {
        if (bytes[marker] != 0x02 || bytes[marker + 1] != 0x15) continue;
        const size_t start = marker + 2;
        if (uuidFromBytes(bytes + start) != TARGET_UUID) return;
        const uint16_t major = (static_cast<uint16_t>(bytes[start + 16]) << 8) | bytes[start + 17];
        const uint16_t minor = (static_cast<uint16_t>(bytes[start + 18]) << 8) | bytes[start + 19];
        const int txPower = static_cast<int8_t>(bytes[start + 20]);
        for (TagState& tag : tags) {
          if (tag.major == major && tag.minor == minor) {
            ingestTag(tag, device->getRSSI(), txPower, READER_ID);
            return;
          }
        }
        return;
      }
    }
  }
};

BeaconCallbacks beaconCallbacks;

void sendJson(int status, JsonDocument& document) {
  String output;
  serializeJson(document, output);
  server.send(status, "application/json; charset=utf-8", output);
}

bool parseJsonBody(JsonDocument& document) {
  const DeserializationError error = deserializeJson(document, server.arg("plain"));
  if (!error) return true;
  server.send(400, "text/plain; charset=utf-8", "JSON inválido");
  return false;
}

void appendMessageJson(JsonArray array, const SupervisorMessage& message) {
  JsonObject item = array.add<JsonObject>();
  item["id"] = message.id;
  item["destino"] = message.target;
  item["nivel"] = message.level;
  item["titulo"] = message.title;
  item["mensaje"] = message.body;
  item["autor"] = message.author;
  addTimestamp(item, "fecha", message.timestamp);
  item["vigente"] = message.active;
  if (message.confirmedBy.isEmpty()) {
    item["confirmado_por"] = nullptr;
    item["confirmado_fecha"] = nullptr;
  } else {
    item["confirmado_por"] = message.confirmedBy;
    addTimestamp(item, "confirmado_fecha", message.confirmedAt);
  }
}

void saveMessages() {
  File file = LittleFS.open("/mensajes.json", "w");
  if (!file) return;
  JsonDocument document;
  JsonArray array = document.to<JsonArray>();
  for (size_t index = 0; index < messageCount; ++index) {
    if (messages[index].used) appendMessageJson(array, messages[index]);
  }
  serializeJson(document, file);
  file.close();
}

void loadMessages() {
  if (!LittleFS.exists("/mensajes.json")) return;
  File file = LittleFS.open("/mensajes.json", "r");
  JsonDocument document;
  if (deserializeJson(document, file)) {
    file.close();
    return;
  }
  file.close();
  messageCount = 0;
  for (JsonObject item : document.as<JsonArray>()) {
    if (messageCount >= MAX_MESSAGES) break;
    SupervisorMessage& message = messages[messageCount++];
    message.used = true;
    message.id = item["id"] | "";
    message.target = item["destino"] | "todos";
    message.level = item["nivel"] | "informacion";
    message.title = item["titulo"] | "";
    message.body = item["mensaje"] | "";
    message.author = item["autor"] | "";
    message.timestamp = item["fecha"] | 0ULL;
    message.active = item["vigente"] | false;
    message.confirmedBy = item["confirmado_por"] | "";
    message.confirmedAt = item["confirmado_fecha"] | 0ULL;
  }
}

const char* readerNameFor(const String& readerId) {
  const int index = readerIndexFor(readerId);
  return index < 0 ? LOCAL_READER.name : READERS[index].name;
}

const char* readerSectorFor(const String& readerId) {
  const int index = readerIndexFor(readerId);
  return index < 0 ? LOCAL_READER.sector : READERS[index].sector;
}

void appendReaderCoordinates(JsonArray coordinates, const String& readerId) {
  const int index = readerIndexFor(readerId);
  const ReaderConfig& reader = index < 0 ? LOCAL_READER : READERS[index];
  coordinates.add(reader.x); coordinates.add(reader.y); coordinates.add(reader.z);
}

void sendState() {
  updateExpiredTags();
  JsonDocument document;
  const uint64_t now = epochNow();
  if (now == 0) document["actualizado"] = nullptr;
  else document["actualizado"] = now;
  JsonObject receiver = document["receptor"].to<JsonObject>();
  receiver["id"] = READER_ID;
  receiver["nombre"] = READER_NAME;
  receiver["ubicacion"] = READER_SECTOR;
  receiver["descripcion"] = "Reader Heltec BLE, LoRa y servidor web local";
  receiver["modo_red"] = loRaReady ? "lora" : "local";

  JsonObject coordination = document["coordinacion"].to<JsonObject>();
  coordination["coordinador_preferido"] = "RX-01";
  coordination["coordinador_activo"] = activeCoordinatorId();
  coordination["reader_local"] = READER_ID;
  coordination["rol_local"] = localCoordinatorRole();
  coordination["peer_disponible"] = peerReaderOnline();
  coordination["modo_degradado"] = !loRaReady;
  uint32_t latestPeer = 0;
  for (size_t index = 0; index < READER_COUNT; ++index) {
    if (index != LOCAL_READER_INDEX && readerLastSeen[index] > latestPeer) latestPeer = readerLastSeen[index];
  }
  if (latestPeer == 0) coordination["ultima_sincronizacion_ms"] = nullptr;
  else coordination["ultima_sincronizacion_ms"] = millis() - latestPeer;

  size_t danger = 0, warning = 0, near = 0, offline = 0;
  for (const TagState& tag : tags) {
    if (tag.status == "peligro") ++danger;
    else if (tag.status == "precaucion") ++warning;
    else if (tag.status == "proximo") ++near;
    else if (tag.status == "sin_senal") ++offline;
  }
  JsonObject counts = document["conteos"].to<JsonObject>();
  counts["peligro"] = danger;
  counts["precaucion"] = warning;
  counts["proximo"] = near;
  counts["seguro"] = TAG_COUNT - danger - warning - near - offline;
  counts["sin_senal"] = offline;

  JsonArray beacons = document["beacons"].to<JsonArray>();
  for (const TagState& tag : tags) {
    JsonObject item = beacons.add<JsonObject>();
    item["id"] = tag.id;
    item["nombre"] = tag.name;
    item["tipo"] = tag.type;
    item["persona"] = tag.owner;
    item["categoria"] = tag.category;
    item["instalacion"] = tag.installation;
    item["uuid"] = TARGET_UUID;
    item["major"] = tag.major;
    item["minor"] = tag.minor;
    item["estado"] = tag.status;
    item["tendencia"] = tag.trend;
    const bool visible = tag.lastSeen != 0 && millis() - tag.lastSeen <= TAG_TIMEOUT_MS;
    if (visible) {
      item["rssi"] = static_cast<int>(roundf(tag.filteredRssi));
      item["distancia"] = serialized(String(tag.distance, 1));
      addTimestamp(item, "ultima_senal", now == 0 ? 0 : now - (millis() - tag.lastSeen));
      item["segundos_sin_senal"] = static_cast<float>(millis() - tag.lastSeen) / 1000.0F;
    } else {
      item["rssi"] = nullptr;
      item["distancia"] = nullptr;
      item["ultima_senal"] = nullptr;
      item["segundos_sin_senal"] = nullptr;
    }
    item["activo"] = visible;
    const String selectedReader = tag.readerId.isEmpty() ? String(READER_ID) : tag.readerId;
    item["reader_id"] = selectedReader;
    item["reader_nombre"] = readerNameFor(selectedReader);
    item["sector"] = readerSectorFor(selectedReader);
    JsonArray coordinates = item["coordenadas"].to<JsonArray>();
    appendReaderCoordinates(coordinates, selectedReader);
    item["transporte"] = "Bluetooth local";
    JsonObject counters = item["contadores"].to<JsonObject>();
    counters["peligro"] = tag.dangerCount;
    counters["proximo"] = tag.nearCount;
    counters["sin_senal"] = tag.offlineCount;
  }

  JsonArray eventArray = document["eventos"].to<JsonArray>();
  for (size_t index = 0; index < eventCount; ++index) {
    const EventRecord& event = events[index];
    JsonObject item = eventArray.add<JsonObject>();
    item["id"] = event.id;
    item["beacon_id"] = event.tagId;
    item["nombre"] = event.name;
    item["persona"] = "";
    item["anterior"] = event.previous;
    item["estado"] = event.current;
    item["distancia"] = serialized(String(event.distance, 1));
    item["rssi"] = event.rssi;
    addTimestamp(item, "fecha", event.timestamp);
  }
  sendJson(200, document);
}

void sendLayout() {
  File file = LittleFS.open("/layout_mina.json", "r");
  if (!file) {
    server.send(500, "text/plain; charset=utf-8", "Layout no disponible");
    return;
  }
  JsonDocument document;
  const DeserializationError error = deserializeJson(document, file);
  file.close();
  if (error) {
    server.send(500, "text/plain; charset=utf-8", "Layout inválido");
    return;
  }
  JsonArray readers = document["readers"].to<JsonArray>();
#if 0
  JsonObject active = readers.add<JsonObject>();
  active["id"] = READER_ID;
  active["nombre"] = READER_NAME;
  active["sector"] = READER_SECTOR;
  active["x"] = READER_X; active["y"] = READER_Y; active["z"] = READER_Z;
  active["transporte"] = "Bluetooth local";
  active["hardware"] = "ESP32 WROOM-32";
  active["disponible"] = true;
#if MINA_READER_NUMBER == 2
  JsonObject future = readers.add<JsonObject>();
  future["id"] = "RX-03";
  const bool rx03Online = lastRx03Heartbeat != 0 && millis() - lastRx03Heartbeat <= READER_HEARTBEAT_TIMEOUT_MS;
  future["nombre"] = rx03Online ? "ESP32 Reader 2 RX-03" : "ESP32 Reader 2 (sin conexion)";
  future["sector"] = "Frente de trabajo";
  future["x"] = 82; future["y"] = 24; future["z"] = -100;
  future["transporte"] = "MINA-LOCAL";
  future["hardware"] = "ESP32 WROOM-32";
  future["disponible"] = rx03Online;
#else
  JsonObject hub = readers.add<JsonObject>();
  hub["id"] = "RX-02";
  hub["nombre"] = "ESP32 Reader 1 (central)";
  hub["sector"] = "Rampa";
  hub["x"] = 48; hub["y"] = 50; hub["z"] = -50;
  hub["transporte"] = "Red MINA-LOCAL";
  hub["hardware"] = "ESP32 WROOM-32";
  hub["disponible"] = WiFi.status() == WL_CONNECTED;
#endif
#endif
  const uint32_t now = millis();
  for (size_t index = 0; index < READER_COUNT; ++index) {
    const ReaderConfig& config = READERS[index];
    const bool online = index == LOCAL_READER_INDEX ||
        (readerLastSeen[index] != 0 && now - readerLastSeen[index] <= READER_HEARTBEAT_TIMEOUT_MS);
    JsonObject item = readers.add<JsonObject>();
    item["id"] = config.id;
    item["nombre"] = online ? String(config.name) : String(config.name) + " (sin conexion LoRa)";
    item["sector"] = config.sector;
    item["x"] = config.x; item["y"] = config.y; item["z"] = config.z;
    item["transporte"] = index == LOCAL_READER_INDEX ? "Bluetooth local" : "LoRa 915 MHz";
    item["hardware"] = "Heltec WiFi LoRa 32 V3";
    item["disponible"] = online;
  }
  sendJson(200, document);
}

void sendMessages() {
  JsonDocument document;
  JsonArray array = document["mensajes"].to<JsonArray>();
  for (size_t index = 0; index < messageCount; ++index) {
    if (messages[index].used) appendMessageJson(array, messages[index]);
  }
  sendJson(200, document);
}

int findMessage(const String& id) {
  for (size_t index = 0; index < messageCount; ++index) {
    if (messages[index].used && messages[index].id == id) return static_cast<int>(index);
  }
  return -1;
}

void publishMessage() {
  if (supervisorName.isEmpty()) {
    server.send(401, "text/plain; charset=utf-8", "Sesión de supervisor requerida");
    return;
  }
  JsonDocument body;
  if (!parseJsonBody(body)) return;
  String target = body["destino"] | "todos";
  String level = body["nivel"] | "informacion";
  String title = body["titulo"] | "";
  String text = body["mensaje"] | "";
  title.trim(); text.trim();
  if (title.isEmpty() || text.isEmpty()) {
    server.send(400, "text/plain; charset=utf-8", "Título y mensaje son obligatorios");
    return;
  }
  if (title.length() > 70 || text.length() > 500) {
    server.send(400, "text/plain; charset=utf-8", "Máximo: título 70 y mensaje 500 caracteres");
    return;
  }
  if (messageCount == MAX_MESSAGES) {
    for (size_t index = MAX_MESSAGES - 1; index > 0; --index) messages[index] = messages[index - 1];
  } else {
    for (size_t index = messageCount; index > 0; --index) messages[index] = messages[index - 1];
    ++messageCount;
  }
  SupervisorMessage& message = messages[0];
  message.used = true;
  message.id = String(millis()) + "-msg";
  message.target = target;
  message.level = level;
  message.title = title;
  message.body = text;
  message.author = supervisorName;
  message.timestamp = epochNow();
  message.active = true;
  message.confirmedBy = "";
  message.confirmedAt = 0;
  saveMessages();
  JsonDocument response;
  JsonArray array = response.to<JsonArray>();
  appendMessageJson(array, message);
  server.send(201, "application/json; charset=utf-8", server.arg("plain"));
}

void handleDynamicApi() {
  String uri = server.uri();
  if (uri.startsWith("/api/mensajes/")) {
    String tail = uri.substring(String("/api/mensajes/").length());
    bool confirm = tail.endsWith("/confirmar");
    if (confirm) tail.remove(tail.length() - String("/confirmar").length());
    const int index = findMessage(tail);
    if (index < 0) {
      server.send(404, "text/plain; charset=utf-8", "Mensaje no encontrado");
      return;
    }
    if (confirm && server.method() == HTTP_POST) {
      if (supervisorName.isEmpty()) {
        server.send(401, "text/plain; charset=utf-8", "Sesión de supervisor requerida");
        return;
      }
      messages[index].confirmedBy = supervisorName;
      messages[index].confirmedAt = epochNow();
      messages[index].active = false;
      saveMessages();
      JsonDocument response;
      JsonArray array = response.to<JsonArray>();
      appendMessageJson(array, messages[index]);
      sendJson(200, response);
      return;
    }
    if (server.method() == HTTP_DELETE) {
      if (!adminAuthenticated) {
        server.send(403, "text/plain; charset=utf-8", "Solo el administrador puede eliminar mensajes");
        return;
      }
      for (size_t position = index; position + 1 < messageCount; ++position) messages[position] = messages[position + 1];
      if (messageCount > 0) --messageCount;
      saveMessages();
      JsonDocument response; response["eliminado"] = tail; sendJson(200, response);
      return;
    }
  }
  if (uri.startsWith("/api/historial-proximidad/") && server.method() == HTTP_DELETE) {
    if (!adminAuthenticated) {
      server.send(403, "text/plain; charset=utf-8", "Se requiere una sesión de administrador");
      return;
    }
    const String id = uri.substring(String("/api/historial-proximidad/").length());
    for (TagState& tag : tags) {
      if (id == tag.id) {
        tag.dangerCount = tag.nearCount = tag.offlineCount = 0;
        size_t write = 0;
        for (size_t read = 0; read < eventCount; ++read) if (events[read].tagId != id) events[write++] = events[read];
        eventCount = write;
        JsonDocument response; response["beacon_id"] = id; sendJson(200, response); return;
      }
    }
    server.send(404, "text/plain; charset=utf-8", "TAG no encontrado");
    return;
  }
  if (uri.startsWith("/api/")) {
    server.send(404, "text/plain; charset=utf-8", "API no disponible");
    return;
  }
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void serveIndex() {
  File file = LittleFS.open("/index.html", "r");
  if (!file) {
    server.send(500, "text/plain; charset=utf-8", "Falta cargar LittleFS. Ejecuta Upload Filesystem Image una vez.");
    return;
  }
  server.streamFile(file, "text/html; charset=utf-8");
  file.close();
}

#if 0
void receiveReaderObservations() {
#if MINA_READER_NUMBER == 2
  JsonDocument body;
  if (!parseJsonBody(body)) return;
  const String token = body["token"] | "";
  const String readerId = body["reader_id"] | "";
  if (token != READER_TOKEN || readerId != "RX-03") {
    server.send(403, "text/plain; charset=utf-8", "Reader no autorizado");
    return;
  }
  lastRx03Heartbeat = millis();
  size_t accepted = 0;
  for (JsonObject reading : body["lecturas"].as<JsonArray>()) {
    const uint16_t major = reading["major"] | 65535;
    const uint16_t minor = reading["minor"] | 65535;
    const int rssi = reading["rssi"] | -127;
    const int txPower = reading["tx_power"] | -59;
    for (TagState& tag : tags) {
      if (tag.major == major && tag.minor == minor && rssi > -127) {
        ingestTag(tag, rssi, txPower, readerId);
        ++accepted;
        break;
      }
    }
  }
  JsonDocument response;
  response["reader_id"] = readerId;
  response["aceptadas"] = accepted;
  response["concentrador"] = "RX-02";
  sendJson(200, response);
#else
  server.send(409, "text/plain; charset=utf-8", "RX-03 no es el concentrador");
#endif
}

void receiveCoordinatorSnapshot() {
#if MINA_READER_NUMBER == 3
  JsonDocument body;
  if (!parseJsonBody(body)) return;
  const String token = body["token"] | "";
  const String readerId = body["reader_id"] | "";
  if (token != READER_TOKEN || readerId != "RX-02") {
    server.send(403, "text/plain; charset=utf-8", "Coordinador no autorizado");
    return;
  }

  lastRx02Snapshot = millis();
  const uint64_t remoteEpoch = body["epoch"] | 0ULL;
  if (remoteEpoch != 0) {
    clockEpochBase = remoteEpoch;
    clockMillisBase = millis();
  }

  size_t accepted = 0;
  for (JsonObject reading : body["lecturas"].as<JsonArray>()) {
    const uint16_t major = reading["major"] | 65535;
    const uint16_t minor = reading["minor"] | 65535;
    const int rssi = reading["rssi"] | -127;
    const int txPower = reading["tx_power"] | -59;
    for (TagState& tag : tags) {
      if (tag.major == major && tag.minor == minor && rssi > -127) {
        ingestTag(tag, rssi, txPower, readerId);
        ++accepted;
        break;
      }
    }
  }

  JsonDocument response;
  response["reader_id"] = READER_ID;
  response["aceptadas"] = accepted;
  response["coordinador_activo"] = "RX-02";
  sendJson(200, response);
#else
  server.send(409, "text/plain; charset=utf-8", "RX-02 es el coordinador preferido");
#endif
}

void postObservationsToHub() {
#if MINA_READER_NUMBER == 3
  const uint32_t now = millis();
  if (now - lastHubPost < HUB_POST_INTERVAL_MS || WiFi.status() != WL_CONNECTED) return;
  lastHubPost = now;

  JsonDocument document;
  document["token"] = READER_TOKEN;
  document["reader_id"] = "RX-03";
  JsonArray readings = document["lecturas"].to<JsonArray>();
  for (const TagState& tag : tags) {
    if (!observationVisible(tag.rx03, now)) continue;
    JsonObject reading = readings.add<JsonObject>();
    reading["uuid"] = TARGET_UUID;
    reading["major"] = tag.major;
    reading["minor"] = tag.minor;
    reading["rssi"] = tag.rx03.rssi;
    reading["tx_power"] = tag.rx03.txPower;
  }
  String payload;
  serializeJson(document, payload);
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(500);
  http.setTimeout(700);
  if (!http.begin(client, "http://192.168.4.1/api/lecturas")) return;
  http.addHeader("Content-Type", "application/json");
  const int status = http.POST(payload);
  http.end();
  const bool connected = status >= 200 && status < 300;
  if (connected != hubOnline) {
    hubOnline = connected;
    Serial.println(connected ? "[HUB] RX-02 recibe las lecturas de RX-03" : "[HUB] RX-02 no responde; se reintentara");
  }
#endif
}

void pushCoordinatorSnapshotToBackup() {
#if MINA_READER_NUMBER == 2
  const uint32_t now = millis();
  if (now - lastBackupSync < BACKUP_SYNC_INTERVAL_MS) return;
  lastBackupSync = now;
  if (lastRx03Heartbeat == 0 || now - lastRx03Heartbeat > READER_HEARTBEAT_TIMEOUT_MS) {
    backupOnline = false;
    return;
  }

  JsonDocument document;
  document["token"] = READER_TOKEN;
  document["reader_id"] = "RX-02";
  document["epoch"] = epochNow();
  JsonArray readings = document["lecturas"].to<JsonArray>();
  for (const TagState& tag : tags) {
    if (!observationVisible(tag.rx02, now)) continue;
    JsonObject reading = readings.add<JsonObject>();
    reading["uuid"] = TARGET_UUID;
    reading["major"] = tag.major;
    reading["minor"] = tag.minor;
    reading["rssi"] = tag.rx02.rssi;
    reading["tx_power"] = tag.rx02.txPower;
  }

  String payload;
  serializeJson(document, payload);
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(500);
  http.setTimeout(700);
  if (!http.begin(client, "http://192.168.4.30/api/coordinacion/snapshot")) {
    backupOnline = false;
    return;
  }
  http.addHeader("Content-Type", "application/json");
  const int status = http.POST(payload);
  http.end();
  const bool connected = status >= 200 && status < 300;
  if (connected != backupOnline) {
    backupOnline = connected;
    Serial.println(connected ? "[COORDINACION] RX-03 sincronizado como respaldo" :
                               "[COORDINACION] RX-03 no recibio la copia del coordinador");
  }
#endif
}

#endif

void receiveReaderObservations() {
  server.send(410, "text/plain; charset=utf-8", "La coordinacion Wi-Fi fue reemplazada por LoRa");
}

void receiveCoordinatorSnapshot() {
  server.send(410, "text/plain; charset=utf-8", "La sincronizacion se realiza por LoRa");
}

void postObservationsToHub() {}
void pushCoordinatorSnapshotToBackup() {}

void configureWeb() {
  server.on("/", HTTP_GET, serveIndex);
  server.on("/diagnostico", HTTP_GET, []() {
    String text = "Reader " + String(READER_ID) + " activo\nRed: " + String(AP_SSID) +
                  "\nIP MINA-LOCAL: " + WiFi.softAPIP().toString() +
                  "\nCoordinador activo: " + activeCoordinatorId() +
                  "\nRol local: " + localCoordinatorRole() +
                  "\nPeer disponible: " + (peerReaderOnline() ? String("si") : String("no")) +
                  "\nLoRa 915 MHz: " + (loRaReady ? String("activo") : String("error"));
    server.send(200, "text/plain; charset=utf-8", text);
  });
  server.on("/api/estado", HTTP_GET, sendState);
  server.on("/api/coordinacion", HTTP_GET, []() {
    JsonDocument response;
    response["coordinador_preferido"] = "RX-01";
    response["coordinador_activo"] = activeCoordinatorId();
    response["reader_local"] = READER_ID;
    response["rol_local"] = localCoordinatorRole();
    response["peer_disponible"] = peerReaderOnline();
    response["modo_degradado"] = !loRaReady;
    sendJson(200, response);
  });
  server.on("/api/coordinacion/snapshot", HTTP_POST, receiveCoordinatorSnapshot);
  server.on("/api/layout", HTTP_GET, sendLayout);
  server.on("/api/lecturas", HTTP_POST, receiveReaderObservations);
  server.on("/api/layout/importar", HTTP_POST, []() {
    server.send(501, "text/plain; charset=utf-8", "En el modo autónomo el layout se carga junto con el firmware");
  });
  server.on("/api/reloj", HTTP_POST, []() {
    JsonDocument body;
    if (!parseJsonBody(body)) return;
    clockEpochBase = body["epoch"] | 0ULL;
    clockMillisBase = millis();
    JsonDocument response; response["sincronizado"] = clockEpochBase != 0; sendJson(200, response);
  });
  server.on("/api/mensajes", HTTP_GET, sendMessages);
  server.on("/api/mensajes", HTTP_POST, publishMessage);
  server.on("/api/mensajes", HTTP_DELETE, []() {
    if (!adminAuthenticated) { server.send(403, "text/plain; charset=utf-8", "Se requiere una sesión de administrador"); return; }
    const size_t removed = messageCount; messageCount = 0; saveMessages();
    JsonDocument response; response["eliminados"] = removed; sendJson(200, response);
  });
  server.on("/api/historial-proximidad", HTTP_DELETE, []() {
    if (!adminAuthenticated) { server.send(403, "text/plain; charset=utf-8", "Se requiere una sesión de administrador"); return; }
    const size_t removed = eventCount; eventCount = 0;
    for (TagState& tag : tags) tag.dangerCount = tag.nearCount = tag.offlineCount = 0;
    JsonDocument response; response["eliminados"] = removed; sendJson(200, response);
  });
  server.on("/api/supervisor/estado", HTTP_GET, []() {
    JsonDocument response; response["autenticado"] = !supervisorName.isEmpty(); response["nombre"] = supervisorName; sendJson(200, response);
  });
  server.on("/api/supervisor/login", HTTP_POST, []() {
    JsonDocument body; if (!parseJsonBody(body)) return;
    const String pin = body["pin"] | ""; String name = body["nombre"] | ""; name.trim();
    if (pin != SUPERVISOR_PIN) { server.send(401, "text/plain; charset=utf-8", "PIN de supervisor incorrecto"); return; }
    if (name.isEmpty() || name.length() > 40) { server.send(400, "text/plain; charset=utf-8", "Nombre de supervisor inválido"); return; }
    supervisorName = name; JsonDocument response; response["autenticado"] = true; response["nombre"] = name; sendJson(200, response);
  });
  server.on("/api/supervisor/logout", HTTP_POST, []() {
    supervisorName = ""; JsonDocument response; response["autenticado"] = false; sendJson(200, response);
  });
  server.on("/api/administrador/estado", HTTP_GET, []() {
    JsonDocument response; response["autenticado"] = adminAuthenticated; response["rol"] = "administrador"; sendJson(200, response);
  });
  server.on("/api/administrador/login", HTTP_POST, []() {
    JsonDocument body; if (!parseJsonBody(body)) return;
    const String pin = body["pin"] | "";
    if (pin != ADMIN_PIN) { server.send(401, "text/plain; charset=utf-8", "PIN de administrador incorrecto"); return; }
    adminAuthenticated = true; JsonDocument response; response["autenticado"] = true; response["rol"] = "administrador"; sendJson(200, response);
  });
  server.on("/api/administrador/logout", HTTP_POST, []() {
    adminAuthenticated = false; JsonDocument response; response["autenticado"] = false; sendJson(200, response);
  });
  server.serveStatic("/static/", LittleFS, "/static/");
  server.on("/generate_204", HTTP_ANY, serveIndex);
  server.on("/hotspot-detect.html", HTTP_ANY, serveIndex);
  server.on("/library/test/success.html", HTTP_ANY, serveIndex);
  server.on("/ncsi.txt", HTTP_ANY, serveIndex);
  server.on("/connecttest.txt", HTTP_ANY, serveIndex);
  server.onNotFound(handleDynamicApi);
  server.begin();
}

#if 0
void startNetwork() {
  WiFi.persistent(false);
  // El ESP32 clásico exige modem sleep al usar Wi-Fi y Bluetooth a la vez.
  WiFi.setSleep(true);
#if MINA_READER_NUMBER == 3
  WiFi.mode(WIFI_STA);
  const IPAddress localIp(192, 168, 4, 30);
  const IPAddress gateway(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  WiFi.config(localIp, gateway, subnet, gateway);
  WiFi.setAutoReconnect(true);

  // RX-02 tiene canal y BSSID reservados. La conexión directa evita el barrido
  // de todos los canales que antes agregaba varios segundos al arranque.
  Serial.printf("[RED] RX-03 enlazando directamente con RX-02 en canal %u...\n",
                AP_CHANNEL);
  WiFi.begin(AP_SSID, nullptr, AP_CHANNEL, PRIMARY_AP_BSSID, true);
  lastStaAttempt = millis();
  staConnecting = true;
  const uint32_t startupStarted = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startupStarted < STARTUP_CONNECT_TIMEOUT_MS) {
    delay(50);
  }
  if (WiFi.status() == WL_CONNECTED) {
    hubLostSince = 0;
    staConnecting = false;
    Serial.printf("[RED] RX-03 conectado a MINA-LOCAL con IP %s en %lu ms\n",
                  WiFi.localIP().toString().c_str(),
                  static_cast<unsigned long>(millis()));
  } else {
    WiFi.disconnect(false, false);
    staConnecting = false;
    // El intento inicial ya consumio la espera; activa el respaldo enseguida.
    hubLostSince = millis() - EMERGENCY_AP_DELAY_MS;
  }
#else
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, 8);
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.printf("[RED] MINA-LOCAL disponible desde RX-02 en %lu ms\n",
                static_cast<unsigned long>(millis()));
#endif
}

void startEmergencyAccessPoint() {
#if MINA_READER_NUMBER == 3
  if (emergencyApActive) return;
  // Cancela el intento STA antes de crear el AP: así el DHCP permanece estable
  // mientras el teléfono se asocia a la red de emergencia.
  WiFi.disconnect(false, false);
  staConnecting = false;
  WiFi.mode(WIFI_AP);
  const IPAddress emergencyIp(192, 168, 4, 1);
  const IPAddress emergencySubnet(255, 255, 255, 0);
  WiFi.softAPConfig(emergencyIp, emergencyIp, emergencySubnet);
  if (WiFi.softAP(EMERGENCY_SSID, nullptr, AP_CHANNEL, false, 4)) {
    dnsServer.start(DNS_PORT, "*", emergencyIp);
    emergencyApActive = true;
    hubOnline = false;
    lastEmergencyProbe = millis();
    Serial.printf("[EMERGENCIA] Red %s activa en http://%s\n", EMERGENCY_SSID, emergencyIp.toString().c_str());
    Serial.printf("[EMERGENCIA] MINA-LOCAL visible desde RX-03 en %lu ms\n",
                  static_cast<unsigned long>(millis()));
    Serial.println("[COORDINACION] RX-03 asumio como coordinador de respaldo");
  } else {
    Serial.println("[EMERGENCIA] No fue posible crear la red de respaldo");
  }
#endif
}

bool findPrimaryAccessPoint(uint8_t selectedBssid[6], int32_t& selectedChannel) {
#if MINA_READER_NUMBER == 3
  // La radio trabaja temporalmente como AP+STA para buscar otro MINA-LOCAL.
  // Se excluye el BSSID propio para que RX-03 no intente conectarse a sí mismo.
  WiFi.mode(WIFI_AP_STA);
  delay(20);
  const String ownBssid = WiFi.softAPmacAddress();
  // Solo revisa el canal conocido de RX-02. Así el AP de respaldo se
  // interrumpe durante el menor tiempo posible al comprobar la recuperación.
  const int16_t networkCount =
      WiFi.scanNetworks(false, true, false, 120, AP_CHANNEL);
  int32_t bestRssi = -1000;
  bool found = false;
  for (int16_t index = 0; index < networkCount; ++index) {
    if (WiFi.SSID(index) != AP_SSID) continue;
    if (WiFi.BSSIDstr(index).equalsIgnoreCase(ownBssid)) continue;
    if (!WiFi.BSSID(index) ||
        memcmp(WiFi.BSSID(index), PRIMARY_AP_BSSID, 6) != 0) continue;
    if (WiFi.RSSI(index) <= bestRssi) continue;
    const uint8_t* candidate = WiFi.BSSID(index);
    if (!candidate) continue;
    memcpy(selectedBssid, candidate, 6);
    selectedChannel = WiFi.channel(index);
    bestRssi = WiFi.RSSI(index);
    found = true;
  }
  WiFi.scanDelete();
  if (!found) WiFi.mode(WIFI_AP);
  return found;
#else
  return false;
#endif
}

void connectToPrimaryAccessPoint(const uint8_t bssid[6], int32_t channel) {
#if MINA_READER_NUMBER == 3
  dnsServer.stop();
  WiFi.softAPdisconnect(false);
  emergencyApActive = false;
  WiFi.mode(WIFI_STA);
  const IPAddress localIp(192, 168, 4, 30);
  const IPAddress gateway(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  WiFi.config(localIp, gateway, subnet, gateway);
  WiFi.begin(AP_SSID, nullptr, channel, bssid, true);
  lastStaAttempt = millis();
  hubLostSince = lastStaAttempt;
  staConnecting = true;
  Serial.printf("[EMERGENCIA] RX-02 detectado en canal %ld; transfiriendo coordinacion\n", static_cast<long>(channel));
#endif
}

void stopEmergencyAccessPoint() {
#if MINA_READER_NUMBER == 3
  if (!emergencyApActive) return;
  dnsServer.stop();
  WiFi.softAPdisconnect(false);
  WiFi.mode(WIFI_STA);
  emergencyApActive = false;
  Serial.println("[EMERGENCIA] RX-02 recuperado; red de respaldo cerrada");
  Serial.println("[COORDINACION] RX-02 vuelve a ser el coordinador preferido");
#endif
}

void maintainReaderNetwork() {
#if MINA_READER_NUMBER == 3
  if (WiFi.status() == WL_CONNECTED) {
    stopEmergencyAccessPoint();
    hubLostSince = 0;
    if (staConnecting) {
      Serial.printf("[RED] RX-03 conectado a MINA-LOCAL con IP %s\n", WiFi.localIP().toString().c_str());
      staConnecting = false;
    }
    return;
  }
  const uint32_t now = millis();

  if (emergencyApActive) {
    if (now - lastEmergencyProbe >= EMERGENCY_RECOVERY_PROBE_MS) {
      lastEmergencyProbe = now;
      uint8_t primaryBssid[6] = {0};
      int32_t primaryChannel = 0;
      if (findPrimaryAccessPoint(primaryBssid, primaryChannel)) {
        connectToPrimaryAccessPoint(primaryBssid, primaryChannel);
      } else {
        Serial.println("[EMERGENCIA] RX-02 sigue ausente; MINA-LOCAL permanece en RX-03");
      }
    }
    return;
  }

  // No recrear el AP mientras RX-03 intenta asociarse con RX-02.
  if (staConnecting) {
    if (now - lastStaAttempt >= STA_CONNECT_TIMEOUT_MS) {
      WiFi.disconnect(false, false);
      staConnecting = false;
      Serial.println("[RED] RX-02 no respondio; RX-03 conserva el respaldo MINA-LOCAL");
      startEmergencyAccessPoint();
    }
    return;
  }

  if (hubLostSince == 0) hubLostSince = now;
  if (now - hubLostSince >= EMERGENCY_AP_DELAY_MS) {
    startEmergencyAccessPoint();
    return;
  }
  if (!staConnecting && now - lastStaAttempt >= STA_RETRY_INTERVAL_MS) {
    if (emergencyApActive) WiFi.mode(WIFI_AP_STA);
    WiFi.begin("MINA-LOCAL");
    lastStaAttempt = now;
    staConnecting = true;
    Serial.println("[RED] Reintentando enlace de RX-03 con MINA-LOCAL");
  }
#endif
}

#endif

struct __attribute__((packed)) LoRaReading {
  uint16_t major;
  uint16_t minor;
  int8_t rssi;
  int8_t txPower;
  uint16_t age100ms;
};

struct __attribute__((packed)) LoRaObservationFrame {
  uint16_t magic;
  uint8_t version;
  uint8_t readerNumber;
  uint16_t sequence;
  uint8_t count;
  LoRaReading readings[TAG_COUNT];
};

void IRAM_ATTR onLoRaPacket() {
  loRaPacketReady = true;
}

void startNetwork() {
  WiFi.persistent(false);
  WiFi.setSleep(true);
  WiFi.mode(WIFI_AP);
  const IPAddress localIp(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(localIp, localIp, subnet);
  if (!WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, 8)) {
    Serial.println("[RED] ERROR al crear MINA-LOCAL");
    return;
  }
  dnsServer.start(DNS_PORT, "*", localIp);
  Serial.printf("[RED] %s disponible desde %s en http://%s\n",
                AP_SSID, READER_ID, localIp.toString().c_str());
}

void startLoRa() {
  SPI.begin(SCK, MISO, MOSI, SS);
  const int16_t state = radio.begin(
      LORA_FREQUENCY_MHZ, LORA_BANDWIDTH_KHZ, LORA_SPREADING_FACTOR,
      LORA_CODING_RATE, 0x12, LORA_POWER_DBM, 8, 1.8F, false);
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[LORA] ERROR de inicio: %d. Revisa antena y placa V3.\n", state);
    return;
  }
  radio.setPacketReceivedAction(onLoRaPacket);
  const int16_t receiveState = radio.startReceive();
  loRaReady = receiveState == RADIOLIB_ERR_NONE;
  Serial.printf("[LORA] %s en %.1f MHz, RX=%d\n",
                loRaReady ? "activo" : "error", LORA_FREQUENCY_MHZ, receiveState);
}

const char* oledSectorLabel() {
  if (LOCAL_READER_INDEX == 0) return "Portal / Control";
  if (LOCAL_READER_INDEX == 1) return "Rampa";
  return "Frente de trabajo";
}

void renderOledStatus() {
  if (!oledReady) return;
  oledDisplay.clear();
  oledDisplay.setTextAlignment(TEXT_ALIGN_CENTER);
  oledDisplay.setFont(ArialMT_Plain_16);
  oledDisplay.drawString(64, 0, READER_ID);
  oledDisplay.setFont(ArialMT_Plain_10);
  oledDisplay.drawString(64, 18, oledSectorLabel());
  oledDisplay.drawString(64, 29, "WiFi: MINA-LOCAL");
  oledDisplay.drawString(64, 40, "IP: 192.168.4.1");
  oledDisplay.drawString(64, 51,
                         String("LoRa 915: ") + (loRaReady ? "OK" : "ERROR"));
  oledDisplay.display();
}

void startOled() {
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, LOW);
  pinMode(RST_OLED, OUTPUT);
  digitalWrite(RST_OLED, LOW);
  delay(20);
  digitalWrite(RST_OLED, HIGH);
  delay(20);
  oledDisplay.init();
  oledDisplay.setBrightness(128);
  oledReady = true;
  renderOledStatus();
  Serial.printf("[OLED] Identificacion %s activa\n", READER_ID);
}

void transmitLocalObservations() {
  if (!loRaReady || millis() - lastLoRaTx < LORA_TX_INTERVAL_MS + LOCAL_READER_INDEX * 170) return;
  lastLoRaTx = millis();

  LoRaObservationFrame frame{};
  frame.magic = 0x4D49;
  frame.version = 1;
  frame.readerNumber = MINA_READER_NUMBER;
  frame.sequence = ++loRaSequence;
  const uint32_t now = millis();
  for (const TagState& tag : tags) {
    const ReaderObservation& observation = tag.observations[LOCAL_READER_INDEX];
    if (!observationVisible(observation, now)) continue;
    LoRaReading& reading = frame.readings[frame.count++];
    reading.major = tag.major;
    reading.minor = tag.minor;
    reading.rssi = static_cast<int8_t>(constrain(observation.rssi, -127, 0));
    reading.txPower = static_cast<int8_t>(constrain(observation.txPower, -127, 0));
    const uint32_t age100ms = (now - observation.lastSeen) / 100;
    reading.age100ms = static_cast<uint16_t>(age100ms > 65535 ? 65535 : age100ms);
  }

  const size_t length = offsetof(LoRaObservationFrame, readings) + frame.count * sizeof(LoRaReading);
  radio.clearPacketReceivedAction();
  const int16_t state = radio.transmit(reinterpret_cast<uint8_t*>(&frame), length);
  radio.setPacketReceivedAction(onLoRaPacket);
  radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) Serial.printf("[LORA] Error TX: %d\n", state);
}

void receiveLoRaObservations() {
  if (!loRaReady || !loRaPacketReady) return;
  loRaPacketReady = false;
  LoRaObservationFrame frame{};
  const size_t length = radio.getPacketLength();
  int16_t state = RADIOLIB_ERR_PACKET_TOO_LONG;
  if (length >= offsetof(LoRaObservationFrame, readings) && length <= sizeof(frame)) {
    state = radio.readData(reinterpret_cast<uint8_t*>(&frame), length);
  }
  radio.startReceive();
  if (state != RADIOLIB_ERR_NONE || frame.magic != 0x4D49 || frame.version != 1 ||
      frame.readerNumber < 1 || frame.readerNumber > READER_COUNT ||
      frame.readerNumber == MINA_READER_NUMBER || frame.count > TAG_COUNT) return;

  const size_t remoteIndex = frame.readerNumber - 1;
  readerLastSeen[remoteIndex] = millis();
  for (uint8_t index = 0; index < frame.count; ++index) {
    const LoRaReading& reading = frame.readings[index];
    for (TagState& tag : tags) {
      if (tag.major != reading.major || tag.minor != reading.minor) continue;
      ingestTag(tag, reading.rssi, reading.txPower, READERS[remoteIndex].id);
      ReaderObservation& observation = tag.observations[remoteIndex];
      const uint32_t age = static_cast<uint32_t>(reading.age100ms) * 100;
      observation.lastSeen = age < millis() ? millis() - age : 1;
      selectBestReader(tag);
      break;
    }
  }
}

void maintainReaderNetwork() {
  receiveLoRaObservations();
  transmitLocalObservations();
}

void startBluetooth() {
  NimBLEDevice::init("");
  scanner = NimBLEDevice::getScan();
  scanner->setScanCallbacks(&beaconCallbacks, true);
  scanner->setActiveScan(true);
  scanner->setMaxResults(0);
  scanner->setInterval(160);
  scanner->setWindow(120);
  scanner->start(0, false, true);
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  startOled();
  Serial.printf("\n[INICIO] Plataforma minera autónoma %s\n", READER_ID);
  if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) Serial.println("[FS] ERROR LittleFS");
  else {
    Serial.printf("[FS] %u bytes usados de %u\n", LittleFS.usedBytes(), LittleFS.totalBytes());
    loadMessages();
  }
  // La red debe quedar visible o enlazada antes de ocupar la radio con BLE.
  startNetwork();
  configureWeb();
  startLoRa();
  renderOledStatus();
  startBluetooth();
#if 0
#if MINA_READER_NUMBER == 3
  Serial.println("[WEB] RX-03 estará disponible en http://192.168.4.30 al conectarse a MINA-LOCAL");
#else
  Serial.printf("[WEB] Conecta el celular o notebook a %s y abre http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
#endif
#endif
  Serial.printf("[WEB] Conecta el celular o notebook a %s y abre http://%s\n",
                AP_SSID, WiFi.softAPIP().toString().c_str());
}

void loop() {
#if 0
#if MINA_READER_NUMBER == 2
  dnsServer.processNextRequest();
#else
  if (emergencyApActive) dnsServer.processNextRequest();
#endif
#endif
  dnsServer.processNextRequest();
  server.handleClient();
  maintainReaderNetwork();
  if (millis() - lastStateRefresh >= STATE_REFRESH_MS) {
    lastStateRefresh = millis();
    updateExpiredTags();
  }
  delay(3);
}

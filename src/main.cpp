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
    {"RX-01", "Heltec Reader 1 RX-01", "Nivel 1 - galeria de produccion, cota -250 m", 270, -70, -250},
    {"RX-02", "Heltec Reader 2 RX-02", "Nivel 2 - galeria intermedia, cota -450 m", 520, 40, -450},
    {"RX-03", "Heltec Reader 3 RX-03", "Nivel 3 - galeria profunda, cota -650 m", 780, -50, -650},
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
// Canal fijo distinto al canal 1 usado habitualmente por el router del lugar.
// Todos los respaldos lo conservan para que la reconexion sea rapida.
constexpr uint8_t AP_CHANNEL = 6;
constexpr char TARGET_UUID[] = "E2C56DB5DFFB48D2B060D0F5A71096E0";
constexpr char SUPERVISOR_PIN[] = "123456";
constexpr char ADMIN_PIN[] = "12345";
// Todos los portales MINA-LOCAL comparten esta credencial de sesion. El PIN
// nunca se guarda en el navegador: despues de validarlo se entrega una cookie
// HttpOnly que sigue siendo valida si el telefono cambia de RX durante el
// roaming entre puntos de acceso con la misma IP.
constexpr char ADMIN_SESSION_TOKEN[] = "mina-admin-v1-8f34c2d7";
constexpr char ADMIN_SESSION_COOKIE_NAME[] = "mina_admin";
constexpr uint8_t DNS_PORT = 53;
constexpr uint32_t TAG_TIMEOUT_MS = 8000;
constexpr uint32_t STATE_REFRESH_MS = 250;
// Confirma la primera deteccion con dos anuncios distintos; 300 ms evita un
// falso positivo aislado sin agregar una demora perceptible al operador.
constexpr uint32_t STATE_CONFIRM_MS = 300;
constexpr uint8_t STATE_CONFIRM_OBSERVATIONS = 2;
constexpr uint32_t READER_SWITCH_CONFIRM_MS = 1500;
constexpr uint8_t READER_SWITCH_OBSERVATIONS = 3;
constexpr uint8_t RSSI_WINDOW_SIZE = 7;
// RX-01 anuncia su estado cada 1,2 s. El respaldo espera cinco tramas ausentes
// antes de duplicar el SSID; al arrancar sin RX-01 conserva la gracia rapida
// definida abajo. Esto evita que un breve ruido LoRa haga oscilar al telefono.
constexpr uint32_t READER_HEARTBEAT_TIMEOUT_MS = 6500;
constexpr uint32_t PORTAL_ELECTION_GRACE_MS = 3000;
constexpr uint32_t PORTAL_HANDOVER_CONFIRM_MS = 2500;
// Primero se intenta el BSSID conocido de RX-02. Si no responde, RX-03
// publica MINA-LOCAL rápidamente, sin efectuar un barrido completo de canales.
constexpr uint32_t EMERGENCY_AP_DELAY_MS = 1200;
constexpr uint32_t STARTUP_CONNECT_TIMEOUT_MS = 2200;
constexpr uint32_t STA_CONNECT_TIMEOUT_MS = 3000;
constexpr uint32_t STA_RETRY_INTERVAL_MS = 4000;
constexpr uint32_t EMERGENCY_RECOVERY_PROBE_MS = 6000;
// El candidato debe estimarse aproximadamente 4 dB mas fuerte que el lector
// actual antes de iniciar el cambio de sector (factor de distancia ~= 0,66).
constexpr float READER_SWITCH_FACTOR = 0.66F;
constexpr char READER_TOKEN[] = "mina-local-rx-2026";
// El teléfono ve un único nombre de red, independientemente del reader activo.
constexpr char EMERGENCY_SSID[] = "MINA-LOCAL";
constexpr size_t MAX_EVENTS = 120;
constexpr size_t MAX_MESSAGES = 16;
constexpr size_t MAX_MESH_MESSAGES = 24;
constexpr size_t MAX_GPS_POINTS = 48;
constexpr char GPS_NODE_ID[] = "!f72ad896";
constexpr uint32_t GPS_NODE_NUM = 4146780310UL;
constexpr char GPS_BEACON_ID[] = "TAG-001";
constexpr uint32_t LORA_TX_INTERVAL_MS = 1200;
constexpr uint16_t LORA_PORTAL_SYNC_MAGIC = 0x5359;
constexpr uint8_t LORA_PORTAL_SYNC_VERSION = 1;
constexpr size_t LORA_PORTAL_SYNC_CHUNK_SIZE = 180;
constexpr size_t LORA_PORTAL_SYNC_MAX_CHUNKS = 6;
constexpr size_t LORA_PORTAL_SYNC_QUEUE_SIZE = 48;
constexpr uint8_t LORA_PORTAL_SYNC_REPEATS = 2;
constexpr uint32_t LORA_PORTAL_SYNC_TX_MS = 480;
constexpr uint32_t LORA_PORTAL_SYNC_ASSEMBLY_TIMEOUT_MS = 15000;
constexpr uint32_t LORA_GATEWAY_PRESENCE_SYNC_MS = 5000;
constexpr float LORA_FREQUENCY_MHZ = 915.0F;
constexpr float LORA_BANDWIDTH_KHZ = 125.0F;
constexpr uint8_t LORA_SPREADING_FACTOR = 7;
constexpr uint8_t LORA_CODING_RATE = 5;
constexpr int8_t LORA_POWER_DBM = 14;
constexpr uint8_t LORA_PORTAL_ACTIVE_FLAG = 0x80;
constexpr uint8_t LORA_READING_COUNT_MASK = 0x7F;

struct ReaderObservation {
  int rssi = -127;
  int txPower = -59;
  float filteredRssi = -127.0F;
  float distance = 0.0F;
  uint32_t lastSeen = 0;
  int8_t rssiWindow[RSSI_WINDOW_SIZE] = {-127, -127, -127, -127, -127};
  uint8_t windowCount = 0;
  uint8_t windowIndex = 0;
  uint32_t sampleCount = 0;
  float spread = 0.0F;
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
  const char* role;
  const char* crew;
  const char* shiftName;
  const char* shiftStart;
  const char* shiftEnd;
  int rssi = -127;
  int txPower = -59;
  float filteredRssi = -127.0F;
  float distance = 0.0F;
  float previousDistance = 0.0F;
  uint32_t lastSeen = 0;
  String readerId;
  String readerCandidateId;
  uint32_t readerCandidateSince = 0;
  uint32_t readerCandidateEvidence = 0;
  uint8_t readerCandidateCount = 0;
  ReaderObservation observations[READER_COUNT];
  String status = "sin_senal";
  String pendingStatus;
  uint32_t pendingStatusSince = 0;
  uint32_t pendingStatusEvidence = 0;
  uint8_t pendingStatusCount = 0;
  String trend = "sin_datos";
  uint32_t dangerCount = 0;
  uint32_t warningCount = 0;
  uint32_t nearCount = 0;
  uint32_t safeCount = 0;
  uint32_t offlineCount = 0;

  TagState(const char* tagId, const char* tagName, const char* tagType,
           const char* tagCategory, const char* tagOwner,
           const char* tagInstallation, uint16_t tagMajor, uint16_t tagMinor,
           const char* workerRole, const char* workerCrew,
           const char* workerShiftName, const char* workerShiftStart,
           const char* workerShiftEnd)
      : id(tagId), name(tagName), type(tagType), category(tagCategory),
        owner(tagOwner), installation(tagInstallation), major(tagMajor),
        minor(tagMinor), role(workerRole), crew(workerCrew),
        shiftName(workerShiftName), shiftStart(workerShiftStart),
        shiftEnd(workerShiftEnd) {}
};

struct EventRecord {
  String id;
  String type;
  String tagId;
  String name;
  String previous;
  String current;
  String previousReaderId;
  String readerId;
  String sector;
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

struct MeshMessage {
  bool used = false;
  String id;
  String direction;
  String body;
  String author;
  String status;
  uint64_t timestamp = 0;
  uint64_t updatedAt = 0;
  uint32_t packetId = 0;
  int rssi = 0;
  float snr = 0.0F;
};

struct GpsPoint {
  double latitude = 0.0;
  double longitude = 0.0;
  float altitude = 0.0F;
  uint64_t timestamp = 0;
  int batteryLevel = -1;
  float voltage = 0.0F;
  uint8_t precisionBits = 0;
  String source = "gnss";
};

struct GpsTrackerState {
  String nodeId = GPS_NODE_ID;
  uint32_t nodeNum = GPS_NODE_NUM;
  String beaconId = GPS_BEACON_ID;
  String assetType = "trabajador";
  String displayName = "Trabajador 01 - Supervisor";
  String hardware = "SenseCAP Tracker T1000-E";
  GpsPoint points[MAX_GPS_POINTS];
  size_t pointCount = 0;
};

struct PortalSyncJob {
  bool used = false;
  uint32_t key = 0;
  String payload;
  uint8_t chunkIndex = 0;
  uint8_t repetitionsRemaining = 0;
};

struct PortalSyncAssembly {
  uint32_t key = 0;
  uint8_t chunkCount = 0;
  uint8_t receivedMask = 0;
  uint32_t startedAt = 0;
  String chunks[LORA_PORTAL_SYNC_MAX_CHUNKS];
};

TagState tags[] = {
    {"TAG-001", "Trabajador 01 - Supervisor", "Beacon personal + geotracker", "persona", "EMP-001",
     "Beacon en casco o credencial + SenseCAP T1000-E", 0, 0, "Supervisor de terreno", "Supervision", "Turno dia", "08:00", "20:00"},
    {"TAG-002", "Trabajador minero 02", "Beacon personal", "persona", "EMP-002",
     "Instalado en casco o credencial", 1, 0, "Mantenedor mecanico", "Cuadrilla A", "Turno dia", "08:00", "20:00"},
    {"TAG-003", "Trabajador minero 03", "Beacon personal", "persona", "EMP-003",
     "Instalado en casco o credencial", 2, 0, "Supervisor de terreno", "Supervision", "Turno dia", "08:00", "20:00"},
};
constexpr size_t TAG_COUNT = sizeof(tags) / sizeof(tags[0]);

EventRecord events[MAX_EVENTS];
size_t eventCount = 0;
SupervisorMessage messages[MAX_MESSAGES];
size_t messageCount = 0;
MeshMessage meshMessages[MAX_MESH_MESSAGES];
size_t meshMessageCount = 0;
GpsTrackerState gpsTracker;

WebServer server(80);
DNSServer dnsServer;
NimBLEScan* scanner = nullptr;
SX1262 radio = new Module(SS, DIO0, RST_LoRa, BUSY_LoRa);
SSD1306Wire oledDisplay(0x3C, SDA_OLED, SCL_OLED, GEOMETRY_128_64,
                        I2C_ONE, 400000);
String supervisorName;
bool adminAuthenticated = false;
String adminName;
uint64_t clockEpochBase = 0;
uint32_t clockMillisBase = 0;
uint32_t lastStateRefresh = 0;
uint32_t lastLoRaTx = 0;
uint32_t readerLastSeen[READER_COUNT] = {0, 0, 0};
uint16_t loRaSequence = 0;
volatile bool loRaPacketReady = false;
bool loRaReady = false;
bool oledReady = false;
bool portalAccessPointActive = false;
bool readerPortalActive[READER_COUNT] = {false, false, false};
uint32_t portalElectionStartedAt = 0;
uint32_t preferredPortalStableSince = 0;
uint32_t meshGatewayLastSeen = 0;
uint32_t meshGatewayDirectLastSeen = 0;
PortalSyncJob portalSyncQueue[LORA_PORTAL_SYNC_QUEUE_SIZE];
size_t portalSyncQueueHead = 0;
size_t portalSyncQueueCount = 0;
PortalSyncAssembly portalSyncAssemblies[READER_COUNT];
uint32_t completedPortalSyncKey[READER_COUNT] = {0, 0, 0};
uint32_t lastPortalSyncTx = 0;
uint32_t lastGatewayPresenceSync = 0;

bool requestHasAdminCookie() {
  if (!server.hasHeader("Cookie")) return false;
  const String cookies = server.header("Cookie");
  const String key = String(ADMIN_SESSION_COOKIE_NAME) + "=";
  int cursor = 0;
  while (cursor < static_cast<int>(cookies.length())) {
    int end = cookies.indexOf(';', cursor);
    if (end < 0) end = cookies.length();
    String entry = cookies.substring(cursor, end);
    entry.trim();
    if (entry.startsWith(key) && entry.substring(key.length()) == ADMIN_SESSION_TOKEN) {
      return true;
    }
    cursor = end + 1;
  }
  return false;
}

bool adminRequestAuthorized() {
  return adminAuthenticated || requestHasAdminCookie();
}

void setAdminSessionCookie(bool enabled) {
  String cookie = String(ADMIN_SESSION_COOKIE_NAME) + "=";
  if (enabled) cookie += ADMIN_SESSION_TOKEN;
  cookie += "; Path=/; SameSite=Lax; HttpOnly";
  cookie += enabled ? "; Max-Age=28800" : "; Max-Age=0";
  server.sendHeader("Set-Cookie", cookie);
}

void renderOledStatus();
void queueSupervisorMessageSync(const SupervisorMessage& message);
void queueSupervisorDeleteSync(const String& id);
void queueSupervisorClearSync();
void queueMeshMessageSync(const MeshMessage& message);
void queueMeshClearSync();
void queueGpsPointSync(const GpsPoint& point);

String authenticatedOperatorName() {
  if (!supervisorName.isEmpty()) return supervisorName;
  if (adminRequestAuthorized() && !adminName.isEmpty()) return adminName;
  return "Administrador";
}

uint64_t epochNow() {
  if (clockEpochBase == 0) return 0;
  return clockEpochBase + static_cast<uint64_t>(millis() - clockMillisBase);
}

bool readerOnline(size_t index, uint32_t now) {
  if (index == LOCAL_READER_INDEX) return true;
  return readerLastSeen[index] != 0 &&
         now - readerLastSeen[index] <= READER_HEARTBEAT_TIMEOUT_MS;
}

int activePortalIndex() {
  const uint32_t now = millis();
  for (size_t index = 0; index < READER_COUNT; ++index) {
    const bool active = index == LOCAL_READER_INDEX
                            ? portalAccessPointActive
                            : readerPortalActive[index];
    if (active && readerOnline(index, now)) return static_cast<int>(index);
  }
  return -1;
}

String activeCoordinatorId() {
  // Cada reader que publica MINA-LOCAL atiende su propio portal. Informar el
  // reader local permite comprobar desde la pagina a que BSSID se asocio el
  // telefono, aunque otros puntos de acceso sigan disponibles por LoRa.
  if (portalAccessPointActive) return READER_ID;
  const int index = activePortalIndex();
  return index >= 0 ? READERS[index].id : "buscando";
}

String localCoordinatorRole() {
  if (portalAccessPointActive) return "punto_acceso_distribuido";
  return "reader_distribuido";
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

String classifyDistanceStable(float distance, const String& current) {
  // Los limites de salida son mas amplios que los de entrada. Esta histeresis
  // evita que pequenas variaciones de RSSI hagan oscilar el estado en el borde.
  if (current == "peligro" && distance <= 2.6F) return "peligro";
  if (current == "precaucion") {
    if (distance <= 1.7F) return "peligro";
    if (distance <= 6.0F) return "precaucion";
  }
  if (current == "proximo") {
    if (distance <= 4.4F) return "precaucion";
    if (distance <= 14.0F) return "proximo";
  }
  if (current == "seguro" && distance <= 10.5F) return "proximo";
  return classifyDistance(distance);
}

int readerIndexFor(const String& readerId);
const char* readerSectorFor(const String& readerId);
void saveHistory();

EventRecord& beginEvent(TagState& tag, const String& type) {
  for (size_t index = min(eventCount, MAX_EVENTS - 1); index > 0; --index) {
    events[index] = events[index - 1];
  }
  if (eventCount < MAX_EVENTS) ++eventCount;
  EventRecord& item = events[0];
  item = EventRecord{};
  item.id = String(epochNow() != 0 ? epochNow() : millis()) + "-" + tag.id + "-" + type;
  item.type = type;
  item.tagId = tag.id;
  item.name = tag.name;
  item.distance = tag.distance;
  item.rssi = static_cast<int>(roundf(tag.filteredRssi));
  item.timestamp = epochNow();
  return item;
}

void addEvent(TagState& tag, const String& previous, const String& current) {
  EventRecord& item = beginEvent(tag, "estado");
  item.previous = previous;
  item.current = current;
  item.readerId = tag.readerId;
  item.sector = readerSectorFor(tag.readerId);
  if (current == "peligro") ++tag.dangerCount;
  if (current == "precaucion") ++tag.warningCount;
  if (current == "proximo") ++tag.nearCount;
  if (current == "seguro") ++tag.safeCount;
  if (current == "sin_senal") ++tag.offlineCount;
  saveHistory();
}

void addReaderEvent(TagState& tag, const String& previousReader, const String& currentReader) {
  EventRecord& item = beginEvent(tag, "sector");
  item.previous = tag.status;
  item.current = tag.status;
  item.previousReaderId = previousReader;
  item.readerId = currentReader;
  item.sector = readerSectorFor(currentReader);
  saveHistory();
}

void commitTagTransition(TagState& tag, const String& next) {
  if (tag.status == next) return;
  const String previous = tag.status;
  tag.status = next;
  tag.pendingStatus = "";
  tag.pendingStatusCount = 0;
  addEvent(tag, previous, next);
}

void confirmTagTransition(TagState& tag, const String& next, uint32_t evidenceAt) {
  if (tag.status == next) {
    tag.pendingStatus = "";
    tag.pendingStatusCount = 0;
    return;
  }
  if (next == "sin_senal") {
    commitTagTransition(tag, next);
    return;
  }
  // UUID, major y minor ya fueron validados antes de llegar aqui. Cuando un
  // TAG reaparece, mostrarlo con el primer anuncio evita demoras sin aumentar
  // el uso de la radio; las transiciones posteriores conservan dos muestras.
  if (tag.status == "sin_senal") {
    commitTagTransition(tag, next);
    return;
  }
  const uint32_t now = millis();
  if (tag.pendingStatus != next) {
    tag.pendingStatus = next;
    tag.pendingStatusSince = now;
    tag.pendingStatusEvidence = evidenceAt;
    tag.pendingStatusCount = 1;
    return;
  }
  if (tag.pendingStatusEvidence != evidenceAt) {
    tag.pendingStatusEvidence = evidenceAt;
    if (tag.pendingStatusCount < 255) ++tag.pendingStatusCount;
  }
  if (tag.pendingStatusCount >= STATE_CONFIRM_OBSERVATIONS &&
      now - tag.pendingStatusSince >= STATE_CONFIRM_MS) {
    commitTagTransition(tag, next);
  }
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

float medianRssi(const ReaderObservation& observation) {
  int values[RSSI_WINDOW_SIZE];
  for (uint8_t index = 0; index < observation.windowCount; ++index) {
    values[index] = observation.rssiWindow[index];
  }
  for (uint8_t left = 0; left < observation.windowCount; ++left) {
    for (uint8_t right = left + 1; right < observation.windowCount; ++right) {
      if (values[right] < values[left]) {
        const int temporary = values[left];
        values[left] = values[right];
        values[right] = temporary;
      }
    }
  }
  if (observation.windowCount == 0) return -127.0F;
  const uint8_t middle = observation.windowCount / 2;
  if (observation.windowCount % 2 != 0) return static_cast<float>(values[middle]);
  return (static_cast<float>(values[middle - 1]) + values[middle]) / 2.0F;
}

void selectBestReader(TagState& tag) {
  const uint32_t now = millis();
  ReaderObservation* strongest = nullptr;
  String strongestId;

  for (size_t index = 0; index < READER_COUNT; ++index) {
    ReaderObservation& candidate = tag.observations[index];
    if (!observationVisible(candidate, now)) continue;
    if (strongest == nullptr || candidate.distance < strongest->distance) {
      strongest = &candidate;
      strongestId = READERS[index].id;
    }
  }

  int currentIndex = readerIndexFor(tag.readerId);
  ReaderObservation* current = nullptr;
  if (currentIndex >= 0 && observationVisible(tag.observations[currentIndex], now)) {
    current = &tag.observations[currentIndex];
  }

  ReaderObservation* selected = current != nullptr ? current : strongest;
  String selectedId = current != nullptr ? tag.readerId : strongestId;
  if (current != nullptr && strongest != nullptr && strongestId != tag.readerId &&
      strongest->distance < current->distance * READER_SWITCH_FACTOR) {
    if (tag.readerCandidateId != strongestId) {
      tag.readerCandidateId = strongestId;
      tag.readerCandidateSince = now;
      tag.readerCandidateEvidence = strongest->lastSeen;
      tag.readerCandidateCount = 1;
    } else if (tag.readerCandidateEvidence != strongest->lastSeen) {
      tag.readerCandidateEvidence = strongest->lastSeen;
      if (tag.readerCandidateCount < 255) ++tag.readerCandidateCount;
    }
    if (tag.readerCandidateCount >= READER_SWITCH_OBSERVATIONS &&
        now - tag.readerCandidateSince >= READER_SWITCH_CONFIRM_MS) {
      selected = strongest;
      selectedId = strongestId;
      tag.readerCandidateId = "";
      tag.readerCandidateCount = 0;
    }
  } else {
    tag.readerCandidateId = "";
    tag.readerCandidateCount = 0;
  }

  if (selected == nullptr) {
    confirmTagTransition(tag, "sin_senal", now);
    tag.trend = "sin_datos";
    return;
  }

  const String previousReader = tag.readerId;
  const uint32_t previousEvidence = tag.lastSeen;
  if (selected->lastSeen != previousEvidence) tag.previousDistance = tag.distance;
  tag.readerId = selectedId;
  tag.rssi = selected->rssi;
  tag.txPower = selected->txPower;
  tag.filteredRssi = selected->filteredRssi;
  tag.distance = selected->distance;
  tag.lastSeen = selected->lastSeen;
  if (selected->lastSeen != previousEvidence) {
    if (tag.previousDistance == 0.0F || fabsf(tag.distance - tag.previousDistance) < 0.6F) tag.trend = "estable";
    else tag.trend = tag.distance < tag.previousDistance ? "acercandose" : "alejandose";
  }
  if (previousReader != selectedId) addReaderEvent(tag, previousReader, selectedId);
  confirmTagTransition(tag, classifyDistanceStable(tag.distance, tag.status), selected->lastSeen);
}

void updateExpiredTags() {
  for (TagState& tag : tags) selectBestReader(tag);
}

void ingestTag(TagState& tag, int rssi, int advertisedTxPower, const String& readerId,
               uint32_t ageMs = 0) {
  ReaderObservation& observation = observationFor(tag, readerId);
  observation.rssi = constrain(rssi, -120, -20);
  if (advertisedTxPower > -100 && advertisedTxPower < -20) observation.txPower = advertisedTxPower;
  observation.rssiWindow[observation.windowIndex] = static_cast<int8_t>(observation.rssi);
  observation.windowIndex = (observation.windowIndex + 1) % RSSI_WINDOW_SIZE;
  if (observation.windowCount < RSSI_WINDOW_SIZE) ++observation.windowCount;
  ++observation.sampleCount;
  int minimum = 0;
  int maximum = -127;
  for (uint8_t index = 0; index < observation.windowCount; ++index) {
    minimum = min(minimum, static_cast<int>(observation.rssiWindow[index]));
    maximum = max(maximum, static_cast<int>(observation.rssiWindow[index]));
  }
  observation.spread = static_cast<float>(maximum - minimum);
  const float median = medianRssi(observation);
  if (observation.filteredRssi < -120.0F) observation.filteredRssi = median;
  else {
    // Limita saltos aislados antes del suavizado exponencial.
    const float boundedDelta = constrain(median - observation.filteredRssi, -10.0F, 10.0F);
    const float absoluteDelta = fabsf(boundedDelta);
    const float alpha = absoluteDelta >= 5.0F ? 0.38F :
        observation.spread <= 6.0F ? 0.30F : 0.20F;
    observation.filteredRssi += alpha * boundedDelta;
  }
  observation.distance = estimateDistance(observation.filteredRssi, observation.txPower);
  const uint32_t now = millis();
  observation.lastSeen = ageMs < now ? now - ageMs : 1;
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

void appendMeshMessageJson(JsonObject item, const MeshMessage& message) {
  item["id"] = message.id;
  item["direccion"] = message.direction;
  item["mensaje"] = message.body;
  item["autor"] = message.author;
  item["estado"] = message.status;
  addTimestamp(item, "fecha", message.timestamp);
  addTimestamp(item, "actualizado", message.updatedAt);
  if (message.packetId == 0) item["packet_id"] = nullptr;
  else item["packet_id"] = message.packetId;
  if (message.direction == "entrante") {
    item["rssi"] = message.rssi;
    item["snr"] = serialized(String(message.snr, 1));
  }
}

void saveMeshMessages() {
  File file = LittleFS.open("/mensajes_mesh.json", "w");
  if (!file) return;
  JsonDocument document;
  document["version"] = 1;
  JsonArray array = document["mensajes"].to<JsonArray>();
  for (size_t index = 0; index < meshMessageCount; ++index) {
    if (meshMessages[index].used) appendMeshMessageJson(array.add<JsonObject>(), meshMessages[index]);
  }
  serializeJson(document, file);
  file.close();
}

void loadMeshMessages() {
  if (!LittleFS.exists("/mensajes_mesh.json")) return;
  File file = LittleFS.open("/mensajes_mesh.json", "r");
  JsonDocument document;
  if (deserializeJson(document, file)) {
    file.close();
    return;
  }
  file.close();
  meshMessageCount = 0;
  for (JsonObject item : document["mensajes"].as<JsonArray>()) {
    if (meshMessageCount >= MAX_MESH_MESSAGES) break;
    MeshMessage& message = meshMessages[meshMessageCount++];
    message.used = true;
    message.id = item["id"] | "";
    message.direction = item["direccion"] | "entrante";
    message.body = item["mensaje"] | "";
    message.author = item["autor"] | "";
    message.status = item["estado"] | "recibido";
    message.timestamp = item["fecha"] | 0ULL;
    message.updatedAt = item["actualizado"] | 0ULL;
    message.packetId = item["packet_id"] | 0U;
    message.rssi = item["rssi"] | 0;
    message.snr = item["snr"] | 0.0F;
  }
}

MeshMessage& prependMeshMessage() {
  if (meshMessageCount == MAX_MESH_MESSAGES) {
    for (size_t index = MAX_MESH_MESSAGES - 1; index > 0; --index) meshMessages[index] = meshMessages[index - 1];
  } else {
    for (size_t index = meshMessageCount; index > 0; --index) meshMessages[index] = meshMessages[index - 1];
    ++meshMessageCount;
  }
  meshMessages[0] = MeshMessage{};
  meshMessages[0].used = true;
  return meshMessages[0];
}

int findMeshMessage(const String& id) {
  for (size_t index = 0; index < meshMessageCount; ++index) {
    if (meshMessages[index].used && meshMessages[index].id == id) return static_cast<int>(index);
  }
  return -1;
}

bool meshConversationAuthorized() {
  return adminRequestAuthorized() || !supervisorName.isEmpty();
}

bool validGatewayBody(JsonDocument& body) {
  if (!parseJsonBody(body)) return false;
  const String token = body["token"] | "";
  if (token == READER_TOKEN) {
    meshGatewayLastSeen = millis();
    meshGatewayDirectLastSeen = meshGatewayLastSeen;
    return true;
  }
  server.send(403, "text/plain; charset=utf-8", "Token de gateway invalido");
  return false;
}

void sendMeshMessages() {
  if (!meshConversationAuthorized()) {
    JsonDocument response;
    response["autorizado"] = false;
    response["requiere_rol"] = "supervisor_o_administrador";
    sendJson(403, response);
    return;
  }
  JsonDocument response;
  response["autorizado"] = true;
  response["node_id"] = GPS_NODE_ID;
  response["trabajador"] = "Trabajador 01";
  response["gateway_disponible"] = meshGatewayLastSeen != 0 && millis() - meshGatewayLastSeen <= 12000;
  JsonArray array = response["mensajes"].to<JsonArray>();
  for (size_t index = 0; index < meshMessageCount; ++index) {
    if (meshMessages[index].used) appendMeshMessageJson(array.add<JsonObject>(), meshMessages[index]);
  }
  sendJson(200, response);
}

void publishMeshMessage() {
  if (!meshConversationAuthorized()) {
    server.send(401, "text/plain; charset=utf-8", "Sesion de supervisor o administrador requerida");
    return;
  }
  JsonDocument body;
  if (!parseJsonBody(body)) return;
  String text = body["mensaje"] | "";
  text.trim();
  if (text.isEmpty()) {
    server.send(400, "text/plain; charset=utf-8", "El mensaje es obligatorio");
    return;
  }
  if (text.length() > 180) {
    server.send(400, "text/plain; charset=utf-8", "Maximo 180 bytes por mensaje Meshtastic");
    return;
  }
  MeshMessage& message = prependMeshMessage();
  const uint64_t moment = epochNow();
  message.id = String(moment != 0 ? moment : millis()) + "-" + String(esp_random(), HEX) + "-mesh";
  message.direction = "saliente";
  message.body = text;
  message.author = authenticatedOperatorName();
  message.status = "en_cola";
  message.timestamp = moment;
  message.updatedAt = moment;
  saveMeshMessages();
  queueMeshMessageSync(message);
  JsonDocument response;
  response["guardado"] = true;
  response["gateway_disponible"] = meshGatewayLastSeen != 0 && millis() - meshGatewayLastSeen <= 12000;
  appendMeshMessageJson(response["mensaje"].to<JsonObject>(), message);
  sendJson(201, response);
}

void clearMeshMessageHistory() {
  if (!meshConversationAuthorized()) {
    server.send(403, "text/plain; charset=utf-8",
                "Sesion de supervisor o administrador requerida");
    return;
  }

  size_t retained = 0;
  size_t removed = 0;
  for (size_t index = 0; index < meshMessageCount; ++index) {
    const MeshMessage& message = meshMessages[index];
    const bool pendingDelivery = message.used &&
        message.direction == "saliente" &&
        (message.status == "en_cola" || message.status == "error" ||
         message.status == "transmitiendo");
    if (pendingDelivery) {
      if (retained != index) meshMessages[retained] = message;
      ++retained;
    } else if (message.used) {
      ++removed;
    }
  }
  for (size_t index = retained; index < meshMessageCount; ++index) {
    meshMessages[index] = MeshMessage{};
  }
  meshMessageCount = retained;
  saveMeshMessages();
  queueMeshClearSync();
  for (size_t index = 0; index < meshMessageCount; ++index) {
    if (meshMessages[index].used) queueMeshMessageSync(meshMessages[index]);
  }

  JsonDocument response;
  response["eliminados"] = removed;
  response["pendientes_conservados"] = retained;
  response["t1000_modificado"] = false;
  sendJson(200, response);
}

void sendMeshQueueToGateway() {
  JsonDocument body;
  if (!validGatewayBody(body)) return;
  JsonDocument response;
  response["disponible"] = false;
  for (size_t reverse = meshMessageCount; reverse > 0; --reverse) {
    MeshMessage& message = meshMessages[reverse - 1];
    if (!message.used || message.direction != "saliente" ||
        (message.status != "en_cola" && message.status != "error")) continue;
    response["disponible"] = true;
    response["id"] = message.id;
    response["node_id"] = GPS_NODE_ID;
    response["node_num"] = GPS_NODE_NUM;
    response["mensaje"] = message.body;
    break;
  }
  sendJson(200, response);
}

void updateMeshMessageFromGateway() {
  JsonDocument body;
  if (!validGatewayBody(body)) return;
  const String id = body["id"] | "";
  const String status = body["estado"] | "";
  const int index = findMeshMessage(id);
  if (index < 0) {
    server.send(404, "text/plain; charset=utf-8", "Mensaje Meshtastic no encontrado");
    return;
  }
  if (status != "transmitiendo" && status != "transmitido" &&
      status != "confirmado" && status != "error") {
    server.send(400, "text/plain; charset=utf-8", "Estado Meshtastic invalido");
    return;
  }
  MeshMessage& message = meshMessages[index];
  message.status = status;
  message.packetId = body["packet_id"] | message.packetId;
  message.updatedAt = epochNow();
  saveMeshMessages();
  queueMeshMessageSync(message);
  JsonDocument response;
  response["actualizado"] = true;
  appendMeshMessageJson(response["mensaje"].to<JsonObject>(), message);
  sendJson(200, response);
}

void receiveMeshMessageFromGateway() {
  JsonDocument body;
  if (!validGatewayBody(body)) return;
  const String nodeId = body["node_id"] | "";
  const uint32_t nodeNum = body["node_num"] | 0U;
  String text = body["mensaje"] | "";
  text.trim();
  const uint32_t packetId = body["packet_id"] | 0U;
  if (nodeId != GPS_NODE_ID || nodeNum != GPS_NODE_NUM) {
    server.send(409, "text/plain; charset=utf-8", "El mensaje no pertenece al T1000-E de Trabajador 01");
    return;
  }
  if (text.isEmpty() || text.length() > 200 || packetId == 0) {
    server.send(400, "text/plain; charset=utf-8", "Mensaje Meshtastic entrante invalido");
    return;
  }
  for (size_t index = 0; index < meshMessageCount; ++index) {
    if (meshMessages[index].direction == "entrante" && meshMessages[index].packetId == packetId) {
      JsonDocument response;
      response["recibido"] = true;
      response["duplicado"] = true;
      sendJson(200, response);
      return;
    }
  }
  MeshMessage& message = prependMeshMessage();
  message.id = String(packetId) + "-in-mesh";
  message.direction = "entrante";
  message.body = text;
  message.author = "Trabajador 01";
  message.status = "recibido";
  uint64_t receivedAt = body["timestamp"] | 0ULL;
  if (receivedAt > 0 && receivedAt < 100000000000ULL) receivedAt *= 1000ULL;
  message.timestamp = receivedAt != 0 ? receivedAt : epochNow();
  message.updatedAt = epochNow();
  message.packetId = packetId;
  message.rssi = body["rssi"] | 0;
  message.snr = body["snr"] | 0.0F;
  saveMeshMessages();
  queueMeshMessageSync(message);
  JsonDocument response;
  response["recibido"] = true;
  response["duplicado"] = false;
  appendMeshMessageJson(response["mensaje"].to<JsonObject>(), message);
  sendJson(201, response);
}

void appendEventJson(JsonArray array, const EventRecord& event) {
  JsonObject item = array.add<JsonObject>();
  item["id"] = event.id;
  item["tipo"] = event.type;
  item["beacon_id"] = event.tagId;
  item["nombre"] = event.name;
  item["anterior"] = event.previous;
  item["estado"] = event.current;
  item["reader_anterior"] = event.previousReaderId;
  item["reader_id"] = event.readerId;
  item["sector"] = event.sector;
  item["distancia"] = serialized(String(event.distance, 1));
  item["rssi"] = event.rssi;
  addTimestamp(item, "fecha", event.timestamp);
}

void saveHistory() {
  File file = LittleFS.open("/historial.json", "w");
  if (!file) return;
  JsonDocument document;
  document["version"] = 1;
  JsonArray array = document["eventos"].to<JsonArray>();
  for (size_t index = 0; index < eventCount; ++index) appendEventJson(array, events[index]);
  JsonArray counters = document["contadores"].to<JsonArray>();
  for (const TagState& tag : tags) {
    JsonObject item = counters.add<JsonObject>();
    item["id"] = tag.id;
    item["peligro"] = tag.dangerCount;
    item["precaucion"] = tag.warningCount;
    item["proximo"] = tag.nearCount;
    item["seguro"] = tag.safeCount;
    item["sin_senal"] = tag.offlineCount;
  }
  serializeJson(document, file);
  file.close();
}

void loadHistory() {
  if (!LittleFS.exists("/historial.json")) return;
  File file = LittleFS.open("/historial.json", "r");
  JsonDocument document;
  if (deserializeJson(document, file)) {
    file.close();
    return;
  }
  file.close();
  eventCount = 0;
  for (JsonObject source : document["eventos"].as<JsonArray>()) {
    if (eventCount >= MAX_EVENTS) break;
    EventRecord& event = events[eventCount++];
    event.id = source["id"] | "";
    event.type = source["tipo"] | "estado";
    event.tagId = source["beacon_id"] | "";
    event.name = source["nombre"] | "";
    event.previous = source["anterior"] | "";
    event.current = source["estado"] | "";
    event.previousReaderId = source["reader_anterior"] | "";
    event.readerId = source["reader_id"] | "";
    event.sector = source["sector"] | "";
    event.distance = source["distancia"] | 0.0F;
    event.rssi = source["rssi"] | -127;
    event.timestamp = source["fecha"] | 0ULL;
  }
  for (JsonObject source : document["contadores"].as<JsonArray>()) {
    const String id = source["id"] | "";
    for (TagState& tag : tags) {
      if (id != tag.id) continue;
      tag.dangerCount = source["peligro"] | 0U;
      tag.warningCount = source["precaucion"] | 0U;
      tag.nearCount = source["proximo"] | 0U;
      tag.safeCount = source["seguro"] | 0U;
      tag.offlineCount = source["sin_senal"] | 0U;
      break;
    }
  }
}

bool gpsAccessAuthorized() {
  return adminRequestAuthorized() || !supervisorName.isEmpty();
}

bool readGpsPoint(JsonObject source, GpsPoint& point) {
  point.latitude = source["latitude"] | 0.0;
  point.longitude = source["longitude"] | 0.0;
  point.altitude = source["altitude"] | 0.0F;
  point.timestamp = source["timestamp"] | 0ULL;
  point.batteryLevel = source["battery_level"] | -1;
  point.voltage = source["voltage"] | 0.0F;
  point.precisionBits = source["precision_bits"] | 0;
  point.source = source["source"] | "gnss";
  return std::isfinite(point.latitude) && std::isfinite(point.longitude) &&
         point.latitude >= -90.0 && point.latitude <= 90.0 &&
         point.longitude >= -180.0 && point.longitude <= 180.0 &&
         point.timestamp != 0;
}

void appendGpsPointJson(JsonObject item, const GpsPoint& point) {
  item["latitude"] = serialized(String(point.latitude, 7));
  item["longitude"] = serialized(String(point.longitude, 7));
  item["altitude"] = serialized(String(point.altitude, 1));
  addTimestamp(item, "timestamp", point.timestamp);
  if (point.batteryLevel < 0) item["battery_level"] = nullptr;
  else item["battery_level"] = point.batteryLevel;
  if (point.voltage <= 0.0F) item["voltage"] = nullptr;
  else item["voltage"] = serialized(String(point.voltage, 2));
  item["precision_bits"] = point.precisionBits;
  item["source"] = point.source;
}

void saveGpsTracker() {
  File file = LittleFS.open("/gps_tracker.json", "w");
  if (!file) return;
  JsonDocument document;
  document["version"] = 1;
  document["node_id"] = gpsTracker.nodeId;
  document["node_num"] = gpsTracker.nodeNum;
  document["beacon_id"] = gpsTracker.beaconId;
  document["asset_type"] = gpsTracker.assetType;
  document["display_name"] = gpsTracker.displayName;
  document["hardware"] = gpsTracker.hardware;
  if (gpsTracker.pointCount > 0) {
    JsonObject position = document["position"].to<JsonObject>();
    appendGpsPointJson(position, gpsTracker.points[0]);
  }
  JsonArray history = document["history"].to<JsonArray>();
  for (size_t index = 0; index < gpsTracker.pointCount; ++index) {
    appendGpsPointJson(history.add<JsonObject>(), gpsTracker.points[index]);
  }
  serializeJson(document, file);
  file.close();
}

void loadGpsTracker() {
  if (!LittleFS.exists("/gps_tracker.json")) return;
  File file = LittleFS.open("/gps_tracker.json", "r");
  JsonDocument document;
  if (deserializeJson(document, file)) {
    file.close();
    return;
  }
  file.close();
  gpsTracker.nodeId = document["node_id"] | GPS_NODE_ID;
  gpsTracker.nodeNum = document["node_num"] | GPS_NODE_NUM;
  gpsTracker.beaconId = document["beacon_id"] | GPS_BEACON_ID;
  gpsTracker.assetType = document["asset_type"] | "trabajador";
  gpsTracker.displayName = document["display_name"] | "Trabajador 01 - Supervisor";
  gpsTracker.hardware = document["hardware"] | "SenseCAP Tracker T1000-E";
  gpsTracker.pointCount = 0;
  for (JsonObject source : document["history"].as<JsonArray>()) {
    if (gpsTracker.pointCount >= MAX_GPS_POINTS) break;
    GpsPoint point;
    if (readGpsPoint(source, point)) gpsTracker.points[gpsTracker.pointCount++] = point;
  }
  if (gpsTracker.pointCount == 0 && document["position"].is<JsonObject>()) {
    GpsPoint point;
    if (readGpsPoint(document["position"].as<JsonObject>(), point)) {
      gpsTracker.points[gpsTracker.pointCount++] = point;
    }
  }
}

void prependGpsPoint(const GpsPoint& point) {
  if (gpsTracker.pointCount > 0 && gpsTracker.points[0].timestamp == point.timestamp) {
    gpsTracker.points[0] = point;
    return;
  }
  const size_t last = min(gpsTracker.pointCount, MAX_GPS_POINTS - 1);
  for (size_t index = last; index > 0; --index) gpsTracker.points[index] = gpsTracker.points[index - 1];
  gpsTracker.points[0] = point;
  if (gpsTracker.pointCount < MAX_GPS_POINTS) ++gpsTracker.pointCount;
}

void sendGpsTracker() {
  JsonDocument document;
  document["autorizado"] = gpsAccessAuthorized();
  document["node_id"] = gpsTracker.nodeId;
  document["node_num"] = gpsTracker.nodeNum;
  document["beacon_id"] = gpsTracker.beaconId;
  document["asset_type"] = gpsTracker.assetType;
  document["display_name"] = gpsTracker.displayName;
  document["hardware"] = gpsTracker.hardware;
  document["tiene_posicion"] = gpsTracker.pointCount > 0;
  document["requiere_rol"] = "supervisor_o_administrador";
  if (!gpsAccessAuthorized()) {
    sendJson(403, document);
    return;
  }
  if (gpsTracker.pointCount > 0) {
    JsonObject position = document["position"].to<JsonObject>();
    appendGpsPointJson(position, gpsTracker.points[0]);
    const uint64_t now = epochNow();
    if (now == 0 || gpsTracker.points[0].timestamp > now) document["edad_ms"] = nullptr;
    else document["edad_ms"] = now - gpsTracker.points[0].timestamp;
  }
  JsonArray history = document["history"].to<JsonArray>();
  for (size_t index = 0; index < gpsTracker.pointCount; ++index) {
    appendGpsPointJson(history.add<JsonObject>(), gpsTracker.points[index]);
  }
  sendJson(200, document);
}

void receiveGpsTrackerUpdate() {
  JsonDocument body;
  if (!parseJsonBody(body)) return;
  const String token = body["token"] | "";
  if (token != READER_TOKEN && !gpsAccessAuthorized()) {
    server.send(403, "text/plain; charset=utf-8", "Token o sesion autorizada requerida");
    return;
  }
  const String nodeId = body["node_id"] | GPS_NODE_ID;
  const uint32_t nodeNum = body["node_num"] | GPS_NODE_NUM;
  if (nodeId != GPS_NODE_ID || nodeNum != GPS_NODE_NUM) {
    server.send(409, "text/plain; charset=utf-8", "El punto GPS no pertenece al T1000-E asociado a TAG-001");
    return;
  }
  JsonObject source = body["position"].is<JsonObject>()
                          ? body["position"].as<JsonObject>()
                          : body.as<JsonObject>();
  GpsPoint point;
  point.latitude = source["latitude"] | 0.0;
  point.longitude = source["longitude"] | 0.0;
  point.altitude = source["altitude"] | 0.0F;
  point.timestamp = source["timestamp"] | 0ULL;
  if (point.timestamp > 0 && point.timestamp < 100000000000ULL) point.timestamp *= 1000ULL;
  point.batteryLevel = source["battery_level"] | -1;
  point.voltage = source["voltage"] | 0.0F;
  point.precisionBits = source["precision_bits"] | 0;
  point.source = source["source"] | "meshtastic_gnss";
  if (!std::isfinite(point.latitude) || !std::isfinite(point.longitude) ||
      point.latitude < -90.0 || point.latitude > 90.0 ||
      point.longitude < -180.0 || point.longitude > 180.0 || point.timestamp == 0) {
    server.send(400, "text/plain; charset=utf-8", "Coordenada o fecha GPS invalida");
    return;
  }
  prependGpsPoint(point);
  saveGpsTracker();
  queueGpsPointSync(point);
  JsonDocument response;
  response["actualizado"] = true;
  response["node_id"] = gpsTracker.nodeId;
  response["beacon_id"] = gpsTracker.beaconId;
  response["timestamp"] = point.timestamp;
  sendJson(200, response);
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

const char* shiftStatusFor(const TagState& tag) {
  if (tag.status == "sin_senal") {
    if (tag.readerId == "RX-03") return "fuera";
    if (tag.readerId.isEmpty()) return "ausente";
    return "sin_senal";
  }
  if (tag.readerId == "RX-02") return "ingresando";
  if (tag.readerId == "RX-01") return "en_turno";
  if (tag.readerId == "RX-03") return "saliendo";
  return "sin_senal";
}

uint64_t latestReaderEventFor(const String& tagId, const char* readerId) {
  for (size_t index = 0; index < eventCount; ++index) {
    const EventRecord& event = events[index];
    if (event.type == "sector" && event.tagId == tagId &&
        event.readerId == readerId && event.timestamp != 0) return event.timestamp;
  }
  return 0;
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
    item["codigo_personal"] = tag.owner;
    item["cargo"] = tag.role;
    item["cuadrilla"] = tag.crew;
    const bool gpsAssociated = strcmp(tag.id, GPS_BEACON_ID) == 0;
    item["gps_asociado"] = gpsAssociated;
    if (gpsAssociated) {
      item["gps_node_id"] = GPS_NODE_ID;
      item["gps_disponible"] = gpsTracker.pointCount > 0;
      item["gps_hardware"] = gpsTracker.hardware;
    }
    item["estado_turno"] = shiftStatusFor(tag);
    JsonObject shift = item["turno"].to<JsonObject>();
    shift["nombre"] = tag.shiftName;
    shift["inicio"] = tag.shiftStart;
    shift["fin"] = tag.shiftEnd;
    addTimestamp(shift, "ultimo_ingreso", latestReaderEventFor(tag.id, "RX-02"));
    addTimestamp(shift, "ultima_presencia_interior", latestReaderEventFor(tag.id, "RX-01"));
    addTimestamp(shift, "ultima_salida", latestReaderEventFor(tag.id, "RX-03"));
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
    counters["precaucion"] = tag.warningCount;
    counters["proximo"] = tag.nearCount;
    counters["seguro"] = tag.safeCount;
    counters["sin_senal"] = tag.offlineCount;
    JsonObject precision = item["precision"].to<JsonObject>();
    const int selectedIndex = readerIndexFor(tag.readerId);
    if (visible && selectedIndex >= 0) {
      const ReaderObservation& observation = tag.observations[selectedIndex];
      precision["rssi_crudo"] = observation.rssi;
      precision["rssi_filtrado"] = serialized(String(observation.filteredRssi, 1));
      precision["muestras"] = observation.sampleCount;
      precision["ventana"] = observation.windowCount;
      precision["objetivo_ventana"] = RSSI_WINDOW_SIZE;
      precision["dispersion_db"] = serialized(String(observation.spread, 1));
      precision["calidad"] = observation.windowCount < 5 ? "inicializando" :
          observation.spread <= 7.0F ? "alta" : observation.spread <= 13.0F ? "media" : "variable";
    } else {
      precision["rssi_crudo"] = nullptr;
      precision["rssi_filtrado"] = nullptr;
      precision["muestras"] = 0;
      precision["ventana"] = 0;
      precision["objetivo_ventana"] = RSSI_WINDOW_SIZE;
      precision["dispersion_db"] = nullptr;
      precision["calidad"] = "sin_senal";
    }
    if (tag.pendingStatus.isEmpty()) precision["estado_pendiente"] = nullptr;
    else precision["estado_pendiente"] = tag.pendingStatus;
    if (tag.readerCandidateId.isEmpty()) precision["reader_candidato"] = nullptr;
    else precision["reader_candidato"] = tag.readerCandidateId;
  }

  JsonArray eventArray = document["eventos"].to<JsonArray>();
  const size_t recentEventCount = min(eventCount, static_cast<size_t>(30));
  for (size_t index = 0; index < recentEventCount; ++index) appendEventJson(eventArray, events[index]);
  sendJson(200, document);
}

void sendReports() {
  updateExpiredTags();
  JsonDocument document;
  JsonObject summary = document["resumen"].to<JsonObject>();
  size_t stateChanges = 0;
  size_t sectorChanges = 0;
  for (size_t index = 0; index < eventCount; ++index) {
    if (events[index].type == "sector") ++sectorChanges;
    else ++stateChanges;
  }
  size_t active = 0, onShift = 0, entering = 0, leaving = 0, absent = 0;
  for (const TagState& tag : tags) {
    const String shiftStatus = shiftStatusFor(tag);
    if (tag.status != "sin_senal" && !tag.readerId.isEmpty()) ++active;
    if (shiftStatus == "en_turno") ++onShift;
    else if (shiftStatus == "ingresando") ++entering;
    else if (shiftStatus == "saliendo") ++leaving;
    else ++absent;
  }
  summary["eventos"] = eventCount;
  summary["cambios_estado"] = stateChanges;
  summary["cambios_sector"] = sectorChanges;
  summary["personas_esperadas"] = TAG_COUNT;
  summary["personas_detectadas"] = active;
  summary["en_turno"] = onShift;
  summary["ingresando"] = entering;
  summary["saliendo"] = leaving;
  summary["ausentes_o_sin_senal"] = absent;
  summary["capacidad_historial"] = MAX_EVENTS;
  addTimestamp(summary, "generado", epochNow());

  JsonArray perTag = document["por_tag"].to<JsonArray>();
  for (const TagState& tag : tags) {
    JsonObject item = perTag.add<JsonObject>();
    item["id"] = tag.id;
    item["nombre"] = tag.name;
    item["codigo_personal"] = tag.owner;
    item["cargo"] = tag.role;
    item["cuadrilla"] = tag.crew;
    item["estado_turno"] = shiftStatusFor(tag);
    item["peligro"] = tag.dangerCount;
    item["precaucion"] = tag.warningCount;
    item["proximo"] = tag.nearCount;
    item["seguro"] = tag.safeCount;
    item["sin_senal"] = tag.offlineCount;
  }
  JsonArray history = document["historial"].to<JsonArray>();
  for (size_t index = 0; index < eventCount; ++index) appendEventJson(history, events[index]);
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
  if (supervisorName.isEmpty() && !adminRequestAuthorized()) {
    server.send(401, "text/plain; charset=utf-8", "Sesión de supervisor o administrador requerida");
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
  const uint64_t messageMoment = epochNow();
  message.id = String(messageMoment != 0 ? messageMoment : millis()) + "-" +
               String(READER_ID) + "-msg";
  message.target = target;
  message.level = level;
  message.title = title;
  message.body = text;
  message.author = authenticatedOperatorName();
  message.timestamp = epochNow();
  message.active = true;
  message.confirmedBy = "";
  message.confirmedAt = 0;
  saveMessages();
  queueSupervisorMessageSync(message);
  JsonDocument response;
  JsonArray array = response.to<JsonArray>();
  appendMessageJson(array, message);
  sendJson(201, response);
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
      if (supervisorName.isEmpty() && !adminRequestAuthorized()) {
        server.send(401, "text/plain; charset=utf-8", "Sesión de supervisor o administrador requerida");
        return;
      }
      messages[index].confirmedBy = authenticatedOperatorName();
      messages[index].confirmedAt = epochNow();
      messages[index].active = false;
      saveMessages();
      queueSupervisorMessageSync(messages[index]);
      JsonDocument response;
      JsonArray array = response.to<JsonArray>();
      appendMessageJson(array, messages[index]);
      sendJson(200, response);
      return;
    }
    if (server.method() == HTTP_DELETE) {
      if (!adminRequestAuthorized()) {
        server.send(403, "text/plain; charset=utf-8", "Solo el administrador puede eliminar mensajes");
        return;
      }
      for (size_t position = index; position + 1 < messageCount; ++position) messages[position] = messages[position + 1];
      if (messageCount > 0) --messageCount;
      saveMessages();
      queueSupervisorDeleteSync(tail);
      JsonDocument response; response["eliminado"] = tail; sendJson(200, response);
      return;
    }
  }
  if (uri.startsWith("/api/historial-proximidad/") && server.method() == HTTP_DELETE) {
    if (!adminRequestAuthorized()) {
      server.send(403, "text/plain; charset=utf-8", "Se requiere una sesión de administrador");
      return;
    }
    const String id = uri.substring(String("/api/historial-proximidad/").length());
    for (TagState& tag : tags) {
      if (id == tag.id) {
        tag.dangerCount = tag.warningCount = tag.nearCount = tag.safeCount = tag.offlineCount = 0;
        size_t write = 0;
        for (size_t read = 0; read < eventCount; ++read) if (events[read].tagId != id) events[write++] = events[read];
        eventCount = write;
        saveHistory();
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
  // El documento de entrada no se almacena en cache: los recursos estaticos
  // ya llevan version, pero iOS puede conservar una copia incompleta del HTML
  // cuando la ventana cautiva se abre durante la asociacion Wi-Fi.
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.streamFile(file, "text/html; charset=utf-8");
  file.close();
}

void redirectCaptivePortal() {
  // Los sistemas operativos consultan URLs diferentes para saber si existe
  // Internet. Una redireccion explicita hace que abran MINA-LOCAL en vez de
  // interpretar un HTML grande como una comprobacion de conectividad fallida.
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.send(302, "text/plain; charset=utf-8", "Abriendo MINA-LOCAL");
}

void serveMutableBackup(const char* path) {
  if (!adminRequestAuthorized()) {
    server.send(403, "text/plain; charset=utf-8", "Se requiere una sesion de administrador");
    return;
  }
  File file = LittleFS.open(path, "r");
  if (!file) {
    server.send(404, "text/plain; charset=utf-8", "Registro no disponible");
    return;
  }
  server.streamFile(file, "application/json; charset=utf-8");
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
  server.on("/", HTTP_ANY, serveIndex);
  server.on("/index.html", HTTP_ANY, serveIndex);
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
  server.on("/api/reportes", HTTP_GET, sendReports);
  server.on("/api/gps", HTTP_GET, sendGpsTracker);
  server.on("/api/gps/actualizar", HTTP_POST, receiveGpsTrackerUpdate);
  server.on("/api/respaldo/historial", HTTP_GET, []() { serveMutableBackup("/historial.json"); });
  server.on("/api/respaldo/gps", HTTP_GET, []() { serveMutableBackup("/gps_tracker.json"); });
  server.on("/api/respaldo/mensajes", HTTP_GET, []() { serveMutableBackup("/mensajes.json"); });
  server.on("/api/respaldo/mesh", HTTP_GET, []() { serveMutableBackup("/mensajes_mesh.json"); });
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
    if (clockEpochBase != 0) {
      const uint64_t synchronizedAt = epochNow();
      bool historyUpdated = false;
      for (size_t index = 0; index < eventCount; ++index) {
        if (events[index].timestamp != 0) continue;
        events[index].timestamp = synchronizedAt;
        historyUpdated = true;
      }
      if (historyUpdated) saveHistory();
    }
    JsonDocument response; response["sincronizado"] = clockEpochBase != 0; sendJson(200, response);
  });
  server.on("/api/mensajes", HTTP_GET, sendMessages);
  server.on("/api/mensajes", HTTP_POST, publishMessage);
  server.on("/api/mensajes", HTTP_DELETE, []() {
    if (!adminRequestAuthorized()) { server.send(403, "text/plain; charset=utf-8", "Se requiere una sesión de administrador"); return; }
    const size_t removed = messageCount; messageCount = 0; saveMessages();
    queueSupervisorClearSync();
    JsonDocument response; response["eliminados"] = removed; sendJson(200, response);
  });
  server.on("/api/meshtastic/mensajes", HTTP_GET, sendMeshMessages);
  server.on("/api/meshtastic/mensajes", HTTP_POST, publishMeshMessage);
  server.on("/api/meshtastic/mensajes", HTTP_DELETE, clearMeshMessageHistory);
  server.on("/api/meshtastic/cola", HTTP_POST, sendMeshQueueToGateway);
  server.on("/api/meshtastic/estado", HTTP_POST, updateMeshMessageFromGateway);
  server.on("/api/meshtastic/recibir", HTTP_POST, receiveMeshMessageFromGateway);
  server.on("/api/historial-proximidad", HTTP_DELETE, []() {
    if (!adminRequestAuthorized()) { server.send(403, "text/plain; charset=utf-8", "Se requiere una sesión de administrador"); return; }
    const size_t removed = eventCount; eventCount = 0;
    for (TagState& tag : tags) {
      tag.dangerCount = tag.warningCount = tag.nearCount = tag.safeCount = tag.offlineCount = 0;
    }
    saveHistory();
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
    JsonDocument response; response["autenticado"] = adminRequestAuthorized(); response["rol"] = "administrador"; response["nombre"] = adminName; sendJson(200, response);
  });
  server.on("/api/administrador/login", HTTP_POST, []() {
    JsonDocument body; if (!parseJsonBody(body)) return;
    const String pin = body["pin"] | ""; String name = body["nombre"] | ""; name.trim();
    if (pin != ADMIN_PIN) { server.send(401, "text/plain; charset=utf-8", "PIN de administrador incorrecto"); return; }
    if (name.isEmpty() || name.length() > 40) { server.send(400, "text/plain; charset=utf-8", "Nombre de administrador inválido"); return; }
    adminAuthenticated = true; adminName = name; setAdminSessionCookie(true); JsonDocument response; response["autenticado"] = true; response["rol"] = "administrador"; response["nombre"] = name; sendJson(200, response);
  });
  server.on("/api/administrador/logout", HTTP_POST, []() {
    adminAuthenticated = false; adminName = ""; setAdminSessionCookie(false); JsonDocument response; response["autenticado"] = false; sendJson(200, response);
  });
  // Los recursos llevan version en la URL. Cachearlos evita volver a transferir
  // cerca de 180 KB cada vez que iOS reabre el portal cautivo.
  server.serveStatic("/static/", LittleFS, "/static/", "public, max-age=86400");
  server.on("/generate_204", HTTP_ANY, redirectCaptivePortal);
  server.on("/gen_204", HTTP_ANY, redirectCaptivePortal);
  server.on("/hotspot-detect.html", HTTP_ANY, redirectCaptivePortal);
  server.on("/library/test/success.html", HTTP_ANY, redirectCaptivePortal);
  server.on("/success.html", HTTP_ANY, redirectCaptivePortal);
  server.on("/canonical.html", HTTP_ANY, redirectCaptivePortal);
  server.on("/ncsi.txt", HTTP_ANY, redirectCaptivePortal);
  server.on("/connecttest.txt", HTTP_ANY, redirectCaptivePortal);
  server.on("/redirect", HTTP_ANY, redirectCaptivePortal);
  server.on("/fwlink", HTTP_ANY, redirectCaptivePortal);
  const char* capturedHeaders[] = {"Cookie"};
  server.collectHeaders(capturedHeaders, 1);
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

struct __attribute__((packed)) LoRaPortalSyncFrame {
  uint16_t magic;
  uint8_t version;
  uint8_t readerNumber;
  uint32_t key;
  uint8_t chunkIndex;
  uint8_t chunkCount;
  uint8_t payloadLength;
  char payload[LORA_PORTAL_SYNC_CHUNK_SIZE];
};

uint32_t portalSyncHash(const String& payload) {
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < payload.length(); ++index) {
    hash ^= static_cast<uint8_t>(payload[index]);
    hash *= 16777619UL;
  }
  return hash == 0 ? 1 : hash;
}

bool enqueuePortalSyncPayload(const String& payload) {
  if (payload.isEmpty() ||
      payload.length() > LORA_PORTAL_SYNC_CHUNK_SIZE * LORA_PORTAL_SYNC_MAX_CHUNKS) {
    Serial.printf("[SYNC] Payload descartado: %u bytes\n",
                  static_cast<unsigned>(payload.length()));
    return false;
  }
  const uint32_t key = portalSyncHash(payload);
  for (size_t offset = 0; offset < portalSyncQueueCount; ++offset) {
    const size_t index = (portalSyncQueueHead + offset) % LORA_PORTAL_SYNC_QUEUE_SIZE;
    if (portalSyncQueue[index].used && portalSyncQueue[index].key == key &&
        portalSyncQueue[index].payload == payload) return true;
  }
  if (portalSyncQueueCount >= LORA_PORTAL_SYNC_QUEUE_SIZE) {
    Serial.println("[SYNC] Cola llena; se reintentara en la proxima reconciliacion");
    return false;
  }
  const size_t index = (portalSyncQueueHead + portalSyncQueueCount) %
                       LORA_PORTAL_SYNC_QUEUE_SIZE;
  PortalSyncJob& job = portalSyncQueue[index];
  job.used = true;
  job.key = key;
  job.payload = payload;
  job.chunkIndex = 0;
  job.repetitionsRemaining = LORA_PORTAL_SYNC_REPEATS;
  ++portalSyncQueueCount;
  return true;
}

void queueSupervisorMessageSync(const SupervisorMessage& message) {
  JsonDocument document;
  document["k"] = "s";
  document["o"] = "u";
  document["i"] = message.id;
  document["d"] = message.target;
  document["l"] = message.level;
  document["t"] = message.title;
  document["b"] = message.body;
  document["a"] = message.author;
  document["ts"] = message.timestamp;
  document["x"] = message.active;
  document["cb"] = message.confirmedBy;
  document["ct"] = message.confirmedAt;
  String payload;
  serializeJson(document, payload);
  enqueuePortalSyncPayload(payload);
}

void queueSupervisorDeleteSync(const String& id) {
  JsonDocument document;
  document["k"] = "s";
  document["o"] = "d";
  document["i"] = id;
  String payload;
  serializeJson(document, payload);
  enqueuePortalSyncPayload(payload);
}

void queueSupervisorClearSync() {
  enqueuePortalSyncPayload("{\"k\":\"s\",\"o\":\"c\"}");
}

void queueMeshMessageSync(const MeshMessage& message) {
  JsonDocument document;
  document["k"] = "m";
  document["o"] = "u";
  document["i"] = message.id;
  document["d"] = message.direction;
  document["b"] = message.body;
  document["a"] = message.author;
  document["s"] = message.status;
  document["ts"] = message.timestamp;
  document["ut"] = message.updatedAt;
  document["p"] = message.packetId;
  document["r"] = message.rssi;
  document["n"] = message.snr;
  String payload;
  serializeJson(document, payload);
  enqueuePortalSyncPayload(payload);
}

void queueMeshClearSync() {
  enqueuePortalSyncPayload("{\"k\":\"m\",\"o\":\"c\"}");
}

void queueGpsPointSync(const GpsPoint& point) {
  JsonDocument document;
  document["k"] = "g";
  document["la"] = serialized(String(point.latitude, 7));
  document["lo"] = serialized(String(point.longitude, 7));
  document["al"] = serialized(String(point.altitude, 1));
  document["ts"] = point.timestamp;
  document["ba"] = point.batteryLevel;
  document["vo"] = serialized(String(point.voltage, 2));
  document["pr"] = point.precisionBits;
  document["so"] = point.source;
  String payload;
  serializeJson(document, payload);
  enqueuePortalSyncPayload(payload);
}

void queueGatewayPresenceSync() {
  JsonDocument document;
  document["k"] = "w";
  document["a"] = true;
  // El pulso cambia en cada envio para que la deduplicacion de fragmentos no
  // confunda un heartbeat nuevo con la segunda copia del anterior.
  document["q"] = millis();
  String payload;
  serializeJson(document, payload);
  enqueuePortalSyncPayload(payload);
}

void clearPortalSyncAssembly(PortalSyncAssembly& assembly) {
  assembly.key = 0;
  assembly.chunkCount = 0;
  assembly.receivedMask = 0;
  assembly.startedAt = 0;
  for (String& chunk : assembly.chunks) chunk = "";
}

int meshStatusRank(const String& status) {
  if (status == "confirmado") return 5;
  if (status == "transmitido") return 4;
  if (status == "transmitiendo") return 3;
  if (status == "error") return 2;
  if (status == "en_cola") return 1;
  return 0;
}

void applySupervisorSync(JsonDocument& document) {
  const String operation = document["o"] | "";
  const String id = document["i"] | "";
  if (operation == "c") {
    messageCount = 0;
    saveMessages();
    Serial.println("[SYNC] Historial operacional limpiado por el otro portal");
    return;
  }
  const int existingIndex = findMessage(id);
  if (operation == "d") {
    if (existingIndex < 0) return;
    for (size_t index = static_cast<size_t>(existingIndex);
         index + 1 < messageCount; ++index) messages[index] = messages[index + 1];
    if (messageCount > 0) --messageCount;
    saveMessages();
    return;
  }
  if (operation != "u" || id.isEmpty()) return;

  SupervisorMessage incoming;
  incoming.used = true;
  incoming.id = id;
  incoming.target = document["d"] | "todos";
  incoming.level = document["l"] | "informacion";
  incoming.title = document["t"] | "";
  incoming.body = document["b"] | "";
  incoming.author = document["a"] | "";
  incoming.timestamp = document["ts"] | 0ULL;
  incoming.active = document["x"] | true;
  incoming.confirmedBy = document["cb"] | "";
  incoming.confirmedAt = document["ct"] | 0ULL;

  if (existingIndex >= 0) {
    SupervisorMessage& existing = messages[existingIndex];
    if (existing.confirmedAt > incoming.confirmedAt) return;
    existing = incoming;
  } else {
    if (messageCount == MAX_MESSAGES) {
      for (size_t index = MAX_MESSAGES - 1; index > 0; --index) {
        messages[index] = messages[index - 1];
      }
    } else {
      for (size_t index = messageCount; index > 0; --index) {
        messages[index] = messages[index - 1];
      }
      ++messageCount;
    }
    messages[0] = incoming;
  }
  saveMessages();
  Serial.printf("[SYNC] Mensaje operacional %s actualizado\n", id.c_str());
}

void applyMeshSync(JsonDocument& document) {
  const String operation = document["o"] | "";
  if (operation == "c") {
    size_t retained = 0;
    for (size_t index = 0; index < meshMessageCount; ++index) {
      const MeshMessage& message = meshMessages[index];
      const bool pending = message.used && message.direction == "saliente" &&
          (message.status == "en_cola" || message.status == "error" ||
           message.status == "transmitiendo");
      if (pending) meshMessages[retained++] = message;
    }
    for (size_t index = retained; index < meshMessageCount; ++index) {
      meshMessages[index] = MeshMessage{};
    }
    meshMessageCount = retained;
    saveMeshMessages();
    return;
  }
  if (operation != "u") return;
  const String id = document["i"] | "";
  if (id.isEmpty()) return;

  MeshMessage incoming;
  incoming.used = true;
  incoming.id = id;
  incoming.direction = document["d"] | "entrante";
  incoming.body = document["b"] | "";
  incoming.author = document["a"] | "";
  incoming.status = document["s"] | "recibido";
  incoming.timestamp = document["ts"] | 0ULL;
  incoming.updatedAt = document["ut"] | 0ULL;
  incoming.packetId = document["p"] | 0U;
  incoming.rssi = document["r"] | 0;
  incoming.snr = document["n"] | 0.0F;

  const int existingIndex = findMeshMessage(id);
  if (existingIndex >= 0) {
    MeshMessage& existing = meshMessages[existingIndex];
    if (existing.updatedAt > incoming.updatedAt &&
        meshStatusRank(existing.status) >= meshStatusRank(incoming.status)) return;
    existing = incoming;
  } else {
    prependMeshMessage() = incoming;
  }
  saveMeshMessages();
  Serial.printf("[SYNC] Chat Meshtastic %s actualizado\n", id.c_str());
}

void applyGpsSync(JsonDocument& document) {
  GpsPoint point;
  point.latitude = document["la"] | 0.0;
  point.longitude = document["lo"] | 0.0;
  point.altitude = document["al"] | 0.0F;
  point.timestamp = document["ts"] | 0ULL;
  point.batteryLevel = document["ba"] | -1;
  point.voltage = document["vo"] | 0.0F;
  point.precisionBits = document["pr"] | 0;
  point.source = document["so"] | "meshtastic_gnss";
  if (!std::isfinite(point.latitude) || !std::isfinite(point.longitude) ||
      point.timestamp == 0) return;
  for (size_t index = 0; index < gpsTracker.pointCount; ++index) {
    if (gpsTracker.points[index].timestamp == point.timestamp) return;
  }
  if (gpsTracker.pointCount > 0 && gpsTracker.points[0].timestamp > point.timestamp) return;
  prependGpsPoint(point);
  saveGpsTracker();
  Serial.println("[SYNC] Posicion GNSS replicada entre portales");
}

void applyPortalSyncPayload(const String& payload) {
  JsonDocument document;
  if (deserializeJson(document, payload)) return;
  const String kind = document["k"] | "";
  if (kind == "s") applySupervisorSync(document);
  else if (kind == "m") applyMeshSync(document);
  else if (kind == "g") applyGpsSync(document);
  else if (kind == "w" && document["a"].as<bool>()) meshGatewayLastSeen = millis();
}

void queueCurrentPortalState() {
  for (size_t index = 0; index < messageCount; ++index) {
    if (messages[index].used) queueSupervisorMessageSync(messages[index]);
  }
  // Al reencontrar otro portal basta reconciliar las conversaciones recientes.
  // Esto evita ocupar el enlace LoRa durante minutos con un historial antiguo.
  const size_t recentMeshCount = min(meshMessageCount, static_cast<size_t>(6));
  for (size_t index = 0; index < recentMeshCount; ++index) {
    if (meshMessages[index].used) queueMeshMessageSync(meshMessages[index]);
  }
  if (gpsTracker.pointCount > 0) queueGpsPointSync(gpsTracker.points[0]);
  if (meshGatewayDirectLastSeen != 0 &&
      millis() - meshGatewayDirectLastSeen <= 12000) queueGatewayPresenceSync();
}

void acceptPortalSyncFrame(const LoRaPortalSyncFrame& frame) {
  if (frame.readerNumber < 1 || frame.readerNumber > READER_COUNT ||
      frame.readerNumber == MINA_READER_NUMBER || frame.chunkCount == 0 ||
      frame.chunkCount > LORA_PORTAL_SYNC_MAX_CHUNKS ||
      frame.chunkIndex >= frame.chunkCount ||
      frame.payloadLength > LORA_PORTAL_SYNC_CHUNK_SIZE) return;
  const size_t remoteIndex = frame.readerNumber - 1;
  if (completedPortalSyncKey[remoteIndex] == frame.key) return;
  PortalSyncAssembly& assembly = portalSyncAssemblies[remoteIndex];
  if (assembly.key != frame.key || assembly.chunkCount != frame.chunkCount ||
      (assembly.startedAt != 0 &&
       millis() - assembly.startedAt > LORA_PORTAL_SYNC_ASSEMBLY_TIMEOUT_MS)) {
    clearPortalSyncAssembly(assembly);
    assembly.key = frame.key;
    assembly.chunkCount = frame.chunkCount;
    assembly.startedAt = millis();
  }
  String& chunk = assembly.chunks[frame.chunkIndex];
  chunk = "";
  chunk.reserve(frame.payloadLength);
  for (uint8_t index = 0; index < frame.payloadLength; ++index) {
    chunk += frame.payload[index];
  }
  assembly.receivedMask |= static_cast<uint8_t>(1U << frame.chunkIndex);
  const uint8_t expectedMask = static_cast<uint8_t>((1U << frame.chunkCount) - 1U);
  if (assembly.receivedMask != expectedMask) return;
  String payload;
  for (uint8_t index = 0; index < frame.chunkCount; ++index) {
    payload += assembly.chunks[index];
  }
  applyPortalSyncPayload(payload);
  completedPortalSyncKey[remoteIndex] = frame.key;
  clearPortalSyncAssembly(assembly);
}

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
  portalElectionStartedAt = millis();
  WiFi.mode(WIFI_AP);
  // Todos los readers BLE publican la misma celda logica de MINA-LOCAL. Cada
  // Heltec conserva un BSSID propio, por lo que el telefono puede elegir el AP
  // mas fuerte sin esperar a que desaparezca el enlace LoRa de otro reader.
  // El AP no se apaga mientras el equipo siga encendido.
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  const IPAddress localIp(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(localIp, localIp, subnet);
  if (!WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, 8)) {
    Serial.println("[RED] ERROR al crear MINA-LOCAL");
    WiFi.mode(WIFI_OFF);
    return;
  }
  dnsServer.start(DNS_PORT, "*", localIp);
  portalAccessPointActive = true;
  readerPortalActive[LOCAL_READER_INDEX] = true;
  Serial.printf("[RED] %s distribuido disponible desde %s en http://%s (canal %u)\n",
                AP_SSID, READER_ID, localIp.toString().c_str(), AP_CHANNEL);
  Serial.printf("[RED] BSSID %s: %s\n", READER_ID,
                WiFi.softAPmacAddress().c_str());
}

void startPortalAccessPoint() {
  if (portalAccessPointActive) return;

  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  const IPAddress localIp(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(localIp, localIp, subnet);
  if (!WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, 8)) {
    Serial.println("[RED] ERROR al crear MINA-LOCAL de respaldo");
    WiFi.mode(WIFI_OFF);
    return;
  }
  dnsServer.start(DNS_PORT, "*", localIp);
  portalAccessPointActive = true;
  readerPortalActive[LOCAL_READER_INDEX] = true;
  Serial.printf("[RED] %s asumido por %s en http://%s (canal %u)\n",
                AP_SSID, READER_ID, localIp.toString().c_str(), AP_CHANNEL);
  renderOledStatus();
}

void stopPortalAccessPoint() {
  if (!portalAccessPointActive) return;

  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  // STA sin asociar no emite MINA-LOCAL y deja lista la pila TCP/IP para un
  // nuevo failover, evitando reiniciar el servidor web.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  portalAccessPointActive = false;
  readerPortalActive[LOCAL_READER_INDEX] = false;
  Serial.printf("[RED] %s libera el portal; queda como reader distribuido\n", READER_ID);
  renderOledStatus();
}

void maintainPortalElection() {
  const uint32_t now = millis();
  bool oledNeedsRefresh = false;
  for (size_t index = 0; index < READER_COUNT; ++index) {
    if (index != LOCAL_READER_INDEX && readerPortalActive[index] &&
        !readerOnline(index, now)) {
      readerPortalActive[index] = false;
      oledNeedsRefresh = true;
    }
  }
  if (oledNeedsRefresh) renderOledStatus();

  // La disponibilidad Wi-Fi ya no depende de la distancia LoRa. Esto evita la
  // zona muerta en la que RX-03 todavia escucha a RX-01 por LoRa, pero el
  // telefono ya no alcanza el SoftAP de RX-01.
  if (!portalAccessPointActive) startPortalAccessPoint();
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
  if (LOCAL_READER_INDEX == 0) return "Punto formacion";
  if (LOCAL_READER_INDEX == 1) return "Control acceso";
  return "Salida personal";
}

void renderOledStatus() {
  if (!oledReady) return;
  oledDisplay.clear();
  oledDisplay.setTextAlignment(TEXT_ALIGN_CENTER);
  oledDisplay.setFont(ArialMT_Plain_16);
  oledDisplay.drawString(64, 0, READER_ID);
  oledDisplay.setFont(ArialMT_Plain_10);
  oledDisplay.drawString(64, 18, oledSectorLabel());
  if (portalAccessPointActive) {
    oledDisplay.drawString(64, 29, "WiFi: MINA-LOCAL");
    oledDisplay.drawString(64, 40, "IP: 192.168.4.1");
  } else {
    oledDisplay.drawString(64, 29, "WiFi: respaldo");
    oledDisplay.drawString(64, 40, String("Portal: ") + activeCoordinatorId());
  }
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
  // El display queda fisicamente invertido dentro del gabinete Heltec.
  // Rota la imagen 180 grados para leerla con "heltec.org" hacia arriba.
  oledDisplay.flipScreenVertically();
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
    // Comparte la lectura ya filtrada para que el coordinador no decida el
    // sector usando un unico pico de RSSI capturado por un reader remoto.
    reading.rssi = static_cast<int8_t>(constrain(lroundf(observation.filteredRssi), -127L, 0L));
    reading.txPower = static_cast<int8_t>(constrain(observation.txPower, -127, 0));
    const uint32_t age100ms = (now - observation.lastSeen) / 100;
    reading.age100ms = static_cast<uint16_t>(age100ms > 65535 ? 65535 : age100ms);
  }

  const uint8_t readingCount = frame.count;
  const size_t length = offsetof(LoRaObservationFrame, readings) +
                        readingCount * sizeof(LoRaReading);
  if (portalAccessPointActive) frame.count |= LORA_PORTAL_ACTIVE_FLAG;
  radio.clearPacketReceivedAction();
  const int16_t state = radio.transmit(reinterpret_cast<uint8_t*>(&frame), length);
  radio.setPacketReceivedAction(onLoRaPacket);
  radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) Serial.printf("[LORA] Error TX: %d\n", state);
}

bool transmitPendingPortalSync() {
  if (!loRaReady || portalSyncQueueCount == 0) return false;
  const uint32_t interval = LORA_PORTAL_SYNC_TX_MS + LOCAL_READER_INDEX * 70;
  if (millis() - lastPortalSyncTx < interval) return false;

  PortalSyncJob& job = portalSyncQueue[portalSyncQueueHead];
  if (!job.used || job.payload.isEmpty()) {
    job = PortalSyncJob{};
    portalSyncQueueHead = (portalSyncQueueHead + 1) % LORA_PORTAL_SYNC_QUEUE_SIZE;
    --portalSyncQueueCount;
    return false;
  }

  const uint8_t chunkCount = static_cast<uint8_t>(
      (job.payload.length() + LORA_PORTAL_SYNC_CHUNK_SIZE - 1) /
      LORA_PORTAL_SYNC_CHUNK_SIZE);
  if (job.chunkIndex >= chunkCount) job.chunkIndex = 0;
  const size_t offset = static_cast<size_t>(job.chunkIndex) *
                        LORA_PORTAL_SYNC_CHUNK_SIZE;
  const size_t remaining = job.payload.length() - offset;
  const uint8_t payloadLength = static_cast<uint8_t>(
      min(remaining, LORA_PORTAL_SYNC_CHUNK_SIZE));

  LoRaPortalSyncFrame frame{};
  frame.magic = LORA_PORTAL_SYNC_MAGIC;
  frame.version = LORA_PORTAL_SYNC_VERSION;
  frame.readerNumber = MINA_READER_NUMBER;
  frame.key = job.key;
  frame.chunkIndex = job.chunkIndex;
  frame.chunkCount = chunkCount;
  frame.payloadLength = payloadLength;
  memcpy(frame.payload, job.payload.c_str() + offset, payloadLength);

  const size_t length = offsetof(LoRaPortalSyncFrame, payload) + payloadLength;
  radio.clearPacketReceivedAction();
  const int16_t state = radio.transmit(reinterpret_cast<uint8_t*>(&frame), length);
  radio.setPacketReceivedAction(onLoRaPacket);
  radio.startReceive();
  lastPortalSyncTx = millis();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[SYNC] Error TX: %d\n", state);
    return true;
  }

  ++job.chunkIndex;
  if (job.chunkIndex >= chunkCount) {
    job.chunkIndex = 0;
    if (job.repetitionsRemaining > 1) {
      --job.repetitionsRemaining;
    } else {
      job = PortalSyncJob{};
      portalSyncQueueHead = (portalSyncQueueHead + 1) % LORA_PORTAL_SYNC_QUEUE_SIZE;
      --portalSyncQueueCount;
    }
  }
  return true;
}

void markRemoteReaderSeen(size_t remoteIndex) {
  const uint32_t now = millis();
  const bool wasOnline = readerOnline(remoteIndex, now);
  readerLastSeen[remoteIndex] = now;
  if (!wasOnline) {
    Serial.printf("[SYNC] %s disponible; reconciliando portales\n",
                  READERS[remoteIndex].id);
    queueCurrentPortalState();
  }
}

void acceptObservationPacket(const uint8_t* packet, size_t length) {
  if (length < offsetof(LoRaObservationFrame, readings) ||
      length > sizeof(LoRaObservationFrame)) return;
  LoRaObservationFrame frame{};
  memcpy(&frame, packet, length);
  const uint8_t readingCount = frame.count & LORA_READING_COUNT_MASK;
  const size_t expectedLength = offsetof(LoRaObservationFrame, readings) +
                                readingCount * sizeof(LoRaReading);
  if (frame.magic != 0x4D49 || frame.version != 1 ||
      frame.readerNumber < 1 || frame.readerNumber > READER_COUNT ||
      frame.readerNumber == MINA_READER_NUMBER || readingCount > TAG_COUNT ||
      length != expectedLength) return;

  const size_t remoteIndex = frame.readerNumber - 1;
  markRemoteReaderSeen(remoteIndex);
  const bool remotePortalActive = (frame.count & LORA_PORTAL_ACTIVE_FLAG) != 0;
  if (readerPortalActive[remoteIndex] != remotePortalActive) {
    readerPortalActive[remoteIndex] = remotePortalActive;
    Serial.printf("[RED] %s informa portal %s\n", READERS[remoteIndex].id,
                  remotePortalActive ? "activo" : "en espera");
    renderOledStatus();
  }
  for (uint8_t index = 0; index < readingCount; ++index) {
    const LoRaReading& reading = frame.readings[index];
    for (TagState& tag : tags) {
      if (tag.major != reading.major || tag.minor != reading.minor) continue;
      const uint32_t age = static_cast<uint32_t>(reading.age100ms) * 100;
      ingestTag(tag, reading.rssi, reading.txPower, READERS[remoteIndex].id, age);
      break;
    }
  }
}

void acceptPortalSyncPacket(const uint8_t* packet, size_t length) {
  if (length < offsetof(LoRaPortalSyncFrame, payload) ||
      length > sizeof(LoRaPortalSyncFrame)) return;
  LoRaPortalSyncFrame frame{};
  memcpy(&frame, packet, length);
  const size_t expectedLength = offsetof(LoRaPortalSyncFrame, payload) +
                                frame.payloadLength;
  if (frame.magic != LORA_PORTAL_SYNC_MAGIC ||
      frame.version != LORA_PORTAL_SYNC_VERSION ||
      frame.readerNumber < 1 || frame.readerNumber > READER_COUNT ||
      frame.readerNumber == MINA_READER_NUMBER ||
      frame.payloadLength > LORA_PORTAL_SYNC_CHUNK_SIZE ||
      length != expectedLength) return;
  markRemoteReaderSeen(frame.readerNumber - 1);
  acceptPortalSyncFrame(frame);
}

void receiveLoRaPackets() {
  if (!loRaReady || !loRaPacketReady) return;
  loRaPacketReady = false;
  const size_t length = radio.getPacketLength();
  uint8_t packet[256] = {0};
  int16_t state = RADIOLIB_ERR_PACKET_TOO_LONG;
  if (length >= sizeof(uint16_t) && length <= sizeof(packet))
    state = radio.readData(packet, length);
  radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) return;
  uint16_t magic = 0;
  memcpy(&magic, packet, sizeof(magic));
  if (magic == 0x4D49) acceptObservationPacket(packet, length);
  else if (magic == LORA_PORTAL_SYNC_MAGIC) acceptPortalSyncPacket(packet, length);
}

void maintainReaderNetwork() {
  receiveLoRaPackets();
  maintainPortalElection();
  const uint32_t now = millis();
  if (meshGatewayDirectLastSeen != 0 &&
      now - meshGatewayDirectLastSeen <= 12000 &&
      now - lastGatewayPresenceSync >= LORA_GATEWAY_PRESENCE_SYNC_MS) {
    lastGatewayPresenceSync = now;
    queueGatewayPresenceSync();
  }
  const uint32_t observationInterval =
      LORA_TX_INTERVAL_MS + LOCAL_READER_INDEX * 170;
  if (now - lastLoRaTx >= observationInterval) transmitLocalObservations();
  else transmitPendingPortalSync();
}

void startBluetooth() {
  NimBLEDevice::init("");
  scanner = NimBLEDevice::getScan();
  scanner->setScanCallbacks(&beaconCallbacks, true);
  // iBeacon incluye los datos en el anuncio. El barrido activo compite con
  // WiFi por la radio de 2.4 GHz sin aportar informacion adicional.
  scanner->setActiveScan(false);
  scanner->setMaxResults(0);
  // Un 75 % de escucha mantiene deteccion rapida y reserva tiempo suficiente
  // para que DHCP, DNS y HTTP del portal cautivo no queden bloqueados.
  scanner->setInterval(160);
  scanner->setWindow(120);
  scanner->start(0, false, true);
}

void printBackupFileToSerial(const char* path) {
  Serial.printf("[BACKUP-BEGIN]%s\n", path);
  File file = LittleFS.open(path, "r");
  if (file) {
    uint8_t buffer[192];
    while (file.available()) {
      const size_t count = file.read(buffer, sizeof(buffer));
      Serial.write(buffer, count);
    }
    file.close();
  } else {
    Serial.print("{}");
  }
  Serial.printf("\n[BACKUP-END]%s\n", path);
}

void serviceSerialMaintenance() {
  static String command;
  while (Serial.available()) {
    const char incoming = static_cast<char>(Serial.read());
    if (incoming == '\r' || incoming == '\n') {
      command.trim();
      if (command == "BACKUP_JSON") {
        printBackupFileToSerial("/mensajes.json");
        printBackupFileToSerial("/mensajes_mesh.json");
        printBackupFileToSerial("/historial.json");
        printBackupFileToSerial("/gps_tracker.json");
        Serial.println("[BACKUP-COMPLETE]");
      }
      command = "";
    } else if (command.length() < 32) {
      command += incoming;
    }
  }
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
    loadMeshMessages();
    loadHistory();
    loadGpsTracker();
  }
  // La red debe quedar inicializada antes de compartir la radio con BLE.
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
  if (portalAccessPointActive) {
    Serial.printf("[WEB] Conecta el celular o notebook a %s y abre http://%s\n",
                  AP_SSID, WiFi.softAPIP().toString().c_str());
  } else {
    Serial.printf("[WEB] %s queda en espera como respaldo del portal\n", READER_ID);
  }
}

void loop() {
  serviceSerialMaintenance();
#if 0
#if MINA_READER_NUMBER == 2
  dnsServer.processNextRequest();
#else
  if (emergencyApActive) dnsServer.processNextRequest();
#endif
#endif
  if (portalAccessPointActive) {
    dnsServer.processNextRequest();
    server.handleClient();
  }
  maintainReaderNetwork();
  if (millis() - lastStateRefresh >= STATE_REFRESH_MS) {
    lastStateRefresh = millis();
    updateExpiredTags();
  }
  delay(3);
}

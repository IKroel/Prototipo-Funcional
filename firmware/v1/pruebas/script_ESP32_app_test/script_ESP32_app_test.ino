// Wisetrack ESP32 Gateway V1 (legacy) — build de pruebas con la app.
// Corte por GPIO23/relé al FMC130. Protocolo y seguridad: ver firmware/README.md.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "esp_mac.h"
#include "esp_random.h"
#include <Preferences.h>
#include "mbedtls/md.h"


// ════════════════════════════════════════════════════════════════════
// HARDWARE
// ════════════════════════════════════════════════════════════════════
#define UART_TX_PIN    17
#define UART_RX_PIN    16
#define UART_BAUD      115200
HardwareSerial &Tracker = Serial2;

#define RELAY_PIN      23
// ⚠ POLARIDAD: si al "Autorizar" el vehículo se CORTA en vez de habilitarse,
//   invierte estos dos valores (intercambia HIGH/LOW).
#define RELAY_LOCKED   LOW     // bloqueado (default seguro / arranque)
#define RELAY_UNLOCKED HIGH    // habilitado (solo al autorizar desde la app)


// ════════════════════════════════════════════════════════════════════
// BLE — Nordic UART Service UUIDs
// ════════════════════════════════════════════════════════════════════
#define NUS_SVC_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID   "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID   "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"


// ════════════════════════════════════════════════════════════════════
// SESIÓN
// ════════════════════════════════════════════════════════════════════
enum SessionState { SESSION_LOCKED, SESSION_AUTHORIZED };

#define DEFAULT_HB_TIMEOUT_MS  10000UL   // 10 s sin heartbeat / tras desconexión → bloquear
#define NONCE_LEN              8


// ════════════════════════════════════════════════════════════════════
// GLOBALS
// ════════════════════════════════════════════════════════════════════
NimBLECharacteristic* gTxChar          = nullptr;
NimBLEServer*         gServer          = nullptr;
bool                  gClientConnected = false;

SessionState  g_session       = SESSION_LOCKED;
unsigned long g_lastHeartbeat = 0;
unsigned long g_hbTimeout     = DEFAULT_HB_TIMEOUT_MS;

uint8_t  g_nonce[NONCE_LEN] = {0};
bool     g_nonceValid        = false;
uint8_t  g_authKey[32]       = {0};
bool     g_hasAuthKey        = false;

struct { int pin = -1; unsigned long releaseAt = 0; } g_pendingPulse;
int           g_lastRelayState = -1;   // monitor: detecta cambios externos en GPIO 23

Preferences g_prefs;
char        g_devName[24] = {0};


// ════════════════════════════════════════════════════════════════════
// BLE — NOTIFICACIÓN Y RESPUESTA
// ════════════════════════════════════════════════════════════════════
static void bleNotify(const uint8_t* data, size_t len) {
  if (!gClientConnected || !gTxChar) return;
  const size_t CHUNK = 180;
  for (size_t off = 0; off < len; ) {
    size_t n = (len - off > CHUNK) ? CHUNK : (len - off);
    gTxChar->setValue(data + off, n);
    gTxChar->notify();
    off += n;
  }
}

static void reply(const String& s) {
  bleNotify((const uint8_t*)s.c_str(), s.length());
}


// ════════════════════════════════════════════════════════════════════
// RELÉ — CONTROL Y PERSISTENCIA
// Escribe el estado en GPIO 23 y lo persiste en NVS.
// ════════════════════════════════════════════════════════════════════
static void applyRelay(int level) {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, level);
  g_prefs.begin("wt", false);
  g_prefs.putInt("r23", level == HIGH ? 1 : 0);
  g_prefs.end();
}

// Estado lógico para la app: 1 = combustible libre, 0 = corte.
// (independiente del nivel físico, invertido por el módulo active-LOW)
static int relayFree() {
  return digitalRead(RELAY_PIN) == RELAY_UNLOCKED ? 1 : 0;
}

static void executeLock() {
  g_session    = SESSION_LOCKED;
  g_nonceValid = false;
  applyRelay(RELAY_LOCKED);
  reply("<LOCKED\n");
  Serial.println("[SESSION] Bloqueado — corte activo");
}


// ════════════════════════════════════════════════════════════════════
// CRIPTOGRAFÍA — HMAC-SHA256 Y UTILIDADES HEX
// ════════════════════════════════════════════════════════════════════

// Calcula HMAC-SHA256(key, data) → out[32]
static bool computeHmac(const uint8_t* key, size_t keyLen,
                         const uint8_t* data, size_t dataLen,
                         uint8_t out[32]) {
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (mbedtls_md_setup(&ctx, info, 1) != 0) { mbedtls_md_free(&ctx); return false; }
  mbedtls_md_hmac_starts(&ctx, key, keyLen);
  mbedtls_md_hmac_update(&ctx, data, dataLen);
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  return true;
}

// Convierte string hex a bytes. Retorna false si longitud o caracteres son inválidos.
static bool hexToBytes(const String& hex, uint8_t* out, size_t outLen) {
  if ((size_t)hex.length() != outLen * 2) return false;
  for (size_t i = 0; i < outLen; i++) {
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int h = nib(hex[i*2]), l = nib(hex[i*2+1]);
    if (h < 0 || l < 0) return false;
    out[i] = (uint8_t)((h << 4) | l);
  }
  return true;
}

// Convierte bytes a string hex en minúsculas.
static String bytesToHex(const uint8_t* data, size_t len) {
  String s; s.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    char buf[3]; snprintf(buf, sizeof(buf), "%02x", data[i]); s += buf;
  }
  return s;
}


// ════════════════════════════════════════════════════════════════════
// MAC BLE
// ════════════════════════════════════════════════════════════════════
static String getBleMacString() {
  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

static void getBleMacBytes(uint8_t out[6]) {
  esp_read_mac(out, ESP_MAC_BT);
}


// ════════════════════════════════════════════════════════════════════
// PERSISTENCIA NVS (namespace "wt": name, authkey, hbtimeout, r23)
// ════════════════════════════════════════════════════════════════════
static void loadName() {
  g_prefs.begin("wt", true);
  String s = g_prefs.getString("name", "");
  g_prefs.end();
  if (s.length() > 0 && s.length() < sizeof(g_devName) - 1) {
    strncpy(g_devName, s.c_str(), sizeof(g_devName) - 1);
  } else {
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
    snprintf(g_devName, sizeof(g_devName), "WT-%02X%02X", mac[4], mac[5]);
  }
  g_devName[sizeof(g_devName) - 1] = 0;
}

static void saveName(const String& name) {
  g_prefs.begin("wt", false);
  g_prefs.putString("name", name);
  g_prefs.end();
  strncpy(g_devName, name.c_str(), sizeof(g_devName) - 1);
  g_devName[sizeof(g_devName) - 1] = 0;
}

static void loadConfig() {
  g_prefs.begin("wt", true);
  size_t len = g_prefs.getBytesLength("authkey");
  if (len == 32) {
    g_prefs.getBytes("authkey", g_authKey, 32);
    g_hasAuthKey = true;
  }
  g_hbTimeout = (unsigned long)g_prefs.getUInt("hbtimeout", DEFAULT_HB_TIMEOUT_MS);
  g_prefs.end();
}

static void saveAuthKey(const uint8_t key[32]) {
  memcpy(g_authKey, key, 32);
  g_hasAuthKey = true;
  g_prefs.begin("wt", false);
  g_prefs.putBytes("authkey", key, 32);
  g_prefs.end();
}

// Borra la device_key del NVS para permitir re-provisionar.
static void clearAuthKey() {
  memset(g_authKey, 0, 32);
  g_hasAuthKey = false;
  g_prefs.begin("wt", false);
  g_prefs.remove("authkey");
  g_prefs.end();
}

static void saveHbTimeout(unsigned long ms) {
  g_hbTimeout = ms;
  g_prefs.begin("wt", false);
  g_prefs.putUInt("hbtimeout", (uint32_t)ms);
  g_prefs.end();
}


// ════════════════════════════════════════════════════════════════════
// BLE ADVERTISING
// Incluye manufacturer data WT para filtro en la app.
// ════════════════════════════════════════════════════════════════════
static void configureAdv() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();

  // Paquete principal (≤31 bytes): flags + manufacturer data WT.
  std::string mfg;
  mfg.push_back((char)0xFF); mfg.push_back((char)0xFF);
  mfg.push_back('W'); mfg.push_back('T');
  mfg.push_back((char)0x01);

  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.setManufacturerData(mfg);
  adv->setAdvertisementData(advData);

  // El nombre va en el scan response (más espacio, no afecta la detección).
  NimBLEAdvertisementData scanData;
  scanData.setName(std::string(g_devName));
  adv->setScanResponseData(scanData);

  adv->start();
}


// ════════════════════════════════════════════════════════════════════
// GPIO HANDLER
// Protege GPIO 23 (relé): rechaza SET/MODE/PULSE sin sesión activa.
// ════════════════════════════════════════════════════════════════════
static void handleGpio(String args) {
  args.trim();
  int    sp1  = args.indexOf(' ');
  String sub  = (sp1 >= 0) ? args.substring(0, sp1) : args;
  String rest = (sp1 >= 0) ? args.substring(sp1 + 1) : "";
  rest.trim();

  if (g_hasAuthKey && g_session != SESSION_AUTHORIZED) {
    if (sub.equalsIgnoreCase("SET") || sub.equalsIgnoreCase("MODE") ||
        sub.equalsIgnoreCase("PULSE")) {
      int sp = rest.indexOf(' ');
      if ((sp >= 0 ? rest.substring(0, sp) : rest).toInt() == RELAY_PIN) {
        reply("<ERR relay_locked_no_session\n"); return;
      }
    }
  }

  if (sub.equalsIgnoreCase("MODE")) {
    int sp = rest.indexOf(' ');
    if (sp < 0) { reply("<ERR bad_args\n"); return; }
    int pin = rest.substring(0, sp).toInt();
    String mode = rest.substring(sp + 1); mode.trim();
    if      (mode.equalsIgnoreCase("OUT"))   { digitalWrite(pin, HIGH); pinMode(pin, OUTPUT); }
    else if (mode.equalsIgnoreCase("IN"))    pinMode(pin, INPUT);
    else if (mode.equalsIgnoreCase("IN_PU")) pinMode(pin, INPUT_PULLUP);
    else if (mode.equalsIgnoreCase("IN_PD")) pinMode(pin, INPUT_PULLDOWN);
    else { reply("<ERR bad_mode\n"); return; }
    reply("<OK\n");

  } else if (sub.equalsIgnoreCase("SET")) {
    int sp = rest.indexOf(' ');
    if (sp < 0) { reply("<ERR bad_args\n"); return; }
    int pin = rest.substring(0, sp).toInt();
    int val = rest.substring(sp + 1).toInt();
    digitalWrite(pin, val ? HIGH : LOW);
    if (pin == RELAY_PIN) {
      g_prefs.begin("wt", false); g_prefs.putInt("r23", val ? 1 : 0); g_prefs.end();
    }
    reply("<OK\n");

  } else if (sub.equalsIgnoreCase("GET")) {
    int pin = rest.toInt();
    reply(String("<GPIO ") + pin + "=" + digitalRead(pin) + "\n");

  } else if (sub.equalsIgnoreCase("PULSE")) {
    int sp = rest.indexOf(' ');
    if (sp < 0) { reply("<ERR bad_args\n"); return; }
    int  pin = rest.substring(0, sp).toInt();
    long ms  = rest.substring(sp + 1).toInt();
    if (ms <= 0) { reply("<ERR bad_duration\n"); return; }
    pinMode(pin, OUTPUT); digitalWrite(pin, LOW);
    g_pendingPulse.pin       = pin;
    g_pendingPulse.releaseAt = millis() + (unsigned long)ms;
    reply(String("<OK pulse ") + pin + " " + ms + "ms\n");

  } else { reply("<ERR bad_subcmd\n"); }
}


// ════════════════════════════════════════════════════════════════════
// DISPATCHER DE COMANDOS INTERNOS
// ════════════════════════════════════════════════════════════════════
static void handleInternal(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  cmd.remove(0, 1); cmd.trim();  // quita '>'

  // ── Infraestructura ────────────────────────────────────────────
  if (cmd.startsWith("GPIO")) {
    handleGpio((cmd.length() > 4) ? cmd.substring(4) : "");

  } else if (cmd.startsWith("BAUD")) {
    long baud = cmd.substring(4).toInt();
    if (baud <= 0) { reply("<ERR bad_baud\n"); return; }
    Tracker.updateBaudRate(baud);
    reply(String("<OK baud=") + baud + "\n");

  } else if (cmd.equalsIgnoreCase("PING")) {
    reply("<PONG\n");

  } else if (cmd.startsWith("NAME ")) {
    String n = cmd.substring(5); n.trim();
    if (n.length() == 0 || n.length() >= 24) { reply("<ERR bad_name\n"); return; }
    saveName(n); configureAdv();
    reply(String("<OK name=") + g_devName + "\n");

  } else if (cmd.startsWith("NAME")) {
    reply(String("<NAME ") + g_devName + "\n");

  } else if (cmd.equalsIgnoreCase("STATUS")) {
    const char* st = (g_session == SESSION_AUTHORIZED) ? "AUTHORIZED" : "LOCKED";
    reply(String("<STATUS name=") + g_devName +
          " session=" + st +
          " relay="   + relayFree() + "\n");

  // ── Provisionamiento ───────────────────────────────────────────
  } else if (cmd.equalsIgnoreCase("MAC")) {
    reply(String("<MAC ") + getBleMacString() + "\n");

  } else if (cmd.equalsIgnoreCase("UNPROVISION")) {
    clearAuthKey();
    g_session = SESSION_LOCKED;
    applyRelay(RELAY_LOCKED);
    reply("<UNPROVISION_OK\n");
    Serial.println("[PROV] Clave borrada — equipo sin provisionar");

  } else if (cmd.startsWith("PROVISION ")) {
    if (g_hasAuthKey) { reply("<ERR already_provisioned\n"); return; }
    String hexMaster = cmd.substring(10); hexMaster.trim();
    uint8_t master[32];
    if (!hexToBytes(hexMaster, master, 32)) {
      reply("<ERR bad_master_format need_64_hex_chars\n"); return;
    }
    // device_key = HMAC-SHA256(master, MAC_BLE_bytes)
    uint8_t mac[6]; getBleMacBytes(mac);
    uint8_t deviceKey[32];
    if (!computeHmac(master, 32, mac, 6, deviceKey)) {
      memset(master, 0, sizeof(master));
      reply("<ERR derivation_failed\n"); return;
    }
    memset(master, 0, sizeof(master));   // master nunca persiste
    saveAuthKey(deviceKey);
    memset(deviceKey, 0, sizeof(deviceKey));
    applyRelay(RELAY_LOCKED);
    g_session = SESSION_LOCKED;
    reply(String("<PROVISION_OK mac=") + getBleMacString() + "\n");
    Serial.printf("[PROV] device_key derivada — MAC %s\n", getBleMacString().c_str());

  // ── Sesión ─────────────────────────────────────────────────────
  } else if (cmd.equalsIgnoreCase("CHALLENGE")) {
    if (!g_hasAuthKey) { reply("<ERR no_key_set\n"); return; }
    esp_fill_random(g_nonce, NONCE_LEN);
    g_nonceValid = true;
    reply(String("<CHALLENGE ") + bytesToHex(g_nonce, NONCE_LEN) + "\n");

  } else if (cmd.startsWith("AUTH ")) {
    if (!g_hasAuthKey)                   { reply("<AUTH_FAIL no_key\n");             return; }
    if (!g_nonceValid)                   { reply("<AUTH_FAIL no_challenge\n");       return; }
    if (g_session == SESSION_AUTHORIZED) { reply("<AUTH_FAIL already_authorized\n"); return; }
    String hexResp = cmd.substring(5); hexResp.trim();
    uint8_t received[32];
    if (!hexToBytes(hexResp, received, 32)) { reply("<AUTH_FAIL bad_format\n"); return; }
    uint8_t expected[32];
    if (!computeHmac(g_authKey, 32, g_nonce, NONCE_LEN, expected)) {
      reply("<AUTH_FAIL internal_error\n"); return;
    }
    g_nonceValid = false;  // nonce de un solo uso — siempre invalidar
    if (memcmp(received, expected, 32) != 0) {
      reply("<AUTH_FAIL wrong_token\n");
      Serial.println("[AUTH] Intento fallido"); return;
    }
    g_session       = SESSION_AUTHORIZED;
    g_lastHeartbeat = millis();
    applyRelay(RELAY_UNLOCKED);
    reply("<AUTH_OK\n");
    Serial.println("[AUTH] Sesión autorizada — relé liberado");

  } else if (cmd.equalsIgnoreCase("HEARTBEAT")) {
    if (g_session != SESSION_AUTHORIZED) { reply("<HB_IGNORED not_authorized\n"); return; }
    g_lastHeartbeat = millis();
    reply("<HB_OK\n");

  } else if (cmd.equalsIgnoreCase("LOCK")) {
    if (g_session == SESSION_LOCKED) { reply("<OK already_locked\n"); return; }
    executeLock();

  } else if (cmd.equalsIgnoreCase("SESSION")) {
    reply(String("<SESSION ") +
          (g_session == SESSION_AUTHORIZED ? "AUTHORIZED" : "LOCKED") + "\n");

  } else if (cmd.startsWith("HB_TIMEOUT ")) {
    unsigned long ms = (unsigned long)cmd.substring(11).toInt();
    if (ms < 1000) { reply("<ERR min_1000ms\n"); return; }
    saveHbTimeout(ms);
    reply(String("<OK hb_timeout=") + ms + "ms\n");

  } else {
    reply("<ERR unknown_cmd\n");
  }
}


// ════════════════════════════════════════════════════════════════════
// CALLBACKS BLE
// ════════════════════════════════════════════════════════════════════
class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& connInfo) override {
    std::string v = chr->getValue();
    if (v.empty()) return;
    // Comandos internos comienzan con '>'; el resto va al FMC130
    if (v[0] == '>') handleInternal(String(v.c_str()));
    else             Tracker.write((const uint8_t*)v.data(), v.size());
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& connInfo) override {
    gClientConnected = true;
    Serial.println("[BLE] Conectado");
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& connInfo, int reason) override {
    gClientConnected = false;
    Serial.println("[BLE] Desconectado");
    // No bloquea de inmediato: reinicia el conteo; el watchdog corta tras hbTimeout.
    if (g_session == SESSION_AUTHORIZED) g_lastHeartbeat = millis();
    NimBLEDevice::startAdvertising();
  }
};


// ════════════════════════════════════════════════════════════════════
// SETUP
// ════════════════════════════════════════════════════════════════════
void setup() {
  // Forzar GPIO 23 en LOW (bloqueado) lo ANTES posible.
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_LOCKED);   // LOW = bloqueado (default seguro)

  Serial.begin(115200);
  delay(200);
  Serial.println("\nWisetrack ESP32 Gateway — llave virtual");

  Tracker.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  loadName();
  loadConfig();

  // Arranca bloqueado (LOW). Solo "Autorizar" desde la app lo lleva a HIGH.
  digitalWrite(RELAY_PIN, RELAY_LOCKED);
  g_session = SESSION_LOCKED;
  Serial.println("[INIT] Arrancando bloqueado (GPIO23 = LOW)");

  NimBLEDevice::init(g_devName);
  NimBLEDevice::setMTU(247);
  gServer = NimBLEDevice::createServer();
  gServer->setCallbacks(new ServerCallbacks());

  NimBLEService* svc = gServer->createService(NUS_SVC_UUID);
  NimBLECharacteristic* rxChar = svc->createCharacteristic(
      NUS_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rxChar->setCallbacks(new RxCallbacks());
  gTxChar = svc->createCharacteristic(NUS_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  svc->start();
  configureAdv();

  Serial.printf("[INIT] Advertising como '%s' | hbTimeout=%lums\n", g_devName, g_hbTimeout);
}


// ════════════════════════════════════════════════════════════════════
// LOOP
// ════════════════════════════════════════════════════════════════════
void loop() {
  // Forward Serial2 (FMC130) → BLE notify
  uint8_t buf[180]; size_t n = 0;
  while (Tracker.available() && n < sizeof(buf)) buf[n++] = Tracker.read();
  if (n > 0) bleNotify(buf, n);
  else       delay(5);

  // Liberar pulso programado
  if (g_pendingPulse.pin >= 0 && (long)(millis() - g_pendingPulse.releaseAt) >= 0) {
    digitalWrite(g_pendingPulse.pin, HIGH);
    if (g_pendingPulse.pin == RELAY_PIN) {
      g_prefs.begin("wt", false); g_prefs.putInt("r23", 1); g_prefs.end();
    }
    reply(String("<PULSE_DONE ") + g_pendingPulse.pin + "\n");
    g_pendingPulse.pin = -1;
  }

  // Watchdog heartbeat — bloqueo inmediato si la sesión expira
  if (g_session == SESSION_AUTHORIZED &&
      millis() - g_lastHeartbeat > g_hbTimeout) {
    Serial.printf("[HB] Sin heartbeat por %lums — bloqueando\n", g_hbTimeout);
    executeLock();
  }

  // Monitor de relé — notifica a la app si el FMC130 cambia el estado externamente
  int currentRelay = digitalRead(RELAY_PIN);
  if (currentRelay != g_lastRelayState) {
    g_lastRelayState = currentRelay;
    reply(String("<RELAY_CHANGED ") + relayFree() + "\n");
    Serial.printf("[RELAY] Estado cambiado externamente → libre=%d\n", relayFree());
  }
}

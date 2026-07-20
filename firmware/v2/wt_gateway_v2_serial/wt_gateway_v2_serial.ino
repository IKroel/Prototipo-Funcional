// Wisetrack ESP32 Gateway V2 — corte por serial (AT) al tracker.
// Arquitectura, protocolo, build y seguridad: ver README.md, firmware/README.md y docs/.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "esp_mac.h"
#include "esp_random.h"
#include <Preferences.h>
#include "mbedtls/md.h"
#include "driver/gpio.h"

// ════════════════════════════════════════════════════════════════════
// DIAGNÓSTICO Y MODO LIBRE
// Los comandos de diagnóstico compilan siempre pero exigen sesión autenticada.
// Los flags de modo libre arrancan en false (comportamiento productivo).
// ════════════════════════════════════════════════════════════════════
#define WT_DEBUG   // Modo libre / banco de pruebas. Comentar para build productivo.

#define FW_VERSION "2.9.5"

// ════════════════════════════════════════════════════════════════════
// HARDWARE
// ════════════════════════════════════════════════════════════════════
#define UART_TX_PIN    22
#define UART_RX_PIN    23
HardwareSerial &Tracker = Serial2;

// ════════════════════════════════════════════════════════════════════
// VALORES POR DEFECTO (viven en NVS tras el primer arranque)
// ════════════════════════════════════════════════════════════════════
#define DEF_BAUD        115200
#define DEF_PROFILE     "gv75cg"
#define DEF_IGN_ON      "IGN_ON"
#define DEF_IGN_OFF     "IGN_OFF"
#define DEF_GEO_IN      "ZonaSegura_ON"
#define DEF_GEO_OUT     "ZonaSegura_OFF"
#define DEF_CMD_CUT_ON  "AT+GTDOS=gv75cg,0,3,1,1,0,0,0,,2,0,0,0,0,,3,0,0,0,0,,0,0,5,,,,FFFF$"
#define DEF_CMD_CUT_OFF "AT+GTDOS=gv75cg,0,3,1,0,0,0,0,,2,0,0,0,0,,3,0,0,0,0,,0,0,5,,,,FFFF$"
#define DEF_KA          60     // segundos entre latidos KA (timer único)

// ════════════════════════════════════════════════════════════════════
// BLE — Nordic UART Service
// ════════════════════════════════════════════════════════════════════
#define NUS_SVC_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID   "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID   "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

#define NONCE_LEN  8

// Origen de un comando (dispatcher por canal).
enum Source { SRC_BLE, SRC_SERIAL };

// ════════════════════════════════════════════════════════════════════
// GLOBALS
// ════════════════════════════════════════════════════════════════════
NimBLECharacteristic* gTxChar          = nullptr;
bool                  gClientConnected = false;

// Configuración (cargada desde NVS en setup, con defaults DEF_*).
uint32_t g_baud       = DEF_BAUD;
String   g_profile    = DEF_PROFILE;   // "" = sin perfil (dispara AUTO_DETECT)
String   g_cmdCutOn   = DEF_CMD_CUT_ON;
String   g_cmdCutOff  = DEF_CMD_CUT_OFF;
String   g_ignOnStr   = DEF_IGN_ON;
String   g_ignOffStr  = DEF_IGN_OFF;
String   g_geoInStr   = DEF_GEO_IN;
String   g_geoOutStr  = DEF_GEO_OUT;
uint32_t g_ka         = DEF_KA;

// Estado del vehículo (último conocido por serial)
bool g_enabled     = true;    // standby: false = ignora comandos de acción
bool g_ignOn       = false;   // ignición
bool g_ignKnown    = false;   // ¿llegó IGN al menos una vez?
bool g_inGeo       = false;   // dentro de geocerca
bool g_geoKnown    = false;   // ¿llegó ZonaSegura al menos una vez?
bool g_cutDisabled = false;   // corte deshabilitado por app/serial (override)
bool g_tripStarted = false;   // para re-armar tras un ciclo de ignición
int  g_lastCut     = -1;      // último estado enviado: -1 desc, 0 sin corte, 1 corte

// Último estado real recibido por serial (no el simulado); base al salir de debug.
bool g_serIgnOn = false, g_serIgnKnown = false;
bool g_serInGeo = false, g_serGeoKnown = false;

// Autenticación BLE
uint8_t g_nonce[NONCE_LEN] = {0};
bool    g_nonceValid       = false;
uint8_t g_authKey[32]      = {0};
bool    g_hasAuthKey       = false;
bool    g_authed           = false;   // sesión BLE autenticada

Preferences g_prefs;
char        g_devName[24] = {0};
String      g_rxBuf;          // buffer de líneas seriales

// Keep-alive
uint32_t g_lastKa = 0;

// Contadores de diagnóstico del bus serial (tracker -> ESP)
uint32_t g_serRxBytes = 0;
uint32_t g_serRxLines = 0;
bool     g_serMon     = false;
uint32_t g_lastSerEcho = 0;

#ifdef WT_DEBUG
// Flags del MODO LIBRE (solo build de testeo). En true = esa restricción se SALTA.
bool g_dbgRelaxGeo   = false;
bool g_dbgAlwaysSend = false;
bool g_dbgIgnoreOvr  = false;

// Snapshot del estado operativo para restaurar al salir del modo debug.
// La app hace >SNAPSHOT al entrar y >RESTORE al salir; además se restaura
// solo en onDisconnect si quedó pendiente (app cerrada / BLE caído).
struct StateSnapshot {
  bool ignOn, ignKnown, inGeo, geoKnown, cutDisabled, tripStarted;
};
StateSnapshot g_snap;
bool g_hasSnapshot = false;
#endif

// ════════════════════════════════════════════════════════════════════
// AUTO_DETECT de perfil — FSM no bloqueante
// TODO: completar idCmd/expect por modelo (Teltonika/Syrus/TRAX).
// ════════════════════════════════════════════════════════════════════
struct ProfileDef {
  const char* name;
  const char* idCmd;    // comando que provoca una respuesta identificable
  const char* expect;   // substring que confirma el perfil
};
static const ProfileDef PROFILES[] = {
  { "gv75cg", "AT+GTRTO=gv75cg,0,,,,,,FFFF$", "+RESP:GTRTO" },
  // { "fmc003", "...", "..." },   // TODO: Teltonika
  // { "syrus",  "...", "..." },   // TODO: Digital Comm/Syrus
};
static const int PROFILE_COUNT = sizeof(PROFILES) / sizeof(PROFILES[0]);
#define AUTODET_TIMEOUT_MS 500

bool     g_autoDetecting = false;
int      g_adIdx         = 0;       // perfil candidato actual
uint32_t g_adSentAt      = 0;       // millis del envío del idCmd

// ════════════════════════════════════════════════════════════════════
// BLE — NOTIFICACIÓN
// ════════════════════════════════════════════════════════════════════
static void reply(const String& s) {
  Serial.printf("[TX APP] %s", s.c_str());
  if (!gClientConnected || !gTxChar) return;
  gTxChar->setValue((const uint8_t*)s.c_str(), s.length());
  gTxChar->notify();
  delay(25);   // evita que dos notify seguidos se pisen
}

// ════════════════════════════════════════════════════════════════════
// CRIPTO Y HEX (idéntico a V1)
// ════════════════════════════════════════════════════════════════════
static bool computeHmac(const uint8_t* key, size_t keyLen,
                        const uint8_t* data, size_t dataLen, uint8_t out[32]) {
  mbedtls_md_context_t ctx; mbedtls_md_init(&ctx);
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (mbedtls_md_setup(&ctx, info, 1) != 0) { mbedtls_md_free(&ctx); return false; }
  mbedtls_md_hmac_starts(&ctx, key, keyLen);
  mbedtls_md_hmac_update(&ctx, data, dataLen);
  mbedtls_md_hmac_finish(&ctx, out);
  mbedtls_md_free(&ctx);
  return true;
}
static bool hexToBytes(const String& hex, uint8_t* out, size_t outLen) {
  if ((size_t)hex.length() != outLen * 2) return false;
  for (size_t i = 0; i < outLen; i++) {
    auto nib = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1; };
    int h = nib(hex[i*2]), l = nib(hex[i*2+1]);
    if (h < 0 || l < 0) return false;
    out[i] = (uint8_t)((h << 4) | l);
  }
  return true;
}
static String bytesToHex(const uint8_t* data, size_t len) {
  String s; s.reserve(len * 2);
  for (size_t i = 0; i < len; i++) { char b[3]; snprintf(b, 3, "%02x", data[i]); s += b; }
  return s;
}
static String getBleMacString() {
  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
  return String(buf);
}

// ════════════════════════════════════════════════════════════════════
// NVS — nombre, clave, configuración y estado operativo
// ════════════════════════════════════════════════════════════════════
static void loadName() {
  g_prefs.begin("wt", true);
  String s = g_prefs.getString("name", "");
  g_prefs.end();
  if (s.length() > 0 && s.length() < (int)sizeof(g_devName) - 1) {
    strncpy(g_devName, s.c_str(), sizeof(g_devName) - 1);
  } else {
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
    snprintf(g_devName, sizeof(g_devName), "WT-%02X%02X", mac[4], mac[5]);
  }
  g_devName[sizeof(g_devName) - 1] = 0;
}
static void saveName(const String& name) {
  g_prefs.begin("wt", false); g_prefs.putString("name", name); g_prefs.end();
  strncpy(g_devName, name.c_str(), sizeof(g_devName) - 1);
  g_devName[sizeof(g_devName) - 1] = 0;
}
// Borra el nombre guardado y vuelve al genérico WT-xxxx (usado al restablecer).
static void resetNameToDefault() {
  g_prefs.begin("wt", false); g_prefs.remove("name"); g_prefs.end();
  loadName();   // sin nombre guardado -> genera "WT-xxxx" desde la MAC
}
static void loadKey() {
  g_prefs.begin("wt", true);
  if (g_prefs.getBytesLength("authkey") == 32) {
    g_prefs.getBytes("authkey", g_authKey, 32); g_hasAuthKey = true;
  }
  g_prefs.end();
}
static void saveAuthKey(const uint8_t key[32]) {
  memcpy(g_authKey, key, 32); g_hasAuthKey = true;
  g_prefs.begin("wt", false); g_prefs.putBytes("authkey", key, 32); g_prefs.end();
}
static void clearAuthKey() {
  memset(g_authKey, 0, 32); g_hasAuthKey = false;
  g_prefs.begin("wt", false); g_prefs.remove("authkey"); g_prefs.end();
}

// Configuración de operación (perfil, comandos AT, strings, intervalos).
static void loadConfig() {
  g_prefs.begin("wt", true);
  g_baud        = g_prefs.getUInt  ("baud",   DEF_BAUD);
  g_profile     = g_prefs.getString("prof",   DEF_PROFILE);
  g_cmdCutOn    = g_prefs.getString("cOn",    DEF_CMD_CUT_ON);
  g_cmdCutOff   = g_prefs.getString("cOff",   DEF_CMD_CUT_OFF);
  g_ignOnStr    = g_prefs.getString("ignOn",  DEF_IGN_ON);
  g_ignOffStr   = g_prefs.getString("ignOff", DEF_IGN_OFF);
  g_geoInStr    = g_prefs.getString("geoIn",  DEF_GEO_IN);
  g_geoOutStr   = g_prefs.getString("geoOut", DEF_GEO_OUT);
  g_ka          = g_prefs.getUInt  ("ka",     DEF_KA);
  g_enabled     = g_prefs.getBool  ("en",     true);
  g_prefs.end();
}
// Setters de un solo campo (string/uint) que persisten en NVS.
static void setCfgStr(const char* key, const String& val) {
  g_prefs.begin("wt", false); g_prefs.putString(key, val); g_prefs.end();
}
static void setCfgU32(const char* key, uint32_t val) {
  g_prefs.begin("wt", false); g_prefs.putUInt(key, val); g_prefs.end();
}
static void setEnabled(bool en) {
  g_enabled = en;
  g_prefs.begin("wt", false); g_prefs.putBool("en", en); g_prefs.end();
}

// Persistencia del estado operativo (no se pierde entre viajes/reboots).
static void saveState() {
  g_prefs.begin("wt", false);
  g_prefs.putBool("ignK", g_ignKnown); g_prefs.putBool("ign", g_ignOn);
  g_prefs.putBool("geoK", g_geoKnown); g_prefs.putBool("geo", g_inGeo);
  g_prefs.putBool("ovr",  g_cutDisabled); g_prefs.putBool("trip", g_tripStarted);
  g_prefs.end();
}
static void loadState() {
  g_prefs.begin("wt", true);
  g_ignKnown    = g_prefs.getBool("ignK", false);
  g_ignOn       = g_prefs.getBool("ign",  false);
  g_geoKnown    = g_prefs.getBool("geoK", false);
  g_inGeo       = g_prefs.getBool("geo",  false);
  g_cutDisabled = g_prefs.getBool("ovr",  false);
  g_tripStarted = g_prefs.getBool("trip", false);
  g_prefs.end();
  // Semilla de los trackers de serial con el último estado persistido.
  g_serIgnOn = g_ignOn; g_serIgnKnown = g_ignKnown;
  g_serInGeo = g_inGeo; g_serGeoKnown = g_geoKnown;
}

// ════════════════════════════════════════════════════════════════════
// ADVERTISING ANÓNIMO (solo Manufacturer Data WT; sin nombre ni scan resp)
// ════════════════════════════════════════════════════════════════════
static void configureAdv() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();
  adv->setName("");              // sin nombre en el advertising packet
  adv->enableScanResponse(false);   // sin scan response -> "Unknown Device"
  std::string mfg;
  mfg.push_back((char)0xFF); mfg.push_back((char)0xFF);
  mfg.push_back('W'); mfg.push_back('T'); mfg.push_back((char)0x01);
  NimBLEAdvertisementData advData; advData.setFlags(0x06); advData.setManufacturerData(mfg);
  adv->setAdvertisementData(advData);
  adv->start();
}

// ════════════════════════════════════════════════════════════════════
// REPORTE DE ESTADO
// ════════════════════════════════════════════════════════════════════
static void sendStatusBle() {
  String s = String("<STATUS name=") + g_devName + " en=" + (g_enabled ? 1 : 0);
  if (g_ignKnown)      s += String(" ign=")  + (g_ignOn ? 1 : 0);
  if (g_geoKnown)      s += String(" geo=")  + (g_inGeo ? 1 : 0);
  if (g_lastCut >= 0)  s += String(" cut=")  + (g_lastCut == 1 ? 1 : 0);
  s += String(" override=") + (g_cutDisabled ? 1 : 0) + "\n";
  reply(s);
}

// JSON de configuración + estado en vivo (>GET_PROFILE).
static String buildProfileJson() {
  auto esc = [](const String& v) -> String {
    String o; for (int i = 0; i < v.length(); i++) {
      char c = v[i]; if (c == '"' || c == '\\') o += '\\'; o += c; } return o; };
  String j = "{";
  j += "\"type\":\"profile\",";
  j += "\"mac\":\"" + getBleMacString() + "\",";
  j += "\"fw\":\"" + String(FW_VERSION) + "\",";
  j += "\"name\":\"" + esc(String(g_devName)) + "\",";
  j += "\"enabled\":" + String(g_enabled ? "true" : "false") + ",";
  j += "\"profile\":\"" + esc(g_profile) + "\",";
  j += "\"baud\":" + String(g_baud) + ",";
  j += "\"cut\":" + String(g_lastCut == 1 ? 1 : 0) + ",";
  j += "\"ign\":" + String(g_ignOn ? "true" : "false") + ",";
  j += "\"geo\":" + String(g_inGeo ? "true" : "false") + ",";
  j += "\"override\":" + String(g_cutDisabled ? "true" : "false") + ",";
  j += "\"cut_on\":\"" + esc(g_cmdCutOn) + "\",";
  j += "\"cut_off\":\"" + esc(g_cmdCutOff) + "\",";
  j += "\"ign_on\":\"" + esc(g_ignOnStr) + "\",";
  j += "\"ign_off\":\"" + esc(g_ignOffStr) + "\",";
  j += "\"geo_in\":\"" + esc(g_geoInStr) + "\",";
  j += "\"geo_out\":\"" + esc(g_geoOutStr) + "\",";
  j += "\"ka\":" + String(g_ka);
  j += "}";
  return j;
}

// Latido KA: mac|name|enabled, sin ':' ni comas (compatibilidad GTDAT).
static String buildKa() {
  String mac = getBleMacString();
  mac.replace(":", "");
  return mac + "|" + String(g_devName) + "|" + (g_enabled ? "1" : "0");
}

#ifdef WT_DEBUG
static void sendDumpBle() {
  reply(String("<DUMP mac=") + getBleMacString()
        + " prov="       + (g_hasAuthKey ? 1 : 0)
        + " en="         + g_enabled
        + " prof="       + g_profile
        + " ignK="       + g_ignKnown   + " ign=" + g_ignOn
        + " geoK="       + g_geoKnown   + " geo=" + g_inGeo
        + " override="   + g_cutDisabled + " trip=" + g_tripStarted
        + " lastCut="    + g_lastCut
        + " relaxGeo="   + g_dbgRelaxGeo + " alwaysSend=" + g_dbgAlwaysSend
        + " ignOvr="     + g_dbgIgnoreOvr
        + " serBytes="   + g_serRxBytes + " serLines=" + g_serRxLines + "\n");
}
#endif

// ════════════════════════════════════════════════════════════════════
// LÓGICA DE CORTE — actúa enviando el comando AT al tracker
// ════════════════════════════════════════════════════════════════════
static void applyCut(bool cut) {
  if (!g_enabled) return;   // en standby el ESP no acciona el corte
  int c = cut ? 1 : 0;
#ifdef WT_DEBUG
  if (c == g_lastCut && !g_dbgAlwaysSend) return;
#else
  if (c == g_lastCut) return;     // solo en cambios
#endif
  g_lastCut = c;
  const String& atCmd = cut ? g_cmdCutOn : g_cmdCutOff;
  Tracker.print(atCmd);
  Tracker.print("\r\n");
  Serial.printf("[TX GPS] %s\n", atCmd.c_str());
  reply(String("<TXGPS ") + atCmd + "\n");
  sendStatusBle();
}

// Regla: corta si (ignición OFF y fuera de geocerca) y no está deshabilitado.
// Mientras no se conozca la geocerca -> libre (no corta).
static void evaluate() {
  if (!g_enabled) return;   // standby: no se evalúa corte
  bool cut;
#ifdef WT_DEBUG
  bool ovrActive = g_cutDisabled && !g_dbgIgnoreOvr;
  bool geoGate   = !g_geoKnown   && !g_dbgRelaxGeo;
  if (ovrActive || geoGate) cut = false;
  else                      cut = (!g_ignOn && !g_inGeo);
#else
  if (g_cutDisabled || !g_geoKnown) cut = false;
  else                              cut = (!g_ignOn && !g_inGeo);
#endif
  applyCut(cut);
}

static void disableCut(const char* source) {
  g_cutDisabled = true;
  g_tripStarted = g_ignOn;
  Serial.printf("[CUT] Deshabilitado por %s\n", source);
  evaluate();
  saveState();
}

static void armCut() {
  g_cutDisabled = false;
  g_tripStarted = false;
  Serial.println("[CUT] Re-armado manual");
  evaluate();
  saveState();
}

#ifdef WT_DEBUG
// Guarda el estado operativo actual para poder restaurarlo al salir de debug.
static void takeSnapshot() {
  g_snap = {g_ignOn, g_ignKnown, g_inGeo, g_geoKnown, g_cutDisabled, g_tripStarted};
  g_hasSnapshot = true;
  Serial.println("[DBG] Snapshot tomado");
}
// Restaura al salir de debug: ignición/geocerca vuelven a lo ÚLTIMO recibido
// por serial (GPS real); si nunca llegó nada por el bus, usa el snapshot
// previo a la conexión. Override/trip (Liberar/Cortar) son acciones reales:
// no se revierten.
static void restoreSnapshot() {
  if (!g_hasSnapshot) return;
  if (g_serIgnKnown) { g_ignOn = g_serIgnOn; g_ignKnown = true; }
  else               { g_ignOn = g_snap.ignOn; g_ignKnown = g_snap.ignKnown; }
  if (g_serGeoKnown) { g_inGeo = g_serInGeo; g_geoKnown = true; }
  else               { g_inGeo = g_snap.inGeo; g_geoKnown = g_snap.geoKnown; }
  g_hasSnapshot = false;
  evaluate();
  saveState();
  Serial.println("[DBG] Estado restaurado (último real de serial)");
}
#endif

static void setIgnition(bool on) {
  g_ignKnown = true;
  if (on) {
    g_ignOn = true;
    if (g_cutDisabled) g_tripStarted = true;
  } else {
    if (g_cutDisabled && g_tripStarted) {
      g_cutDisabled = false;
      g_tripStarted = false;
      Serial.println("[CUT] Re-armado (fin de ciclo de ignición)");
    }
    g_ignOn = false;
  }
  evaluate();
  saveState();
}

// ════════════════════════════════════════════════════════════════════
// STANDBY — vía config serial (clave 3) o BLE
// ════════════════════════════════════════════════════════════════════
static void enterStandby() {
  if (!g_enabled) return;
  applyCut(false);          // libera el corte antes de quedar inactivo
  setEnabled(false);
  Serial.println("[STBY] Standby ON (tracker)");
  sendStatusBle();
}
static void exitStandby() {
  if (g_enabled) return;
  setEnabled(true);
  Serial.println("[STBY] Standby OFF (tracker)");
  evaluate();               // reaplica el estado real
  sendStatusBle();
}

// ════════════════════════════════════════════════════════════════════
// GTDAT — envuelve un payload para que el tracker lo reenvíe a plataforma
// ════════════════════════════════════════════════════════════════════
static void sendGtdat(const String& payload) {
  String prof = g_profile.length() ? g_profile : String("gv75cg");
  String m = String("AT+GTDAT=") + prof + ",2,," + payload + ",0,,,,FFFF$";
  Tracker.print(m);
  Tracker.print("\r\n");
  Serial.printf("[GTDAT] %s\n", m.c_str());
  if (g_serMon) reply(String("<TXGPS ") + m + "\n");
}

// ════════════════════════════════════════════════════════════════════
// LATIDO PERIÓDICO (KA) — buildKa() por GTDAT cada g_ka s (0 = off)
// ════════════════════════════════════════════════════════════════════
static void pumpKeepAlive() {
  uint32_t secs = g_ka;
  if (secs == 0) return;    // 0 = deshabilitado
  if ((millis() - g_lastKa) < (secs * 1000UL)) return;
  g_lastKa = millis();
  sendGtdat(buildKa());
}

// ════════════════════════════════════════════════════════════════════
// AUTO_DETECT — FSM no bloqueante
// ════════════════════════════════════════════════════════════════════
static void startAutoDetect(Source src) {
  if (PROFILE_COUNT == 0) {
    if (src == SRC_BLE) reply("<ERR no_profiles\n");
    return;
  }
  g_autoDetecting = true;
  g_adIdx = 0;
  g_adSentAt = 0;           // 0 -> el tick enviará el primer idCmd ya
  Serial.println("[AUTODET] Inicio");
  if (src == SRC_BLE) reply("<AUTO_DETECT start\n");
}
// Llamada desde processSerialLine: ¿la línea confirma el candidato actual?
static void autoDetectOnLine(const String& line) {
  if (!g_autoDetecting) return;
  if (line.indexOf(PROFILES[g_adIdx].expect) >= 0) {
    g_profile = PROFILES[g_adIdx].name;
    setCfgStr("prof", g_profile);
    g_autoDetecting = false;
    Serial.printf("[AUTODET] Perfil = %s\n", g_profile.c_str());
    reply(String("<PROFILE_DETECTED ") + g_profile + "\n");
  }
}
// Tick en loop(): envía idCmd del candidato y avanza por timeout.
static void pumpAutoDetect() {
  if (!g_autoDetecting) return;
  uint32_t now = millis();
  if (g_adSentAt == 0 || (now - g_adSentAt) >= AUTODET_TIMEOUT_MS) {
    if (g_adSentAt != 0) g_adIdx++;     // el candidato anterior no respondió
    if (g_adIdx >= PROFILE_COUNT) {
      g_autoDetecting = false;
      Serial.println("[AUTODET] Sin coincidencia");
      reply("<AUTO_DETECT none\n");
      return;
    }
    Tracker.print(PROFILES[g_adIdx].idCmd);
    Tracker.print("\r\n");
    g_adSentAt = now;
    Serial.printf("[AUTODET] Probando %s\n", PROFILES[g_adIdx].name);
  }
}

// ════════════════════════════════════════════════════════════════════
// PARSER SERIAL (tracker -> ESP)
// ════════════════════════════════════════════════════════════════════
static bool isPrintableLine(const String& s) {
  if (s.length() == 0 || s.length() > 120) return false;
  for (int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c != '\t' && (c < 0x20 || c > 0x7E)) return false;
  }
  return true;
}

static void handleInternal(String cmd);   // fwd

// ════════════════════════════════════════════════════════════════════
// CONFIG REMOTA DESDE EL GPS — "<clave>|<valor>" (1=name 2=ka 3=enabled 4=profile)
// ════════════════════════════════════════════════════════════════════
static bool applySerialConfig(const String& line) {
  int sep = line.indexOf('|');
  if (sep <= 0) return false;
  for (int i = 0; i < sep; i++) if (!isDigit(line[i])) return false;
  int    key = line.substring(0, sep).toInt();
  String val = line.substring(sep + 1); val.trim();
  if (val.length() == 0) return false;

  switch (key) {
    case 1: if (val.length() < 24) { saveName(val); configureAdv(); } break;
    case 2: g_ka = (uint32_t)val.toInt(); setCfgU32("ka", g_ka); break;
    case 3: { bool en = (val.toInt() != 0); if (en) exitStandby(); else enterStandby(); } break;
    case 4: g_profile = val; setCfgStr("prof", g_profile); break;
    default: return false;   // clave desconocida
  }
  if (g_serMon) reply(String("<CFG ") + key + "=" + val + "\n");
  return true;
}

static void processSerialLine(const String& line) {
  if (line.length() == 0) return;
  g_serRxLines++;
  Serial.printf("[RX GPS] %s\n", line.c_str());

  // AUTO_DETECT mira cada línea entrante.
  autoDetectOnLine(line);

  if (g_serMon && (millis() - g_lastSerEcho) >= 100) {
    g_lastSerEcho = millis();
    if (isPrintableLine(line)) {
      reply(String("<SER ") + line + "\n");
    } else {
      int n = line.length(); if (n > 40) n = 40;
      reply(String("<SERHEX ") + bytesToHex((const uint8_t*)line.c_str(), n) + "\n");
    }
  }

  // Desde el GPS solo se acepta config "<clave>|<valor>" y (más abajo) los
  // tokens de ignición/geocerca. Comandos ">" u otra cosa se ignoran.
  if (isDigit(line[0]) && applySerialConfig(line)) return;

  bool evChanged = false;
  if      (line.indexOf(g_ignOnStr)  >= 0) { setIgnition(true);  g_serIgnOn = true;  g_serIgnKnown = true; evChanged = true; }
  else if (line.indexOf(g_ignOffStr) >= 0) { setIgnition(false); g_serIgnOn = false; g_serIgnKnown = true; evChanged = true; }

  if      (line.indexOf(g_geoInStr)  >= 0) { g_inGeo = true;  g_geoKnown = true; g_serInGeo = true;  g_serGeoKnown = true; evaluate(); saveState(); evChanged = true; }
  else if (line.indexOf(g_geoOutStr) >= 0) { g_inGeo = false; g_geoKnown = true; g_serInGeo = false; g_serGeoKnown = true; evaluate(); saveState(); evChanged = true; }

  if (evChanged) {
    sendStatusBle();
#ifdef WT_DEBUG
    sendDumpBle();
#endif
  }
}

static void pumpSerial() {
  while (Tracker.available()) {
    char c = (char)Tracker.read();
    g_serRxBytes++;
    if (c == '\n' || c == '\r') {
      if (g_rxBuf.length() > 0) { processSerialLine(g_rxBuf); g_rxBuf = ""; }
    } else {
      g_rxBuf += c;
      if (g_rxBuf.length() > 200) g_rxBuf = "";
    }
  }
}

// ════════════════════════════════════════════════════════════════════
// DISPATCHER DE COMANDOS BLE (app -> ESP)
// ════════════════════════════════════════════════════════════════════
static void handleInternal(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  Serial.printf("[RX APP] %s\n", cmd.c_str());
  cmd.remove(0, 1); cmd.trim();   // quita '>'

  // ───────────────────────── Canal BLE ─────────────────────────
  if (cmd.equalsIgnoreCase("PING")) {
    reply("<PONG\n");

  } else if (cmd.equalsIgnoreCase("VERSION")) {
    reply(String("<VERSION fw=") + FW_VERSION + " mac=" + getBleMacString()
          + " prov=" + (g_hasAuthKey ? 1 : 0) + "\n");

  } else if (cmd.equalsIgnoreCase("GET_PROFILE")) {
    reply(String("<PROFILE ") + buildProfileJson() + "\n");

  } else if (cmd.equalsIgnoreCase("REPORT")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    sendGtdat(buildProfileJson());
    reply("<OK report_sent\n");

  } else if (cmd.startsWith("NAME ")) {
    String n = cmd.substring(5); n.trim();
    if (n.length() == 0 || n.length() >= 24) { reply("<ERR bad_name\n"); return; }
    saveName(n); configureAdv();
    reply(String("<OK name=") + g_devName + "\n");

  } else if (cmd.startsWith("NAME")) {
    reply(String("<NAME ") + g_devName + "\n");

  } else if (cmd.equalsIgnoreCase("STATUS")) {
    sendStatusBle();

  } else if (cmd.equalsIgnoreCase("SERSTATS")) {
    reply(String("<SERSTATS bytes=") + g_serRxBytes
          + " lines=" + g_serRxLines + " mon=" + (g_serMon ? 1 : 0) + "\n");

  } else if (cmd.startsWith("SERMON")) {
    String a = cmd.substring(6); a.trim();
    g_serMon = (a == "1" || a.equalsIgnoreCase("on"));
    reply(String("<SERMON ") + (g_serMon ? 1 : 0) + "\n");

  } else if (cmd.equalsIgnoreCase("MAC")) {
    reply(String("<MAC ") + getBleMacString() + "\n");

  } else if (cmd.equalsIgnoreCase("AUTO_DETECT")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    startAutoDetect(SRC_BLE);

  } else if (cmd.equalsIgnoreCase("UNPROVISION")) {
    if (g_hasAuthKey && !g_authed) { reply("<ERR not_authed\n"); return; }
    clearAuthKey(); g_authed = false;
    resetNameToDefault();   // el equipo vuelve a llamarse WT-xxxx
    reply(String("<UNPROVISION_OK name=") + g_devName + "\n");

  } else if (cmd.startsWith("PROVISION ")) {
    if (g_hasAuthKey) { reply("<ERR already_provisioned\n"); return; }
    String hexMaster = cmd.substring(10); hexMaster.trim();
    uint8_t master[32];
    if (!hexToBytes(hexMaster, master, 32)) { reply("<ERR bad_master_format\n"); return; }
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
    uint8_t deviceKey[32];
    if (!computeHmac(master, 32, mac, 6, deviceKey)) {
      memset(master, 0, 32); reply("<ERR derivation_failed\n"); return;
    }
    memset(master, 0, 32);
    saveAuthKey(deviceKey); memset(deviceKey, 0, 32);
    reply(String("<PROVISION_OK mac=") + getBleMacString() + "\n");

  } else if (cmd.equalsIgnoreCase("CHALLENGE")) {
    if (!g_hasAuthKey) { reply("<ERR no_key_set\n"); return; }
    esp_fill_random(g_nonce, NONCE_LEN); g_nonceValid = true;
    reply(String("<CHALLENGE ") + bytesToHex(g_nonce, NONCE_LEN) + "\n");

  } else if (cmd.startsWith("AUTH ")) {
    if (!g_hasAuthKey)  { reply("<AUTH_FAIL no_key\n");       return; }
    if (!g_nonceValid)  { reply("<AUTH_FAIL no_challenge\n"); return; }
    uint8_t received[16];
    if (!hexToBytes(cmd.substring(5), received, 16)) { reply("<AUTH_FAIL bad_format\n"); return; }
    uint8_t expected[32];
    if (!computeHmac(g_authKey, 32, g_nonce, NONCE_LEN, expected)) {
      reply("<AUTH_FAIL internal_error\n"); return;
    }
    g_nonceValid = false;
    if (memcmp(received, expected, 16) != 0) { reply("<AUTH_FAIL wrong_token\n"); return; }
    g_authed = true;
    reply("<AUTH_OK\n");

  } else if (cmd.equalsIgnoreCase("DISABLECUT")) {
    if (!g_authed)  { reply("<ERR not_authed\n");      return; }
    if (!g_enabled) { reply("<ERR device_disabled\n"); return; }
    disableCut("app");
    reply("<CUT_DISABLED\n");

  } else if (cmd.equalsIgnoreCase("ARMCUT")) {
    if (!g_authed)  { reply("<ERR not_authed\n");      return; }
    if (!g_enabled) { reply("<ERR device_disabled\n"); return; }
    armCut();
    reply("<CUT_ARMED\n");

  } else if (cmd.startsWith("SET_PROFILE ")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    g_profile = cmd.substring(12); g_profile.trim();
    setCfgStr("prof", g_profile);
    reply(String("<OK profile=") + g_profile + "\n");

  } else if (cmd.startsWith("SET_CUTON ")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    g_cmdCutOn = cmd.substring(10); setCfgStr("cOn", g_cmdCutOn);
    reply("<OK cut_on\n");

  } else if (cmd.startsWith("SET_CUTOFF ")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    g_cmdCutOff = cmd.substring(11); setCfgStr("cOff", g_cmdCutOff);
    reply("<OK cut_off\n");

  } else if (cmd.startsWith("SET_IGNON ")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    g_ignOnStr = cmd.substring(10); g_ignOnStr.trim(); setCfgStr("ignOn", g_ignOnStr);
    reply("<OK ign_on_str\n");

  } else if (cmd.startsWith("SET_IGNOFF ")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    g_ignOffStr = cmd.substring(11); g_ignOffStr.trim(); setCfgStr("ignOff", g_ignOffStr);
    reply("<OK ign_off_str\n");

  } else if (cmd.startsWith("SET_GEOIN ")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    g_geoInStr = cmd.substring(10); g_geoInStr.trim(); setCfgStr("geoIn", g_geoInStr);
    reply("<OK geo_in_str\n");

  } else if (cmd.startsWith("SET_GEOOUT ")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    g_geoOutStr = cmd.substring(11); g_geoOutStr.trim(); setCfgStr("geoOut", g_geoOutStr);
    reply("<OK geo_out_str\n");

  } else if (cmd.startsWith("SET_KA ")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    g_ka = (uint32_t)cmd.substring(7).toInt(); setCfgU32("ka", g_ka);
    reply(String("<OK ka=") + g_ka + "\n");

#ifdef WT_DEBUG
  } else if (cmd.startsWith("TESTGPS")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    String a = cmd.substring(7); a.trim();
    bool on = (a.equalsIgnoreCase("ON") || a == "1");
    const String& atCmd = on ? g_cmdCutOn : g_cmdCutOff;
    Tracker.print(atCmd); Tracker.print("\r\n");
    Serial.printf("[TX GPS] %s\n", atCmd.c_str());
    reply(String("<TXGPS ") + atCmd + "\n");

  } else if (cmd.startsWith("SIM ")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    String tok = cmd.substring(4); tok.trim();
    reply(String("<SIM ") + tok + "\n");
    processSerialLine(tok);

  } else if (cmd.startsWith("SETIGN")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    g_ignKnown = true; g_ignOn = (cmd.substring(6).toInt() != 0);
    evaluate(); saveState(); sendDumpBle();

  } else if (cmd.startsWith("SETGEOKNOWN")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    g_geoKnown = (cmd.substring(11).toInt() != 0);
    evaluate(); saveState(); sendDumpBle();

  } else if (cmd.startsWith("SETGEO")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    g_geoKnown = true; g_inGeo = (cmd.substring(6).toInt() != 0);
    evaluate(); saveState(); sendDumpBle();

  } else if (cmd.startsWith("SETOVR")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    g_cutDisabled = (cmd.substring(6).toInt() != 0);
    evaluate(); saveState(); sendDumpBle();

  } else if (cmd.startsWith("SETEN")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    bool en = (cmd.substring(5).toInt() != 0);
    if (en) exitStandby(); else enterStandby();
    sendDumpBle();

  } else if (cmd.startsWith("RELAXGEO")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    g_dbgRelaxGeo = (cmd.substring(8).toInt() != 0);
    evaluate(); sendDumpBle();

  } else if (cmd.startsWith("ALWAYSSEND")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    g_dbgAlwaysSend = (cmd.substring(10).toInt() != 0);
    sendDumpBle();

  } else if (cmd.startsWith("IGNOVR")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    g_dbgIgnoreOvr = (cmd.substring(6).toInt() != 0);
    evaluate(); sendDumpBle();

  } else if (cmd.equalsIgnoreCase("DUMP")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    sendDumpBle();

  } else if (cmd.equalsIgnoreCase("SNAPSHOT")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    takeSnapshot();
    reply("<SNAPSHOT_OK\n");

  } else if (cmd.equalsIgnoreCase("RESTORE")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    restoreSnapshot();
    reply("<RESTORE_OK\n");
#endif

  } else {
    reply("<ERR unknown_cmd\n");
  }
}

// ════════════════════════════════════════════════════════════════════
// CALLBACKS BLE
// ════════════════════════════════════════════════════════════════════
class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
    std::string v = chr->getValue();
    if (!v.empty() && v[0] == '>') handleInternal(String(v.c_str()));
  }
};
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    gClientConnected = true; Serial.println("[BLE] Conectado");
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    gClientConnected = false; g_authed = false;
#ifdef WT_DEBUG
    // Si la sesión de debug no alcanzó a hacer >RESTORE, se restaura solo.
    if (g_hasSnapshot) restoreSnapshot();
#endif
    Serial.println("[BLE] Desconectado (se mantiene el estado del corte)");
    NimBLEDevice::startAdvertising();
  }
};

// ════════════════════════════════════════════════════════════════════
// SETUP
// ════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nWisetrack ESP32 Gateway V2 (solo serial, corte por AT)");

  loadConfig();

  Tracker.begin(g_baud, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  gpio_set_pull_mode((gpio_num_t)UART_RX_PIN, GPIO_PULLUP_ONLY);

  delay(300);
  loadName();
  loadKey();
  loadState();

  NimBLEDevice::init(g_devName);
  NimBLEDevice::setMTU(247);
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  NimBLEService* svc = server->createService(NUS_SVC_UUID);
  NimBLECharacteristic* rx = svc->createCharacteristic(
      NUS_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RxCallbacks());
  gTxChar = svc->createCharacteristic(NUS_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  svc->start();
  configureAdv();

  evaluate();
  Serial.printf("[INIT] '%s' listo. profile=%s en=%d geoKnown=%d ign=%d geo=%d override=%d\n",
                g_devName, g_profile.c_str(), g_enabled, g_geoKnown, g_ignOn, g_inGeo, g_cutDisabled);
}

// ════════════════════════════════════════════════════════════════════
// LOOP
// ════════════════════════════════════════════════════════════════════
void loop() {
  pumpSerial();
  pumpAutoDetect();
  pumpKeepAlive();
  delay(5);
}

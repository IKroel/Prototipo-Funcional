/*
  Wisetrack ESP32 Gateway — V2 (corte por ignición/geocerca)
  ════════════════════════════════════════════════════════════════════
  FIRMWARE ÚNICO V2 — "SOLO SERIAL".
  El corte lo ejecuta el tracker (GV75CG por defecto) en su DOUT,
  comandado por serial.

  Lógica (decisión en el ESP):
    - CORTE      si ignición OFF Y fuera de geocerca.
    - Sin corte  si ignición ON  o dentro de geocerca.
    Al cambiar de estado, el ESP envía por serial al tracker:
    - La app solo DESHABILITA el corte (>DISABLECUT). Se re-arma tras un
      ciclo de ignición (arrancar y volver a apagar) o con >ARMCUT (Admin).
    - Un mensaje serial del tracker (re_enable_str) también deshabilita.
    - Al desconectar la app, se mantiene el último estado.
    - Se conserva la autenticación BLE: solo un cliente autenticado deshabilita.

  V2.8:
    1. Advertising anónimo: sin nombre ni scan response; solo Manufacturer
       Data 0xFFFF+W+T+0x01. Scanners genéricos ven "Unknown Device".
    3. Todos los parámetros viven en NVS (Preferences). Sin hardcode: los
       #define DEF_* son solo valores por defecto al primer arranque.
    4. Dispatcher con Source (BLE/SERIAL). Por serial solo se atiende una
       whitelist; el resto se ignora en silencio (el tracker no hace HMAC).
    6. Standby controlado por el tracker (WT_DISABLE / WT_ENABLE por serial).
       En standby se ignoran comandos de acción pero el keep-alive sigue.
    7. Keep-alive periódico al tracker con intervalo dual por ignición.
    8. Comando >GET_PROFILE: JSON con configuración + estado en vivo.
    5. AUTO_DETECT de perfil no bloqueante (FSM en loop()).

  Hardware:
    Serial2 (GPIO23 RX / GPIO22 TX) <-> tracker vía MAX3232 (RS-232<->TTL).

  Build — Flash Encryption Release Mode (antes del primer release a prod):
    Ver docs/README_BUILD.md. Activarlo es IRREVERSIBLE: perder el
    binario firmado inutiliza el chip. Tener el flujo de firma listo antes.

  Librería: NimBLE-Arduino (h2zero) v1.4.x
*/

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "esp_mac.h"
#include "esp_random.h"
#include <Preferences.h>
#include "mbedtls/md.h"
#include "driver/gpio.h"

// ════════════════════════════════════════════════════════════════════
// FIRMWARE ÚNICO V2 PRODUCTIVO
// Los comandos de diagnóstico (>TESTGPS / >SIM / >SET* / >RELAX* / >DUMP)
// vienen SIEMPRE compilados pero exigen sesión BLE autenticada. En la app
// solo se exponen al entrar como Admin con el PIN de debug (9999).
// Los flags de modo libre arrancan en false -> comportamiento productivo.
// ════════════════════════════════════════════════════════════════════
#define WT_DEBUG

#define FW_VERSION "2.9"

// ════════════════════════════════════════════════════════════════════
// HARDWARE
// ════════════════════════════════════════════════════════════════════
#define UART_TX_PIN    22
#define UART_RX_PIN    23
HardwareSerial &Tracker = Serial2;

// Esta variante NO usa salida local de corte. El corte lo ejecuta el
// tracker por su DOUT, comandado por serial (cmd_cut_on / cmd_cut_off).

// ════════════════════════════════════════════════════════════════════
// VALORES POR DEFECTO (solo se aplican al primer arranque; luego viven
// en NVS y se editan en runtime con los setters >SET* o desde la app).
// ════════════════════════════════════════════════════════════════════
#define DEF_BAUD        115200
#define DEF_PROFILE     "gv75cg"
#define DEF_IGN_ON      "IGN_ON"
#define DEF_IGN_OFF     "IGN_OFF"
#define DEF_GEO_IN      "ZonaSegura_ON"
#define DEF_GEO_OUT     "ZonaSegura_OFF"
#define DEF_REENABLE    "DISABLE_CUT"
#define DEF_CMD_CUT_ON  "AT+GTDOS=gv75cg,0,3,1,1,0,0,0,,2,0,0,0,0,,3,0,0,0,0,,0,0,5,,,,FFFF$"
#define DEF_CMD_CUT_OFF "AT+GTDOS=gv75cg,0,3,1,0,0,0,0,,2,0,0,0,0,,3,0,0,0,0,,0,0,5,,,,FFFF$"
#define DEF_CMD_ACK     "OK"
#define DEF_KA_ON       30     // segundos entre keep-alive con ignición ON
#define DEF_KA_OFF      300    // segundos entre keep-alive con ignición OFF

// Comandos de standby que el tracker envía por serial (no configurables).
#define MSG_STANDBY_OFF "WT_DISABLE"   // entra en standby
#define MSG_STANDBY_ON  "WT_ENABLE"    // sale de standby

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
String   g_cmdAck     = DEF_CMD_ACK;
String   g_ignOnStr   = DEF_IGN_ON;
String   g_ignOffStr  = DEF_IGN_OFF;
String   g_geoInStr   = DEF_GEO_IN;
String   g_geoOutStr  = DEF_GEO_OUT;
String   g_reEnableStr= DEF_REENABLE;
uint32_t g_kaOn       = DEF_KA_ON;
uint32_t g_kaOff      = DEF_KA_OFF;

// Estado del vehículo (último conocido por serial)
bool g_enabled     = true;    // standby: false = ignora comandos de acción
bool g_ignOn       = false;   // ignición
bool g_ignKnown    = false;   // ¿llegó IGN al menos una vez?
bool g_inGeo       = false;   // dentro de geocerca
bool g_geoKnown    = false;   // ¿llegó ZonaSegura al menos una vez?
bool g_cutDisabled = false;   // corte deshabilitado por app/serial (override)
bool g_tripStarted = false;   // para re-armar tras un ciclo de ignición
int  g_lastCut     = -1;      // último estado enviado: -1 desc, 0 sin corte, 1 corte

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
#endif

// ════════════════════════════════════════════════════════════════════
// AUTO_DETECT de perfil — FSM no bloqueante
// Cada perfil define un comando de identificación y un token esperado en
// la respuesta del tracker. TODO: completar idCmd/expect reales por
// modelo (Queclink/Teltonika/Syrus/TRAX). Aquí va el conocido (gv75cg).
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
  g_cmdAck      = g_prefs.getString("ack",    DEF_CMD_ACK);
  g_ignOnStr    = g_prefs.getString("ignOn",  DEF_IGN_ON);
  g_ignOffStr   = g_prefs.getString("ignOff", DEF_IGN_OFF);
  g_geoInStr    = g_prefs.getString("geoIn",  DEF_GEO_IN);
  g_geoOutStr   = g_prefs.getString("geoOut", DEF_GEO_OUT);
  g_reEnableStr = g_prefs.getString("reEn",   DEF_REENABLE);
  g_kaOn        = g_prefs.getUInt  ("kaOn",   DEF_KA_ON);
  g_kaOff       = g_prefs.getUInt  ("kaOff",  DEF_KA_OFF);
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
  j += "\"ack\":\"" + esc(g_cmdAck) + "\",";
  j += "\"ign_on\":\"" + esc(g_ignOnStr) + "\",";
  j += "\"ign_off\":\"" + esc(g_ignOffStr) + "\",";
  j += "\"geo_in\":\"" + esc(g_geoInStr) + "\",";
  j += "\"geo_out\":\"" + esc(g_geoOutStr) + "\",";
  j += "\"ka_on\":" + String(g_kaOn) + ",";
  j += "\"ka_off\":" + String(g_kaOff);
  j += "}";
  return j;
}

// JSON corto de latido (KA) — mensaje periódico por GTDAT.
static String buildKaJson() {
  uint32_t iv = g_ignOn ? g_kaOn : g_kaOff;
  String j = "{";
  j += "\"type\":\"ka\",";
  j += "\"mac\":\"" + getBleMacString() + "\",";
  j += "\"name\":\"" + String(g_devName) + "\",";
  j += "\"app_link\":" + String(gClientConnected ? "true" : "false") + ",";
  j += "\"gps_link\":" + String(g_serRxLines > 0 ? "true" : "false") + ",";
  j += "\"interval\":" + String(iv);
  j += "}";
  return j;
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
// STANDBY — controlado por el tracker (WT_DISABLE / WT_ENABLE por serial)
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
// GTDAT — envuelve cualquier JSON para que el GV75CG lo reenvíe a
//   Wisetrack. Se refleja por BLE SOLO en el monitor admin (SERMON on).
//   NOTA: el campo de datos de GTDAT es delimitado por comas y el JSON
//   las contiene. Validar contra el manual @Track del GV75CG (puede
//   requerir payload sin comas o con encoding).
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
// LATIDO PERIÓDICO (KA, V2.9) — intervalo dual según ignición.
//   Emite buildKaJson() por GTDAT cada ka_on / ka_off segundos
//   (0 = deshabilitado). El profile completo se envía bajo pedido (>REPORT).
// ════════════════════════════════════════════════════════════════════
static void pumpKeepAlive() {
  uint32_t secs = g_ignOn ? g_kaOn : g_kaOff;
  if (secs == 0) return;    // 0 = deshabilitado
  if ((millis() - g_lastKa) < (secs * 1000UL)) return;
  g_lastKa = millis();
  sendGtdat(buildKaJson());
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

static void handleInternal(String cmd, Source src);   // fwd

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

  // Comando estructurado por serial (>...): dispatcher con whitelist.
  if (line[0] == '>') { handleInternal(line, SRC_SERIAL); return; }

  // Standby controlado por el tracker.
  if (line.indexOf(MSG_STANDBY_OFF) >= 0) { enterStandby(); return; }
  if (line.indexOf(MSG_STANDBY_ON)  >= 0) { exitStandby();  return; }

  bool evChanged = false;
  if      (line.indexOf(g_ignOnStr)  >= 0) { setIgnition(true);  evChanged = true; }
  else if (line.indexOf(g_ignOffStr) >= 0) { setIgnition(false); evChanged = true; }

  if      (line.indexOf(g_geoInStr)  >= 0) { g_inGeo = true;  g_geoKnown = true; evaluate(); saveState(); evChanged = true; }
  else if (line.indexOf(g_geoOutStr) >= 0) { g_inGeo = false; g_geoKnown = true; evaluate(); saveState(); evChanged = true; }

  if (line.indexOf(g_reEnableStr) >= 0) { disableCut("serial"); evChanged = true; }

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
// DISPATCHER DE COMANDOS (app/tracker -> ESP)
//   src = SRC_BLE    -> set completo (con auth donde aplica)
//   src = SRC_SERIAL -> whitelist; el resto se ignora en silencio
// ════════════════════════════════════════════════════════════════════
static void handleInternal(String cmd, Source src) {
  cmd.trim();
  if (cmd.length() == 0) return;
  Serial.printf("[RX %s] %s\n", src == SRC_BLE ? "APP" : "SER", cmd.c_str());
  cmd.remove(0, 1); cmd.trim();   // quita '>'

  // Por serial el tracker no puede generar HMAC: solo lectura no sensible.
  if (src == SRC_SERIAL) {
    if (cmd.equalsIgnoreCase("GET_PROFILE")) { reply(String("<PROFILE ") + buildProfileJson() + "\n"); return; }
    if (cmd.equalsIgnoreCase("STATUS"))      { sendStatusBle(); return; }
    if (cmd.equalsIgnoreCase("VERSION"))     { reply(String("<VERSION fw=") + FW_VERSION + " mac=" + getBleMacString() + "\n"); return; }
    return;   // cualquier otro comando por serial se ignora en silencio
  }

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
    reply("<UNPROVISION_OK\n");

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

  } else if (cmd.startsWith("SET_KAON ")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    g_kaOn = (uint32_t)cmd.substring(9).toInt(); setCfgU32("kaOn", g_kaOn);
    reply(String("<OK ka_on=") + g_kaOn + "\n");

  } else if (cmd.startsWith("SET_KAOFF ")) {
    if (!g_authed) { reply("<ERR not_authed\n"); return; }
    g_kaOff = (uint32_t)cmd.substring(10).toInt(); setCfgU32("kaOff", g_kaOff);
    reply(String("<OK ka_off=") + g_kaOff + "\n");

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
    if (!v.empty() && v[0] == '>') handleInternal(String(v.c_str()), SRC_BLE);
  }
};
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    gClientConnected = true; Serial.println("[BLE] Conectado");
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    gClientConnected = false; g_authed = false;
    Serial.println("[BLE] Desconectado (se mantiene el estado del corte)");
    NimBLEDevice::startAdvertising();
  }
};

// ════════════════════════════════════════════════════════════════════
// SETUP
// ══════════════════════════════════════════════
// Wisetrack ESP32 — TEST de relé (GPIO23) por BLE, sin auth ni FMC130.
// Comandos y cableado: ver firmware/README.md.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "esp_mac.h"

// Módulo relé active-LOW: LOW energiza el relé (libre), HIGH lo desenergiza (corte)
#define RELAY_PIN      23
#define RELAY_LOCKED   HIGH    // corte activo (estado seguro / boot)
#define RELAY_UNLOCKED LOW     // combustible libre

#define NUS_SVC_UUID  "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX_UUID   "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX_UUID   "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

NimBLECharacteristic* gTxChar          = nullptr;
bool                  gClientConnected = false;
char                  gName[20]        = {0};

static void reply(const String& s) {
  if (!gClientConnected || !gTxChar) return;
  gTxChar->setValue((const uint8_t*)s.c_str(), s.length());
  gTxChar->notify();
}

static void setRelay(int level) {
  digitalWrite(RELAY_PIN, level);
  int free = (level == RELAY_UNLOCKED) ? 1 : 0;   // 1 = libre, 0 = corte
  reply(String("<RELAY ") + free + "\n");
  Serial.printf("[RELAY] GPIO23 = %s → %s\n",
                level == HIGH ? "HIGH" : "LOW", free ? "libre" : "corte");
}

static void handleCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  if (cmd[0] == '>') { cmd.remove(0, 1); cmd.trim(); }

  if      (cmd.equalsIgnoreCase("ON"))     setRelay(RELAY_UNLOCKED);
  else if (cmd.equalsIgnoreCase("OFF"))    setRelay(RELAY_LOCKED);
  else if (cmd.equalsIgnoreCase("TOGGLE")) setRelay(digitalRead(RELAY_PIN) == HIGH ? LOW : HIGH);
  else if (cmd.equalsIgnoreCase("STATUS")) reply(String("<RELAY ") + (digitalRead(RELAY_PIN) == RELAY_UNLOCKED ? 1 : 0) + "\n");
  else if (cmd.equalsIgnoreCase("PING"))   reply("<PONG\n");
  else                                     reply("<ERR unknown_cmd\n");
}

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* chr, NimBLEConnInfo& info) override {
    std::string v = chr->getValue();
    if (!v.empty()) handleCommand(String(v.c_str()));
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    gClientConnected = true; Serial.println("[BLE] Conectado");
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    gClientConnected = false; Serial.println("[BLE] Desconectado");
    NimBLEDevice::startAdvertising();
  }
};

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nWisetrack ESP32 — TEST de relé");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_UNLOCKED); // arranca en LOW = libre (solicitado)

  uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_BT);
  snprintf(gName, sizeof(gName), "WT-TEST-%02X%02X", mac[4], mac[5]);

  NimBLEDevice::init(gName);
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* svc = server->createService(NUS_SVC_UUID);
  NimBLECharacteristic* rx = svc->createCharacteristic(
      NUS_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RxCallbacks());
  gTxChar = svc->createCharacteristic(NUS_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  svc->start();

  // UUID del servicio en el paquete principal; nombre en el scan response
  // (evita desbordar 31 bytes con el UUID de 128 bits + nombre).
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);
  advData.setCompleteServices(NimBLEUUID(NUS_SVC_UUID));
  adv->setAdvertisementData(advData);
  NimBLEAdvertisementData scanData;
  scanData.setName(std::string(gName));
  adv->setScanResponseData(scanData);
  adv->start();

  Serial.printf("[INIT] Advertising como '%s' | GPIO23 = HIGH (corte)\n", gName);
}

void loop() {
  delay(20);
}

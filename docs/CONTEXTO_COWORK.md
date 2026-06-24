# Wisetrack — Llave Virtual ESP32 + FMC130
## Contexto para continuar en Cowork

---

## Arquitectura hardware

```
ESP32 GPIO 23 → módulo relé → FMC130 DIN2 (pulso negativo a GND vehículo)
FMC130 DOUT1  → relé corte combustible (directo, sin pasar por ESP32)

GPIO 23 HIGH = relé cerrado = DIN2 FMC130 con señal = combustible LIBRE
GPIO 23 LOW  = relé abierto = DIN2 FMC130 sin señal = CORTE activo

Serial2 (GPIO 16/17) ↔ FMC130 (forward transparente por BLE)
```

---

## Firmware ESP32 — wt_gateway.ino

**Estado:** completo y limpio.

**Comandos clave:**
- `>MAC` → `<MAC XX:XX:XX:XX:XX:XX`
- `>PROVISION <master_hex_64>` → deriva device_key, descarta master
- `>CHALLENGE` → `<CHALLENGE <nonce_hex_16>`
- `>AUTH <token_hex_64>` → `<AUTH_OK` | `<AUTH_FAIL <razón>`
- `>HEARTBEAT` → `<HB_OK` (cada 5s mientras sesión activa)
- `>LOCK` → bloqueo inmediato
- `>STATUS` → `<STATUS name=... session=... relay=<0|1>`
- `<RELAY_CHANGED <0|1>` → notificación automática si FMC130 cambia relé

**Derivación de clave:**
```
device_key = HMAC-SHA256(MASTER_SECRET, MAC_BLE_bytes[6])
token_auth = HMAC-SHA256(device_key, nonce_bytes[8])
```

**Sesión:** LOCKED → (AUTH) → AUTHORIZED → (LOCK/timeout/disconnect) → LOCKED
Sin PENDING_LOCK — bloqueo siempre inmediato.

**NVS keys:** `name`, `authkey` (device_key 32 bytes), `hbtimeout`, `r23`

**Librería:** NimBLE-Arduino (h2zero) v1.4.x

---

## App Flutter — wt_app/

**Estado:** prototipo sin servidor, sin control de roles (todos los roles activos).

**Estructura:**
```
lib/
  config.dart              → kMasterSecret (REEMPLAZAR), UUIDs NUS, constantes
  models/device_state.dart → DeviceState, SessionState, RelayState
  services/
    crypto_service.dart    → deriveDeviceKey(), signNonce()
    ble_service.dart       → BleService (ChangeNotifier) — toda la lógica BLE
  screens/
    scanner_screen.dart    → escaneo BLE filtrado por manufacturer data WT
    device_screen.dart     → control del vehículo, log BLE
    provision_screen.dart  → provisionamiento del ESP32
```

**Dependencias pubspec.yaml:**
- `flutter_blue_plus: ^1.31.0`
- `crypto: ^3.0.3`
- `convert: ^3.1.1`
- `permission_handler: ^11.3.0`
- `provider: ^6.1.1`

**Filtro BLE:** manufacturer data `0xFFFF` + bytes `[0x57, 0x54, 0x01]` ('W','T',v1)
o fallback nombre que empiece con "WT-".

---

## Custom Scenario FMC130 (pendiente configurar)

```
Scenario 1:
  Trigger #1: Ignition    = Is, Low=0, High=0  (ignición OFF)
  Trigger #2: Geofence Z1 = Is, Low=0, High=0  (fuera de zona segura)
  Trigger #3: DIN2        = Is, Low=0, High=0  (ESP32 no autoriza)
  Logic: AND
  Output: DOUT1 ON  (corte activo)
  Permanent Output Control: ON
```

---

## Pendiente

### App:
- [ ] Roles: admin / operador / instalador (actualmente todos pueden todo)
- [ ] Login con servidor + JWT + vault (MASTER_SECRET cifrado)
- [ ] Lista de vehículos asignados por usuario (allowlist)
- [ ] Búsqueda de patentes con normalización (mayús/minus, guiones)
- [ ] Refresh de allowlist en foreground (ETag)
- [ ] Cache offline para operación sin red

### Servidor:
- [ ] Endpoints REST: POST /auth/login, GET /vehicles, POST /vehicles/{mac}/token
- [ ] Derivación de device_key en servidor: HMAC(MASTER_SECRET, MAC)
- [ ] Firma de nonce en servidor: HMAC(device_key, nonce)
- [ ] JWT con vault (MASTER_SECRET cifrado con session_key)

### Base de datos:
- [ ] Tablas: vehicle, app_user, user_vehicle, avl_event_raw, avl_event
- [ ] Columna fleet_code_normalized (GENERATED) para búsqueda insensible a formato
- [ ] Tabla device_io_map para semántica de AVL IDs por instalación

### Provisioning:
- [ ] Procedimiento para 6 dispositivos (flash → nombre → PROVISION)
- [ ] Registro en tabla device con MAC + device_key derivada

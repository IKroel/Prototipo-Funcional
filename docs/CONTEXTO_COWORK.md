# Contexto del proyecto — Llave Virtual BLE (accesorio ESP32 + GPS)

> Documento único de contexto. Detalla **qué hace el script (firmware)** y **qué
> hace la app**. Para compilar/provisionar ver `docs/README_BUILD.md`; para el
> índice de versiones de firmware ver `firmware/README.md`.

---

## 1. Resumen del sistema

Un módulo **ESP32** se conecta al equipo **GPS (GV75CG)** como **accesorio**. El
corte de combustible y las geocercas los ejecuta el GPS; el ESP32 solo **decide
cuándo debe estar activo el corte** (según ignición y geocerca) y se lo ordena al
GPS por **serial** (comando AT). Una **app móvil** identifica el equipo por
Bluetooth, se autentica y puede **deshabilitar** el corte para permitir el uso.

```
[App movil] --BLE (NUS)--> [ESP32] --Serial2 + MAX3232--> [GPS GV75CG] --> corte (DOUT)
```

---

## 2. Hardware

- **ESP32** con firmware `wt_gateway_v2_serial` (fw 2.8).
- **MAX3232** (conversor RS-232 <-> TTL) entre ESP32 y GPS.
- **Fuente OKI** (DC-DC) para alimentación.
- Serial2 del ESP: **GPIO23 = RX, GPIO22 = TX**, 115200 8N1, pull-up en RX.
- El ESP32 **no** usa salida local de corte (sin GPIO21/GPIO23 como salida).

---

## 3. El script (firmware `wt_gateway_v2_serial.ino`, fw 2.8)

### 3.1 Arranque (`setup`)
Carga desde NVS la configuración y el último estado; abre Serial2 al baud
configurado; inicializa el BLE (NimBLE, MTU 247, servicio NUS); publica el
*advertising anónimo*; y re-aplica el estado de corte según lo persistido.

### 3.2 Lógica de corte (autónoma, en el ESP)
- **Corta** si `ignición OFF` **y** `fuera de geocerca`.
- **No corta** si `ignición ON` **o** `dentro de geocerca`.
- Mientras no se conozca la geocerca (`geoKnown=false`) -> no corta (queda libre).
- El corte se ejecuta **solo en el cambio** de estado, enviando al GPS el comando
  AT configurable `cmd_cut_on` / `cmd_cut_off` (por defecto `AT+GTDOS=...` GV75CG).
- La app solo **deshabilita** (`>DISABLECUT`). Se **re-arma** solo tras un ciclo
  de ignición (arrancar y volver a apagar) o con `>ARMCUT` (Admin).
- Un mensaje serial del GPS (`re_enable_str`, por defecto `DISABLE_CUT`) también
  deshabilita.
- **Sin heartbeat**: al desconectar la app, el ESP mantiene el último estado.
- Estado (ignición / geocerca / override / viaje) **persiste en NVS**.

### 3.3 Lectura del GPS (parser serial)
Lee líneas de Serial2 y busca tokens configurables: `IGN_ON`/`IGN_OFF`,
`ZonaSegura_ON`/`ZonaSegura_OFF`, y `DISABLE_CUT`. Ante un evento reconocido
recalcula el corte y notifica el estado por BLE. Las líneas no imprimibles (ruido
del bus) se descartan.

### 3.4 Standby (controlado por el GPS)
Comandos por serial `WT_DISABLE` / `WT_ENABLE`. En standby el ESP **ignora las
acciones de corte** (responde `<ERR device_disabled` por BLE) pero el keep-alive
sigue. El estado `enabled` persiste en NVS.

### 3.5 Keep-alive
Envía periódicamente al GPS `WT_ALIVE,<mac>,<fw>,<relay>,<profile>` para que el
GPS lo reenvíe a plataforma. Intervalo dual: `ka_on` (ignición ON, def. 30 s) /
`ka_off` (OFF, def. 300 s).

### 3.6 AUTO_DETECT de perfil (no bloqueante)
FSM en `loop()` que prueba comandos de identificación por modelo y fija el perfil
detectado. **Pendiente**: hoy solo trae el perfil real `gv75cg`; faltan los
comandos de identificación de otros modelos.

### 3.7 Configuración en NVS
Todos los parámetros viven en NVS con defaults (`DEF_*`) y se editan en runtime:
baud, perfil, `cmd_cut_on/off`, ack, strings de ignición/geocerca, intervalos
keep-alive, y `enabled`. Sin valores hardcodeados de operación.

### 3.8 Seguridad
- **Advertising anónimo**: no difunde nombre ni datos identificables; solo la app
  lo reconoce por Manufacturer Data `0xFFFF`+`W`+`T`+`0x01`. Un scanner genérico
  lo ve como "Unknown Device". La patente NO viaja en claro.
- **Autenticación BLE** challenge-response: `device_key = HMAC-SHA256(master, MAC)`;
  token = primeros 16 bytes de `HMAC-SHA256(device_key, nonce)`. Sin auth, las
  acciones se rechazan.
- **Dispatcher por canal**: por serial solo se atiende una whitelist
  (`GET_PROFILE`, `STATUS`, `VERSION`); el resto se ignora en silencio (el GPS no
  puede generar HMAC).
- **Flash Encryption (Release Mode)**: diseñado, **pendiente de activar** antes de
  producción (irreversible; ver `docs/README_BUILD.md`).

### 3.9 Comandos y notificaciones (protocolo BLE — Nordic UART Service)
UUIDs: svc `6E400001...`, RX(write) `6E400002...`, TX(notify) `6E400003...`.

App->ESP (productivos): `>PING`, `>VERSION`, `>GET_PROFILE`, `>NAME [txt]`,
`>STATUS`, `>MAC`, `>PROVISION <hex64>`, `>UNPROVISION`, `>CHALLENGE`,
`>AUTH <hex32>`, `>DISABLECUT`, `>ARMCUT`, `>AUTO_DETECT`, `>SET_*` (config),
`>SERSTATS`, `>SERMON <0|1>`.

App->ESP (debug, exigen auth): `>TESTGPS ON|OFF`, `>SIM <token>`,
`>SETIGN/SETGEO/SETGEOKNOWN/SETOVR`, `>RELAXGEO/ALWAYSSEND/IGNOVR`, `>SETEN`, `>DUMP`.

ESP->App: `<STATUS ...`, `<VERSION fw=2.8 ...`, `<PROFILE {json}`,
`<PROFILE_DETECTED <name>`, `<CHALLENGE`, `<AUTH_OK`/`<AUTH_FAIL`, `<PROVISION_OK`,
`<UNPROVISION_OK`, `<CUT_DISABLED`/`<CUT_ARMED`, `<TXGPS <cmd>`, `<DUMP ...`,
`<SER`/`<SERHEX` (si SERMON), `<ERR ...` (p.ej. `device_disabled`, `unknown_cmd`).

---

## 4. La app (`wt_gateway_app/`, prototipo Flutter)

Local, sin servidor. La versión productiva la desarrollará el equipo de Desarrollo.

### 4.1 Login, roles y permisos
- Login por **usuario + PIN** (el PIN se guarda hasheado con SHA-256 salteado por
  id; nunca en claro).
- Roles (`app_user.dart`): **Operador**, **Instalador**, **Administrador**.
  - Operador: solo ve/opera las patentes asignadas y cercanas.
  - Instalador: puede provisionar, renombrar/ícono, ver consola BLE, ve todos.
  - Administrador: además gestiona usuarios, ajustes y puede re-armar el corte.
- **PIN 9999 en Admin**: entra como Admin y **desbloquea el modo debug / Banco de
  pruebas**. Con el PIN normal, la sesión es productiva (sin debug).

### 4.2 Pantallas y navegación
Barra inferior con **Vehículos** (escáner) y **Usuario** (perfil/ajustes).
- **Escáner**: busca equipos por BLE filtrando por Manufacturer Data WT; el
  operador solo ve sus patentes asignadas que estén en rango (proximidad).
- **Detalle del vehículo**: muestra estado (ignición / geocerca / corte /
  override); botón "Deshabilitar corte" (requiere auth) y "Armar corte" (Admin);
  consola de log BLE (instalador/admin); botón Banco de pruebas si hay debug.
- **Provisionar**: deriva la `device_key` en el equipo y lo deja bloqueado.
- **Perfil**: gestión de usuarios (admin), tema claro/oscuro, acceso a debug.

### 4.3 Servicio BLE (`ble_service.dart`)
- Al conectar: descubre el NUS, activa notificaciones, pide `>STATUS` y `>VERSION`
  y hace **autenticación silenciosa** (`>CHALLENGE` -> `>AUTH`).
- **Reintentos**: hasta 3 de auth (timeout 5 s) y hasta 3 de reconexión automática
  si la caída no fue pedida por el usuario.
- Acción principal V2: `>DISABLECUT` (y `>ARMCUT` para Admin).
- Escribe con confirmación (Write Request) para no perder comandos.
- Parsea `<STATUS`/`<DUMP` para reflejar el estado; `<DUMP` no se loguea (es
  frecuente por el sondeo del bench).

### 4.4 Banco de pruebas (solo debug)
Estado en vivo (lee `<DUMP` con sondeo cada ~2 s), toggles de "modo libre"
(relajar geocerca / reenviar siempre / ignorar override), setters de estado, y
envío directo (`TESTGPS`, `SIM`, `DUMP`, comando libre).

### 4.5 Seguridad de la app (limitación conocida)
`kMasterSecret` está **embebido en el binario** (`config.dart`). Quien decompile
el APK obtiene el master y puede derivar la `device_key` de cualquier equipo. Es
aceptable solo para prototipo/demo. **Producción (pendiente)**: el master no debe
compilarse; debe entregarlo el servidor por sesión (login -> JWT -> vault) o mover
la firma del nonce al servidor.

---

## 5. Alineación app <-> firmware (importante)

El **firmware ya está en V2.8**, pero la **app prototipo aún usa un subconjunto
del protocolo** (no consume todavía las funciones nuevas). Al reconstruir la app,
Desarrollo debe incorporar:

- `>GET_PROFILE` (leer configuración/estado en JSON) en vez de solo `<STATUS`.
- `>SET_*` para configurar el equipo desde la app.
- `>AUTO_DETECT` y el manejo de `<PROFILE_DETECTED`.
- Manejo de standby (`<ERR device_disabled`).

---

## 6. Estructura del repositorio

```
Prototipo Funcional/
├── README.md                      Índice del proyecto y estado
├── docs/
│   ├── CONTEXTO_COWORK.md         Este documento
│   ├── README_BUILD.md            Build del APK + Flash Encryption + provisioning
│   └── resumen_firmware_v2.docx   Documento de diseño V2 (referencia)
├── firmware/
│   ├── README.md                  Índice de versiones de firmware
│   ├── v1/                        Legacy (corte por GPIO23/relé; NO usar)
│   └── v2/wt_gateway_v2_serial/   Firmware actual (fw 2.8, solo serial, sin IO21)
└── wt_gateway_app/                App Flutter (prototipo)
```

---

## 7. Pendientes

- Pruebas en terreno junto con el señuelo.
- Homologación del GV75CG.
- Activar Flash Encryption (Release Mode) antes de producción.
- Completar los perfiles de AUTO_DETECT (comandos de identificación por modelo).
- App productiva (Desarrollo): master fuera del binario, roles con servidor/JWT,
  allowlist de patentes, cache offline, backoffice, y consumo del protocolo V2.8.

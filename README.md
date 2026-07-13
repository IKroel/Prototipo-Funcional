<div align="center">

# 🔐 Llave Virtual BLE

### Accesorio ESP32 + GPS — Corte de combustible controlado por Bluetooth

**Wisetrack**

<br/>

![Firmware](https://img.shields.io/badge/firmware-v2.9-blue?style=for-the-badge)
![Plataforma](https://img.shields.io/badge/ESP32-NimBLE-informational?style=for-the-badge&logo=espressif)
![GPS](https://img.shields.io/badge/GPS-GV75CG-success?style=for-the-badge)

</div>

<br/>

<div align="center">

```mermaid
flowchart LR
    A["📱 App móvil"] -- BLE / NUS --> B["🔧 ESP32"]
    B -- Serial2 + MAX3232 --> C["🛰️ GPS GV75CG"]
    C -- DOUT --> D["⛔ Corte de combustible"]
```

</div>

Un módulo **ESP32** actúa como accesorio del equipo **GPS (GV75CG)** para
habilitar/inhabilitar el **corte de combustible** según ignición y geocerca,
controlado desde una **app móvil** por Bluetooth. El corte y las geocercas viven
en el GPS; el ESP32 solo **decide cuándo debe estar activo el corte** y se lo
ordena al tracker por **serial** (comando AT). La app identifica el equipo por
BLE, se autentica y puede **deshabilitar** el corte.

> [!NOTE]
> Este es un **prototipo**, se entregaran las indicaciones y sugerencias para poder integrar una app movil en relacion a la funcionalidad del firmware y hardware.

---

## 📑 Índice

| # | Sección | # | Sección |
|---|---------|---|---------|
| 1 | [Estado actual](#-1--estado-actual) | 8 | [Comunicación serial ESP32 ↔ GPS](#-8--comunicación-serial-esp32--gps) |
| 2 | [Estructura del repositorio](#-2--estructura-del-repositorio) | 9 | [Configuración en NVS](#-9--configuración-en-nvs) |
| 3 | [Hardware](#-3--hardware) | 10 | [Seguridad](#-10--seguridad) |
| 4 | [Lógica de corte](#-4--lógica-de-corte) | 11 | [App móvil (Flutter)](#-11--app-móvil-flutter) |
| 5 | [Protocolo BLE](#-5--protocolo-ble--nordic-uart-service) | 12 | [Temas sugeridos](#-12--temas-sugeridos-firmware--app) |
| 6 | [Comandos App → ESP32](#-6--comandos-app--esp32) | 13 | [Por dónde empezar](#-13--por-dónde-empezar) |
| 7 | [Respuestas ESP32 → App](#-7--respuestas--notificaciones-esp32--app) | | |

---

## ✅ 1 · Estado actual

| | Ítem |
|:--:|------|
| ✅ | Prototipo (Modulo ESP32 + Conversor Serial + GPS + App Movil). |
| ✅ | Compatibilidad con GPS: **GV75CG**. |
| ✅ | Firmware **V2.9**. |
| ⏳ | **Pendiente:** Flash Encryption y App Productiva |

---

## 🗂️ 2 · Estructura del repositorio

```
Prototipo Funcional/
├── README.md                      
├── docs/
│   ├── CONTEXTO_COWORK.md         Arquitectura y Protocolo
│   ├── README_BUILD.md            Build del APK (Solo Testing Local, para Pruebas de Comunicacion)
├── firmware/
│   ├── README.md                  Índice de versiones de firmware
│   ├── v1/                        Legacy (NO usar)
│   └── v2/wt_gateway_v2_serial/   ★ Firmware actual
└── wt_gateway_app/                App Flutter (Solo Testing Local, para Pruebas de Comunicacion)
```

---

## 🔌 3 · Hardware

| Componente | Detalle |
|------------|---------|
| 🔧 **ESP32** | Corre el firmware `wt_gateway_v2_serial` (fw 2.9). |
| 🛰️ **GPS GV75CG** | Ejecuta el corte por su salida digital; recibe comandos AT por serial. |
| 🔀 **MAX3232** | Conversor RS-232 ↔ TTL entre ESP32 y GPS. |
| 🔋 **Fuente OKI** | DC-DC para alimentación. |

> [!IMPORTANT]
> **Serial del ESP32:** `GPIO23 = RX`, `GPIO22 = TX`.
> **Velocidad 115200 baud**
---

## ⛔ 4 · Lógica de corte

En el ESP32:

| Ignición | Geocerca | Resultado |
|:--------:|:--------:|-----------|
| OFF | Fuera | 🔴 **Corta** (bloquea) |
| ON | — | 🟢 No corta |
| OFF | Dentro | 🟢 No corta |

- El corte se ejecuta **solo al cambiar de estado de la Ignicion**.
- La app solo **deshabilita el corte** (`>DISABLECUT`).
- El estado de la ESP32 (ignición / geocerca / override / viaje / `enabled`) **persiste en NVS**.

---

## 📡 5 · Protocolo BLE — Nordic UART Service

Servicio NUS con MTU 247.

| Rol | UUID |
|-----|------|
| Servicio | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX (App escribe) | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` |
| TX (ESP notifica) | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` |

Los comandos de la app empiezan con `>` y las respuestas del ESP con `<`. La app
escribe con **Write Request**. Las acciones sensibles exigen sesión
autenticada (`<ERR not_authed` si falta) — ver [§10 · Seguridad](#-10--seguridad).

---

## 📤 6 · Comandos App → ESP32

<details open>
<summary><b>Productivos — sin auth previa</b></summary>

| Comando | Función |
|---------|---------|
| `>PING` | Ping de conectividad → `<PONG`. |
| `>VERSION` | Versión de firmware y MAC. |
| `>GET_PROFILE` | Configuración + estado en vivo (JSON). |
| `>STATUS` | Estado resumido. |
| `>MAC` | MAC BLE del equipo. |
| `>NAME [txt]` | Lee (`>NAME`) o fija (`>NAME xxx`) el nombre interno. |
| `>SERSTATS` | Estadísticas del bus serial (bytes RX, líneas). |
| `>SERMON <0\|1>` | Activa/desactiva el monitor serial por BLE. |
| `>PROVISION <hex64>` | Provisiona: deriva la `device_key` desde el master. |
| `>CHALLENGE` | Solicita nonce para autenticarse. |
| `>AUTH <hex32>` | Envía el token calculado sobre el nonce. |

</details>

<details open>
<summary><b>Productivos — exigen auth</b> 🔒</summary>

| Comando | Función |
|---------|---------|
| `>DISABLECUT` | Deshabilita el corte (acción principal de la app). |
| `>ARMCUT` | Re-arma el corte (solo Admin). |
| `>REPORT` | Empuja el `profile` completo a Wisetrack por GTDAT (bajo pedido). |
| `>UNPROVISION` | Borra la `device_key` (vuelve a estado sin provisionar). |
| `>AUTO_DETECT` | Lanza la detección de perfil del tracker. |
| `>SET_PROFILE <p>` | Fija el perfil (p. ej. `gv75cg`). |
| `>SET_CUTON <at>` / `>SET_CUTOFF <at>` | Comandos AT de corte on/off. |
| `>SET_IGNON <s>` / `>SET_IGNOFF <s>` | Tokens serial de ignición. |
| `>SET_GEOIN <s>` / `>SET_GEOOUT <s>` | Tokens serial de geocerca. |
| `>SET_KAON <s>` / `>SET_KAOFF <s>` | Intervalos del latido KA (segundos, según ignición). |

</details>

<details>
<summary><b>Debug / Banco de pruebas</b> 🧪 <i>(exigen auth; se exponen solo con PIN 9999)</i></summary>

| Comando | Función |
|---------|---------|
| `>TESTGPS ON\|OFF` | Modo prueba de GPS. |
| `>SIM <token>` | Inyecta un token serial simulado (IGN/geocerca/etc.). |
| `>DUMP` | Vuelca el estado completo (sondeado por el bench). |
| `>SETIGN` / `>SETGEO` / `>SETGEOKNOWN` / `>SETOVR` | Fija estados manualmente. |
| `>SETEN` | Fija el flag `enabled` (standby). |
| `>RELAXGEO` / `>ALWAYSSEND` / `>IGNOVR` | Flags de "modo libre". |

</details>

---

## 📥 7 · Respuestas / notificaciones ESP32 → App

| Mensaje | Significado |
|---------|-------------|
| `<PONG` | Respuesta a `>PING`. |
| `<VERSION fw=2.9 mac=…` | Versión y MAC. |
| `<PROFILE {json}` | Configuración + estado (respuesta a `>GET_PROFILE`). |
| `<PROFILE_DETECTED <name>` | Perfil detectado por AUTO_DETECT. |
| `<AUTO_DETECT start\|none` | Progreso/resultado de la detección. |
| `<STATUS name=… en=…` | Estado resumido. |
| `<DUMP …` | Estado completo (no se loguea; frecuente por el sondeo). |
| `<CHALLENGE <nonce>` | Nonce para autenticación. |
| `<AUTH_OK` / `<AUTH_FAIL <motivo>` | Resultado de auth (`no_key`, `no_challenge`, `bad_format`, `wrong_token`, `internal_error`). |
| `<PROVISION_OK mac=…` / `<UNPROVISION_OK` | Provisioning. |
| `<CUT_DISABLED` / `<CUT_ARMED` | Corte deshabilitado / re-armado. |
| `<TXGPS <cmd>` | Eco del comando AT enviado al GPS. |
| `<NAME …` / `<OK …` | Confirmaciones de setters. |
| `<SER …` / `<SERHEX …` | Líneas del bus serial (si SERMON activo). |
| `<ERR <motivo>` | Error: `not_authed`, `device_disabled`, `unknown_cmd`, `bad_name`, `no_key_set`, `already_provisioned`, `bad_master_format`, `derivation_failed`, `no_profiles`. |

---

## 🔁 8 · Comunicación serial ESP32 ↔ GPS

**El ESP escucha del GPS** (tokens configurables):

| Token (default) | Evento |
|-----------------|--------|
| `IGN_ON` / `IGN_OFF` | Ignición encendida / apagada. |
| `ZonaSegura_ON` / `ZonaSegura_OFF` | Dentro / fuera de geocerca. |
| `DISABLE_CUT` | La ESP32 pide deshabilitar el corte. |
| `WT_DISABLE` / `WT_ENABLE` | Standby: apaga / reactiva el gateway. |

**El ESP envía al GPS:**

- ⛔ **Corte:** `cmd_cut_on` / `cmd_cut_off` (AT, por defecto `AT+GTDOS=gv75cg,…`).
- 💓 **Latido KA (V2.9):** JSON de salud envuelto en `AT+GTDAT=gv75cg,2,,<json>,0,,,,FFFF$`
  para que el GPS lo reenvíe a plataforma. Intervalo dual: `ka_on` (ign ON, def. 30 s) /
  `ka_off` (OFF, def. 300 s). Payload:
  `{"type":"ka","mac":…,"name":…,"app_link":bool,"gps_link":bool,"interval":s}`.
- 📋 **Profile bajo pedido:** con `>REPORT` se empuja el JSON completo (`type:"profile"`)
  por el mismo GTDAT. El campo `type` distingue latido vs profile en plataforma.
- ⚠️ **Pendiente:** el JSON tiene comas y el campo de datos de GTDAT es delimitado por
  comas — validar contra el manual @Track del GV75CG (puede requerir payload sin comas).

---

## 💾 9 · Configuración en NVS

Todos los parámetros de operación viven en NVS con valores por
defecto (`DEF_*`) que solo aplican al primer arranque; luego se editan en runtime
con los setters `>SET_*` o desde la app.

> Parámetros: `baud`, `profile`, `cmd_cut_on/off`, `cmd_ack`, tokens de
> ignición/geocerca, `re_enable_str`, intervalos `ka_on`/`ka_off`, `enabled`.

---

## 🛡️ 10 · Seguridad

- 🕵️ **Advertising anónimo:** no difunde nombre ni datos identificables; solo la
  app lo reconoce por Manufacturer Data `0xFFFF`+`W`+`T`+`0x01`. Un scanner
  genérico lo ve como "Unknown Device". La patente **no** viaja en claro.
- 🔑 **Autenticación BLE challenge-response:**
  `device_key = HMAC-SHA256(master, MAC)`; token = primeros 16 bytes de
  `HMAC-SHA256(device_key, nonce)`. Sin auth, las acciones se rechazan.
- 🚧 **Dispatcher por canal:** por serial solo se atiende una whitelist
  (`GET_PROFILE`, `STATUS`, `VERSION`); el resto se ignora en silencio.
- 🔒 **Flash Encryption (Pendiente):** (irreversible; ver `docs/README_BUILD.md`).

> [!WARNING]
> **Limitación conocida (prototipo):** `kMasterSecret` está embebido en el binario
> de la app (`wt_gateway_app/lib/config.dart`). Quien decompile el APK obtiene el
> master. Aceptable solo para prototipo/demo; en producción el master **no** debe
> compilarse en el binario.

---

## 📱 11 · App móvil (Flutter)

Local, sin servidor. Detalle completo en `docs/CONTEXTO_COWORK.md`.

- 🔐 **Login** por usuario + PIN (PIN hasheado con SHA-256 salteado; nunca en claro).
- 👥 **Roles:** **Operador** (solo sus patentes en rango), **Instalador**
  (provisiona, renombra, ve consola BLE, ve todos), **Administrador** (además
  gestiona usuarios/ajustes y re-arma el corte).
- 🧪 **PIN 9999 en Admin:** desbloquea el **modo debug / Banco de pruebas**. Con el
  PIN normal la sesión es productiva (sin debug).
- 🖥️ **Pantallas:** escáner de vehículos (filtra por Manufacturer Data WT y
  proximidad), detalle del vehículo (estado + "Deshabilitar/Armar corte" +
  consola BLE), provisionar, perfil/ajustes.
- 📶 **Servicio BLE:** al conectar descubre el NUS, activa notificaciones, pide
  `>STATUS`/`>VERSION` y hace **auth silenciosa**; hasta 3 reintentos de auth y de
  reconexión automática.

---

## 🚀 12 · Temas sugeridos (firmware + app)

<table>
<tr><th>📱 App productiva</th><th>🔧 Firmware</th></tr>
<tr valign="top"><td>

- Consumir `>GET_PROFILE` (JSON) en vez de solo `<STATUS`.
- Usar los setters `>SET_*` para configurar desde la app.
- Manejar `>AUTO_DETECT` y `<PROFILE_DETECTED`.
- Manejar standby (`<ERR device_disabled`).
- **Master fuera del binario** (servidor por sesión: login → JWT → vault).
- Roles con servidor/JWT, allowlist de patentes, cache offline, backoffice.

</td><td>

- Completar los perfiles de **AUTO_DETECT** (hoy solo `gv75cg`).
- Activar **Flash Encryption (Release Mode)**; evaluar **NVS encryption** + **Secure Boot V2**.
- Prueba
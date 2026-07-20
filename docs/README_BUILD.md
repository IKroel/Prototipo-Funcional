# WT Gateway — Build y seguridad del firmware ESP32

Compilación, provisioning y endurecimiento del firmware del accesorio ESP32. La
app móvil de referencia se mantiene en un repositorio privado aparte y no forma
parte de este repo.

## Requisitos

- Arduino IDE (o arduino-cli) con el core **arduino-esp32**, o ESP-IDF.
- Librería **NimBLE-Arduino (h2zero) v1.4.x**.
- Hardware: ESP32, **MAX3232** (RS-232 ↔ TTL) hacia el GPS, fuente DC-DC.

## Compilar y flashear

1. Abre `firmware/v2/wt_gateway_v2_serial/wt_gateway_v2_serial.ino` en Arduino IDE.
2. Instala **NimBLE-Arduino v1.4.x** desde el gestor de librerías.
3. Selecciona la placa ESP32 y el puerto serie.
4. Para el build **productivo**, comenta la línea `#define WT_DEBUG` (los comandos
   de diagnóstico siguen exigiendo sesión autenticada y los flags de modo libre
   arrancan en false).
5. Compila y sube (Upload).

El mismo binario sirve para todos los equipos; cada uno se individualiza al
provisionar.

## Provisionar un equipo

1. Flashea el firmware (idéntico para todos los equipos).
2. En la app aparece como `WT-XXYY` (sin provisionar).
3. Selecciónalo → **Provisionar** → ingresa la patente → confirma.
4. El ESP32 deriva su `device_key = HMAC-SHA256(master, MAC)`, descarta el master
   y queda bloqueado.
5. Repite para cada equipo.

El **master no vive en este repo**: se genera con `tools/wt_auth.py gen-master` y
se entrega por canal privado. Para la derivación de clave y el cálculo de tokens
de autenticación ver `../README.md` (§10) y `INTEGRACION_BLE.md`.

## Flash Encryption — Release Mode

Protege el binario en flash con AES-256; la clave se genera dentro del chip y
nunca sale de él. **Pendiente de activar antes del primer release a producción.**

> ⚠ **IRREVERSIBLE.** En *Release Mode* el chip solo acepta binarios cifrados con
> su clave. Si pierdes el binario firmado / el flujo de firma, el chip queda
> inutilizable. Deja el proceso de build+firma andando ANTES de activarlo, y
> prueba primero en *Development Mode* en un equipo de descarte.

Pasos (ESP-IDF / `idf.py`; con Arduino se hace vía `arduino-esp32` + `espefuse`):

1. En `menuconfig` → *Security features* → activa **Enable flash encryption on
   boot** y elige **Release** (no Development).
2. Compila y flashea la primera vez por USB; el bootloader cifra la flash y quema
   los eFuses (`FLASH_CRYPT_CNT`, `FLASH_CRYPT_CONFIG`).
3. A partir de ahí, las actualizaciones deben ir cifradas (`idf.py
   encrypted-flash` o el binario pre-cifrado para OTA).
4. Recomendado activar también **Secure Boot V2** para impedir binarios no
   firmados.

Notas para este firmware:

- La `device_key` ya vive en NVS. Si el partition NVS NO está cifrado, considera
  habilitar **NVS encryption** (partición `nvs_keys`) para que la clave no quede
  en claro en flash.
- Documenta y respalda (fuera del chip, en bóveda) el esquema de firma; sin él no
  hay forma de actualizar los equipos en campo.

# Firmware ESP32 — Índice de versiones

Este directorio agrupa el firmware del accesorio ESP32 por versión. **Usar siempre
la versión marcada como actual.**

## ★ Actual — V2 (producción)

`v2/wt_gateway_v2_serial/wt_gateway_v2_serial.ino` — **fw 2.9**

- El corte lo ejecuta el GPS (GV75CG) por su DOUT, comandado por **serial** (AT).
- El ESP32 **no** usa salida local de corte (sin GPIO21/GPIO23 como salida).
- Advertising anónimo, configuración en NVS, dispatcher BLE/serial, standby,
  keep-alive, `>GET_PROFILE` y AUTO_DETECT (ver cabecera del `.ino`).
- Requiere librería **NimBLE-Arduino (h2zero) v1.4.x** y un **MAX3232** entre ESP y GPS.

Detalle de build y Flash Encryption: `../docs/README_BUILD.md`.

## Legacy — V1 (archivado, NO usar)

`v1/` conserva la primera arquitectura, en la que el ESP accionaba el corte por
**GPIO23 → relé → entrada digital del GPS** (modelo FMC130). Se mantiene solo como
referencia histórica.

- `v1/produccion/script_ESP32/` — firmware V1 productivo.
- `v1/pruebas/script_ESP32_app_test/` — build de pruebas con la app.
- `v1/pruebas/script_ESP32_test_rele/` — prueba del relé.

## Diferencias clave V1 → V2

| Tema | V1 (legacy) | V2 (actual) |
|------|-------------|-------------|
| Ejecuta el corte | ESP32 vía GPIO23/relé | GPS vía serial (AT) |
| Salida local | Sí (GPIO23; variante con GPIO21) | No |
| Configuración | Hardcodeada | En NVS, editable en runtime |
| Visibilidad BLE | Nombre visible | Advertising anónimo |
| Estado al GPS | — | Latido KA (`mac|name|enabled`) por GTDAT |
| Config remota | — | `clave|valor` por serial (name, ka_on, enabled, profile) |

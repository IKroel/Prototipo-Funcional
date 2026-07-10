# Prototipo Funcional — Llave Virtual BLE (accesorio ESP32 + GPS)

Solución de seguridad Wisetrack: un módulo **ESP32** actúa como accesorio del
equipo **GPS (GV75CG)** para habilitar/inhabilitar el corte de combustible según
ignición y geocerca, controlado desde una **app móvil** por Bluetooth.

El corte y las geocercas viven en el GPS; el ESP32 solo decide cuándo habilitar el
corte y se lo ordena por serial. Ver `docs/CONTEXTO_COWORK.md` para la arquitectura.

## Estado actual (jun-2026)

- ✅ Prototipo funcional validado (módulo + fuente + conversor serial + app).
- ✅ Equipo GPS definido: **GV75CG** (se descartó el Teltonika FMC130).
- ✅ Firmware V2.8 (`wt_gateway_v2_serial`): advertising anónimo, config en NVS,
  standby, keep-alive, `>GET_PROFILE`, AUTO_DETECT.
- ⏳ Pendiente: pruebas en terreno con señuelo, homologación GV75CG, activar Flash
  Encryption, completar perfiles de AUTO_DETECT, app productiva (Desarrollo).

## Estructura

```
Prototipo Funcional/
├── README.md              Este índice
├── docs/
│   ├── CONTEXTO_COWORK.md Arquitectura y protocolo (V2)
│   └── README_BUILD.md    Build del APK, Flash Encryption y provisioning
├── firmware/
│   ├── README.md          Índice de versiones de firmware
│   ├── v1/                Legacy (corte por GPIO23/relé; NO usar)
│   └── v2/wt_gateway_v2_serial/   ★ Firmware actual (fw 2.8)
└── wt_gateway_app/        App Flutter (prototipo; la reharía Desarrollo)
```

## Por dónde empezar

- **Firmware**: `firmware/README.md` → `firmware/v2/wt_gateway_v2_serial/`.
- **Compilar app / provisionar equipos**: `docs/README_BUILD.md`.
- **Entender el sistema**: `docs/CONTEXTO_COWORK.md`.

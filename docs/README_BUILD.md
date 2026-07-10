# WT Gateway — Build del APK

App Flutter del prototipo de llave virtual BLE. Esta carpeta contiene el código
fuente (`src/`) y un script que genera el proyecto Flutter completo y compila el APK.

## Requisitos (en tu PC, no en este entorno)

- Flutter SDK estable (recomendado 3.24 o superior) — `flutter doctor` sin errores
- Android SDK + plataforma de build (Android Studio o command-line tools)
- Un teléfono Android con depuración USB, o emulador

## Compilar (camino rápido)

```bash
cd wt_gateway_app
bash build_apk.sh
```

En **Windows**, hacer doble clic en `wt_gateway_app\build_apk.bat` (o ejecutarlo
en una consola). Además de compilar, limpia builds previos (`flutter clean`) y,
si hay un teléfono conectado por ADB, **actualiza la app instalada** (o la
reinstala si cambió la firma debug/release).

El script ejecuta `flutter create` para generar el scaffolding de Android,
copia el código de `src/` encima, resuelve dependencias y compila.
El APK queda en:

```
build/app/outputs/flutter-apk/app-release.apk
```

Instálalo con:

```bash
flutter install            # con el teléfono conectado
# o
adb install build/app/outputs/flutter-apk/app-release.apk
```

## Compilar paso a paso (manual)

Si prefieres no usar el script:

```bash
cd wt_gateway_app
flutter create --org cl.wisetrack --project-name wt_gateway_app --platforms=android .
rm -rf lib && cp -r src/lib lib
cp src/pubspec.yaml pubspec.yaml
cp src/analysis_options.yaml analysis_options.yaml
cp src/AndroidManifest.xml android/app/src/main/AndroidManifest.xml
flutter pub get
flutter build apk --release
```

## Notas importantes

- **MASTER_SECRET**: ya está generado en `src/lib/config.dart`
  (`cfc03b94…d0a5f76c`). Es un único valor compartido. La app deriva la
  `device_key` de cada ESP32 con `HMAC-SHA256(master, MAC)` leyendo la MAC
  por BLE en tiempo de ejecución — **no hace falta cargar las MAC de los 6
  equipos en la app**. Solo asegúrate de que todos los ESP32 se provisionen
  con este mismo master (la app lo envía sola en `>PROVISION`).
- **minSdk**: flutter_blue_plus requiere `minSdkVersion 21`. El `flutter create`
  actual ya usa 21+, no necesitas tocar nada.
- **Permisos**: el `AndroidManifest.xml` incluido ya trae BLUETOOTH_SCAN/CONNECT
  y ubicación. La app los pide en runtime al abrir el escáner.
- **Debug vs release**: para iterar rápido usa `flutter run`. Para entregar el
  APK de prueba usa `flutter build apk --release` (o `--debug` si necesitas logs).
- **App única productiva**: `bash build_apk.sh` genera la APK final. No hay
  APK separada de testeo; el script `build_apk_debug.sh` quedó obsoleto.
- **Acceso a debug (Banco de pruebas)**: entra como **Administrador** pero con
  el PIN **9999** (en vez de su PIN normal). Eso desbloquea el botón 🧪 "Banco
  de pruebas" en la pantalla del equipo y el "Modo debug" en Perfil: estado en
  vivo, modo libre (relajar geocerca / reenviar siempre / ignorar override),
  setters de ign/geo/override y envío directo (`GTDOS CUT_ON/OFF`, `SIM`, `DUMP`,
  comando libre). Con el PIN normal del admin la sesión es productiva (sin debug).
- **Firmware único**: `firmware/v2/wt_gateway_v2_serial/wt_gateway_v2_serial.ino`
  trae SIEMPRE los comandos de diagnóstico (`>TESTGPS`, `>SIM`, `>SET*`,
  `>RELAX*`, `>DUMP`), pero todos exigen sesión BLE autenticada y los flags de
  modo libre arrancan en false → comportamiento productivo. Esta variante NO usa
  salida local de corte (sin GPIO21): el corte lo ejecuta el tracker por serial.
  Las variantes antiguas `wt_gateway_v2/` (con IO21) y `wt_gateway_v2_debug/` ya
  fueron eliminadas. El firmware V1 legacy quedó archivado en `firmware/v1/`.
- **Tema claro/oscuro**: interruptor en Perfil → Preferencias (se recuerda).

## Provisionar los 6 equipos

1. Flashea `firmware/v2/wt_gateway_v2_serial/wt_gateway_v2_serial.ino` (idéntico para todos).
2. Abre la app → aparece como `WT-XXYY` (sin provisionar).
3. Tócalo → "Provisionar" → ingresa patente → confirma.
4. El ESP32 deriva su `device_key`, descarta el master y queda bloqueado.
5. Repite para cada equipo.

## Flash Encryption — Release Mode (firmware ESP32)

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

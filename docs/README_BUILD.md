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

## Provisionar los 6 equipos

1. Flashea `script_ESP32.ino` (idéntico para todos).
2. Abre la app → aparece como `WT-XXYY` (sin provisionar).
3. Tócalo → "Provisionar" → ingresa patente → confirma.
4. El ESP32 deriva su `device_key`, descarta el master y queda bloqueado.
5. Repite para cada equipo.

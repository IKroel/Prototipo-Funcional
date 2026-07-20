# Instructivo de uso — App Llave Virtual BLE
## Modo Producción · Operación en terreno

Manual para el personal de instalación. Explica cómo conectar a un equipo, leer su estado y liberar el vehículo desde el teléfono.

---

## 1. ¿Qué es?

Es la **Llave Virtual BLE de Wisetrack**. Se comunica por **Bluetooth** con el accesorio instalado en el vehículo y permite **liberar o cortar el combustible** directamente desde el teléfono. Funciona de forma **local: sin servidor ni conexión a internet**.

La app tiene dos pestañas en la barra inferior:

| Pestaña | Para qué sirve |
|---|---|
| **Equipos** | Busca y lista los accesorios cercanos. |
| **Ajustes** | Tema claro/oscuro y cambio de modo (producción / pruebas). |

Este documento cubre el **Modo Producción**, que es el que viene activo por defecto y el que se usa en el día a día.

---

## 2. Antes de empezar

| Requisito | Detalle |
|---|---|
| **Teléfono** | Android con Bluetooth. App instalada desde el archivo entregado. |
| **Permisos** | Bluetooth y Ubicación. Se piden al abrir la app la primera vez; deben concederse. |
| **Bluetooth** | Activado. Si está apagado, la app avisa y ofrece activarlo. |
| **Accesorio** | Debe estar energizado y cerca (idealmente a menos de ~10 m). |

> La app usa la ubicación **solo** como requisito de Android para buscar por Bluetooth. No rastrea ni comparte tu posición.

---

## 3. Palabras clave

- **Equipo / accesorio:** el dispositivo instalado en el vehículo. Una vez configurado se identifica por su patente.
- **Provisionar:** configurar un equipo nuevo por primera vez (asignarle nombre/patente).
- **Liberar:** dejar el vehículo operativo (el motor puede arrancar).
- **Cortar:** armar el corte de combustible (el motor no arrancará cuando se cumplan las condiciones).
- **Ignición:** si el motor está encendido o apagado.
- **Geocerca:** zona geográfica definida. El corte puede depender de estar dentro o fuera de ella.

---

## 4. Conectarse a un equipo

1. **Abre la app.** Inicia en la pestaña **Equipos** y empieza a buscar sola. Concede los permisos si es la primera vez.
2. **Revisa el Bluetooth.** Si está apagado aparece un aviso naranjo; toca **Activar**. Puedes volver a buscar con el botón de recarga (↻) arriba a la derecha.
3. **Lee el listado.** Los equipos se agrupan en **Cerca de ti** y **Más lejos** según la señal (barras). Las pestañas **Sin asignar** y **Asignados** separan los equipos nuevos de los ya configurados; un equipo nuevo muestra la etiqueta **NUEVO**. Usa el buscador para filtrar por patente.
4. **Toca un equipo para conectarte.** La app se conecta y se autentica sola. El texto bajo el nombre pasa de *conectando…* → *conectado* → **autenticado**.
5. **Si el equipo es nuevo (sin configurar),** verás la pantalla *Dispositivo sin configurar*. Toca **Provisionar**, escribe la patente y confirma. Al terminar queda como **Asignado**.

> Para **renombrar** un equipo o cambiar su **tipo** (auto, camioneta, camión, maquinaria), usa el ícono de lápiz en el detalle, o mantén presionada su tarjeta en el listado.

---

## 5. El panel de estado

Al conectarte a un equipo asignado verás un panel oscuro con el estado actual del vehículo:

- **Chip superior:** resumen — **Vehículo liberado** (verde) o **Corte activo** (rojo).
- **Ignición:** motor encendido o apagado.
- **Geocerca:** dentro o fuera de la zona.
- **Corte:** si el corte de combustible está activo o inactivo.
- **Firmware:** versión del equipo.

---

## 6. Acciones

- **Liberar vehículo** (botón verde): acción principal. Se habilita solo cuando el corte está activo. Deja el vehículo operativo. La app pide confirmar.
- **Volver a cortar** (botón secundario): rearma el corte. Se habilita solo cuando el vehículo está liberado.

> Mientras el corte esté activo, **el motor no arrancará.** Libera el vehículo para poder operarlo. Si armas un corte con el motor encendido o dentro de la geocerca, queda armado pero se aplicará recién al apagar el motor y salir de la zona.

---

## 7. Flujo típico

1. Abre la app y ubica el equipo en **Asignados**.
2. Tócalo y espera a que diga **autenticado**.
3. Revisa el panel de estado.
4. Toca **Liberar vehículo** y confirma. El chip pasa a **Vehículo liberado**.
5. Vuelve atrás; la conexión se cierra sola.

---

## 8. Solución de problemas

| Síntoma | Qué hacer |
|---|---|
| No aparecen equipos | Verifica que el Bluetooth esté activo, los permisos concedidos y que el accesorio esté energizado y en rango. Toca el botón de recarga (↻). |
| Aviso "Bluetooth apagado" | Toca **Activar**; la búsqueda se reanuda sola al encenderlo. |
| "No se pudo autenticar" | Toca **Reintentar autenticación**. |
| "Dispositivo sin configurar" | Es un equipo nuevo. Toca **Provisionar** y asígnale patente. |
| "Liberar" está deshabilitado | El vehículo ya está liberado, o aún no está autenticado. Espera a **autenticado**. |
| "El equipo ya está provisionado" | Restablécelo primero (ícono de lápiz → **Restablecer equipo**) y vuelve a provisionar. |

---

*Wisetrack · Llave Virtual BLE — Modo Producción.*

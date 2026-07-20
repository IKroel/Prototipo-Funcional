# Instructivo de uso — WTKey
## Modo Pruebas · Banco de laboratorio y diagnóstico

Manual para el personal de laboratorio y validación de equipos. El Modo Pruebas agrega herramientas para **simular escenarios**, una **consola BLE** y el manejo de la **clave maestra**, sin cambiar la forma de probar el corte (se prueba con los botones reales).

> Este documento asume que ya sabes conectarte a un equipo y leer el panel de estado. Si no, revisa primero el instructivo de **Modo Producción**.

---

## 1. Cómo activarlo

1. Ve a la pestaña **Ajustes**.
2. En **Modo de la app**, activa el interruptor **Modo pruebas**. El texto confirma: *"Banco de pruebas, consola y master key visibles."*

> El cambio queda guardado. Al entrar al detalle de un equipo verás la etiqueta **PRUEBAS** junto al nombre.

---

## 2. Simular escenario

En el detalle del equipo aparece una tarjeta **Simular escenario** con dos controles para forzar el estado del equipo durante la prueba, sin necesidad del vehículo real:

| Control | Valores |
|---|---|
| **Motor** | Encendido / Apagado |
| **Geocerca** | Dentro / Fuera |

Cambia estos valores y observa cómo reacciona el corte en el panel de estado. **El corte no se simula:** se prueba con los botones reales *Liberar* / *Volver a cortar*.

---

## 3. Consola BLE (Terminal)

En modo pruebas aparece un ícono de terminal en la barra del detalle. Abre una consola para enviar comandos directos al equipo y leer sus respuestas. Comandos rápidos disponibles:

| Comando | Qué hace |
|---|---|
| `>STATUS` | Estado resumido del equipo. |
| `>DUMP` | Volcado completo de variables internas (alimenta el simulador). |
| `>VERSION` | Versión de firmware. |
| `>MAC` | Dirección MAC del equipo. |

---

## 4. Clave maestra (master key)

En **Ajustes → Diagnóstico** la clave se muestra completa (en producción va enmascarada). Puedes:

- **Copiar** la clave activa.
- **Cambiar** por un override local (64 caracteres hexadecimales) guardado solo en ese teléfono.
- **Restaurar** la clave original compilada.

---

## 5. Flujo de una prueba de corte

1. Conéctate al equipo (debe decir **autenticado**).
2. En **Simular escenario**, pon **Motor: Apagado** y **Geocerca: Fuera** (condiciones para que el corte actúe).
3. Toca **Volver a cortar** y confirma. El panel debe pasar a **Corte activo**.
4. Verifica con **Motor: Encendido** que la regla se comporta como se espera.
5. Al terminar, toca **Liberar vehículo** para dejar el equipo operativo.

> **Al salir del detalle en Modo Pruebas, el equipo se restaura solo** al estado que tenía antes de la prueba. Aun así, confirma siempre que el vehículo quede **liberado** antes de cerrar. No dejes un corte armado tras una prueba.

---

## 6. Seguridad

La clave maestra es sensible: permite autenticar los equipos. **No la compartas ni la dejes visible fuera del banco de pruebas.** Trabaja en **Modo Producción** para la operación diaria y reserva el **Modo Pruebas** para diagnóstico.

---

*Wisetrack · WTKey — Modo Pruebas.*

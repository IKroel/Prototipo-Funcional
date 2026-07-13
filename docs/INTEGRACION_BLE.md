# 🔌 Integración BLE con la ESP32

Guía para que el equipo de desarrollo se conecte e interactúe con el equipo
**sin la app de referencia** (que queda fuera de este repo). Complementa la
sección *"Guía de conexión para una app nueva"* del `README.md`.

---

## 1 · ¿Necesito el master para interactuar?

Depende de la acción:

| Acción | ¿Requiere auth? | ¿Requiere master? |
|--------|:---:|:---:|
| `>PING`, `>VERSION`, `>GET_PROFILE`, `>STATUS`, `>MAC` | No | No |
| `>DISABLECUT`, `>ARMCUT`, `>SET_*`, `>REPORT`, provisioning | Sí | Sí |

Para **leer/monitorear** no hace falta nada: conectas por NUS y consultas. Para
**comandar** (corte, configuración) hay que autenticarse, y eso depende del
master (o de la `device_key` derivada de él).

---

## 2 · El master NO vive en el repo

`kMasterSecret` es una **llave universal**: con él y la MAC (pública en el
advertising) se deriva la `device_key` de cualquier equipo. Por eso:

- El valor del master **no se versiona**. Se entrega **fuera de banda** a quien
  integra (canal privado, gestor de secretos, o servidor por sesión).
- En este repo solo está documentado el **mecanismo** y las herramientas para
  usarlo, nunca el valor.

> Producción: el master debe entregarlo el servidor Wisetrack por sesión (login
> → JWT → vault) para que el cliente nunca lo compile. Lo de abajo es para
> **pruebas** del equipo de desarrollo.

---

## 3 · Generar un master

```bash
python3 tools/wt_auth.py gen-master
# -> 64 caracteres hex (32 bytes)
```

El mismo master debe usarse para **provisionar** el equipo y para **autenticarse**
contra él. Guárdalo en tu entorno local (variable de entorno o archivo fuera de
git), no lo subas.

---

## 4 · Provisionar un equipo (una sola vez)

El equipo nace sin llave. Se le graba la `device_key` derivada del master:

```
>PROVISION <master_hex_64>
<PROVISION_OK mac=AA:BB:CC:DD:EE:FF
```

El firmware calcula y guarda `device_key = HMAC-SHA256(master, MAC)` en NVS; el
master **no** se almacena en el equipo. Para revertir: `>UNPROVISION` (exige auth).

---

## 5 · Autenticarse (handshake)

```
>CHALLENGE
<CHALLENGE 1122334455667788        # nonce de 8 bytes (16 hex chars)
>AUTH <token_hex_32>               # token de 16 bytes (32 hex chars)
<AUTH_OK
```

Donde:

```
device_key = HMAC-SHA256(master, MAC_bytes[6])
token      = HMAC-SHA256(device_key, nonce_bytes)[:16]   # primeros 16 bytes
```

Calcúlalo con la herramienta:

```bash
# con el master, la MAC del equipo y el nonce que llegó en <CHALLENGE
python3 tools/wt_auth.py token --master <master_hex> --mac AA:BB:CC:DD:EE:FF --nonce 1122334455667788
# -> pega el resultado en  >AUTH <resultado>
```

La sesión autenticada **se pierde al desconectar**: repite el handshake en cada
reconexión. El estado del corte **no** se revierte al desconectar.

---

## 6 · Probar sin escribir código (nRF Connect)

1. Escanea y filtra por Manufacturer Data `0xFFFF 0x57 0x54 0x01` (el equipo es
   anónimo, no difunde nombre).
2. Conecta, descubre el servicio NUS `6E400001-…`.
3. Suscribe notificaciones en **TX** `6E400003-…` (antes de enviar nada).
4. Escribe comandos en **RX** `6E400002-…` (empiezan con `>`, uno por escritura).
5. Para acciones sensibles: `>CHALLENGE` → copia el nonce → calcula el token con
   `wt_auth.py token …` → `>AUTH <token>` → ya puedes mandar `>DISABLECUT`, etc.

Las respuestas llegan por TX empezando con `<` y terminadas en `\n`; con MTU
chico un `<PROFILE {json}` largo llega fragmentado (acumula hasta el `\n`).

---

## 7 · Referencia rápida

- UUIDs, tabla de comandos y respuestas: `README.md` §5–§7.
- Modelo de seguridad: `README.md` §10.
- Herramienta de auth: `tools/wt_auth.py` (`gen-master`, `device-key`, `token`).

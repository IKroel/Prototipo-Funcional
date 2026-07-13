#!/usr/bin/env python3
# ============================================================
# wt_auth.py — Utilidad de autenticación BLE Wisetrack
# ============================================================
# Genera el master, deriva la device_key y firma el nonce para
# poder probar la interacción con la ESP32 (p. ej. desde nRF
# Connect) sin necesidad de la app.
#
# Regla del firmware:
#   device_key = HMAC-SHA256(master, MAC_BLE[6 bytes])
#   token      = HMAC-SHA256(device_key, nonce)[:16]   (16 bytes)
#   nonce      = 8 bytes  (16 hex chars, llega en <CHALLENGE)
#   token      = 16 bytes (32 hex chars, se envía en >AUTH)
# ============================================================
import argparse
import hashlib
import hmac
import secrets
import sys


def gen_master() -> str:
    """Genera un master nuevo (32 bytes -> 64 hex chars)."""
    return secrets.token_hex(32)


def mac_to_bytes(mac: str) -> bytes:
    return bytes(int(b, 16) for b in mac.strip().upper().replace("-", ":").split(":"))


def derive_device_key(master_hex: str, mac: str) -> bytes:
    master = bytes.fromhex(master_hex.strip())
    return hmac.new(master, mac_to_bytes(mac), hashlib.sha256).digest()


def sign_nonce(device_key: bytes, nonce_hex: str) -> str:
    nonce = bytes.fromhex(nonce_hex.strip())
    tag = hmac.new(device_key, nonce, hashlib.sha256).digest()
    return tag[:16].hex()  # 16 bytes -> 32 hex chars para >AUTH


def main() -> int:
    p = argparse.ArgumentParser(description="Utilidad de auth BLE Wisetrack.")
    sub = p.add_subparsers(dest="cmd", required=True)

    sub.add_parser("gen-master", help="Genera un master nuevo (64 hex).")

    dk = sub.add_parser("device-key", help="Deriva la device_key de un equipo.")
    dk.add_argument("--master", required=True, help="Master en hex (64 chars).")
    dk.add_argument("--mac", required=True, help="MAC BLE, ej. AA:BB:CC:DD:EE:FF")

    tk = sub.add_parser("token", help="Calcula el token para >AUTH desde un nonce.")
    tk.add_argument("--master", required=True, help="Master en hex (64 chars).")
    tk.add_argument("--mac", required=True, help="MAC BLE del equipo.")
    tk.add_argument("--nonce", required=True, help="Nonce hex recibido en <CHALLENGE.")

    a = p.parse_args()

    if a.cmd == "gen-master":
        print(gen_master())
    elif a.cmd == "device-key":
        print(derive_device_key(a.master, a.mac).hex())
    elif a.cmd == "token":
        dkb = derive_device_key(a.master, a.mac)
        print(sign_nonce(dkb, a.nonce))
    return 0


if __name__ == "__main__":
    sys.exit(main())

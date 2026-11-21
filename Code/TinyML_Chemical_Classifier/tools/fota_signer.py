#!/usr/bin/env python3
"""
Neoway N706B Secure FOTA Signer & SMS Trigger Generator
Author: Antigravity / Google DeepMind
Target: Neoway N706B OpenCPU BME688 AI Gas Scanner

Signs the compiled release package (nwy_open_app_fota.pkt) using ECDSA secp256r1
over SHA-256 and outputs the exact SMS command string needed to trigger
a remote over-the-air update.
"""

import os
import sys
import json
import socket
import binascii
import hashlib
import argparse

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import utils

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
DEFAULT_KEY_PATH = os.path.join(SCRIPT_DIR, "fota_private_key.pem")
DEFAULT_PKT_PATH = os.path.join(PROJECT_DIR, "release", "nwy_open_app_fota.pkt")

def get_local_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('8.8.8.8', 80))
        ip = s.getsockname()[0]
    except Exception:
        ip = '127.0.0.1'
    finally:
        s.close()
    return ip

def load_or_create_private_key(key_path):
    if os.path.exists(key_path):
        with open(key_path, "rb") as f:
            return serialization.load_pem_private_key(f.read(), password=None)
    else:
        print(f"[*] Generating new ECDSA secp256r1 Keypair at {key_path}...")
        key = ec.generate_private_key(ec.SECP256R1())
        with open(key_path, "wb") as f:
            f.write(key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.TraditionalOpenSSL,
                encryption_algorithm=serialization.NoEncryption()
            ))
        return key

def compute_sha256(file_path):
    h = hashlib.sha256()
    with open(file_path, "rb") as f:
        while True:
            chunk = f.read(8192)
            if not chunk:
                break
            h.update(chunk)
    return h.digest(), h.hexdigest()

def main():
    parser = argparse.ArgumentParser(description="Neoway N706B Secure FOTA Signer")
    parser.add_argument("--pkt", type=str, default=DEFAULT_PKT_PATH, help="Path to nwy_open_app_fota.pkt")
    parser.add_argument("--key", type=str, default=DEFAULT_KEY_PATH, help="Path to fota_private_key.pem")
    parser.add_argument("--url", type=str, default="", help="HTTP download URL for the .pkt file")
    args = parser.parse_args()

    print("\n" + "="*70)
    print("  NEOWAY N706B — SECURE FOTA FIRMWARE SIGNER (ECDSA secp256r1)")
    print("="*70)

    if not os.path.exists(args.pkt):
        print(f"[!] Error: Firmware package not found at: {args.pkt}")
        print("    Please run build_TinyML_Chemical_Classifier.bat first!")
        sys.exit(1)

    # 1. Load Private Key
    private_key = load_or_create_private_key(args.key)
    print(f"[*] Loaded ECDSA Private Key from: {args.key}")

    # 2. Compute Size & SHA-256
    fw_size = os.path.getsize(args.pkt)
    fw_hash_raw, fw_hash_hex = compute_sha256(args.pkt)
    print(f"[*] Firmware Binary : {args.pkt}")
    print(f"[*] Binary Size     : {fw_size:,} bytes")
    print(f"[*] SHA-256 Hash    : {fw_hash_hex}")

    # 3. Sign the Hash
    der_signature = private_key.sign(
        fw_hash_raw,
        ec.ECDSA(utils.Prehashed(hashes.SHA256()))
    )
    sig_hex = binascii.hexlify(der_signature).decode()
    print(f"[*] ECDSA Signature : {sig_hex}")

    # 4. Determine Download URL
    if args.url:
        fw_url = args.url
    else:
        local_ip = get_local_ip()
        fw_url = f"http://{local_ip}:8080/nwy_open_app_fota.pkt"

    sms_command = f"FOTA {fw_url} {fw_size} {sig_hex}"

    print("\n" + "-"*70)
    print(">>> READY-TO-SEND SMS COMMAND TO TRIGGER SECURE FOTA UPDATE <<<")
    print("-"*70)
    print(sms_command)
    print("-"*70)

    # 5. Save metadata JSON
    meta_path = os.path.join(SCRIPT_DIR, "fota_metadata.json")
    meta_data = {
        "filename": os.path.basename(args.pkt),
        "size": fw_size,
        "sha256": fw_hash_hex,
        "signature": sig_hex,
        "url": fw_url,
        "sms_command": sms_command
    }
    with open(meta_path, "w") as f:
        json.dump(meta_data, f, indent=2)
    print(f"[*] Saved FOTA metadata to: {meta_path}\n")

if __name__ == "__main__":
    main()

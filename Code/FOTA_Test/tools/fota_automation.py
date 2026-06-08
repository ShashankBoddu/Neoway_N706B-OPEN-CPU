import os
import json
import hashlib
import binascii
import paho.mqtt.client as mqtt
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import utils

# ==========================================
# HARDCODED CONFIGURATIONS
# ==========================================
PRIVATE_KEY_PATH = r"E:\projects\DevelopmentLevelCode\Neoway_N706B\Code\FOTA_Test\tools\fota_private_key.pem"
FIRMWARE_ZIP_PATH = r"C:\Users\fervi\Downloads\app.bin"
# VERCEL_FIRMWARE_URL = "https://6a23c6d8e482b8123008e30b--frabjous-hamster-7c3cbe.netlify.app/app.bin"
VERCEL_FIRMWARE_URL = "https://project-508sg.vercel.app/app.bin"
# FIRMWARE_ZIP_PATH = r"E:\projects\DevelopmentLevelCode\Neoway_N706B\Code\FOTA_Test\release\nwy_open_app_fota.pkt"
# VERCEL_FIRMWARE_URL = "https://6a2402c3926549b7d9893742--thunderous-otter-3352fa.netlify.app/nwy_open_app_fota.pkt"

# FIRMWARE_ZIP_PATH = r"E:\projects\DevelopmentLevelCode\Neoway_N706B\Code\FOTA_Test\release\nwy_open_app.zip"
# VERCEL_FIRMWARE_URL = "https://6a23cba0d3d1cef3bdcdd4c4--thunderous-otter-3352fa.netlify.app/nwy_open_app.zip"

MQTT_BROKER = "mqtt.fervidlabs.in"
MQTT_PORT = 1883
MQTT_USER = "fervid"
MQTT_PASSWORD = "Fervid@123"
MQTT_TOPIC = "nwy_n706b/fervid/rx"
# ==========================================

def get_or_generate_keys():
    """Load the private key if it exists, otherwise generate a new one."""
    if os.path.exists(PRIVATE_KEY_PATH):
        print(f"[*] Found existing private key at {PRIVATE_KEY_PATH}")
        with open(PRIVATE_KEY_PATH, "rb") as key_file:
            private_key = serialization.load_pem_private_key(key_file.read(), password=None)
    else:
        print(f"[*] No private key found! Generating a new ECDSA secp256r1 Keypair...")
        private_key = ec.generate_private_key(ec.SECP256R1())
        # Save it to file
        with open(PRIVATE_KEY_PATH, "wb") as key_file:
            key_file.write(private_key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.TraditionalOpenSSL,
                encryption_algorithm=serialization.NoEncryption()
            ))
        print(f"[*] Saved new private key to {PRIVATE_KEY_PATH}")

    # Extract Public Key in RAW format (64 bytes: 32 bytes X + 32 bytes Y)
    public_numbers = private_key.public_key().public_numbers()
    raw_public_key = (
        public_numbers.x.to_bytes(32, byteorder='big') + 
        public_numbers.y.to_bytes(32, byteorder='big')
    )
    
    print("\n" + "="*70)
    print(">>> YOUR PUBLIC KEY FOR C CODE (COPY THIS INTO nwy_hal_secure_fota.c) <<<")
    print("="*70)
    print("static const uint8_t PUBLIC_KEY_RAW[64] = {")
    for i in range(0, 64, 8):
        chunk = raw_public_key[i:i+8]
        print("    " + ", ".join([f"0x{b:02x}" for b in chunk]) + ("," if i < 56 else ""))
    print("};")
    print("="*70 + "\n")
    
    return private_key

def compute_sha256(file_path):
    sha256_hash = hashlib.sha256()
    with open(file_path, "rb") as f:
        for byte_block in iter(lambda: f.read(4096), b""):
            sha256_hash.update(byte_block)
    return sha256_hash.digest()

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print("[*] Successfully connected to MQTT Broker.")
    else:
        print(f"[!] Failed to connect to MQTT Broker. Return code: {rc}")

def main():
    print("--- COMPLETE FOTA AUTOMATION SCRIPT ---")
    
    # 1. Get or Generate Key
    private_key = get_or_generate_keys()
    
    user_input = input("Do you want to proceed with signing the firmware and sending the MQTT Trigger? (y/n): ")
    if user_input.lower() != 'y':
        print("Exiting...")
        return

    # 2. Check if firmware exists
    if not os.path.exists(FIRMWARE_ZIP_PATH):
        print(f"[!] Error: Firmware file not found at {FIRMWARE_ZIP_PATH}")
        print("    Please compile your code and place the zip there first!")
        return

    # 3. Get Size & Hash
    fw_size = os.path.getsize(FIRMWARE_ZIP_PATH)
    print(f"[*] Firmware Size: {fw_size} bytes")
    print("[*] Computing SHA-256 hash of the firmware...")
    fw_hash = compute_sha256(FIRMWARE_ZIP_PATH)
    
    # 4. Sign the Hash
    print("[*] Signing the firmware hash with ECDSA Private Key...")
    der_signature = private_key.sign(
        fw_hash,
        ec.ECDSA(utils.Prehashed(hashes.SHA256()))
    )
    hex_signature = binascii.hexlify(der_signature).decode()
    print(f"[*] Generated Signature: {hex_signature[:30]}... (truncated)")

    # 5. Build MQTT Payload
    payload = {
        "command": "OTA",
        "OTAConfigURL": VERCEL_FIRMWARE_URL,
        "Configsize": fw_size,
        "signature": hex_signature
    }
    json_payload = json.dumps(payload, separators=(',', ':'))
    print(f"\n[*] MQTT Payload Ready:\n{json_payload}\n")

    # 6. Publish via MQTT
    print(f"[*] Connecting to MQTT Broker {MQTT_BROKER}:{MQTT_PORT}...")
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1)
    if MQTT_USER and MQTT_PASSWORD:
        client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
    
    client.on_connect = on_connect
    try:
        client.connect(MQTT_BROKER, MQTT_PORT, 60)
    except Exception as e:
        print(f"[!] Failed to connect: {e}")
        return

    client.loop_start()
    print(f"[*] Publishing FOTA trigger to topic '{MQTT_TOPIC}'...")
    info = client.publish(MQTT_TOPIC, json_payload, qos=0)
    info.wait_for_publish()
    print("[*] Publish complete. Device should start downloading the FOTA package.")
    
    client.loop_stop()
    client.disconnect()

if __name__ == "__main__":
    main()

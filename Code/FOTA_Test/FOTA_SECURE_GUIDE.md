# Neoway N706B Secure FOTA (ECDSA + SHA-256) Guide

## Overview
This project implements a Secure Firmware Over-The-Air (FOTA) update mechanism for the Neoway N706B module. The implementation ensures that any firmware downloaded and installed on the device is strictly verified for authenticity and integrity using **ECDSA (secp256r1) over SHA-256**. 

Because the native Neoway SDK (`nwy_open_stub.a`) does not export the required MbedTLS ECC and SHA-256 functions, this project bundles the lightweight standalone libraries `micro-ecc` and a custom `sha256` implementation directly within the source tree (`src/crypto`).

---

## 1. How It Works
1. **Trigger**: The device receives an MQTT message on `nwy_n706b/fervid/rx` with a JSON payload containing the `OTAConfigURL`, `Configsize`, and the ECDSA `signature` (in ASN.1 DER format encoded as Base64/Hex - handled in code).
2. **Download & Hash**: The device begins downloading the firmware from the `OTAConfigURL` in chunks. As each chunk arrives, it is fed into `sha256_update()` on-the-fly and written to the FOTA flash partition.
3. **Verify**: Once the download completes, the final SHA-256 hash is computed. The `signature` is then verified against this hash using the baked-in **Public Key**. 
4. **Flash**: If verification succeeds, the firmware is marked as valid and the module reboots to apply the update. If it fails, the update is rejected and the downloaded file is discarded.

---

## 2. What Is Needed?
To securely update your firmware, you need the following:
1. **An ECDSA secp256r1 Keypair**: A private key to sign your firmware files, and a public key baked into the device.
2. **Raw Public Key Coordinates**: `micro-ecc` requires the public key as a raw 64-byte array (32 bytes X coordinate + 32 bytes Y coordinate), NOT a standard PEM file.
3. **Firmware Signature**: You must sign your `nwy_open_app.zip` release package before uploading it to your HTTP server.

---

## 3. How to Generate and Add Your Public Key

A helper Python script is provided in the `tools` directory to extract the raw 64-byte public key from a standard PEM public key file.

### Step-by-Step Instructions:
1. Ensure you have your public key in PEM format (e.g., `public_key.pem`).
2. Run the extraction script:
   ```bash
   python tools/extract_pubkey.py <path_to_your_public_key.pem>
   ```
3. The script will output a C-style array:
   ```c
   static const uint8_t PUBLIC_KEY_RAW[64] = {
       0x..., 0x..., // ... 64 bytes
   };
   ```
4. Open the file: `src/hal/nwy_hal_secure_fota.c`.
5. Find the `PUBLIC_KEY_RAW` placeholder array at the top of the file and replace it with the output from the Python script.
6. Rebuild the project using `.\build_FOTA_Test.bat` to bake the key into your firmware.

---

## 4. How to Perform an OTA Update

When you are ready to deploy a new firmware version:
1. Build the new firmware using `.\build_FOTA_Test.bat`.
2. Locate the generated zip package: `release\nwy_open_app.zip`.
3. Sign this zip package using your ECDSA private key.
4. Upload the zip package to your HTTP/HTTPS server (ensure the module can access the URL).
5. Send an MQTT message to the topic `nwy_n706b/fervid/rx` with the following JSON structure:

```json
{
  "command": "OTA",
  "OTAConfigURL": "http://your-server.com/nwy_open_app.zip",
  "Configsize": 123456,
  "signature": "<base64_or_hex_encoded_der_signature>"
}
```

*Note: Replace `123456` with the exact byte size of the zip package, and provide the correct URL and signature.*

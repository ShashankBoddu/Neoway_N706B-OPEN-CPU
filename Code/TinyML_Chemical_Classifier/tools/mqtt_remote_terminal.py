#!/usr/bin/env python3
"""
Neoway N706B — Remote MQTT Command Terminal & Live Monitor
Connects to Fervid MQTT Broker (mqtt.fervidlabs.in) to send commands to and
receive replies/alerts from the N706B BME688 AI Gas Scanner in real-time.

Broker Details:
  - Host : mqtt.fervidlabs.in:1883
  - User : fervid
  - Pass : Fervid@123
  - Pub  : sensor/output/N706B/BM688 (PC -> N706B)
  - Sub  : sensor/input/N706B/BM688  (N706B -> PC)
"""

import os
import sys
import json
import time
import argparse
import threading
try:
    import paho.mqtt.client as mqtt
except ImportError:
    print("[ERROR] paho-mqtt not installed. Run: pip install paho-mqtt")
    sys.exit(1)

BROKER_HOST = "mqtt.fervidlabs.in"
BROKER_PORT = 1883
BROKER_USER = "fervid"
BROKER_PASS = "Fervid@123"

TOPIC_SUB_FROM_BOARD = "sensor/input/N706B/BM688"
TOPIC_PUB_TO_BOARD   = "sensor/output/N706B/BM688"

def on_connect(client, userdata, flags, rc, properties=None):
    if rc == 0:
        print(f"\n[+] Connected to MQTT Broker: {BROKER_HOST}:{BROKER_PORT}")
        print(f"[*] Subscribed to Board Telemetry & Replies: {TOPIC_SUB_FROM_BOARD}")
        print(f"[*] Command Publish Topic: {TOPIC_PUB_TO_BOARD}")
        print("=" * 66)
        print("  Type any command (STATUS, START <LABEL>, STOP, LIST, UPLOAD ALL, FOTA)")
        print("  or type 'exit' / 'quit' to close.")
        print("=" * 66 + "\n")
        client.subscribe(TOPIC_SUB_FROM_BOARD, qos=0)
    else:
        print(f"[-] Connection failed with error code: {rc}")

def on_message(client, userdata, msg):
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    payload_str = msg.payload.decode('utf-8', errors='replace').strip()
    print(f"\n[{timestamp}] <<< [N706B REPLY / ALERT]:")
    print(f"    {payload_str}")
    print("\ncmd > ", end="", flush=True)

def main():
    parser = argparse.ArgumentParser(description="Neoway N706B Remote MQTT Command Terminal")
    parser.add_argument("--cmd", type=str, help="Single command to send and wait for response (e.g. --cmd STATUS)")
    parser.add_argument("--timeout", type=int, default=10, help="Wait timeout in seconds for --cmd mode")
    args = parser.parse_args()

    client_id = f"N706B_PC_Terminal_{int(time.time() % 10000)}"
    try:
        # Paho MQTT v2/v1 compatibility
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=client_id)
    except AttributeError:
        client = mqtt.Client(client_id=client_id)

    client.username_pw_set(BROKER_USER, BROKER_PASS)
    client.on_connect = on_connect
    client.on_message = on_message

    print(f"[*] Connecting to {BROKER_HOST}:{BROKER_PORT}...")
    try:
        client.connect(BROKER_HOST, BROKER_PORT, keepalive=60)
    except Exception as e:
        print(f"[-] Could not connect to broker: {e}")
        sys.exit(1)

    client.loop_start()

    if args.cmd:
        # Single command mode
        time.sleep(1.5) # Wait for connection
        print(f"[*] Sending command: \"{args.cmd}\" -> {TOPIC_PUB_TO_BOARD}")
        client.publish(TOPIC_PUB_TO_BOARD, args.cmd)
        print(f"[*] Waiting {args.timeout}s for board response...")
        time.sleep(args.timeout)
        client.loop_stop()
        client.disconnect()
        return

    # Interactive mode
    try:
        time.sleep(1.0)
        while True:
            cmd = input("cmd > ").strip()
            if not cmd:
                continue
            if cmd.lower() in ("exit", "quit", "q"):
                print("[*] Exiting...")
                break

            # Smart FOTA auto-enhancement from release package on disk
            if cmd.lower() in ("fota", "ota") or cmd.lower().startswith("fota ") or cmd.lower().startswith("ota "):
                release_pkt = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "release", "nwy_open_app_fota.pkt")
                release_pkt = os.path.normpath(release_pkt)
                meta_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fota_metadata.json")
                fw_sz = os.path.getsize(release_pkt) if os.path.exists(release_pkt) else None
                fw_sig = None

                if os.path.exists(meta_file):
                    try:
                        with open(meta_file, "r") as mf:
                            mdata = json.load(mf)
                        if fw_sz is None or mdata.get("size") == fw_sz:
                            fw_sz = mdata.get("size")
                            fw_sig = mdata.get("signature")
                    except Exception:
                        pass

                if fw_sz and fw_sig:
                    parts = cmd.split(maxsplit=1)
                    if len(parts) > 1 and parts[1].startswith("http"):
                        cmd = f"FOTA {parts[1]} {fw_sz} {fw_sig}"
                        print(f"[*] Auto-appended verified Size ({fw_sz}B) & Signature to URL")
                    elif len(parts) == 1:
                        cmd = f"FOTA {fw_sz} {fw_sig}"
                        print(f"[*] Auto-appended verified Size ({fw_sz}B) & Signature (using configured server)")

            client.publish(TOPIC_PUB_TO_BOARD, cmd)
            print(f"    -> [SENT to {TOPIC_PUB_TO_BOARD}]: \"{cmd}\"")
            time.sleep(0.5)
    except (KeyboardInterrupt, EOFError):
        print("\n[*] Interrupted by user.")
    finally:
        client.loop_stop()
        client.disconnect()

if __name__ == "__main__":
    main()

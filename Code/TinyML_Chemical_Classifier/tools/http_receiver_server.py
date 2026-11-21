#!/usr/bin/env python3
"""
Neoway N706B Remote SD Card HTTP Receiver & FOTA Server
Author: Antigravity / Google DeepMind
Target: Neoway N706B OpenCPU BME688 AI Gas Scanner

This server provides two essential services:
1. HTTP POST Receiver: Receives streamed .bmerawdata, .bmelabelinfo, and .csv
   dataset files uploaded directly from the module's MicroSD card.
2. HTTP GET File Server: Serves firmware packages (e.g. nwy_open_app_fota.pkt)
   for remote FOTA updates over cellular LTE.
"""

import os
import sys
import socket
import argparse
import hashlib
from datetime import datetime
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn
from urllib.parse import urlparse, parse_qs

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
RECEIVE_DIR = os.path.join(SCRIPT_DIR, "received_datasets")
RELEASE_DIR = os.path.join(PROJECT_DIR, "release")

os.makedirs(RECEIVE_DIR, exist_ok=True)

def get_local_ip():
    """Attempt to detect the primary local network IP address."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # Connecting to a public address (doesn't actually send packets)
        s.connect(('8.8.8.8', 80))
        ip = s.getsockname()[0]
    except Exception:
        ip = '127.0.0.1'
    finally:
        s.close()
    return ip

class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True

class N706BHTTPHandler(BaseHTTPRequestHandler):
    server_version = "Neoway-N706B-Receiver/1.0"

    def do_GET(self):
        """Handle file downloads (e.g. FOTA firmware packages)."""
        parsed = urlparse(self.path)
        path = parsed.path.lstrip('/')

        # Search for requested file in release dir or script dir
        candidate_paths = [
            os.path.join(RELEASE_DIR, path),
            os.path.join(SCRIPT_DIR, path),
            os.path.join(RECEIVE_DIR, path)
        ]

        target_file = None
        for cp in candidate_paths:
            if os.path.isfile(cp):
                target_file = cp
                break

        if not target_file:
            self.send_response(404)
            self.send_header("Content-Type", "text/plain")
            self.end_headers()
            self.wfile.write(b"404 Not Found\n")
            print(f"[{datetime.now().strftime('%H:%M:%S')}] [GET 404] File not found: {self.path}")
            return

        file_size = os.path.getsize(target_file)
        print(f"[{datetime.now().strftime('%H:%M:%S')}] [GET 200] Serving '{os.path.basename(target_file)}' ({file_size:,} bytes) to {self.client_address[0]}")

        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(file_size))
        self.send_header("Connection", "close")
        self.end_headers()

        with open(target_file, "rb") as f:
            while True:
                chunk = f.read(16384)
                if not chunk:
                    break
                self.wfile.write(chunk)

    def do_POST(self):
        """Handle file uploads from the Neoway N706B MicroSD card."""
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)

        # 1. Determine uploaded file name
        filename = None
        if 'filename' in query and query['filename']:
            filename = query['filename'][0]
        elif 'X-Filename' in self.headers:
            filename = self.headers['X-Filename']
        elif 'x-filename' in self.headers:
            filename = self.headers['x-filename']

        if not filename or filename in ('.', '..', '/'):
            ts = datetime.now().strftime("%Y%m%d_%H%M%S")
            filename = f"upload_{ts}.dat"

        # Sanitize filename
        filename = os.path.basename(filename.replace('\\', '/'))

        # 2. Determine upload size
        content_len = self.headers.get('Content-Length')
        if not content_len:
            self.send_response(411) # Length Required
            self.end_headers()
            return

        total_bytes = int(content_len)
        save_path = os.path.join(RECEIVE_DIR, filename)

        print("\n" + "="*70)
        print(f"[{datetime.now().strftime('%Y-%m-%d %H:%M:%S')}] INCOMING SD DATASET UPLOAD")
        print(f"  * From Device IP : {self.client_address[0]}")
        print(f"  * File Target    : {filename}")
        print(f"  * Expected Size  : {total_bytes:,} bytes")
        print("="*70)

        # 3. Stream body directly to file on disk
        bytes_received = 0
        sha = hashlib.sha256()

        with open(save_path, "wb") as f:
            while bytes_received < total_bytes:
                chunk_to_read = min(8192, total_bytes - bytes_received)
                chunk = self.rfile.read(chunk_to_read)
                if not chunk:
                    break
                f.write(chunk)
                sha.update(chunk)
                bytes_received += len(chunk)

        file_hash = sha.hexdigest()
        print(f"[STATUS] Upload completed successfully!")
        print(f"  * Saved Path     : {save_path}")
        print(f"  * Bytes Received : {bytes_received:,} of {total_bytes:,}")
        print(f"  * SHA-256 Digest : {file_hash}")
        print("="*70 + "\n")

        # 4. Return HTTP 200 OK
        resp = f'{{"status":"ok","filename":"{filename}","bytes":{bytes_received},"sha256":"{file_hash}"}}\n'.encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(resp)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, format, *args):
        # Suppress default noisy console logs
        return

def main():
    parser = argparse.ArgumentParser(description="Neoway N706B Remote SD Card & FOTA HTTP Server")
    parser.add_argument("--port", type=int, default=8080, help="Listen port (default: 8080)")
    parser.add_argument("--host", type=str, default="0.0.0.0", help="Listen host (default: 0.0.0.0)")
    args = parser.parse_args()

    local_ip = get_local_ip()
    server = ThreadedHTTPServer((args.host, args.port), N706BHTTPHandler)

    print("\n" + "#"*70)
    print("  NEOWAY N706B — REMOTE SD UPLOADER & FOTA HTTP SERVER")
    print("#"*70)
    print(f" Listening Address  : http://{args.host}:{args.port}")
    print(f" Primary Local IP   : {local_ip}")
    print(f" Dataset Save Dir   : {RECEIVE_DIR}")
    print(f" Firmware Source Dir: {RELEASE_DIR}")
    print("\n[MODULE SMS COMMANDS TO USE WITH THIS SERVER]:")
    print(f"  1. Set Upload Server -> Send SMS:  SERVER http://{local_ip}:{args.port}/upload")
    print(f"  2. List SD Files     -> Send SMS:  LIST")
    print(f"  3. Upload Latest     -> Send SMS:  UPLOAD LATEST")
    print(f"  4. Upload All        -> Send SMS:  UPLOAD ALL")
    print(f"  5. Upload Specific   -> Send SMS:  UPLOAD <filename>")
    print("#"*70 + "\n")
    print("Ready and awaiting connections from Neoway N706B... (Press Ctrl+C to stop)\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping server...")
        server.server_close()

if __name__ == "__main__":
    main()

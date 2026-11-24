#!/usr/bin/env python3
"""
Neoway N706B Remote SD Card HTTP Receiver & FOTA Server
Author: Antigravity / Google DeepMind
Target: Neoway N706B OpenCPU BME688 AI Gas Scanner

This server provides three essential services:
1. Dual-Stack HTTP POST Receiver: Accepts dataset files from both IPv4 (127.0.0.1)
   and IPv6 (::1 / localhost) without Windows connection refused errors.
2. HTTP GET File Server: Serves firmware packages (e.g. nwy_open_app_fota.pkt)
   for remote FOTA updates over cellular LTE.
3. Built-in Cellular Tunnel (Pinggy Raw TCP): Automatically exposes this server
   to 4G LTE modules with zero proxy screening or browser caution pages.
"""

import os
import sys
import socket
import argparse
import hashlib
import subprocess
import shutil
import re
import time
from datetime import datetime
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn
from urllib.parse import urlparse, parse_qs

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.dirname(SCRIPT_DIR)
RECEIVE_DIR = os.path.join(SCRIPT_DIR, "received_datasets")
RELEASE_DIR = os.path.join(PROJECT_DIR, "release")

os.makedirs(RECEIVE_DIR, exist_ok=True)

ACTIVE_TUNNEL_URL = None

def get_local_ip():
    """Attempt to detect the primary local network IP address."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(('8.8.8.8', 80))
        ip = s.getsockname()[0]
    except Exception:
        ip = '127.0.0.1'
    finally:
        s.close()
    return ip

def kill_stale_ssh():
    """Clean up any old background ssh tunnel processes."""
    if sys.platform == "win32":
        try:
            subprocess.run(["taskkill", "/F", "/IM", "ssh.exe"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception:
            pass

def start_auto_tunnel(local_port):
    """Start automatic Pinggy raw TCP tunnel so the user doesn't need a 2nd window."""
    ssh_exe = shutil.which("ssh")
    if not ssh_exe and os.path.exists(r"C:\Windows\System32\OpenSSH\ssh.exe"):
        ssh_exe = r"C:\Windows\System32\OpenSSH\ssh.exe"

    if not ssh_exe:
        return None, None

    cmd = [ssh_exe, "-p", "443", "-o", "StrictHostKeyChecking=no", f"-R0:127.0.0.1:{local_port}", "tcp@a.pinggy.io"]
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        public_url = None
        lines_seen = []
        start_time = time.time()
        while time.time() - start_time < 15:
            line = proc.stdout.readline()
            if not line:
                break
            line_str = line.strip()
            lines_seen.append(line_str)
            match = re.search(r'tcp://([^\s:]+):(\d+)', line)
            if match:
                host = match.group(1)
                port = match.group(2)
                public_url = f"http://{host}:{port}/upload"
                break

        if not public_url:
            for l in lines_seen:
                if any(k in l.lower() for k in ["pro", "limit", "exist", "denied", "error", "multiple", "refused"]):
                    print(f"    [!] Tunnel note: {l}")
        return proc, public_url
    except Exception as e:
        return None, None

class ThreadedDualStackServer(ThreadingMixIn, HTTPServer):
    """Dual-stack server that accepts both IPv4 and IPv6 connections on Windows."""
    daemon_threads = True

    def __init__(self, server_address, RequestHandlerClass, bind_and_activate=True):
        try:
            self.address_family = socket.AF_INET6
            super().__init__(('::', server_address[1]), RequestHandlerClass, bind_and_activate=False)
            self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
            if bind_and_activate:
                self.server_bind()
                self.server_activate()
        except Exception:
            # Fallback to standard IPv4 if IPv6 dual-stack is unavailable
            self.address_family = socket.AF_INET
            super().__init__(server_address, RequestHandlerClass, bind_and_activate)

class N706BHTTPHandler(BaseHTTPRequestHandler):
    server_version = "Neoway-N706B-Receiver/1.0"

    def do_GET(self):
        """Handle file downloads (e.g. FOTA packages) and Web Dashboard."""
        parsed = urlparse(self.path)
        path = parsed.path.lstrip('/')

        # Root route: Serve HTML Web Dashboard
        if path == "" or path == "index.html":
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Connection", "close")
            self.end_headers()

            # Scan received datasets
            datasets = []
            if os.path.exists(RECEIVE_DIR):
                for f in sorted(os.listdir(RECEIVE_DIR), reverse=True):
                    fp = os.path.join(RECEIVE_DIR, f)
                    if os.path.isfile(fp):
                        sz = os.path.getsize(fp)
                        mtime = datetime.fromtimestamp(os.path.getmtime(fp)).strftime("%Y-%m-%d %H:%M:%S")
                        datasets.append((f, sz, mtime))

            # Scan firmware packages
            firmware_pkgs = []
            if os.path.exists(RELEASE_DIR):
                for f in sorted(os.listdir(RELEASE_DIR)):
                    if f.endswith(('.pkt', '.zip')):
                        fp = os.path.join(RELEASE_DIR, f)
                        sz = os.path.getsize(fp)
                        firmware_pkgs.append((f, sz))

            # Render HTML dashboard
            tunnel_banner = ""
            if ACTIVE_TUNNEL_URL:
                tunnel_banner = f"""
    <div style="background:#064e3b; border:2px solid #10b981; border-radius:8px; padding:16px; margin-bottom:20px;">
      <h3 style="color:#34d399; margin:0 0 8px 0; font-size:16px;">&#x2714; 4G Cellular Tunnel Active & Ready</h3>
      <p style="margin:0 0 6px 0; font-size:13px; color:#e2e8f0;">Send this SMS to your Neoway N706B device right now:</p>
      <div style="background:#022c22; padding:10px 14px; border-radius:6px; font-family:monospace; font-size:15px; color:#6ee7b7; font-weight:bold; word-break:break-all;">
        SERVER {ACTIVE_TUNNEL_URL}
      </div>
      <p style="margin:8px 0 0 0; font-size:13px; color:#a7f3d0;">Then send: <strong style="color:#fef08a;">UPLOAD ALL</strong> (or <strong style="color:#fef08a;">UPLOAD LATEST</strong>) to receive datasets!</p>
    </div>
"""

            html = f"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Neoway N706B Remote SD & FOTA Manager</title>
<style>
  body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; background: #0f172a; color: #f8fafc; margin: 0; padding: 24px; }}
  .container {{ max-width: 1000px; margin: 0 auto; }}
  h1 {{ color: #38bdf8; margin-bottom: 4px; }}
  .subtitle {{ color: #94a3b8; margin-top: 0; font-size: 14px; }}
  .card {{ background: #1e293b; border-radius: 10px; padding: 20px; margin-bottom: 24px; box-shadow: 0 4px 6px -1px rgba(0,0,0,0.3); border: 1px solid #334155; }}
  .badge-active {{ background: #059669; color: #fff; padding: 4px 10px; border-radius: 9999px; font-size: 12px; font-weight: 600; display: inline-block; }}
  .grid {{ display: grid; grid-template-columns: 1fr 1fr; gap: 16px; margin-top: 16px; }}
  .code-box {{ background: #020617; border: 1px solid #1e293b; border-radius: 6px; padding: 12px; font-family: monospace; font-size: 13px; color: #a5f3fc; overflow-x: auto; }}
  .cmd {{ background: #334155; padding: 2px 6px; border-radius: 4px; color: #facc15; font-weight: bold; font-family: monospace; }}
  table {{ width: 100%; border-collapse: collapse; margin-top: 12px; }}
  th, td {{ padding: 10px 12px; text-align: left; border-bottom: 1px solid #334155; font-size: 14px; }}
  th {{ color: #94a3b8; font-weight: 600; background: #0f172a; }}
  a {{ color: #38bdf8; text-decoration: none; }}
  a:hover {{ text-decoration: underline; }}
  .warn {{ background: #451a03; border-left: 4px solid #f97316; padding: 12px 16px; border-radius: 4px; color: #fed7aa; margin-top: 12px; font-size: 13px; }}
</style>
</head>
<body>
<div class="container">
  <div style="display: flex; justify-content: space-between; align-items: center; margin-bottom: 20px;">
    <div>
      <h1>Neoway N706B Control Server</h1>
      <p class="subtitle">Remote MicroSD Dataset Receiver & Cellular FOTA Management</p>
    </div>
    <span class="badge-active">SERVER ONLINE</span>
  </div>

  {tunnel_banner}

  <div class="card">
    <h3 style="margin-top:0; color:#e2e8f0;">Cellular Connectivity Instructions</h3>
    <div class="grid">
      <div>
        <h4 style="color:#38bdf8; margin-bottom:8px;">Live 4G Connection Endpoint</h4>
        <p style="font-size:13px; color:#cbd5e1; margin-top:0;">Active Server URL for Neoway N706B:</p>
        <div class="code-box">{ACTIVE_TUNNEL_URL if ACTIVE_TUNNEL_URL else "http://localhost:8080/upload"}</div>
        <p style="font-size:12px; color:#94a3b8; margin-top:8px;">(Raw TCP tunnel bypasses all cloud warning pages & screening)</p>
      </div>
      <div>
        <h4 style="color:#38bdf8; margin-bottom:8px;">SMS Command Quick Reference</h4>
        <table style="margin-top:0;">
          <tr><td><span class="cmd">LIST</span></td><td>List all datasets on MicroSD card</td></tr>
          <tr><td><span class="cmd">UPLOAD LATEST</span></td><td>Upload most recent recording</td></tr>
          <tr><td><span class="cmd">UPLOAD ALL</span></td><td>Queue and upload all recordings</td></tr>
          <tr><td><span class="cmd">STATUS</span></td><td>Check SD storage and battery</td></tr>
        </table>
      </div>
    </div>
  </div>

  <div class="card">
    <h3 style="margin-top:0; color:#e2e8f0;">Received Datasets ({len(datasets)})</h3>
    {"<p style='color:#64748b; font-size:14px;'>No datasets received yet. Send SMS 'UPLOAD ALL' from your phone to start receiving!</p>" if not datasets else f'''
    <table>
      <thead>
        <tr>
          <th>File Name</th>
          <th>Size</th>
          <th>Received At</th>
          <th>Action</th>
        </tr>
      </thead>
      <tbody>
        {"".join(f"<tr><td><strong>{f}</strong></td><td>{sz:,} bytes</td><td>{mt}</td><td><a href='/{f}' download>Download</a></td></tr>" for f, sz, mt in datasets)}
      </tbody>
    </table>
    '''}
  </div>

  <div class="card">
    <h3 style="margin-top:0; color:#e2e8f0;">Available FOTA Firmware Packages ({len(firmware_pkgs)})</h3>
    {"<p style='color:#64748b; font-size:14px;'>No firmware packages found in release/ folder.</p>" if not firmware_pkgs else f'''
    <table>
      <thead>
        <tr>
          <th>Package Name</th>
          <th>Size</th>
          <th>Direct Download Link</th>
        </tr>
      </thead>
      <tbody>
        {"".join(f"<tr><td><strong>{f}</strong></td><td>{sz:,} bytes</td><td><a href='/{f}'>http://localhost:8080/{f}</a></td></tr>" for f, sz in firmware_pkgs)}
      </tbody>
    </table>
    '''}
  </div>
</div>
</body>
</html>
"""
            self.wfile.write(html.encode("utf-8"))
            return

        # Handle browser GET /upload gracefully
        if path == "upload":
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(b"<html><body style='font-family:sans-serif;background:#0f172a;color:#f8fafc;padding:30px;'>"
                             b"<h2 style='color:#38bdf8;'>Neoway N706B Remote SD Upload Endpoint is ACTIVE!</h2>"
                             b"<p>This endpoint receives dataset file streams via <strong>HTTP POST</strong> from your 4G module.</p>"
                             b"<p><a href='/' style='color:#38bdf8;'>&larr; View Received Datasets Dashboard</a></p>"
                             b"</body></html>")
            return

        # Suppress 404 noise for browser icon probes
        if "favicon" in path or "apple-touch-icon" in path:
            self.send_response(204)
            self.end_headers()
            return

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
            self.send_response(411)
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

        # 4. Return HTTP 200 OK with JSON confirmation
        resp = f'{{"status":"ok","filename":"{filename}","bytes":{bytes_received},"sha256":"{file_hash}"}}\n'.encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(resp)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, format, *args):
        # Filter noisy browser icon probes
        first_line = args[0] if args else ""
        if any(icon in str(first_line) for icon in ["favicon", "apple-touch-icon"]):
            return
        ts = datetime.now().strftime("%H:%M:%S")
        print(f"[{ts}] [{self.client_address[0]}] {format % args}")

def main():
    global ACTIVE_TUNNEL_URL

    parser = argparse.ArgumentParser(description="Neoway N706B Remote SD Card & FOTA HTTP Server")
    parser.add_argument("--port", type=int, default=8080, help="Listen port (default: 8080)")
    parser.add_argument("--host", type=str, default="0.0.0.0", help="Listen host (default: 0.0.0.0)")
    parser.add_argument("--no-tunnel", action="store_true", help="Disable automatic cellular tunnel")
    args = parser.parse_args()

    tunnel_proc = None
    if not args.no_tunnel:
        kill_stale_ssh()
        print("\n[*] Initializing automatic secure cellular tunnel (Pinggy Raw TCP)...")
        tunnel_proc, public_url = start_auto_tunnel(args.port)
        if public_url:
            ACTIVE_TUNNEL_URL = public_url
            print(f"[+] Tunnel online! Public endpoint: {ACTIVE_TUNNEL_URL}")

    local_ip = get_local_ip()
    server = ThreadedDualStackServer((args.host, args.port), N706BHTTPHandler)

    print("\n" + "="*74)
    print("      NEOWAY N706B — REMOTE SD UPLOADER & FOTA HTTP SERVER")
    print("="*74)
    print(" [1] LOCAL WEB DASHBOARD (Open on this PC in Chrome/Edge):")
    print(f"     -> http://localhost:{args.port}   or   http://127.0.0.1:{args.port}")
    print()
    if ACTIVE_TUNNEL_URL:
        print(" [2] LIVE 4G CELLULAR LTE UPLOAD LINK (Direct Raw TCP — Zero Warning Pages):")
        print("     ====================================================================")
        print("     >>> SEND THIS EXACT SMS COMMAND TO YOUR BOARD NOW:")
        print(f"         SERVER {ACTIVE_TUNNEL_URL}")
        print("     ====================================================================")
        print("     >>> THEN SEND THIS SMS COMMAND TO UPLOAD DATASETS:")
        print("         UPLOAD ALL")
        print("     ====================================================================")
    else:
        print(" [2] MANUAL TUNNEL INSTRUCTIONS:")
        print("     Open a second PowerShell window and run:")
        print(f"       ssh -p 443 -o StrictHostKeyChecking=no -R0:127.0.0.1:{args.port} tcp@a.pinggy.io")
        print("     Then send SMS: SERVER http://<link>/upload")
    print()
    print(" [3] QUICK SMS CHEAT SHEET:")
    print("     * Set Server URL : SERVER <url>")
    print("     * List Files     : LIST")
    print("     * Upload Latest  : UPLOAD LATEST")
    print("     * Upload All     : UPLOAD ALL")
    print("     * Upload One     : UPLOAD <filename>")
    print("     * Device Status  : STATUS")
    print("="*74)
    print(f" Received Datasets Folder : {RECEIVE_DIR}")
    print(f" Firmware Release Folder  : {RELEASE_DIR}")
    print("="*74)
    print("Ready and awaiting connections from Neoway N706B... (Press Ctrl+C to stop)\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping server...")
        if tunnel_proc:
            try:
                tunnel_proc.terminate()
            except Exception:
                pass
        server.server_close()

if __name__ == "__main__":
    main()

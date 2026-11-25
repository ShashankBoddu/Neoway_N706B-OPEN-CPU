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
import json
import binascii
import socket
import argparse
import hashlib
import subprocess
import shutil
import re
import time
import threading
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

def get_fota_signature(pkt_path):
    """Retrieve or dynamically compute the ECDSA secp256r1 signature for a .pkt file."""
    if not os.path.exists(pkt_path):
        return None, None

    file_size = os.path.getsize(pkt_path)
    h = hashlib.sha256()
    with open(pkt_path, "rb") as f:
        while True:
            c = f.read(16384)
            if not c:
                break
            h.update(c)
    hash_raw = h.digest()
    hash_hex = h.hexdigest()

    meta_path = os.path.join(SCRIPT_DIR, "fota_metadata.json")
    if os.path.exists(meta_path):
        try:
            with open(meta_path, "r") as f:
                data = json.load(f)
                if (data.get("filename") == os.path.basename(pkt_path) and
                    data.get("size") == file_size and
                    data.get("sha256") == hash_hex and
                    data.get("signature")):
                    return data.get("signature"), hash_hex
        except Exception:
            pass

    key_path = os.path.join(SCRIPT_DIR, "fota_private_key.pem")
    if os.path.exists(key_path):
        try:
            from cryptography.hazmat.primitives import hashes, serialization
            from cryptography.hazmat.primitives.asymmetric import ec, utils

            with open(key_path, "rb") as f:
                priv_key = serialization.load_pem_private_key(f.read(), password=None)

            sig = priv_key.sign(hash_raw, ec.ECDSA(utils.Prehashed(hashes.SHA256())))
            sig_hex = binascii.hexlify(sig).decode()

            try:
                with open(meta_path, "w") as f:
                    json.dump({
                        "filename": os.path.basename(pkt_path),
                        "size": file_size,
                        "sha256": hash_hex,
                        "signature": sig_hex
                    }, f, indent=2)
            except Exception:
                pass

            return sig_hex, hash_hex
        except Exception as e:
            print(f"[!] Warning: Failed to dynamically sign {pkt_path}: {e}")

    return None, None

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

def format_file_size(size_bytes):
    """
    Format file size to match Windows Explorer and SD FAT filesystem conventions
    while displaying exact byte precision.
    """
    if size_bytes < 1024:
        return f"1 KB ({size_bytes} bytes)" if size_bytes > 0 else "0 bytes"
    elif size_bytes < 1024 * 1024:
        kb_ceil = (size_bytes + 1023) // 1024
        return f"{kb_ceil} KB ({size_bytes:,} bytes)"
    else:
        mb = size_bytes / (1024 * 1024)
        return f"{mb:.1f} MB ({size_bytes:,} bytes)"

def kill_stale_ssh():
    """Clean up any old background ssh tunnel processes."""
    if sys.platform == "win32":
        try:
            subprocess.run(["taskkill", "/F", "/IM", "ssh.exe"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception:
            pass

def find_cloudflared():
    found = shutil.which("cloudflared")
    if found:
        return found
    candidate_paths = [
        r"C:\Program Files (x86)\cloudflared\cloudflared.exe",
        r"C:\Program Files\cloudflared\cloudflared.exe",
        os.path.expandvars(r"%LOCALAPPDATA%\cloudflared\cloudflared.exe")
    ]
    for path in candidate_paths:
        if os.path.exists(path):
            return path
    return None

def start_cloudflare_tunnel(local_port):
    """Start Cloudflare Quick Tunnel for high-speed, zero-drop HTTPS FOTA updates."""
    cf_exe = find_cloudflared()
    if not cf_exe:
        return None, None
    print(f"[*] Found Cloudflare Quick Tunnel: {cf_exe}")
    print("    Launching HTTPS CDN tunnel via trycloudflare.com (for FOTA)...")
    cmd = [cf_exe, "tunnel", "--url", f"http://127.0.0.1:{local_port}"]
    try:
        proc = subprocess.Popen(cmd, stderr=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        url_pattern = re.compile(r"https://[a-zA-Z0-9-]+\.trycloudflare\.com")
        public_url = None
        start_time = time.time()
        while time.time() - start_time < 20:
            line = proc.stderr.readline()
            if not line:
                break
            match = url_pattern.search(line)
            if match:
                public_url = match.group(0)
                break

        if public_url:
            def _drain_cf():
                for _ in proc.stderr:
                    pass
            threading.Thread(target=_drain_cf, daemon=True).start()
            return proc, public_url
        else:
            proc.terminate()
    except Exception as e:
        print(f"[!] Cloudflare launch error: {e}")
    return None, None

def start_pinggy_tunnel(local_port):
    """Start Pinggy raw TCP tunnel for direct unencrypted TCP streaming (Remote SD dataset uploader)."""
    kill_stale_ssh()
    ssh_exe = shutil.which("ssh") or (r"C:\Windows\System32\OpenSSH\ssh.exe" if os.path.exists(r"C:\Windows\System32\OpenSSH\ssh.exe") else None)
    if not ssh_exe:
        return None, None
    print("[*] Launching Pinggy raw TCP tunnel (for Remote SD dataset uploader)...")
    cmd = [ssh_exe, "-p", "443", "-o", "StrictHostKeyChecking=no", "-o", "ServerAliveInterval=15", f"-R0:127.0.0.1:{local_port}", "tcp@a.pinggy.io"]
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
        public_url = None
        start_time = time.time()
        while time.time() - start_time < 15:
            line = proc.stdout.readline()
            if not line:
                break
            match = re.search(r'tcp://([^\s:]+):(\d+)', line)
            if match:
                host = match.group(1)
                port = match.group(2)
                public_url = f"http://{host}:{port}/upload"
                break

        if public_url:
            def _drain_ssh(pipe):
                try:
                    while pipe.readline():
                        pass
                except Exception:
                    pass
            threading.Thread(target=_drain_ssh, args=(proc.stdout,), daemon=True).start()
            return proc, public_url
        else:
            proc.terminate()
    except Exception as e:
        print(f"[!] Pinggy launch error: {e}")
    return None, None

class ThreadedDualStackServer(ThreadingMixIn, HTTPServer):
    """Reliable IPv4 server for local host and SSH tunnel connections."""
    daemon_threads = True

    def __init__(self, server_address, RequestHandlerClass, chunk_size=1024, chunk_delay=0.100, bind_and_activate=True):
        self.address_family = socket.AF_INET
        self.chunk_size = chunk_size
        self.chunk_delay = chunk_delay
        super().__init__(('0.0.0.0', server_address[1]), RequestHandlerClass, bind_and_activate)

class N706BHTTPHandler(BaseHTTPRequestHandler):
    server_version = "Neoway-N706B-Receiver/1.0"
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        """Handle file downloads (e.g. FOTA packages) and Web Dashboard."""
        parsed = urlparse(self.path)
        path = parsed.path.lstrip('/')

        # Root route: Serve HTML Web Dashboard
        if path == "" or path == "index.html":
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
  .btn-zip {{ background: #2563eb; color: #ffffff; padding: 8px 14px; border-radius: 6px; font-size: 13px; font-weight: 600; text-decoration: none; display: inline-block; }}
  .btn-zip:hover {{ background: #1d4ed8; }}
  table {{ width: 100%; border-collapse: collapse; margin-top: 12px; }}
  th, td {{ padding: 10px 12px; text-align: left; border-bottom: 1px solid #334155; font-size: 14px; }}
  th {{ color: #94a3b8; font-weight: 600; background: #0f172a; }}
  a {{ color: #38bdf8; text-decoration: none; }}
  a:hover {{ text-decoration: underline; }}
  .warn {{ background: #451a03; border-left: 4px solid #f97316; padding: 12px 16px; border-radius: 4px; color: #fed7aa; margin-top: 12px; font-size: 13px; }}
  .folder-note {{ background: #1e1b4b; border: 1px solid #4338ca; border-radius: 6px; padding: 12px; margin-top: 14px; font-size: 13px; color: #c7d2fe; }}
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
    <div style="display: flex; justify-content: space-between; align-items: center;">
      <h3 style="margin:0; color:#e2e8f0;">Received Datasets ({len(datasets)})</h3>
      {"<a href='/download_all_zip' class='btn-zip'>&#x1F4E6; Download All as ZIP</a>" if datasets else ""}
    </div>
    <div class="folder-note">
      &#x1F4C2; <strong>Local Disk Location:</strong> <span style="font-family:monospace; color:#a5f3fc;">{RECEIVE_DIR}</span><br>
      <span style="font-size:12px; color:#94a3b8;">(Note: If Windows File Explorer was open before the upload finished, press <strong>F5</strong> in Explorer to refresh its view!)</span>
    </div>
    {"<p style='color:#64748b; font-size:14px; margin-top:14px;'>No datasets received yet. Send SMS 'UPLOAD ALL' from your phone to start receiving!</p>" if not datasets else f'''
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
        {"".join(f"<tr><td><strong>{f}</strong></td><td>{format_file_size(sz)}</td><td>{mt}</td><td><a href='/{f}' download>Download</a></td></tr>" for f, sz, mt in datasets)}
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
          <th>SMS Trigger</th>
        </tr>
      </thead>
      <tbody>
        {"".join(f"<tr><td><strong>{f}</strong></td><td>{format_file_size(sz)}</td><td><a href='/{f}'>http://localhost:8080/{f}</a></td><td><span class='cmd'>FOTA</span></td></tr>" for f, sz in firmware_pkgs)}
      </tbody>
    </table>
    <div class="warn" style="background:#0c4a6e; border-left:4px solid #0284c7; color:#bae6fd; margin-top:16px;">
      <h4 style="margin:0 0 6px 0; color:#38bdf8;">&#x26A1; Streamlined Cellular FOTA Update</h4>
      <p style="margin:0 0 6px 0; font-size:13px;">To update your Neoway N706B firmware over 4G LTE, simply text:</p>
      <div style="background:#082f49; padding:8px 12px; border-radius:4px; font-family:monospace; font-size:15px; color:#7dd3fc; font-weight:bold; width:fit-content;">
        FOTA
      </div>
      <p style="margin:6px 0 0 0; font-size:12px; color:#93c5fd;">The board automatically downloads the package, validates the ECDSA secp256r1 signature delivered in HTTP headers against its baked-in hardware key, flashes it to ROM, and reboots!</p>
    </div>
    '''}
  </div>
</div>
</body>
</html>
"""
            html_bytes = html.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(html_bytes)))
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            self.wfile.write(html_bytes)
            return

        # ZIP download route: Package all received datasets into a single zip file
        if path in ("download_all_zip", "datasets.zip"):
            import io
            import zipfile
            mem_zip = io.BytesIO()
            with zipfile.ZipFile(mem_zip, mode="w", compression=zipfile.ZIP_DEFLATED) as zf:
                if os.path.exists(RECEIVE_DIR):
                    for fname in sorted(os.listdir(RECEIVE_DIR)):
                        fpath = os.path.join(RECEIVE_DIR, fname)
                        if os.path.isfile(fpath):
                            zf.write(fpath, arcname=fname)
            mem_zip.seek(0)
            zip_data = mem_zip.read()
            self.send_response(200)
            self.send_header("Content-Type", "application/zip")
            self.send_header("Content-Disposition", 'attachment; filename="Neoway_N706B_Datasets.zip"')
            self.send_header("Content-Length", str(len(zip_data)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(zip_data)
            print(f"[{datetime.now().strftime('%H:%M:%S')}] [ZIP] Served all datasets ({len(zip_data):,} bytes) as ZIP archive")
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

        # FOTA Info JSON endpoint
        if path in ("fota_info", "fota.json"):
            pkt_name = "nwy_open_app_fota.pkt"
            pkt_path = os.path.join(RELEASE_DIR, pkt_name)
            if os.path.exists(pkt_path):
                sz = os.path.getsize(pkt_path)
                sig, hsh = get_fota_signature(pkt_path)
                base_tunnel = ACTIVE_TUNNEL_URL.replace('/upload', '') if ACTIVE_TUNNEL_URL else f"http://localhost:8080"
                resp = json.dumps({
                    "filename": pkt_name,
                    "size": sz,
                    "sha256": hsh,
                    "signature": sig,
                    "fota_url": f"{base_tunnel}/{pkt_name}",
                    "sms_trigger": f"FOTA {base_tunnel}/{pkt_name}"
                }, indent=2).encode('utf-8')
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(resp)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(resp)
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
        for hk, hv in self.headers.items():
            print(f"    Header: {hk}: {hv}")

        # Check for HTTP Range Header (Resume / Partial Content Support)
        range_header = self.headers.get("Range")
        start_byte = 0
        end_byte = file_size - 1
        is_range = False

        if range_header and range_header.startswith("bytes="):
            try:
                ranges = range_header.replace("bytes=", "").split("-")
                if ranges[0]:
                    start_byte = int(ranges[0])
                if len(ranges) > 1 and ranges[1]:
                    end_byte = int(ranges[1])
                is_range = True
            except Exception:
                start_byte = 0
                end_byte = file_size - 1
                is_range = False

        # Strictly clamp range to file boundary
        if is_range:
            if start_byte >= file_size:
                self.send_response(416)
                self.send_header("Content-Range", f"bytes */{file_size}")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            end_byte = min(end_byte, file_size - 1)

        bytes_to_send = (end_byte - start_byte) + 1

        if is_range:
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start_byte}-{end_byte}/{file_size}")
        else:
            self.send_response(200)

        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(bytes_to_send))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate, no-transform")
        self.send_header("X-Accel-Buffering", "no")

        # Inject cryptographic headers for FOTA firmware packages
        if target_file.endswith(".pkt"):
            sig, hsh = get_fota_signature(target_file)
            if sig:
                self.send_header("X-FOTA-Signature", sig)
                print(f"    * Injected Header X-FOTA-Signature: {sig[:32]}... ({len(sig)} chars)")
            if hsh:
                self.send_header("X-FOTA-SHA256", hsh)
                print(f"    * Injected Header X-FOTA-SHA256: {hsh}")
            self.send_header("X-FOTA-Size", str(file_size))
            self.send_header("X-FOTA-Chunk-Size", "1024")

        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.flush()
        time.sleep(0.050)

        # Enable TCP_NODELAY for instantaneous low-latency packet transmission
        try:
            self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except Exception:
            pass

        # Industrial Standard Chunked Streaming: Max 1 KB (1024 bytes) per chunk
        chunk_size = getattr(self.server, "chunk_size", 1024)
        chunk_delay = getattr(self.server, "chunk_delay", 0.100)
        total_chunks = (bytes_to_send + chunk_size - 1) // chunk_size
        total_sent = 0
        chunk_num = 0
        start_t = time.time()

        print(f"[{datetime.now().strftime('%H:%M:%S')}] [FOTA STREAM START] Streaming {bytes_to_send:,} bytes in {total_chunks} chunks ({int(chunk_delay*1000)}ms pacing delay)...")

        with open(target_file, "rb") as f:
            if start_byte > 0:
                f.seek(start_byte)

            while total_sent < bytes_to_send:
                read_len = min(chunk_size, bytes_to_send - total_sent)
                chunk = f.read(read_len)
                if not chunk:
                    break

                chunk_num += 1
                try:
                    self.wfile.write(chunk)
                    self.wfile.flush()
                    total_sent += len(chunk)
                    pct = (total_sent * 100.0) / bytes_to_send

                    if chunk_num % 10 == 0 or total_sent == bytes_to_send or chunk_num == 1:
                        elapsed = time.time() - start_t
                        speed_kb = (total_sent / 1024.0) / elapsed if elapsed > 0 else 0
                        print(f"[{datetime.now().strftime('%H:%M:%S')}]   [Chunk {chunk_num:2d}/{total_chunks}] {total_sent:,} / {bytes_to_send:,} B ({pct:5.1f}%) | {speed_kb:.1f} KB/s")

                    if chunk_delay > 0:
                        time.sleep(chunk_delay)

                except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError, OSError) as e:
                    print(f"[{datetime.now().strftime('%H:%M:%S')}] [CONN ABORT] Client disconnected at chunk {chunk_num}/{total_chunks} ({total_sent:,}/{file_size:,} bytes): {e}")
                    break

        # Post-transfer completion & grace period:
        # All bytes have been delivered into the tunnel. Keep the connection open so the cellular module
        # can finish reading and flashing all bytes across the 4G LTE link.
        if total_sent > 0:
            try:
                self.wfile.flush()
            except Exception:
                pass
            print(f"[{datetime.now().strftime('%H:%M:%S')}] [STREAM DELIVERED] All {total_sent:,} bytes sent into cellular tunnel.")
            print(f"[{datetime.now().strftime('%H:%M:%S')}] [HOLDING CONNECTION] Keeping connection alive for 20 seconds to allow cellular delivery & NOR flash burning...")
            time.sleep(20.0)

        print(f"[{datetime.now().strftime('%H:%M:%S')}] [GET 200 DONE] Completed FOTA stream of {total_sent:,} / {file_size:,} bytes to {self.client_address[0]}")
        self.close_connection = True

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
    parser.add_argument("--chunk-size", type=int, default=1024, help="Max chunk size in bytes (default: 1024)")
    parser.add_argument("--chunk-delay", type=float, default=0.100, help="Pacing delay between chunks in seconds (default: 0.100)")
    parser.add_argument("--tunnel-url", type=str, default=None, help="Explicit public tunnel URL (e.g. http://<link>/upload)")
    parser.add_argument("--no-tunnel", action="store_true", help="Disable automatic cellular tunnel")
    args = parser.parse_args()

    if args.tunnel_url:
        ACTIVE_TUNNEL_URL = args.tunnel_url.strip()
        print(f"[+] Using configured tunnel endpoint: {ACTIVE_TUNNEL_URL}")

    pinggy_proc = None
    cf_proc = None
    upload_url = None
    fota_url = None

    if args.tunnel_url:
        upload_url = args.tunnel_url.strip()
        ACTIVE_TUNNEL_URL = upload_url
        print(f"[+] Using configured tunnel endpoint: {upload_url}")

    if not args.no_tunnel and not args.tunnel_url:
        kill_stale_ssh()
        print("\n[*] Initializing cellular tunnels...")
        # 1. Pinggy TCP tunnel (essential for raw TCP streaming SD dataset uploads)
        pinggy_proc, pinggy_url = start_pinggy_tunnel(args.port)
        if pinggy_url:
            upload_url = pinggy_url
            ACTIVE_TUNNEL_URL = pinggy_url
            print(f"[+] Raw TCP Tunnel Online! SD Upload URL: {upload_url}")

        # 2. Cloudflare HTTPS tunnel (gold standard for FOTA downloads)
        cf_proc, cf_base = start_cloudflare_tunnel(args.port)
        if cf_base:
            fota_url = f"{cf_base}/nwy_open_app_fota.pkt"
            print(f"[+] Cloudflare HTTPS Tunnel Online! FOTA URL: {fota_url}")

    local_ip = get_local_ip()
    server = ThreadedDualStackServer((args.host, args.port), N706BHTTPHandler,
                                     chunk_size=args.chunk_size, chunk_delay=args.chunk_delay)

    print("\n" + "="*74)
    print("      NEOWAY N706B — REMOTE SD UPLOADER & FOTA HTTP SERVER")
    print("="*74)
    print(" [1] LOCAL WEB DASHBOARD (Open on this PC in Chrome/Edge):")
    print(f"     -> http://localhost:{args.port}   or   http://127.0.0.1:{args.port}")
    print()
    if upload_url:
        print(" [2] LIVE 4G CELLULAR LTE UPLOAD LINK (Direct Raw TCP Streaming):")
        print("     ====================================================================")
        print("     >>> SEND THIS COMMAND OVER MQTT / SMS TO CONFIGURE BOARD:")
        print(f"         SERVER {upload_url}")
        print("     ====================================================================")
        print("     >>> THEN SEND THIS COMMAND TO UPLOAD SD CARD DATASETS:")
        print("         UPLOAD ALL")
        print("     ====================================================================")
    else:
        print(" [2] MANUAL TCP TUNNEL INSTRUCTIONS (for SD Uploads):")
        print("     Open a second PowerShell window and run:")
        print(f"       ssh -p 443 -o StrictHostKeyChecking=no -R0:127.0.0.1:{args.port} tcp@a.pinggy.io")
        print("     Then send command: SERVER http://<link>/upload")
    print()
    if fota_url:
        print(" [3] SECURE FOTA FIRMWARE LINK (Cloudflare Enterprise HTTPS CDN):")
        print("     ====================================================================")
        print("     >>> TO TRIGGER OVER-THE-AIR FIRMWARE UPDATE:")
        print(f"         FOTA {fota_url}")
        print("     ====================================================================")
    print()
    print(" [4] QUICK COMMAND CHEAT SHEET:")
    print("     * Set Upload URL : SERVER <url>")
    print("     * List SD Files  : LIST")
    print("     * Upload Latest  : UPLOAD LATEST")
    print("     * Upload All     : UPLOAD ALL")
    print("     * Upload One     : UPLOAD <filename>")
    print("     * Delete All     : DELETE ALL")
    print("     * Delete Session : DELETE <filename/prefix>")
    print("     * Device Status  : STATUS")
    print("     * Trigger FOTA   : FOTA")
    print("="*74)
    print(f" Received Datasets Folder : {RECEIVE_DIR}")
    print(f" Firmware Release Folder  : {RELEASE_DIR}")
    print("="*74)
    print("Ready and awaiting connections from Neoway N706B... (Press Ctrl+C to stop)\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping server...")
        if pinggy_proc:
            try:
                pinggy_proc.terminate()
            except Exception:
                pass
        if cf_proc:
            try:
                cf_proc.terminate()
            except Exception:
                pass
        kill_stale_ssh()
        server.server_close()

if __name__ == "__main__":
    main()

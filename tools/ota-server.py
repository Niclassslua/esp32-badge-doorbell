#!/usr/bin/env python3
"""
OTA firmware server for TR22 badge.

Serves a single .bin file over HTTP and automatically picks up new builds —
no restart needed. Each HEAD/GET request is logged with the request headers,
served firmware metadata, and response headers. Each GET request reads the
file fresh from disk, so just rebuild and power-cycle the badge.

Usage:
    python3 tools/ota-server.py firmware/native-esp32/build/tr22_custom.bin

Set OTA_FIRMWARE_URL in main/ota_config.h to:
    http://<your-LAN-IP>:8070/tr22_custom.bin

Find your LAN IP:
    macOS/Linux:  ip addr   or   ifconfig
    Windows:      ipconfig

Stop with Ctrl-C.
"""
import http.server
import os
import struct
import sys
import time

PORT = 8070

# ── ESP32 app-description parser ─────────────────────────────────────────────
# The firmware binary layout (no secure boot):
#   esp_image_header_t        24 bytes
#   esp_image_segment_header_t 8 bytes
#   esp_app_desc_t            starts at offset 32
#
# esp_app_desc_t relevant fields (see esp-idf esp_app_desc.h):
#   +0  magic_word  uint32  (must be ESP_APP_DESC_MAGIC_WORD = 0xABCD5432)
#   +4  secure_ver  uint32
#   +8  reserv1[2]  uint32[2]
#   +16 version     char[32]
#   +48 project     char[32]
#   +80 time        char[16]   ← __TIME__
#   +96 date        char[16]   ← __DATE__
#   +112 idf_ver    char[32]

_APP_DESC_OFFSET = 32
_APP_DESC_MAGIC  = 0xABCD5432

def _extract_app_desc(data: bytes):
    """Return (date_str, time_str) from a firmware binary, or (None, None)."""
    if len(data) < _APP_DESC_OFFSET + 112:
        return None, None
    magic = struct.unpack_from('<I', data, _APP_DESC_OFFSET)[0]
    if magic != _APP_DESC_MAGIC:
        return None, None
    base = _APP_DESC_OFFSET
    app_time = data[base+80:base+96].rstrip(b'\x00').decode('ascii', errors='replace').strip()
    app_date = data[base+96:base+112].rstrip(b'\x00').decode('ascii', errors='replace').strip()
    return app_date, app_time

if len(sys.argv) < 2:
    print(__doc__)
    print("usage: ota-server.py <path-to-firmware.bin>")
    sys.exit(1)

BIN_PATH = os.path.abspath(sys.argv[1])
if not os.path.isfile(BIN_PATH):
    print(f"error: file not found: {BIN_PATH}")
    sys.exit(1)

BIN_NAME = os.path.basename(BIN_PATH)


class FirmwareHandler(http.server.BaseHTTPRequestHandler):
    """Serves the firmware binary, reading it fresh from disk on every request."""

    request_count = 0

    def _load_binary(self):
        """Read binary from disk; return (data, mtime) or raise OSError."""
        with open(BIN_PATH, "rb") as f:
            data = f.read()
        mtime = os.path.getmtime(BIN_PATH)
        return data, mtime

    def _firmware_headers(self, data):
        """Return common response headers describing the firmware binary."""
        app_date, app_time = _extract_app_desc(data)
        headers = [
            ("Content-Type", "application/octet-stream"),
            ("Content-Length", str(len(data))),
            ("Content-Disposition", f'attachment; filename="{BIN_NAME}"'),
        ]
        if app_date:
            headers.append(("X-App-Date", app_date))
        if app_time:
            headers.append(("X-App-Time", app_time))
        return headers

    def _send_firmware_headers(self, data):
        """Send common headers describing the firmware binary."""
        headers = self._firmware_headers(data)
        for name, value in headers:
            self.send_header(name, value)
        return headers

    def _next_request_id(self):
        type(self).request_count += 1
        return type(self).request_count

    def _log_request(self):
        """Print a concise trace of what the badge/client asked for."""
        request_id = self._next_request_id()
        now = time.strftime("%H:%M:%S")
        host, port = self.client_address
        print(f"\n[{now}] OTA request #{request_id}: {self.command} {self.path}", flush=True)
        print(f"  client: {host}:{port}", flush=True)

        if self.headers:
            print("  request headers:", flush=True)
            for name, value in self.headers.items():
                print(f"    {name}: {value}", flush=True)
        else:
            print("  request headers: (none)", flush=True)

        app_version = self.headers.get("X-TR22-App-Version")
        app_date = self.headers.get("X-TR22-App-Date")
        app_time = self.headers.get("X-TR22-App-Time")
        ota_check = self.headers.get("X-TR22-OTA-Check")
        if ota_check or app_version or app_date or app_time:
            print(
                "  badge build: "
                f"ota-check={ota_check or '?'}  "
                f"version={app_version or '?'}  "
                f"built={app_date or '?'} {app_time or '?'}",
                flush=True,
            )

        return request_id

    def _log_response(self, request_id, status, headers, data=None, mtime=None, body_note=None):
        """Print what the OTA server sent back."""
        print(f"  response #{request_id}: HTTP {status}", flush=True)
        if data is not None:
            app_date, app_time = _extract_app_desc(data)
            app_build = f"{app_date} {app_time}" if app_date and app_time else "unknown"
            size_kb = len(data) / 1024.0
            if mtime is not None:
                disk_mtime = time.strftime("%Y-%m-%d %H:%M:%S %z", time.localtime(mtime))
                print(
                    f"  firmware: {len(data)} bytes ({size_kb:.1f} KB), "
                    f"app={app_build}, file-mtime={disk_mtime}",
                    flush=True,
                )
            else:
                print(
                    f"  firmware: {len(data)} bytes ({size_kb:.1f} KB), app={app_build}",
                    flush=True,
                )
        if headers:
            print("  response headers:", flush=True)
            for name, value in headers:
                print(f"    {name}: {value}", flush=True)
        if body_note:
            print(f"  body: {body_note}", flush=True)

    def _send_logged_error(self, request_id, status, message):
        self.send_error(status, message)
        print(f"  response #{request_id}: HTTP {status} {message}", flush=True)

    def do_HEAD(self):
        """Let the badge check version metadata without downloading the binary."""
        request_id = self._log_request()
        if self.path != f"/{BIN_NAME}":
            self._send_logged_error(request_id, 404, "Not found")
            return
        try:
            data, mtime = self._load_binary()
        except OSError as e:
            self._send_logged_error(request_id, 500, str(e))
            return
        self.send_response(200)
        headers = self._send_firmware_headers(data)
        self.end_headers()
        self._log_response(request_id, 200, headers, data, mtime, "metadata only; no body")

    def do_GET(self):
        request_id = self._log_request()
        if self.path != f"/{BIN_NAME}":
            self._send_logged_error(request_id, 404, "Not found")
            return

        try:
            data, mtime = self._load_binary()
        except OSError as e:
            self._send_logged_error(request_id, 500, str(e))
            return

        app_date, app_time = _extract_app_desc(data)
        ver_str = f"{app_date} {app_time}" if app_date and app_time else "unknown version"

        self.send_response(200)
        headers = self._send_firmware_headers(data)
        self.end_headers()
        self._log_response(request_id, 200, headers, data, mtime, "starting firmware transfer")
        try:
            self.wfile.write(data)
            print(f"  body #{request_id}: served {len(data)} bytes ({ver_str})", flush=True)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            # ESP32 OTA clients may close the socket as soon as they have the image.
            print(
                f"  body #{request_id}: client closed connection during transfer "
                f"({ver_str})",
                flush=True,
            )

    def log_message(self, fmt, *args):
        # Suppress the default per-request line; we print our own above.
        pass


print(f"  file : {BIN_PATH}")
print(f"  URL  : http://0.0.0.0:{PORT}/{BIN_NAME}")
print("  Reads file fresh on every request — just rebuild and power-cycle the badge.")
print("  Ctrl-C to stop\n")

with http.server.HTTPServer(("", PORT), FirmwareHandler) as httpd:
    httpd.serve_forever()

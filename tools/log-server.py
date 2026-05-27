#!/usr/bin/env python3
"""
Log shipping server for TR22 badge.

Accepts HTTP POST /logs with a JSON body from the badge firmware, pretty-prints
each batch to the terminal with ANSI colours, and appends raw JSON-lines to a
per-device log file on disk.

Usage:
    python3 tools/log-server.py [--port 8071] [--out logs/]

    Default port : 8071
    Default out  : logs/   (created automatically)

Set LOG_SERVER_URL in firmware/native-esp32/main/ota_config.h to:
    http://<your-LAN-IP>:8071/logs

Find your LAN IP:
    macOS/Linux:  ip addr   or   ifconfig
    Windows:      ipconfig

Stop with Ctrl-C.
"""

import argparse
import datetime
import http.server
import json
import os
import sys

# ── ANSI colour helpers ───────────────────────────────────────────────────────

RESET  = "\033[0m"
BOLD   = "\033[1m"
DIM    = "\033[2m"
RED    = "\033[31m"
YELLOW = "\033[33m"
GREEN  = "\033[32m"
CYAN   = "\033[36m"
BLUE   = "\033[34m"
MAGENTA= "\033[35m"
WHITE  = "\033[37m"

_LEVEL_COLOR = {
    "E": RED,
    "W": YELLOW,
    "I": GREEN,
    "D": CYAN,
    "V": DIM,
    "?": WHITE,
}

_RESET_REASON = {
    1:  "POWERON",
    2:  "EXT_SYS",
    3:  "SW",
    4:  "PANIC",
    5:  "INT_WDT",
    6:  "TASK_WDT",
    7:  "WDT",
    8:  "DEEPSLEEP",
    9:  "BROWNOUT",
    10: "SDIO",
    11: "USB",
    12: "JTAG",
    13: "EFUSE",
    14: "PWR_GLITCH",
    15: "CPU_LOCKUP",
}

# Devices we've already printed a header for this run
_seen_devices: set = set()

# ── Pretty printer ────────────────────────────────────────────────────────────

def _reset_name(code: int) -> str:
    return _RESET_REASON.get(code, f"UNKNOWN({code})")


def _pretty_print(data: dict) -> None:
    dev = data.get("device", {})
    mac = dev.get("mac", "??:??:??:??:??:??")
    seq = data.get("seq", "?")

    # Separator line
    now = datetime.datetime.now().strftime("%H:%M:%S")
    print(f"\n{BOLD}{BLUE}━━━ [{now}] {mac}  seq={seq} ━━━{RESET}")

    # Device block on first batch or every time (easy to change)
    if mac not in _seen_devices:
        _seen_devices.add(mac)
        chip_names = {0: "ESP32", 1: "ESP32-S2", 9: "ESP32-S3",
                      5: "ESP32-C3", 12: "ESP32-C2", 13: "ESP32-C6",
                      16: "ESP32-H2"}
        chip_name = chip_names.get(dev.get("chip_model", -1),
                                   f"chip#{dev.get('chip_model','?')}")
        print(f"  {BOLD}chip    {RESET}: {chip_name}  "
              f"cores={dev.get('chip_cores','?')}  "
              f"rev={dev.get('chip_rev','?')}")
        print(f"  {BOLD}idf     {RESET}: {dev.get('idf_ver','?')}")
        print(f"  {BOLD}app     {RESET}: {dev.get('app_version','?')}  "
              f"{dev.get('app_date','?')} {dev.get('app_time','?')}")
        print(f"  {BOLD}reset   {RESET}: {_reset_name(dev.get('reset_reason', 0))}")

    # Runtime stats
    heap      = data.get("heap_free", 0)
    heap_min  = data.get("heap_free_min", 0)
    uptime_s  = data.get("uptime_ms", 0) / 1000.0
    dropped   = data.get("dropped_bytes", 0)

    stats = (f"  heap={heap//1024}KB  min={heap_min//1024}KB  "
             f"uptime={uptime_s:.1f}s")
    if dropped:
        stats += f"  {RED}{BOLD}dropped={dropped}B{RESET}"
    print(stats)

    # Log entries
    logs = data.get("logs", [])
    if not logs:
        print(f"  {DIM}(no log entries in this batch){RESET}")
    else:
        for entry in logs:
            lvl  = entry.get("level", "?")
            tag  = entry.get("tag",   "")
            msg  = entry.get("msg",   "")
            ts   = entry.get("ts_ms", 0)
            color = _LEVEL_COLOR.get(lvl, WHITE)
            print(f"  {color}{lvl}{RESET} {DIM}({ts:>7}){RESET} "
                  f"{MAGENTA}{tag}{RESET}: {msg}")


# ── File saver ────────────────────────────────────────────────────────────────

def _save_to_file(data: dict, out_dir: str) -> None:
    dev = data.get("device", {})
    mac = dev.get("mac", "unknown").replace(":", "")
    date_str = datetime.date.today().isoformat()   # YYYY-MM-DD

    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, f"{mac}_{date_str}.jsonl")

    # Annotate with server-side timestamp before saving
    record = dict(data)
    record["received_at"] = datetime.datetime.utcnow().isoformat() + "Z"

    with open(path, "a", encoding="utf-8") as f:
        f.write(json.dumps(record, separators=(",", ":")) + "\n")


# ── HTTP handler ──────────────────────────────────────────────────────────────

class LogHandler(http.server.BaseHTTPRequestHandler):
    out_dir: str = "logs"

    def do_POST(self) -> None:
        if self.path != "/logs":
            self.send_error(404, "Only POST /logs is supported")
            return

        length = int(self.headers.get("Content-Length", 0))
        if length == 0:
            self.send_error(400, "Empty body")
            return

        body = self.rfile.read(length)
        try:
            data = json.loads(body)
        except json.JSONDecodeError as exc:
            self.send_error(400, f"JSON parse error: {exc}")
            return

        self.send_response(200)
        self.end_headers()

        try:
            _pretty_print(data)
            _save_to_file(data, self.out_dir)
        except Exception as exc:      # pragma: no cover
            print(f"{RED}[server error] {exc}{RESET}", file=sys.stderr)

    def log_message(self, fmt: str, *args) -> None:
        # Suppress the default per-request line; we print our own above.
        pass


# ── Entry point ───────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="TR22 badge log shipping server",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument("--port", type=int, default=8071,
                        help="TCP port to listen on (default: 8071)")
    parser.add_argument("--out", default="logs",
                        help="Directory for JSONL log files (default: logs/)")
    args = parser.parse_args()

    LogHandler.out_dir = args.out
    os.makedirs(args.out, exist_ok=True)

    print(f"{BOLD}TR22 log server{RESET}")
    print(f"  listening : http://0.0.0.0:{args.port}/logs")
    print(f"  saving to : {os.path.abspath(args.out)}/")
    print(f"  Ctrl-C to stop\n")

    with http.server.HTTPServer(("", args.port), LogHandler) as httpd:
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print(f"\n{DIM}Stopped.{RESET}")


if __name__ == "__main__":
    main()

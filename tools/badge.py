#!/usr/bin/env python3
"""
badge — CLI tool for the TR22 door-sign / doorbell badge.

Commands:
    status              Show current sign state and display text.
    ring                Set the sign to "Bitte klingeln".
    dnd                 Set the sign to "Nicht Stören".
    text  <MESSAGE>     Set the custom display text.
                        Uppercase only (badge font limitation).
    led <#RRGGBB|off>   Set or clear the custom steady LED color.
    clear               Clear the custom display text.
    ota                 Reboot the badge into recovery so it checks/applies OTA.
    listen              Run a local HTTP server that receives doorbell
                        webhook POSTs from the badge and prints them.

Options:
    --host HOST         Badge IP or hostname  [env: BADGE_HOST, default: badge.local]
    --port PORT         Badge HTTP port       [default: 80]
    --listen-port PORT  Local port for 'listen' command  [default: 3000]

Examples:
    badge.py status
    badge.py dnd
    badge.py text "BACK AT 3PM" --led-color "#0040ff"
    badge.py led off
    badge.py ota --build
    badge.py listen

    BADGE_HOST=192.168.1.42 badge.py ring
    badge.py --host 192.168.1.42 text "IN A MEETING"
"""

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer


# ── HTTP helpers ─────────────────────────────────────────────────────────────

def _url(host: str, port: int, path: str) -> str:
    base = f"http://{host}" if port == 80 else f"http://{host}:{port}"
    return base + path


def _get(host: str, port: int, path: str) -> dict:
    url = _url(host, port, path)
    try:
        with urllib.request.urlopen(url, timeout=8) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.URLError as exc:
        _die(f"Cannot reach badge at {host}:{port} — {exc.reason}")
    except json.JSONDecodeError:
        _die("Badge returned non-JSON response.")


def _post(host: str, port: int, path: str, payload: dict) -> dict:
    url = _url(host, port, path)
    body = json.dumps(payload).encode()
    req = urllib.request.Request(
        url,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=8) as resp:
            return json.loads(resp.read().decode())
    except urllib.error.URLError as exc:
        _die(f"Cannot reach badge at {host}:{port} — {exc.reason}")
    except json.JSONDecodeError:
        _die("Badge returned non-JSON response.")


def _die(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


# ── Commands ─────────────────────────────────────────────────────────────────

def cmd_status(host: str, port: int) -> None:
    data = _get(host, port, "/status")
    state  = data.get("state", "unknown")
    label  = data.get("label", state)
    text   = data.get("custom_text", "")
    led    = data.get("led_color")

    icon   = "🟢" if state == "please_ring" else "🔴" if state == "do_not_disturb" else "❓"
    print(f"{icon}  {label}")
    if text:
        print(f"    {text}")
    print(f"    LED: {led if led else 'default'}")


def cmd_set_state(host: str, port: int, state: str) -> None:
    data = _post(host, port, "/status", {"state": state})
    if data.get("ok"):
        icon = "🟢" if state == "please_ring" else "🔴"
        label = "Bitte klingeln" if state == "please_ring" else "Nicht Stören"
        print(f"{icon}  Sign set to: {label}.")
    else:
        _die(data.get("error", "unknown error"))


def cmd_text(host: str, port: int, message: str, led_color: str | None = None) -> None:
    payload = {"text": message}
    if led_color is not None:
        payload["led_color"] = led_color
    data = _post(host, port, "/custom", payload)
    if data.get("ok"):
        print(f"✏️   Display text set to: {message!r}")
        if led_color is not None:
            print(f"💡  LED color set to: {led_color}")
    else:
        _die(data.get("error", "unknown error"))


def cmd_clear(host: str, port: int) -> None:
    data = _post(host, port, "/display", {"text": ""})
    if data.get("ok"):
        print("✏️   Display text cleared.")
    else:
        _die(data.get("error", "unknown error"))

def cmd_led(host: str, port: int, led_color: str) -> None:
    data = _post(host, port, "/custom", {"led_color": led_color})
    if data.get("ok"):
        print(f"💡  LED color set to: {led_color}")
    else:
        _die(data.get("error", "unknown error"))


def cmd_ota(host: str, port: int, build_first: bool) -> None:
    if build_first:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        build_script = os.path.join(script_dir, "build-firmware.sh")
        print("Building firmware...")
        try:
            subprocess.check_call([build_script])
        except subprocess.CalledProcessError as exc:
            _die(f"build failed with exit code {exc.returncode}")

    data = _post(host, port, "/ota", {})
    if data.get("ok"):
        print("OTA requested; badge is rebooting into recovery.")
    else:
        _die(data.get("error", "unknown error"))


def cmd_listen(listen_port: int, badge_host: str, badge_port: int) -> None:
    """
    Start a local HTTP server that receives doorbell POSTs.

    Point BADGE_DOORBELL_URL in badge_config.h at:
        http://<THIS-MACHINE-IP>:<listen_port>/doorbell
    """
    blink_command = (
        "end=$((SECONDS+10)); while [ $SECONDS -lt $end ]; do "
        "./blink1-tool --white -b 150; sleep 1; ./blink1-tool --off; sleep 1; "
        "done"
    )
    blink_process = None
    blink_busy_until = 0.0

    class Handler(BaseHTTPRequestHandler):
        def do_POST(self):  # noqa: N802
            nonlocal blink_busy_until, blink_process

            length  = int(self.headers.get("Content-Length", 0))
            raw     = self.rfile.read(length)

            try:
                payload = json.loads(raw.decode()) if raw else {}
            except json.JSONDecodeError:
                payload = {"raw": raw.decode(errors="replace")}

            event = payload.get("event", "?")
            print(f"\n🔔  DOORBELL  [{self.client_address[0]}]  event={event!r}")
            if len(payload) > 1:
                for k, v in payload.items():
                    if k != "event":
                        print(f"    {k}: {v}")

            now = time.monotonic()
            blink_running = blink_process is not None and blink_process.poll() is None
            if not blink_running and now >= blink_busy_until:
                blink_busy_until = now + 10.0
                blink_process = subprocess.Popen(["/bin/zsh", "-c", blink_command])
            else:
                print("    blink1 alert already running; skipped")

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"ok":true}')

        def log_message(self, fmt, *args):  # silence default access log
            pass

    server = HTTPServer(("", listen_port), Handler)
    print(f"🎧  Listening for doorbell events on port {listen_port}")
    print(f"    Set BADGE_DOORBELL_URL to http://<YOUR-IP>:{listen_port}/doorbell")
    print("    Press Ctrl-C to stop.\n")

    # Show a quick badge status so the user knows it's reachable.
    try:
        data   = _get(badge_host, badge_port, "/status")
        state  = data.get("state", "unknown")
        label  = data.get("label", state)
        text   = data.get("custom_text", "")
        icon   = "🟢" if state == "please_ring" else "🔴"
        print(f"Badge status: {icon} {label}" + (f"  ({text})" if text else ""))
    except SystemExit:
        print("(badge unreachable — doorbell listener still active)")

    print()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


# ── Argument parsing ─────────────────────────────────────────────────────────

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="badge.py",
        description="Control the TR22 door-sign / doorbell badge.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__.split("Examples:")[1].strip() if "Examples:" in __doc__ else "",
    )
    p.add_argument(
        "--host",
        default=os.environ.get("BADGE_HOST", "badge.local"),
        metavar="HOST",
        help="Badge IP / hostname  (env: BADGE_HOST, default: badge.local)",
    )
    p.add_argument(
        "--port",
        type=int,
        default=int(os.environ.get("BADGE_PORT", "80")),
        metavar="PORT",
        help="Badge HTTP port  (default: 80)",
    )

    sub = p.add_subparsers(dest="command", metavar="COMMAND")
    sub.required = True

    sub.add_parser("status",    help="Show current sign state and display text.")
    sub.add_parser("ring",      help='Set the sign to "Bitte klingeln".')
    sub.add_parser("dnd",       help='Set the sign to "Nicht Stören".')
    txt = sub.add_parser("text", help="Set the custom display text.")
    txt.add_argument("message", help="Text to show on the badge display (uppercase recommended).")
    txt.add_argument("--led-color", help='Optional custom steady LED color, e.g. "#00ff80" or "off".')

    led = sub.add_parser("led", help="Set or clear the custom steady LED color.")
    led.add_argument("color", help='LED color as "#RRGGBB", "RRGGBB", or "off".')

    sub.add_parser("clear", help="Clear the custom display text.")

    ota = sub.add_parser("ota", help="Reboot into recovery to check/apply OTA.")
    ota.add_argument(
        "--build",
        action="store_true",
        help="Build the normal firmware before triggering OTA.",
    )

    lst = sub.add_parser("listen", help="Run a webhook server to receive doorbell events.")
    lst.add_argument(
        "--listen-port",
        type=int,
        default=int(os.environ.get("BADGE_LISTEN_PORT", "3000")),
        metavar="PORT",
        help="Local port to listen on  (env: BADGE_LISTEN_PORT, default: 3000)",
    )

    return p


def main() -> None:
    parser = build_parser()
    args   = parser.parse_args()

    host = args.host
    port = args.port

    if args.command == "status":
        cmd_status(host, port)
    elif args.command == "ring":
        cmd_set_state(host, port, "please_ring")
    elif args.command == "dnd":
        cmd_set_state(host, port, "do_not_disturb")
    elif args.command == "text":
        cmd_text(host, port, args.message, args.led_color)
    elif args.command == "led":
        cmd_led(host, port, args.color)
    elif args.command == "clear":
        cmd_clear(host, port)
    elif args.command == "ota":
        cmd_ota(host, port, args.build)
    elif args.command == "listen":
        cmd_listen(args.listen_port, host, port)


if __name__ == "__main__":
    main()

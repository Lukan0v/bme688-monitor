#!/usr/bin/env python3
"""BME688 Sync — runs on the Pi, pushes data to Railway relay every 2s.

Usage:
  python3 sync.py --url https://YOUR-APP.up.railway.app --key YOUR_API_KEY

Or set environment variables:
  export RAILWAY_URL=https://YOUR-APP.up.railway.app
  export BME688_API_KEY=YOUR_API_KEY
  python3 sync.py
"""

import os
import sys
import json
import time
import csv
import argparse
from datetime import datetime, timedelta
from urllib.request import Request, urlopen
from urllib.error import URLError

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB_DATA = os.path.join(BASE_DIR, "web_data.json")
SETTINGS_CONF = os.path.join(BASE_DIR, "settings.conf")
SETTINGS_RELOAD = os.path.join(BASE_DIR, "settings_reload")
DATA_DIR = os.path.join(BASE_DIR, "data")
SYNC_STATUS = os.path.join(BASE_DIR, "sync_status")


def post_json(url, data, api_key):
    """POST JSON to relay server."""
    body = json.dumps(data).encode("utf-8")
    req = Request(url, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    req.add_header("X-API-Key", api_key)
    try:
        resp = urlopen(req, timeout=5)
        return json.loads(resp.read())
    except URLError as e:
        print(f"  Fehler: {e}", file=sys.stderr)
        return None


def get_json(url, api_key):
    """GET JSON from relay server."""
    req = Request(url)
    req.add_header("X-API-Key", api_key)
    try:
        resp = urlopen(req, timeout=5)
        return json.loads(resp.read())
    except URLError:
        return None


def read_web_data():
    """Read current sensor data from the C++ app."""
    try:
        with open(WEB_DATA, "r") as f:
            return json.load(f)
    except (FileNotFoundError, json.JSONDecodeError):
        return None


def read_settings():
    """Read settings.conf into a dict."""
    settings = {}
    try:
        with open(SETTINGS_CONF, "r") as f:
            for line in f:
                line = line.strip()
                if "=" in line:
                    k, v = line.split("=", 1)
                    settings[k.strip()] = v.strip()
    except FileNotFoundError:
        pass
    return settings


def write_settings(settings):
    """Write settings to settings.conf and signal reload."""
    with open(SETTINGS_CONF, "w") as f:
        for k, v in settings.items():
            f.write(f"{k}={v}\n")
    with open(SETTINGS_RELOAD, "w") as f:
        f.write("1")


def load_history(days=7):
    """Load CSV history data."""
    result = []
    today = datetime.now().date()
    for d in range(days):
        date = today - timedelta(days=d)
        fname = os.path.join(DATA_DIR, date.strftime("%Y-%m-%d.csv"))
        if not os.path.exists(fname):
            continue
        with open(fname, "r") as f:
            for row in csv.reader(f):
                if len(row) < 5:
                    continue
                try:
                    h, m = map(int, row[0].split(":"))
                    result.append({
                        "date": str(date),
                        "time": row[0],
                        "minute": h * 60 + m,
                        "temperature": float(row[1]),
                        "humidity": float(row[2]),
                        "pressure": float(row[3]),
                        "eco2": float(row[4]),
                    })
                except (ValueError, IndexError):
                    continue
    return result


def main():
    parser = argparse.ArgumentParser(description="BME688 Pi → Railway sync")
    parser.add_argument("--url", default=os.environ.get("RAILWAY_URL", ""),
                        help="Railway server URL")
    parser.add_argument("--key", default=os.environ.get("BME688_API_KEY", "changeme"),
                        help="API key")
    args = parser.parse_args()

    if not args.url:
        print("Fehler: Keine Railway URL angegeben!")
        print("  python3 sync.py --url https://DEIN-APP.up.railway.app --key DEIN_KEY")
        sys.exit(1)

    url = args.url.rstrip("/")
    key = args.key
    print(f"BME688 Sync gestartet")
    print(f"  Relay: {url}")
    print(f"  Intervall: 2s live, 60s history")

    last_history_push = 0
    cycle = 0

    while True:
        try:
            # 1. Push live data (every 2s)
            live = read_web_data()
            if live:
                result = post_json(f"{url}/api/push/live", live, key)
                # Write sync status for the desktop app
                try:
                    with open(SYNC_STATUS, "w") as sf:
                        sf.write(f"{int(time.time())} {'ok' if result else 'err'}\n")
                except OSError:
                    pass
                if cycle % 30 == 0:  # Log every 60s
                    ts = live.get("timestamp", "?")
                    print(f"  [{ts}] Live push OK — "
                          f"{live['current']['temperature']:.1f}°C, "
                          f"{live['current']['humidity']:.0f}%, "
                          f"{live['current']['pressure']:.1f}hPa")

            # 2. Push settings (every 10s)
            if cycle % 5 == 0:
                settings = read_settings()
                if settings:
                    post_json(f"{url}/api/push/settings", settings, key)

            # 3. Push history (every 60s)
            now = time.time()
            if now - last_history_push >= 60:
                last_history_push = now
                history = load_history(days=7)
                if history:
                    post_json(f"{url}/api/push/history", history, key)
                    if cycle % 30 == 0:
                        print(f"  History push: {len(history)} Punkte")

            # 4. Check for pending settings from iOS app (every 2s)
            if cycle % 1 == 0:
                pending = get_json(f"{url}/api/pending_settings", key)
                if pending and pending.get("pending"):
                    new_settings = pending["settings"]
                    print(f"  Neue Einstellungen von iOS empfangen!")
                    # Merge into current settings
                    current = read_settings()
                    current.update({k: str(v) for k, v in new_settings.items()})
                    write_settings(current)
                    # Acknowledge
                    post_json(f"{url}/api/ack_settings", {}, key)
                    print(f"  Einstellungen angewendet und bestaetigt")

            cycle += 1
            time.sleep(2)

        except KeyboardInterrupt:
            print("\nSync gestoppt.")
            break
        except Exception as e:
            print(f"  Fehler: {e}", file=sys.stderr)
            time.sleep(5)


if __name__ == "__main__":
    main()

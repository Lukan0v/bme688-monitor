#!/usr/bin/env python3
"""BME688 Monitor Web Interface — Flask server running on the Raspberry Pi."""

import json
import os
import csv
from datetime import datetime, timedelta
from flask import Flask, render_template, jsonify, request

app = Flask(__name__)

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB_DATA = os.path.join(BASE_DIR, "web_data.json")
SETTINGS_CONF = os.path.join(BASE_DIR, "settings.conf")
SETTINGS_RELOAD = os.path.join(BASE_DIR, "settings_reload")
DATA_DIR = os.path.join(BASE_DIR, "data")


def read_web_data():
    """Read current sensor data from the JSON file written by the C++ app."""
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
                    key, val = line.split("=", 1)
                    settings[key.strip()] = val.strip()
    except FileNotFoundError:
        pass
    return settings


def write_settings(settings: dict):
    """Write settings dict to settings.conf and signal the C++ app to reload."""
    with open(SETTINGS_CONF, "w") as f:
        for key, val in settings.items():
            f.write(f"{key}={val}\n")
    # Signal the C++ app to reload settings
    with open(SETTINGS_RELOAD, "w") as f:
        f.write("1")


def load_history_csv(days=7):
    """Load historical data from CSV files."""
    result = []
    today = datetime.now().date()
    for d in range(days):
        date = today - timedelta(days=d)
        fname = os.path.join(DATA_DIR, date.strftime("%Y-%m-%d.csv"))
        if not os.path.exists(fname):
            continue
        day_data = []
        with open(fname, "r") as f:
            for row in csv.reader(f):
                if len(row) < 5:
                    continue
                try:
                    time_str = row[0]
                    h, m = map(int, time_str.split(":"))
                    day_data.append({
                        "date": str(date),
                        "time": time_str,
                        "minute": h * 60 + m,
                        "temperature": float(row[1]),
                        "humidity": float(row[2]),
                        "pressure": float(row[3]),
                        "eco2": float(row[4]),
                    })
                except (ValueError, IndexError):
                    continue
        result.extend(day_data)
    return result


# --- Routes ---

@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/live")
def api_live():
    """Current sensor readings, trends, weather, settings."""
    data = read_web_data()
    if data is None:
        return jsonify({"error": "Keine Sensordaten verfügbar"}), 503
    return jsonify(data)


@app.route("/api/history")
def api_history():
    """Historical data from CSV logs."""
    days = request.args.get("days", 7, type=int)
    days = min(days, 30)
    data = load_history_csv(days)
    return jsonify(data)


@app.route("/api/settings", methods=["GET"])
def api_get_settings():
    return jsonify(read_settings())


@app.route("/api/settings", methods=["POST"])
def api_set_settings():
    """Update settings. Expects JSON body with key-value pairs."""
    updates = request.get_json()
    if not updates:
        return jsonify({"error": "Keine Daten"}), 400

    # Read current settings, merge updates
    settings = read_settings()

    # Validate and apply
    valid_keys = {
        "utc_offset_min": (int, -720, 840),
        "sensor_interval_ms": (int, 50, 2000),
        "log_days": (int, 1, 30),
        "gas_interval_s": (int, 5, 300),
        "heater_filter": (int, 0, 1),
        "heater_blanking_ms": (int, 250, 6000),
        "night_mode_auto": (int, 0, 1),
        "night_start_h": (int, 0, 23),
        "night_end_h": (int, 0, 23),
        "night_brightness": (int, 10, 100),
    }

    for key, value in updates.items():
        if key in valid_keys:
            typ, lo, hi = valid_keys[key]
            try:
                v = typ(value)
                v = max(lo, min(hi, v))
                settings[key] = str(v)
            except (ValueError, TypeError):
                continue

    write_settings(settings)
    return jsonify({"ok": True, "settings": settings})


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=8080, debug=False)

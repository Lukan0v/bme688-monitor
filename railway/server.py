#!/usr/bin/env python3
"""BME688 Monitor — Railway relay server.

Receives data from the Pi, serves it to the iOS app.
Runs on Railway (or any cloud host).
"""

import os
import time
import json
from flask import Flask, jsonify, request, abort

app = Flask(__name__)

# Simple API key auth — set via Railway environment variable
API_KEY = os.environ.get("BME688_API_KEY", "changeme")

# In-memory store (Railway containers are ephemeral, that's fine for live data)
store = {
    "live": None,
    "live_time": 0,
    "history": [],
    "history_time": 0,
    "settings": {},
    "settings_time": 0,
}


def check_auth():
    """Verify the API key from the Pi."""
    key = request.headers.get("X-API-Key") or request.args.get("key")
    if key != API_KEY:
        abort(401, "Ungültiger API-Key")


# ── Pi pushes data here ──────────────────────────────────

@app.route("/api/push/live", methods=["POST"])
def push_live():
    check_auth()
    store["live"] = request.get_json()
    store["live_time"] = time.time()
    return jsonify({"ok": True})


@app.route("/api/push/history", methods=["POST"])
def push_history():
    check_auth()
    store["history"] = request.get_json()
    store["history_time"] = time.time()
    return jsonify({"ok": True})


@app.route("/api/push/settings", methods=["POST"])
def push_settings():
    """Pi pushes current settings (for read-only display in the app)."""
    check_auth()
    store["settings"] = request.get_json()
    store["settings_time"] = time.time()
    return jsonify({"ok": True})


# ── iOS app reads from here ──────────────────────────────

@app.route("/api/live")
def get_live():
    if store["live"] is None:
        return jsonify({"error": "Keine Sensordaten. Pi offline?"}), 503
    # Add staleness info
    data = dict(store["live"])
    data["_relay_age_s"] = round(time.time() - store["live_time"], 1)
    return jsonify(data)


@app.route("/api/history")
def get_history():
    return jsonify(store["history"])


@app.route("/api/settings", methods=["GET"])
def get_settings():
    return jsonify(store["settings"])


@app.route("/api/settings", methods=["POST"])
def set_settings():
    """iOS app sends new settings — store them for Pi to pick up."""
    data = request.get_json()
    if not data:
        return jsonify({"error": "Keine Daten"}), 400
    store["pending_settings"] = data
    store["pending_settings_time"] = time.time()
    return jsonify({"ok": True, "settings": data})


@app.route("/api/pending_settings")
def get_pending_settings():
    """Pi polls this to check if the iOS app changed settings."""
    check_auth()
    pending = store.get("pending_settings")
    if pending and store.get("pending_settings_time", 0) > store.get("settings_time", 0):
        return jsonify({"pending": True, "settings": pending})
    return jsonify({"pending": False})


@app.route("/api/ack_settings", methods=["POST"])
def ack_settings():
    """Pi confirms it applied the settings."""
    check_auth()
    store["pending_settings"] = None
    return jsonify({"ok": True})


# ── Health check ─────────────────────────────────────────

@app.route("/")
def index():
    live_age = round(time.time() - store["live_time"], 1) if store["live_time"] else None
    return jsonify({
        "service": "BME688 Monitor Relay",
        "status": "online",
        "last_data_age_s": live_age,
        "has_data": store["live"] is not None,
    })


if __name__ == "__main__":
    port = int(os.environ.get("PORT", 8080))
    app.run(host="0.0.0.0", port=port)

#!/bin/bash
# Start the BME688 web interface
cd "$(dirname "$0")"
exec python3 server.py

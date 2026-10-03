#!/usr/bin/env bash
# Serve the app from this computer:  bash run.sh [port]
# Then open http://localhost:8768 here, or http://<this computer's address>:8768 on a phone on the same Wi-Fi.
cd "$(dirname "$0")/web"
PORT="${1:-8768}"
echo "Web VMU: http://localhost:$PORT"
hostname -I 2>/dev/null | tr ' ' '\n' | grep -E '^[0-9]+\.' | sed "s#.*#         http://&:$PORT#"
exec python3 -m http.server "$PORT"

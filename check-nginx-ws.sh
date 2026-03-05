#!/bin/bash
# Check that nginx forwards WebSocket headers to the backend.
# Run on the VPS: bash check-nginx-ws.sh

set -e
echo "=== Checking nginx config for WebSocket support ==="
if ! command -v nginx &>/dev/null; then
  echo "nginx not found. Install: sudo apt install nginx"
  exit 1
fi

CONF=$(nginx -T 2>/dev/null || true)
if echo "$CONF" | grep -q "proxy_set_header Sec-WebSocket-Key"; then
  echo "[OK] Sec-WebSocket-Key is present in nginx config"
else
  echo "[FAIL] Sec-WebSocket-Key is NOT found in nginx config"
  echo "Run: sudo bash deploy-web.sh"
  echo "Then: sudo systemctl reload nginx"
  exit 1
fi
if echo "$CONF" | grep -q "proxy_http_version 1.1"; then
  echo "[OK] proxy_http_version 1.1 is set"
else
  echo "[WARN] proxy_http_version 1.1 not found (required for WebSocket)"
fi
echo ""
echo "If connections still fail:"
echo "1. If using Cloudflare: Dashboard -> Network -> WebSockets = On"
echo "2. Restart nginx: sudo systemctl restart nginx"
echo "3. Ensure only one server block handles port 80 (run deploy-web.sh)"
echo "=== Done ==="

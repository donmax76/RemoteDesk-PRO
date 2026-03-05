#!/bin/bash
# Полный скрипт развёртывания: кладёте рядом с server.py и index.html, запускаете:
#   sudo bash deploy-web.sh
# Больше ничего не нужно — конфиг nginx встроен в скрипт.

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ ! -f "index.html" ]; then
  echo "Error: index.html not found in $SCRIPT_DIR"
  exit 1
fi

echo "=== Deploy Remote Desktop Web ==="
echo "Project dir: $SCRIPT_DIR"
echo ""

echo "[1/6] Installing nginx if needed..."
if ! command -v nginx &>/dev/null; then
  apt-get update -qq && apt-get install -y nginx
else
  echo "      nginx already installed"
fi

echo "[2/6] Creating /var/www/remote-desktop ..."
mkdir -p /var/www/remote-desktop

echo "[3/6] Copying index.html ..."
cp -f index.html /var/www/remote-desktop/
chown -R www-data:www-data /var/www/remote-desktop 2>/dev/null || true

echo "[4/6] Writing nginx config ..."
mkdir -p /etc/nginx/conf.d 2>/dev/null || true
cat > /etc/nginx/conf.d/websocket_upgrade.conf << 'MAPEOF'
map $http_upgrade $connection_upgrade {
    default upgrade;
    ''      close;
}
MAPEOF

cat > /etc/nginx/sites-available/remote-desktop << 'NGINX_EOF'
# Backend must get HTTP/1.1 and: Upgrade, Connection, Sec-WebSocket-Key, Sec-WebSocket-Version
server {
    listen 80 default_server;
    listen [::]:80 default_server;
    server_name _;
    root /var/www/remote-desktop;
    index index.html;

    # Force HTTP/1.1 to backend (WebSocket requires 1.1)
    proxy_http_version 1.1;

    location / {
        try_files $uri $uri/ /index.html;
    }

    location /client {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection $connection_upgrade;
        proxy_set_header Sec-WebSocket-Key $http_sec_websocket_key;
        proxy_set_header Sec-WebSocket-Version $http_sec_websocket_version;
        proxy_read_timeout 86400;
    }

    location /host {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto $scheme;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection $connection_upgrade;
        proxy_set_header Sec-WebSocket-Key $http_sec_websocket_key;
        proxy_set_header Sec-WebSocket-Version $http_sec_websocket_version;
        proxy_read_timeout 86400;
    }

    location /admin {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection $connection_upgrade;
        proxy_set_header Sec-WebSocket-Key $http_sec_websocket_key;
        proxy_set_header Sec-WebSocket-Version $http_sec_websocket_version;
        proxy_read_timeout 86400;
    }
}
NGINX_EOF

echo "[5/6] Enabling site, disabling others on port 80..."
ln -sf /etc/nginx/sites-available/remote-desktop /etc/nginx/sites-enabled/remote-desktop
rm -f /etc/nginx/sites-enabled/default 2>/dev/null || true
for f in /etc/nginx/sites-enabled/*; do
  [ -f "$f" ] || continue
  case "$(basename "$f")" in
    remote-desktop) ;;
    *) echo "      Disabling $(basename "$f") so only Remote Desktop is served on :80"
       rm -f "$f" ;;
  esac
done

echo "[6/6] Testing and reloading nginx..."
nginx -t && systemctl reload nginx

echo ""
echo "=== Done ==="
echo "Open in browser: http://YOUR_SERVER_IP/"
echo "In the client form: Server = YOUR_SERVER_IP, Port = 80"
echo ""
echo "If connection fails with 'Sec-WebSocket-Key': use Port 8080 in the form (direct to server), then: ufw allow 8080/tcp && ufw reload"
echo "Make sure server.py is running on port 8080 (e.g. python3 server.py)."

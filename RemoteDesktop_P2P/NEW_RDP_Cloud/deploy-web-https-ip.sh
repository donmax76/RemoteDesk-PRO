#!/bin/bash
# HTTPS по IP без домена: самоподписанный сертификат.
# Запуск: sudo bash deploy-web-https-ip.sh
#        sudo bash deploy-web-https-ip.sh 192.168.1.100   # если нужно указать IP вручную
# В браузере при первом заходе на https://IP появится предупреждение — нажмите «Дополнительно» → «Перейти на сайт».

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ ! -f "index.html" ]; then
  echo "Error: index.html not found in $SCRIPT_DIR"
  exit 1
fi

# IP сервера: первый аргумент или автоопределение
SERVER_IP="${1}"
if [ -z "$SERVER_IP" ]; then
  SERVER_IP=$(hostname -I 2>/dev/null | awk '{print $1}')
fi
if [ -z "$SERVER_IP" ]; then
  SERVER_IP=$(ip -4 route get 8.8.8.8 2>/dev/null | grep -oP 'src \K\S+')
fi
if [ -z "$SERVER_IP" ]; then
  echo "Could not detect server IP. Run: sudo bash deploy-web-https-ip.sh YOUR_IP"
  exit 1
fi

echo "=== Deploy Remote Desktop Web (HTTPS by IP) ==="
echo "IP: $SERVER_IP"
echo ""

echo "[1/6] Installing nginx..."
apt-get update -qq
apt-get install -y nginx 2>/dev/null || true

echo "[2/6] Creating /var/www/remote-desktop ..."
mkdir -p /var/www/remote-desktop
cp -f index.html /var/www/remote-desktop/
chown -R www-data:www-data /var/www/remote-desktop 2>/dev/null || true

echo "[3/6] Writing nginx WebSocket map..."
mkdir -p /etc/nginx/conf.d 2>/dev/null || true
cat > /etc/nginx/conf.d/websocket_upgrade.conf << 'MAPEOF'
map $http_upgrade $connection_upgrade {
    default upgrade;
    ''      close;
}
MAPEOF

echo "[4/6] Generating self-signed certificate for IP $SERVER_IP..."
SSL_DIR="/etc/nginx/ssl-remote-desktop"
mkdir -p "$SSL_DIR"
CERT_FILE="$SSL_DIR/cert.pem"
KEY_FILE="$SSL_DIR/key.pem"

# Генерируем сертификат с SAN для IP (и 127.0.0.1 для локальных проверок)
OPENSSL_CONF=$(mktemp)
trap "rm -f $OPENSSL_CONF" EXIT
cat > "$OPENSSL_CONF" << EOF
[req]
distinguished_name = dn
req_extensions = san
[san]
subjectAltName = IP:$SERVER_IP,IP:127.0.0.1
[dn]
EOF
openssl req -x509 -nodes -days 3650 -newkey rsa:2048 \
  -keyout "$KEY_FILE" -out "$CERT_FILE" \
  -subj "/CN=RemoteDesktop-$SERVER_IP" \
  -config "$OPENSSL_CONF" 2>/dev/null
chmod 644 "$CERT_FILE"
chmod 600 "$KEY_FILE"

echo "[5/6] Writing nginx config (HTTP redirect → HTTPS, SSL on 443)..."
cat > /etc/nginx/sites-available/remote-desktop << NGINX_EOF
# Редирект HTTP → HTTPS
server {
    listen 80 default_server;
    listen [::]:80 default_server;
    server_name _;
    return 301 https://\$host\$request_uri;
}

# HTTPS (самоподписанный сертификат для IP)
server {
    listen 443 ssl default_server;
    listen [::]:443 ssl default_server;
    server_name _;

    ssl_certificate $CERT_FILE;
    ssl_certificate_key $KEY_FILE;
    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_prefer_server_ciphers on;

    root /var/www/remote-desktop;
    index index.html;
    proxy_http_version 1.1;

    location / {
        try_files \$uri \$uri/ /index.html;
    }

    location /client {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Host \$host;
        proxy_set_header X-Real-IP \$remote_addr;
        proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto \$scheme;
        proxy_set_header Upgrade \$http_upgrade;
        proxy_set_header Connection \$connection_upgrade;
        proxy_set_header Sec-WebSocket-Key \$http_sec_websocket_key;
        proxy_set_header Sec-WebSocket-Version \$http_sec_websocket_version;
        proxy_read_timeout 86400;
    }

    location /host {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Host \$host;
        proxy_set_header X-Real-IP \$remote_addr;
        proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto \$scheme;
        proxy_set_header Upgrade \$http_upgrade;
        proxy_set_header Connection \$connection_upgrade;
        proxy_set_header Sec-WebSocket-Key \$http_sec_websocket_key;
        proxy_set_header Sec-WebSocket-Version \$http_sec_websocket_version;
        proxy_read_timeout 86400;
    }

    location /admin {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Host \$host;
        proxy_set_header X-Forwarded-Proto \$scheme;
        proxy_set_header Upgrade \$http_upgrade;
        proxy_set_header Connection \$connection_upgrade;
        proxy_set_header Sec-WebSocket-Key \$http_sec_websocket_key;
        proxy_set_header Sec-WebSocket-Version \$http_sec_websocket_version;
        proxy_read_timeout 86400;
    }
}
NGINX_EOF

echo "[6/6] Enabling site and reloading nginx..."
ln -sf /etc/nginx/sites-available/remote-desktop /etc/nginx/sites-enabled/remote-desktop
rm -f /etc/nginx/sites-enabled/default 2>/dev/null || true
for f in /etc/nginx/sites-enabled/*; do
  [ -f "$f" ] || continue
  case "$(basename "$f")" in
    remote-desktop) ;;
    *) echo "      Disabling $(basename "$f")"; rm -f "$f" ;;
  esac
done
nginx -t && systemctl reload nginx

echo ""
echo "=== Done (HTTPS by IP) ==="
echo "Open in browser: https://$SERVER_IP/"
echo "At first visit the browser will warn (self-signed cert) — click Advanced → Proceed to $SERVER_IP."
echo "In the client form: Server = $SERVER_IP, Port = 443, check 'Use WSS (SSL)'."
echo ""
echo "Make sure server.py is running on port 8080 (e.g. python3 server.py)."
echo "Open firewall: sudo ufw allow 80/tcp && sudo ufw allow 443/tcp && sudo ufw reload"

#!/bin/bash
# Deploy with HTTPS (Let's Encrypt). Usage:
#   sudo bash deploy-web-https.sh YOUR_DOMAIN
#   sudo EMAIL=admin@example.com bash deploy-web-https.sh rd.example.com
# Requires: nginx, certbot. First run deploy-web.sh or this script will do the same + HTTPS.

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

DOMAIN="${1}"
if [ -z "$DOMAIN" ]; then
  echo "Usage: sudo bash deploy-web-https.sh YOUR_DOMAIN"
  echo "Example: sudo bash deploy-web-https.sh rd.myserver.com"
  exit 1
fi

if [ ! -f "index.html" ]; then
  echo "Error: index.html not found in $SCRIPT_DIR"
  exit 1
fi

echo "=== Deploy Remote Desktop Web (HTTPS) ==="
echo "Domain: $DOMAIN"
echo ""

echo "[1/7] Installing nginx and certbot..."
apt-get update -qq
apt-get install -y nginx certbot python3-certbot-nginx 2>/dev/null || apt-get install -y nginx certbot

echo "[2/7] Creating /var/www/remote-desktop ..."
mkdir -p /var/www/remote-desktop
cp -f index.html /var/www/remote-desktop/
chown -R www-data:www-data /var/www/remote-desktop 2>/dev/null || true

echo "[3/7] Writing nginx WebSocket map..."
mkdir -p /etc/nginx/conf.d 2>/dev/null || true
cat > /etc/nginx/conf.d/websocket_upgrade.conf << 'MAPEOF'
map $http_upgrade $connection_upgrade {
    default upgrade;
    ''      close;
}
MAPEOF

echo "[4/7] Writing nginx HTTP config (for certbot)..."
cat > /etc/nginx/sites-available/remote-desktop << NGINX_HTTP
server {
    listen 80 default_server;
    listen [::]:80 default_server;
    server_name _;
    root /var/www/remote-desktop;
    index index.html;
    location / { try_files \$uri \$uri/ /index.html; }
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
        proxy_set_header Upgrade \$http_upgrade;
        proxy_set_header Connection \$connection_upgrade;
        proxy_set_header Sec-WebSocket-Key \$http_sec_websocket_key;
        proxy_set_header Sec-WebSocket-Version \$http_sec_websocket_version;
        proxy_read_timeout 86400;
    }
}
NGINX_HTTP

ln -sf /etc/nginx/sites-available/remote-desktop /etc/nginx/sites-enabled/remote-desktop
rm -f /etc/nginx/sites-enabled/default 2>/dev/null || true
for f in /etc/nginx/sites-enabled/*; do
  [ -f "$f" ] || continue
  case "$(basename "$f")" in
    remote-desktop) ;;
    *) rm -f "$f" ;;
  esac
done
nginx -t && systemctl reload nginx

echo "[5/7] Getting SSL certificate (Let's Encrypt)..."
CERTBOT_EMAIL="${EMAIL:-admin@$DOMAIN}"
if certbot certonly --webroot -w /var/www/remote-desktop -d "$DOMAIN" --non-interactive --agree-tos --email "$CERTBOT_EMAIL" 2>/dev/null; then
  echo "      Certificate obtained."
else
  echo "      Certbot failed. Ensure DNS for $DOMAIN points to this server and port 80 is open. You can run:"
  echo "      sudo certbot certonly --webroot -w /var/www/remote-desktop -d $DOMAIN"
  echo "      Then run this script again to write HTTPS config."
  exit 1
fi

echo "[6/7] Writing nginx HTTPS config..."
CERT_PATH="/etc/letsencrypt/live/$DOMAIN/fullchain.pem"
KEY_PATH="/etc/letsencrypt/live/$DOMAIN/privkey.pem"
if [ ! -f "$CERT_PATH" ] || [ ! -f "$KEY_PATH" ]; then
  echo "Error: Certificates not found at $CERT_PATH"
  exit 1
fi

cat > /etc/nginx/sites-available/remote-desktop << NGINX_HTTPS
# Redirect HTTP to HTTPS
server {
    listen 80 default_server;
    listen [::]:80 default_server;
    server_name _;
    return 301 https://\$host\$request_uri;
}

# HTTPS
server {
    listen 443 ssl default_server;
    listen [::]:443 ssl default_server;
    server_name _;

    ssl_certificate $CERT_PATH;
    ssl_certificate_key $KEY_PATH;
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
NGINX_HTTPS

echo "[7/7] Testing and reloading nginx..."
nginx -t && systemctl reload nginx

echo ""
echo "=== Done (HTTPS) ==="
echo "Open in browser: https://$DOMAIN/"
echo "In the client form: Server = $DOMAIN, Port = 443, check 'Use WSS (SSL)'"
echo ""
echo "Renew cert (cron): sudo certbot renew --quiet && sudo systemctl reload nginx"
echo "Make sure server.py is running on port 8080 (e.g. python3 server.py)."

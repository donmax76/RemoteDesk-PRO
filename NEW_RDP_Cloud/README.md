# RemoteDesk PRO — Система удалённого мониторинга Windows

Состоит из трёх компонентов:
1. **C++ Host** — запускается на Windows-машине (хост)
2. **Python VPS** — relay-сервер на VPS
3. **Web Client** — браузерный интерфейс

---

## Структура проекта

```
NewPlanModeApp2/
├── host_cpp/           # C++ приложение для Windows-хоста
│   ├── main.cpp        # Главный файл
│   ├── host.h          # Конфиги и утилиты
│   ├── logger.h        # Логгер
│   ├── ws_client.h     # WebSocket клиент
│   ├── screen_capture.h# Захват экрана (DXGI)
│   ├── file_manager.h  # Файловый менеджер
│   ├── process_manager.h # Процессы и сервисы
│   ├── CMakeLists.txt
│   └── host_config.json.template
├── vps_server/
│   ├── server.py       # Python relay сервер
│   ├── requirements.txt
│   └── rdp-server.service
└── web_client/
    └── index.html      # Браузерный клиент (один файл)
```

---

## Установка и запуск

### 1. VPS Server (Python)

```bash
# На вашем VPS сервере
sudo apt install python3 python3-pip python3-venv

mkdir /opt/rdp_server
cp server.py requirements.txt /opt/rdp_server/
cd /opt/rdp_server

python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt

# Запуск (тестовый)
RDP_PORT=8080 python3 server.py

# Или через systemd
sudo cp rdp-server.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable rdp-server
sudo systemctl start rdp-server
```

Открыть порт в firewall:
```bash
sudo ufw allow 8080/tcp
```

**Если перед приложением стоит прокси (nginx, caddy и т.п.):** прокси должен передавать заголовки WebSocket без изменений. Иначе возможны ошибки 426/400 (Connection, Upgrade, Sec-WebSocket-Key). Пример для **nginx**:

```nginx
location / {
    proxy_pass http://127.0.0.1:8080;
    proxy_http_version 1.1;
    proxy_set_header Host $host;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header Upgrade $http_upgrade;
    proxy_set_header Connection "Upgrade";
    proxy_set_header Sec-WebSocket-Key $http_sec_websocket_key;
    proxy_set_header Sec-WebSocket-Version $http_sec_websocket_version;
}
```

Либо минимально: `proxy_set_header Upgrade $http_upgrade;` и `proxy_set_header Connection "Upgrade";` — остальные заголовки клиента (Sec-WebSocket-Key, Sec-WebSocket-Version) nginx по умолчанию пробрасывает. Не включайте опции, которые «очищают» или перезаписывают заголовки запроса.

### 2. C++ Host (Windows)

Требования:
- Visual Studio 2019+ или MSVC + CMake
- Windows 10/11 (для DXGI Desktop Duplication)

```bash
# В Developer Command Prompt
cd host_cpp
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release

# Или открыть CMakeLists.txt в Visual Studio
```

Настройка `host_config.json`:
```json
{
  "server": "IP_ВАШЕГО_VPS",
  "port": 8080,
  "token": "мой-токен-комнаты",
  "password": "пароль",
  "quality": 80,
  "fps": 30,
  "scale": 100,
  "codec": "jpeg"
}
```

Запуск:
```
RemoteDesktopHost.exe host_config.json
```

### 3. Web Client

Просто откройте `index.html` в браузере:
- Введите IP и порт VPS
- Введите тот же `token` и `password`
- Нажмите CONNECT

---

## Возможности

| Функция | Описание |
|---------|----------|
| 🖥 Screen Stream | JPEG поток с настройкой FPS/качества/масштаба |
| ⏺ Recording | Запись в формат RDV (JPEG frames) на хосте |
| 📁 File Manager | Просмотр, загрузка, выгрузка, удаление, переименование |
| ⚙ Processes | Список процессов, kill, запуск с UAC/Admin |
| 🔧 Services | Старт/стоп/рестарт Windows сервисов |
| 📝 Config Editor | Редактирование конфиг-файлов на хосте |
| $_ Terminal | Выполнение команд через cmd.exe |
| 📊 Dashboard | RAM, uptime, hostname, статистика |

---

## Безопасность

- Используйте сложный `token` (UUID-подобный)
- Включите SSL (`RDP_SSL_CERT`, `RDP_SSL_KEY`) для WSS
- Ограничьте доступ к порту по IP (firewall)
- Пароль хранится в виде SHA-256 хэша

---

## Зависимости C++ (header-only)

Поместите `nlohmann/json.hpp` в `host_cpp/third_party/nlohmann/`:
```
https://github.com/nlohmann/json/releases/latest
```

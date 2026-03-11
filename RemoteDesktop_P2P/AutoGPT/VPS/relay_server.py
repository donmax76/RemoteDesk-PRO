#!/usr/bin/env python3
"""
WebSocket Relay - с контролем потока
"""

import asyncio
import json
import os
import hashlib
import time
import struct
from datetime import datetime
from aiohttp import web, WSMsgType
from aiohttp.web_exceptions import HTTPBadRequest, HTTPNotFound

# Исправленный импорт для BadHttpMessage
try:
    from aiohttp.http import BadHttpMessage
except ImportError:
    try:
        from aiohttp.http_exceptions import BadHttpMessage
    except ImportError:
        from aiohttp.http_parser import BadHttpMessage

# Сетевые настройки (для доступа из сети и двух портов: управление + стриминг)
# Можно переопределить через переменные окружения: CONTROL_PORT, STREAMING_PORT
HOST = os.environ.get('RELAY_HOST', '0.0.0.0')   # Слушать на всех интерфейсах
CONTROL_PORT = int(os.environ.get('CONTROL_PORT', '8080'))   # Порт управляющих команд (join, control, file_list)
STREAMING_PORT = int(os.environ.get('STREAMING_PORT', '8081'))  # Порт стриминга (кадры, FILE_DATA); тот же /ws на обоих

rooms = {}
# Конфигурация комнат: {room_id: password_hash}
room_config = {}

def hash_password(password):
    """Хеширует пароль для безопасного хранения"""
    return hashlib.sha256(password.encode('utf-8')).hexdigest()

def load_room_config():
    """Загружает конфигурацию комнат из файла (хеши паролей)"""
    global room_config
    config_file = '/opt/signal_server/room_config.json'
    
    # Если файла нет - разрешаем доступ всем (обратная совместимость)
    if not os.path.exists(config_file):
        log("No room_config.json found - allowing access to all rooms (no password required)")
        room_config = {}
        return
    
    try:
        with open(config_file, 'r') as f:
            room_config = json.load(f)
        if room_config:
            log(f"Loaded {len(room_config)} room configurations (password hashes)")
        else:
            log("room_config.json is empty - allowing access to all rooms")
    except Exception as e:
        log(f"Failed to load room config: {e}")
        log("Allowing access to all rooms (no password required)")
        room_config = {}

def check_room_access(room_id, password):
    """Проверяет доступ к комнате по паролю (сравнивает хеш)"""
    # Если конфигурация пустая - разрешаем доступ (для обратной совместимости)
    if not room_config:
        return True
    
    # Если комната не в конфигурации - запрещаем доступ
    if room_id not in room_config:
        return False
    
    # Хешируем полученный пароль и сравниваем с хешем в конфиге
    password_hash = hash_password(password)
    stored_hash = room_config.get(room_id)
    
    return password_hash == stored_hash

def log(msg):
    print(f"[{datetime.now().strftime('%H:%M:%S')}] {msg}", flush=True)

def log_debug(msg):
    """Логирует только важные сообщения, игнорируя спам от ботов"""
    # Можно включить для отладки, но обычно не нужно
    # print(f"[{datetime.now().strftime('%H:%M:%S')}] DEBUG: {msg}", flush=True)
    pass

@web.middleware
async def error_handler(request, handler):
    """Обрабатывает ошибки HTTP запросов, игнорируя некорректные запросы от ботов"""
    try:
        return await handler(request)
    except (BadHttpMessage, HTTPBadRequest) as e:
        # Полностью игнорируем некорректные HTTP запросы (боты/сканеры) - не логируем
        # Это нормальные попытки подключения от ботов/сканеров портов
        return web.Response(status=400, text="Bad Request")
    except HTTPNotFound as e:
        # Игнорируем 404 ошибки (несуществующие endpoints) - это нормально
        return web.Response(status=404, text="Not Found")
    except (asyncio.CancelledError, KeyboardInterrupt) as e:
        # Игнорируем отмену задач при завершении работы сервера (Ctrl+C)
        # Это нормальное поведение при graceful shutdown
        raise  # Пробрасываем дальше для корректного завершения
    except Exception as e:
        # Проверяем тип ошибки - игнорируем типичные ошибки от ботов
        err_type = type(e).__name__
        err_msg = str(e).lower()
        
        # Игнорируем типичные ошибки от ботов/некорректных запросов
        if any(x in err_msg for x in ['upgrade', 'pri', 'pause', 'bad http', 'invalid', 'not found', 'connection']):
            return web.Response(status=400, text="Bad Request")
        
        # Для WebSocket endpoint - пропускаем только реальные ошибки
        if request.path == '/ws':
            raise
        
        # Логируем только неожиданные ошибки для других endpoints (не от ботов)
        # Проверяем User-Agent чтобы не логировать ошибки от ботов
        user_agent = request.headers.get('User-Agent', '').lower()
        if not any(indicator in user_agent for indicator in ['bot', 'crawler', 'spider', 'scanner']):
            ip = request.headers.get('X-Forwarded-For', getattr(request, 'remote', 'unknown'))
            log(f"Error handling request from {ip}: {err_type}: {err_msg}")
        return web.Response(status=500, text="Internal Server Error")

async def ws_handler(request):
    """WebSocket handler с защитой от сторонних подключений"""
    # Проверяем User-Agent и другие признаки ботов/сканеров
    user_agent = request.headers.get('User-Agent', '').lower()
    origin = request.headers.get('Origin', '')
    referer = request.headers.get('Referer', '')
    
    # Блокируем известных ботов и сканеров
    bot_indicators = ['bot', 'crawler', 'spider', 'scanner', 'nmap', 'masscan', 'zmap', 'shodan', 'curl', 'wget', 'python-requests']
    if any(indicator in user_agent for indicator in bot_indicators):
        ip = request.headers.get('X-Forwarded-For', request.remote)
        log_debug(f"Blocked bot/scanner connection from {ip}: {user_agent[:50]}")
        return web.Response(status=403, text="Forbidden")
    
    # КРИТИЧНО: Разрешаем подключения с Origin даже без User-Agent
    # Это необходимо для .NET Framework 4.7.2 клиентов, которые не могут установить User-Agent
    # Origin устанавливается корректно через SetRequestHeader, поэтому это надежный индикатор легитимного клиента
    if not user_agent and not origin:
        ip = request.headers.get('X-Forwarded-For', request.remote)
        log_debug(f"Blocked connection without User-Agent and Origin from {ip}")
        return web.Response(status=403, text="Forbidden")
    
    # Если есть Origin - разрешаем подключение (даже без User-Agent)
    # Это позволяет .NET Framework 4.7.2 клиентам подключаться
    if origin and not user_agent:
        log_debug(f"Allowing connection with Origin but without User-Agent from {ip if 'ip' in locals() else request.remote}")
    
    # Пытаемся создать WebSocket соединение
    ws = web.WebSocketResponse(max_msg_size=100*1024*1024, heartbeat=30)
    
    # Проверяем, что это действительно WebSocket запрос
    try:
        await ws.prepare(request)
    except (BadHttpMessage, HTTPBadRequest, ValueError) as e:
        # Некорректный WebSocket запрос - полностью игнорируем (не логируем)
        # Это нормальные попытки подключения от ботов/сканеров
        return web.Response(status=400, text="Bad Request")
    except Exception as e:
        # Проверяем тип ошибки - игнорируем типичные ошибки от ботов
        err_msg = str(e).lower()
        if any(x in err_msg for x in ['upgrade', 'pri', 'pause', 'bad http', 'connection']):
            return web.Response(status=400, text="Bad Request")
        # Логируем только реальные ошибки
        ip = request.headers.get('X-Forwarded-For', request.remote)
        log(f"WebSocket connection error from {ip}: {type(e).__name__}: {e}")
        return web.Response(status=500, text="Internal Server Error")
    
    room = None
    role = None
    ip = request.headers.get('X-Forwarded-For', request.remote)
    
    try:
        async for msg in ws:
            if msg.type == WSMsgType.TEXT:
                try:
                    data = json.loads(msg.data)
                    cmd = data.get('cmd')
                    
                    # Если комната уже удалена - прекращаем обработку
                    if room and room not in rooms:
                        log(f"[{room}] Room not found during TEXT handling - closing ws")
                        break
                    
                    if cmd == 'ping':
                        # Пересылаем ping другой стороне (хост ответит pong) — так клиент получает pong и хост обновляет last_viewer_msg_time
                        if room is not None and role is not None:
                            if role in ('viewer', 'client', 'viewer_file', 'viewer_screen'):
                                host_ws = rooms.get(room, {}).get('host_main_ws') or rooms.get(room, {}).get('host')
                                if host_ws and not host_ws.closed:
                                    try:
                                        await host_ws.send_str(msg.data)
                                    except Exception as e:
                                        log_debug(f"Error forwarding ping to host: {e}")
                            elif role in ('host', 'host_data', 'host_file', 'host_screen'):
                                viewer_ws = rooms.get(room, {}).get('viewer_main_ws')
                                if viewer_ws and not viewer_ws.closed:
                                    try:
                                        await viewer_ws.send_str(msg.data)
                                    except Exception as e:
                                        log_debug(f"Error forwarding ping to viewer: {e}")
                        else:
                            # До join — отвечаем pong от имени реле (обратная совместимость)
                            try:
                                if not ws.closed:
                                    await ws.send_str('{"cmd":"pong"}')
                            except Exception as e:
                                log(f"Error sending pong: {e}")
                        continue

                    if cmd == 'join':
                        room = data.get('room', 'default')
                        role = data.get('role', 'viewer')
                        conn_id = data.get('conn_id', 0)
                        password = data.get('password', '')

                        # Проверяем доступ к комнате
                        if not check_room_access(room, password):
                            log(f"Access denied: {role} from {ip} to room '{room}'")
                            if room_config:
                                if room in room_config:
                                    provided_hash = hash_password(password) if password else 'empty'
                                    expected_hash = room_config[room]
                                    log(f"  Provided password hash: {provided_hash}")
                                    log(f"  Expected hash: {expected_hash}")
                                    log(f"  Hashes match: {provided_hash == expected_hash}")
                                else:
                                    log(f"  Room '{room}' not found in config")
                                    log(f"  Available rooms: {list(room_config.keys())}")
                            else:
                                log(f"  No room config loaded (should allow all)")
                            try:
                                await ws.send_str(json.dumps({
                                    'cmd': 'error',
                                    'message': f'Access denied: wrong room ID or password for room "{room}"'
                                }))
                            except Exception as e:
                                # Игнорируем ошибки отправки - соединение может быть закрыто
                                pass
                            # Закрываем соединение через 1 секунду, чтобы клиент успел получить сообщение
                            await asyncio.sleep(1)
                            try:
                                await ws.close()
                            except Exception as e:
                                # Игнорируем ошибки закрытия - соединение может быть уже закрыто
                                pass
                            return ws
                        else:
                            log(f"Access granted: {role} from {ip} to room '{room}'")
                        
                        if room not in rooms:
                            rooms[room] = {
                                'host': None,  # Основное соединение хоста
                                'host_main_ws': None,  # Основное соединение для TEXT сообщений
                                'host_file_connections': [],  # Соединения хоста для файлов
                                'host_screen_connections': [],  # Соединения хоста для экрана
                                'host_data': [],  # Доп. соединения хоста для передачи файлов (обратная совместимость)
                                'viewers': [],  # Список соединений viewer (обратная совместимость)
                                'viewer_main_ws': None,  # Основное соединение клиента для TEXT сообщений
                                'viewer_file_connections': [],  # Соединения клиента для файлов
                                'viewer_screen_connections': [],  # Соединения клиента для экрана
                                'sending': False,
                                'frame_sender_task': None,  # Задача frame_sender (для предотвращения множественных запусков)
                                'frame': None,
                                'file_transfer_queue': None,  # Очередь для асинхронной пересылки файлов
                                'file_transfer_task': None,  # Задача для пересылки файлов
                                'pending_data_buffer': [],   # Буфер для данных, ожидающих отправки при отсутствии viewers
                                'recv': 0,
                                'sent': 0,
                                'bytes': 0,
                                'last_frame_time': 0,  # Добавляем для корректной работы
                                'file_transfer': False,  # Явно инициализируем
                                '_frame_round_robin': 0  # Round-robin по viewer_screen для большей пропускной способности
                            }
                        
                        # Вспомогательная функция: считаем только активные (не закрытые) host-соединения
                        def _host_active_counts(r):
                            main_ws = r.get('host_main_ws') or r.get('host')
                            main = 1 if (main_ws and not main_ws.closed) else 0
                            file_conns = r.get('host_file_connections', [])
                            screen_conns = r.get('host_screen_connections', [])
                            data_conns = r.get('host_data', [])
                            n_file = len([c for c in file_conns if c and not c.closed])
                            n_screen = len([c for c in screen_conns if c and not c.closed])
                            n_data = len([c for c in data_conns if c and not c.closed])
                            total = main + n_file + n_screen + n_data
                            return main, n_file, n_screen, n_data, total

                        # Обработка ролей хоста
                        if role == 'host':
                            rooms[room]['host'] = ws
                            rooms[room]['host_main_ws'] = ws
                            _, n_file, n_screen, _, host_total = _host_active_counts(rooms[room])
                            log(f"HOST (main) {ip} -> {room} (host_total: {host_total})")
                        elif role == 'host_file':
                            # Удаляем закрытые из списка, чтобы счёт был верным без перезапуска
                            rooms[room]['host_file_connections'] = [c for c in rooms[room].get('host_file_connections', []) if c and not c.closed]
                            if ws not in rooms[room]['host_file_connections']:
                                rooms[room]['host_file_connections'].append(ws)
                            if not rooms[room].get('host_main_ws') and not rooms[room].get('host'):
                                rooms[room]['host_main_ws'] = ws
                                rooms[room]['host'] = ws
                                log(f"HOST_FILE #{conn_id} -> set as main connection")
                            _, n_file, n_screen, _, host_total = _host_active_counts(rooms[room])
                            log(f"HOST_FILE #{conn_id} {ip} -> {room} (file: {n_file}, host_total: {host_total})")
                        elif role == 'host_screen':
                            rooms[room]['host_screen_connections'] = [c for c in rooms[room].get('host_screen_connections', []) if c and not c.closed]
                            if ws not in rooms[room]['host_screen_connections']:
                                rooms[room]['host_screen_connections'].append(ws)
                            _, n_file, n_screen, _, host_total = _host_active_counts(rooms[room])
                            log(f"HOST_SCREEN #{conn_id} {ip} -> {room} (screen: {n_screen}, host_total: {host_total})")
                        elif role == 'host_data':
                            rooms[room]['host_data'] = [c for c in rooms[room].get('host_data', []) if c and not c.closed]
                            if ws not in rooms[room]['host_data']:
                                rooms[room]['host_data'].append(ws)
                            _, n_file, n_screen, n_data, host_total = _host_active_counts(rooms[room])
                            log(f"HOST_DATA #{conn_id} {ip} -> {room} (data: {n_data}, host_total: {host_total})")
                        # Обработка ролей клиента
                        elif role == 'viewer' or role == 'client':
                            # Основное соединение клиента
                            rooms[room]['viewer_main_ws'] = ws
                            # Очищаем закрытые соединения перед добавлением нового
                            rooms[room]['viewers'] = [v for v in rooms[room].get('viewers', []) if v and not v.closed]
                            # Добавляем viewer в список для обратной совместимости
                            if ws not in rooms[room]['viewers']:
                                rooms[room]['viewers'].append(ws)
                            # КРИТИЧНО: viewer (main) НЕ добавляется в viewer_screen_connections
                            # Это гарантирует что кадры идут только через viewer_screen_connections
                            log(f"VIEWER (main) #{conn_id} {ip} -> {room} (total: {len(rooms[room]['viewers'])})")
                        elif role == 'viewer_file':
                            if ws not in rooms[room]['viewer_file_connections']:
                                rooms[room]['viewer_file_connections'].append(ws)
                            # КРИТИЧНО: Если viewer_main_ws еще не установлен, используем первое file соединение
                            if not rooms[room].get('viewer_main_ws') and not rooms[room].get('viewers'):
                                rooms[room]['viewer_main_ws'] = ws
                                rooms[room]['viewers'] = [ws]  # Для обратной совместимости
                                log(f"VIEWER_FILE #{conn_id} -> set as main connection")
                            log(f"VIEWER_FILE #{conn_id} {ip} -> {room} (total: {len(rooms[room]['viewer_file_connections'])})")
                        elif role == 'viewer_screen':
                            if ws not in rooms[room]['viewer_screen_connections']:
                                rooms[room]['viewer_screen_connections'].append(ws)
                            # КРИТИЧНО: Если viewer_main_ws еще не установлен, используем первое screen соединение
                            # Это гарантирует что кадры могут отправляться даже если нет viewer (main)
                            if not rooms[room].get('viewer_main_ws') and not rooms[room].get('viewers'):
                                rooms[room]['viewer_main_ws'] = ws
                                rooms[room]['viewers'] = [ws]  # Для обратной совместимости
                                log(f"VIEWER_SCREEN #{conn_id} -> set as main connection (fallback)")
                            log(f"VIEWER_SCREEN #{conn_id} {ip} -> {room} (total: {len(rooms[room]['viewer_screen_connections'])})")
                        
                        # Проверяем готовность (host и viewer должны быть подключены)
                        # КРИТИЧНО: Проверяем все возможные варианты подключения хоста
                        # ВАЖНО: Проверяем что соединения существуют И не закрыты
                        host_ready = False
                        if rooms[room].get('host') and not rooms[room]['host'].closed:
                            host_ready = True
                        elif rooms[room].get('host_main_ws') and not rooms[room]['host_main_ws'].closed:
                            host_ready = True
                        elif rooms[room].get('host_file_connections'):
                            for conn in rooms[room]['host_file_connections']:
                                if conn and not conn.closed:
                                    host_ready = True
                                    break
                        elif rooms[room].get('host_screen_connections'):
                            for conn in rooms[room]['host_screen_connections']:
                                if conn and not conn.closed:
                                    host_ready = True
                                    break
                        elif rooms[room].get('host_data'):
                            for conn in rooms[room]['host_data']:
                                if conn and not conn.closed:
                                    host_ready = True
                                    break
                        
                        # КРИТИЧНО: Проверяем все возможные варианты подключения клиента
                        # ВАЖНО: Проверяем что соединения существуют И не закрыты
                        viewer_ready = False
                        if rooms[room].get('viewer_main_ws') and not rooms[room]['viewer_main_ws'].closed:
                            viewer_ready = True
                        elif rooms[room].get('viewers'):
                            for v in rooms[room]['viewers']:
                                if v and not v.closed:
                                    viewer_ready = True
                                    break
                        elif rooms[room].get('viewer_file_connections'):
                            for conn in rooms[room]['viewer_file_connections']:
                                if conn and not conn.closed:
                                    viewer_ready = True
                                    break
                        elif rooms[room].get('viewer_screen_connections'):
                            for conn in rooms[room]['viewer_screen_connections']:
                                if conn and not conn.closed:
                                    viewer_ready = True
                                    break
                        
                        if host_ready and viewer_ready:
                            log(f"=== CONNECTED {room} (host + viewer) ===")
                            try:
                                host_ws = rooms[room]['host_main_ws'] or rooms[room]['host']
                                if host_ws and not host_ws.closed:
                                    await host_ws.send_str('{"cmd":"ready"}')
                            except Exception as e:
                                # Игнорируем ошибки отправки - соединение может быть закрыто
                                pass
                            # Отправляем ready основному соединению клиента
                            try:
                                viewer_ws = rooms[room]['viewer_main_ws']
                                if viewer_ws and not viewer_ws.closed:
                                    await viewer_ws.send_str('{"cmd":"ready"}')
                            except Exception as e:
                                # Игнорируем ошибки отправки - соединение может быть закрыто
                                pass
                            # Обратная совместимость: отправляем ready всем viewers
                            for v in rooms[room]['viewers']:
                                if not v.closed:
                                    try:
                                        await v.send_str('{"cmd":"ready"}')
                                    except Exception as e:
                                        # Игнорируем ошибки отправки - соединение может быть закрыто
                                        pass
                            # КРИТИЧНО: Запуск sender loop только если ещё не запущен (предотвращаем множественные запуски)
                            sender_task = rooms[room].get('frame_sender_task')
                            if not rooms[room].get('sending', False) or (sender_task and sender_task.done()):
                                # Отменяем старую задачу если она есть и завершена
                                if sender_task and not sender_task.done():
                                    sender_task.cancel()
                                rooms[room]['sending'] = True
                                rooms[room]['frame_sender_task'] = asyncio.create_task(frame_sender(room))
                                log(f"[{room}] Frame sender started")
                            # Если sender завис - перезапускаем (только если действительно завис)
                            elif not rooms[room].get('file_transfer', False) and time.time() - rooms[room].get('last_frame_time', 0) > 30.0:
                                # Проверяем, что sender действительно завис (нет активной задачи)
                                sender_task = rooms[room].get('frame_sender_task')
                                if sender_task and sender_task.done():
                                    log(f"[{room}] Restarting stuck frame_sender (no frames for 30s)")
                                    rooms[room]['sending'] = False
                                    await asyncio.sleep(0)  # Только yield, без задержки
                                    rooms[room]['sending'] = True
                                    rooms[room]['last_frame_time'] = time.time()
                                    rooms[room]['frame_sender_task'] = asyncio.create_task(frame_sender(room))
                    
                    elif cmd in ('control', 'terminal', 'file_list', 'file_download', 'file_upload', 
                                 'file_upload_info', 'file_upload_chunk',
                                 'file_delete', 'file_edit', 'file_monitor', 'service_start', 
                                 'service_stop', 'service_restart', 'program_run', 'stream_start', 'stream_stop',
                                 'set_stream_config'):
                        # Команды от клиента -> хосту (host_main_ws, при отсутствии — любой открытый host)
                        if room and role in ('viewer', 'client', 'viewer_file', 'viewer_screen'):
                            r = rooms.get(room, {})
                            host_ws = r.get('host_main_ws') or r.get('host')
                            if not host_ws or host_ws.closed:
                                # Fallback: любое открытое host-соединение (host мог переподключиться через file/screen)
                                for conn_list in (r.get('host_file_connections') or [], r.get('host_screen_connections') or []):
                                    for c in conn_list:
                                        if c and not c.closed:
                                            host_ws = c
                                            break
                                    if host_ws and not host_ws.closed:
                                        break
                            if not host_ws or host_ws.closed:
                                if cmd in ('stream_start', 'stream_stop', 'file_download'):
                                    log(f"[{room}] WARNING: Cannot forward '{cmd}' to host — host not connected (host_main_ws missing or closed)")
                            else:
                                async def safe_send(ws, data):
                                    try:
                                        if ws and not ws.closed:
                                            await ws.send_str(data)
                                    except Exception as e:
                                        log_debug(f"[{room}] Error sending cmd to host: {e}")
                                asyncio.create_task(safe_send(host_ws, msg.data))
                                if cmd in ('stream_start', 'stream_stop'):
                                    log(f"[{room}] Forwarded '{cmd}' to host")
                    
                    elif cmd == 'ready':
                        # Команда ready от хоста -> пересылаем клиенту
                        if room and role in ('host', 'host_data', 'host_file', 'host_screen'):
                            viewer_ws = rooms.get(room, {}).get('viewer_main_ws')
                            if viewer_ws and not viewer_ws.closed:
                                try:
                                    await viewer_ws.send_str(msg.data)
                                    log(f"[{room}] Ready command forwarded from host to viewer")
                                except Exception as e:
                                    # Игнорируем ошибки отправки - соединение может быть закрыто
                                    pass
                            # Обратная совместимость: отправляем всем viewers
                            for v in rooms.get(room, {}).get('viewers', []):
                                if not v.closed:
                                    try:
                                        await v.send_str(msg.data)
                                    except Exception as e:
                                        # Игнорируем ошибки отправки - соединение может быть закрыто
                                        pass
                        # Команда ready от клиента -> пересылаем хосту (обычно не используется, но на всякий случай)
                        elif room and role in ('viewer', 'client', 'viewer_file', 'viewer_screen'):
                            host_ws = rooms.get(room, {}).get('host_main_ws') or rooms.get(room, {}).get('host')
                            if host_ws and not host_ws.closed:
                                try:
                                    await host_ws.send_str(msg.data)
                                    log(f"[{room}] Ready command forwarded from viewer to host")
                                except Exception as e:
                                    # Игнорируем ошибки отправки - соединение может быть закрыто
                                    pass
                    
                    elif cmd == 'pong':
                        # Команда pong от хоста -> пересылаем клиенту (для heartbeat)
                        if room and role in ('host', 'host_data', 'host_file', 'host_screen'):
                            viewer_ws = rooms.get(room, {}).get('viewer_main_ws')
                            if viewer_ws and not viewer_ws.closed:
                                try:
                                    await viewer_ws.send_str(msg.data)
                                except Exception as e:
                                    # Игнорируем ошибки отправки - соединение может быть закрыто
                                    pass
                            # Обратная совместимость: отправляем всем viewers
                            for v in rooms.get(room, {}).get('viewers', []):
                                if not v.closed:
                                    try:
                                        await v.send_str(msg.data)
                                    except Exception as e:
                                        # Игнорируем ошибки отправки - соединение может быть закрыто
                                        pass
                        # Команда pong от клиента -> пересылаем хосту (для heartbeat)
                        elif room and role in ('viewer', 'client', 'viewer_file', 'viewer_screen'):
                            host_ws = rooms.get(room, {}).get('host_main_ws') or rooms.get(room, {}).get('host')
                            if host_ws and not host_ws.closed:
                                try:
                                    await host_ws.send_str(msg.data)
                                except Exception as e:
                                    # Игнорируем ошибки отправки - соединение может быть закрыто
                                    pass
                    
                    elif cmd in ('terminal_out', 'file_list_result', 'file_download_result', 'stream_config_updated', 'stream_config_info',
                                'file_download_info', 'file_download_chunk', 'file_download_complete', 'file_download_status',
                                'file_download_folder_begin', 'file_download_folder_done',
                                'file_download_start', 'file_download_error'):
                        # Команды от хоста -> клиенту (используем основное соединение)
                        if room and role in ('host', 'host_data', 'host_file', 'host_screen'):
                            # КРИТИЧНО: Сохраняем total_bytes и время начала из file_download_folder_begin для использования при file_download_folder_done
                            if cmd == 'file_download_folder_begin':
                                try:
                                    if isinstance(data, dict) and 'total_bytes' in data:
                                        rooms[room]['_folder_total_bytes'] = int(data['total_bytes'])
                                        rooms[room]['_folder_transfer_start'] = time.time()  # Только для папки — не перезаписывать при каждом файле
                                        rooms[room]['_file_transfer_start'] = time.time()
                                        rooms[room]['_file_bytes_forwarded'] = 0
                                except (ValueError, KeyError, TypeError):
                                    pass
                            # Устанавливаем флаг передачи файла для file_download_start (НЕ перезаписываем время/счётчик папки)
                            elif cmd == 'file_download_start':
                                rooms[room]['file_transfer'] = True
                                # Не перезаписываем _file_transfer_start и _file_bytes_forwarded если идёт передача папки
                                if '_folder_total_bytes' not in rooms[room]:
                                    rooms[room]['_file_transfer_start'] = time.time()
                                    rooms[room]['_file_bytes_forwarded'] = 0
                                # Сбрасываем очередь и задачу если они были
                                if rooms[room].get('file_transfer_task'):
                                    try:
                                        rooms[room]['file_transfer_task'].cancel()
                                    except Exception as e:
                                        # Игнорируем ошибки отмены задачи - может быть уже завершена
                                        pass
                                rooms[room]['file_transfer_queue'] = None
                                rooms[room]['file_transfer_task'] = None
                                log(f"[{room}] File transfer started")
                            elif cmd == 'file_download_error':
                                # Ошибка загрузки — флаг file_transfer не сбрасываем (для папок идут следующие файлы)
                                pass
                            elif cmd == 'file_download_folder_done':
                                # Завершение потоковой передачи папки - сбрасываем флаг и логируем финальную статистику
                                rooms[room]['file_transfer'] = False
                                # Используем время начала папки (_folder_transfer_start), иначе elapsed ≈ 0 и скорость нереальная
                                t0 = rooms[room].get('_folder_transfer_start', rooms[room].get('_file_transfer_start', time.time()))
                                elapsed = time.time() - t0
                                if elapsed < 0.1:
                                    elapsed = 0.1  # Не делить на ~0 и не показывать миллионы MB/s
                                # КРИТИЧНО: Используем сохраненный total_bytes из file_download_folder_begin, если есть
                                total = rooms[room].get('_folder_total_bytes', rooms[room].get('_file_bytes_forwarded', 0))
                                forwarded = rooms[room].get('_file_bytes_forwarded', 0)
                                speed = total / elapsed if elapsed > 0 else 0
                                pct = f", forwarded {forwarded/1024/1024:.1f}MB ({100*forwarded/total:.1f}%)" if (total > 0 and forwarded < total) else ""
                                log(f"[{room}] Folder transfer completed: {cmd} (total: {total/1024/1024:.1f}MB{pct}, time: {elapsed:.1f}s, speed: {speed/1024/1024:.1f} MB/s)")
                                # Очищаем сохраненные данные
                                if '_file_bytes_forwarded' in rooms[room]:
                                    del rooms[room]['_file_bytes_forwarded']
                                if '_file_transfer_start' in rooms[room]:
                                    del rooms[room]['_file_transfer_start']
                                if '_folder_transfer_start' in rooms[room]:
                                    del rooms[room]['_folder_transfer_start']
                                if '_folder_total_bytes' in rooms[room]:
                                    del rooms[room]['_folder_total_bytes']
                            
                            # Отправляем основному соединению клиента (viewer_main_ws = control, порт 8080)
                            viewer_ws = rooms.get(room, {}).get('viewer_main_ws')
                            viewers = rooms.get(room, {}).get('viewers', [])
                            viewer_file_conns = rooms.get(room, {}).get('viewer_file_connections', [])
                            # Основное соединение клиента; не использовать viewer_file для TEXT-команд
                            target_viewer = viewer_ws if viewer_ws and not viewer_ws.closed else None
                            if not target_viewer and viewers:
                                for v in viewers:
                                    if v and not v.closed and v not in viewer_file_conns:
                                        target_viewer = v
                                        break
                            if not target_viewer:
                                log(f"[{room}] WARNING: No viewer (main) to send '{cmd}' to — client may not be connected on control port")
                            else:
                                # КРИТИЧНО: folder_done отправляем только после опустошения очереди FILE_DATA,
                                # иначе клиент получит "завершение" раньше последних чанков и файлы останутся недокачанными (74% и т.д.)
                                if cmd == 'file_download_folder_done':
                                    rooms[room]['_pending_folder_done'] = msg.data
                                    log(f"[{room}] Folder done queued — will send to viewer after file queue is drained")
                                elif cmd == 'file_download_start':
                                    try:
                                        if target_viewer and not target_viewer.closed:
                                            log(f"[{room}] Sending file_download_start to viewer...")
                                            await target_viewer.send_str(msg.data)
                                            log(f"[{room}] ✓ file_download_start sent to viewer successfully")
                                        else:
                                            log(f"[{room}] ERROR: Viewer WebSocket is closed, cannot send file_download_start")
                                    except Exception as e:
                                        log(f"[{room}] ERROR sending file_download_start to viewer: {e}")
                                else:
                                    # Для других команд используем асинхронную отправку
                                    async def safe_send_viewer(ws, data):
                                        try:
                                            if ws and not ws.closed:
                                                await ws.send_str(data)
                                        except Exception as e:
                                            log_debug(f"[{room}] Error sending {cmd} to viewer: {e}")
                                    
                                    asyncio.create_task(safe_send_viewer(target_viewer, msg.data))
                                
                except Exception as e:
                    log_debug(f"Error processing message: {e}")
            
            elif msg.type == WSMsgType.BINARY:
                # Обрабатываем бинарные данные от host, host_file, host_screen соединений
                try:
                    if room and role in ('host', 'host_data', 'host_file', 'host_screen') and room in rooms:
                        # Проверяем префиксы FILE_DATA и FILE_END
                        if msg.data.startswith(b'FILE_DATA') or msg.data.startswith(b'FILE_END'):
                            # Это данные файла - пересылаем напрямую viewer_file соединениям
                            # КРИТИЧНО: Используем viewer_file_connections для файлов
                            viewer_file_conns = rooms[room].get('viewer_file_connections', [])
                            # КРИТИЧНО: Инициализируем viewers ДО условия, чтобы избежать UnboundLocalError
                            viewers = rooms[room].get('viewers', [])
                            
                            # Обратная совместимость: если нет viewer_file_connections, используем viewers
                            if not viewer_file_conns:
                                active_viewers = [v for v in viewers if not v.closed] if viewers else []
                            else:
                                active_viewers = [v for v in viewer_file_conns if not v.closed] if viewer_file_conns else []
                            
                            # Обновляем список viewers — только удаляем закрытые, НЕ подменяем на active_viewers
                            # (active_viewers может быть только viewer_file_connections; в viewers должны остаться main + screen)
                            closed_removed = [v for v in (viewers or []) if v and not v.closed]
                            rooms[room]['viewers'] = closed_removed
                            if viewers and not closed_removed:
                                log_debug(f"[{room}] All viewers disconnected, cleaning up")
                            
                            is_file_end = msg.data.startswith(b'FILE_END')
                            
                            # Если есть активные viewers - пересылаем данные
                            if active_viewers:
                                # Устанавливаем флаг передачи файла
                                if not rooms[room].get('file_transfer', False):
                                    rooms[room]['file_transfer'] = True
                                    rooms[room]['_file_transfer_start'] = time.time()
                                    rooms[room]['_file_bytes_forwarded'] = 0
                                
                                # Создаем очередь если её нет (увеличиваем размер для больших файлов)
                                if rooms[room].get('file_transfer_queue') is None:
                                    # КРИТИЧНО: Увеличиваем размер очереди до 20000 для больших файлов и высокой скорости
                                    # Это позволяет накапливать больше данных без блокировки отправки от хоста
                                    # Увеличено с 10000 до 20000 для предотвращения потери данных при очень быстрой передаче
                                    rooms[room]['file_transfer_queue'] = asyncio.Queue(maxsize=20000)
                                    rooms[room]['file_transfer_task'] = asyncio.create_task(_forward_file_data_worker(room))
                                
                                # Логируем FILE_END для диагностики
                                if is_file_end:
                                    log(f"[{room}] FILE_END received from host, adding to queue...")
                                
                                # КРИТИЧНО: Целевое соединение viewer = индекс отправителя (host_file), иначе 0.
                                # conn_id в BINARY недоступен (он только в join). Берём индекс текущего ws в host_file_connections.
                                target_idx = 0
                                if role == 'host_file':
                                    try:
                                        target_idx = rooms[room]['host_file_connections'].index(ws)
                                    except (ValueError, KeyError):
                                        target_idx = 0
                                payload = (msg.data, target_idx, is_file_end)
                                
                                # КРИТИЧНО: Кладем данные в очередь БЕЗ блокировки для максимальной скорости
                                # Используем put_nowait для немедленной обработки
                                try:
                                    rooms[room]['file_transfer_queue'].put_nowait(payload)
                                    # КРИТИЧНО: Логируем только периодически для уменьшения спама
                                    if '_queue_put_count' not in rooms[room]:
                                        rooms[room]['_queue_put_count'] = 0
                                    rooms[room]['_queue_put_count'] += 1
                                    if rooms[room]['_queue_put_count'] % 1000 == 0:  # Каждые 1000 элементов
                                        log_debug(f"[{room}] Queue status: put {rooms[room]['_queue_put_count']} items, queue size: {rooms[room]['file_transfer_queue'].qsize()}")
                                except asyncio.QueueFull:
                                    # КРИТИЧНО: Ждём освобождения места БЕЗ таймата — никогда не сбрасываем чанки.
                                    # Гарантирует целостность файлов при любой скорости клиента.
                                    await rooms[room]['file_transfer_queue'].put(payload)
                                
                                # Логируем прогресс
                                if '_file_bytes_forwarded' not in rooms[room]:
                                    rooms[room]['_file_bytes_forwarded'] = 0
                                rooms[room]['_file_bytes_forwarded'] += len(msg.data)
                                if rooms[room]['_file_bytes_forwarded'] % (10 * 1024 * 1024) < len(msg.data):
                                    elapsed = time.time() - rooms[room].get('_file_transfer_start', time.time())
                                    speed = rooms[room]['_file_bytes_forwarded'] / elapsed if elapsed > 0 else 0
                                    log_debug(f"[{room}] Forwarded: {rooms[room]['_file_bytes_forwarded']/1024/1024:.1f}MB | {speed/1024/1024:.1f} MB/s")
                                
                                # НЕ сбрасываем флаг при FILE_END - пусть воркер сам завершится после отправки
                                # Воркер сам завершится после успешной отправки FILE_END клиенту
                            else:
                                # Нет активных viewers - это нормально при переподключении или между файлами
                                # НЕ логируем предупреждение - это не ошибка, а нормальная ситуация
                                # Просто пропускаем данные (они будут отправлены при следующем подключении viewer)
                                if is_file_end:
                                    log_debug(f"[{room}] FILE_END received but no active viewers - skipping (normal during reconnection)")
                            
                            # Обновляем время последнего кадра
                            rooms[room]['last_frame_time'] = time.time()
                        elif role == 'host_file' and len(msg.data) >= 4 and msg.data[:4] == b'FILE':
                            # КРИТИЧНО: FILE-prefixed binary от host_file — файловые чанки (пайплайн).
                            # Пересылаем НАПРЯМУЮ на viewer_main_ws (не через frame систему),
                            # чтобы каждый чанк дошёл без потерь (frame система заменяет данные).
                            viewer_ws = rooms[room].get('viewer_main_ws')
                            if viewer_ws and not viewer_ws.closed:
                                try:
                                    await viewer_ws.send_bytes(msg.data)
                                except Exception as e:
                                    log_debug(f"[{room}] Error forwarding FILE chunk to viewer: {e}")
                            else:
                                # Fallback: viewer_screen или viewers
                                sent = False
                                for vlist in (rooms[room].get('viewer_screen_connections', []), rooms[room].get('viewers', [])):
                                    for v in vlist:
                                        if v and not v.closed:
                                            try:
                                                await v.send_bytes(msg.data)
                                                sent = True
                                                break
                                            except Exception:
                                                pass
                                    if sent:
                                        break
                        else:
                            # КРИТИЧНО: Обычный кадр стрима (от host, host_screen)
                            # НЕ обрабатываем кадры от host_file - они для файлов!
                            # Это гарантирует независимость стрима и файлов
                            if role in ('host', 'host_screen'):
                                rooms[room]['recv'] += 1
                                rooms[room]['bytes'] += len(msg.data)
                                rooms[room]['frame'] = msg.data
                                rooms[room]['last_frame_time'] = time.time()
                                # КРИТИЧНО: Если sender не запущен - запускаем (проверяем задачу)
                                sender_task = rooms[room].get('frame_sender_task')
                                if not rooms[room].get('sending', False) or (sender_task and sender_task.done()):
                                    # Отменяем старую задачу если она есть и завершена
                                    if sender_task and not sender_task.done():
                                        sender_task.cancel()
                                    rooms[room]['sending'] = True
                                    rooms[room]['frame_sender_task'] = asyncio.create_task(frame_sender(room))
                            elif role == 'host_file':
                                # Не-FILE бинарные данные от host_file — fallback для стрима
                                rooms[room]['recv'] += 1
                                rooms[room]['bytes'] += len(msg.data)
                                rooms[room]['frame'] = msg.data
                                rooms[room]['last_frame_time'] = time.time()
                                sender_task = rooms[room].get('frame_sender_task')
                                if not rooms[room].get('sending', False) or (sender_task and sender_task.done()):
                                    if sender_task and not sender_task.done():
                                        sender_task.cancel()
                                    rooms[room]['sending'] = True
                                    rooms[room]['frame_sender_task'] = asyncio.create_task(frame_sender(room))
                            else:
                                log_debug(f"[{room}] Frame received from unexpected role: {role}")
                except KeyError:
                    log(f"[{room}] Room removed during binary handling - closing ws")
                    break
            
            elif msg.type in (WSMsgType.CLOSE, WSMsgType.ERROR):
                break
                
    except asyncio.CancelledError:
        # Игнорируем отмену задач при завершении работы сервера (Ctrl+C)
        # Это нормальное поведение при graceful shutdown
        pass
    except (KeyboardInterrupt, SystemExit):
        # Игнорируем прерывания при завершении работы
        raise  # Пробрасываем дальше для корректного завершения
    except Exception as e:
        # Игнорируем типичные ошибки от ботов/некорректных подключений
        err_type = type(e).__name__
        err_msg = str(e).lower()
        
        # Игнорируем ошибки состояния при завершении работы
        if err_type in ('InvalidStateError', 'CancelledError'):
            pass  # Не логируем - это нормально при завершении
        elif any(x in err_msg for x in ['upgrade', 'pri', 'pause', 'bad http', 'connection closed', 'connection reset', 'invalid state']):
            # Не логируем - это нормальные ошибки от ботов или при завершении
            pass
        else:
            # Логируем только реальные ошибки
            ip = request.headers.get('X-Forwarded-For', request.remote) if 'request' in locals() else 'unknown'
            log(f"Error in WebSocket handler from {ip}: {err_type}: {e}")
    
    # Очищаем соединение при отключении
    if room and room in rooms:
        if role == 'host':
            rooms[room]['host'] = None
            rooms[room]['host_main_ws'] = None
            # Уведомляем основное соединение клиента
            viewer_ws = rooms[room].get('viewer_main_ws')
            if viewer_ws and not viewer_ws.closed:
                try:
                    await viewer_ws.send_str('{"cmd":"host_left"}')
                except Exception as e:
                    # Игнорируем ошибки отправки - соединение может быть закрыто
                    pass
            # Уведомляем всех viewers (обратная совместимость)
            for v in rooms[room].get('viewers', []):
                if not v.closed:
                    try:
                        await v.send_str('{"cmd":"host_left"}')
                    except Exception as e:
                        # Игнорируем ошибки отправки - соединение может быть закрыто
                        pass
            # Удаляем доп. соединения хоста
            rooms[room]['host_data'] = []
            rooms[room]['host_file_connections'] = []
            rooms[room]['host_screen_connections'] = []
        elif role == 'host_file':
            if ws in rooms[room].get('host_file_connections', []):
                rooms[room]['host_file_connections'].remove(ws)
                log(f"Host file connection removed from room {room}, remaining: {len(rooms[room]['host_file_connections'])}")
        elif role == 'host_screen':
            if ws in rooms[room].get('host_screen_connections', []):
                rooms[room]['host_screen_connections'].remove(ws)
                log(f"Host screen connection removed from room {room}, remaining: {len(rooms[room]['host_screen_connections'])}")
        elif role == 'host_data':
            # Обратная совместимость
            if ws in rooms[room].get('host_data', []):
                rooms[room]['host_data'].remove(ws)
        elif role in ('viewer', 'client'):
            # Удаляем основное соединение клиента
            rooms[room]['viewer_main_ws'] = None
            # Удаляем viewer из списка (обратная совместимость)
            if ws in rooms[room].get('viewers', []):
                rooms[room]['viewers'].remove(ws)
            log(f"Viewer (main) removed from room {room}, remaining: {len(rooms[room].get('viewers', []))}")
            # Уведомляем host только если нет активных соединений клиента
            has_active_viewers = (rooms[room].get('viewer_main_ws') or 
                                 rooms[room].get('viewers') or 
                                 rooms[room].get('viewer_file_connections') or 
                                 rooms[room].get('viewer_screen_connections'))
            if not has_active_viewers:
                host_ws = rooms[room].get('host_main_ws') or rooms[room].get('host')
                if host_ws and not host_ws.closed:
                    try:
                        await host_ws.send_str('{"cmd":"viewer_left"}')
                        log(f"Notified host that all viewers left room {room}")
                    except Exception as e:
                        log(f"Error notifying host about viewer_left: {e}")
        elif role == 'viewer_file':
            if ws in rooms[room].get('viewer_file_connections', []):
                rooms[room]['viewer_file_connections'].remove(ws)
                log(f"Viewer file connection removed from room {room}, remaining: {len(rooms[room]['viewer_file_connections'])}")
            # Клиента нет — если это было последнее соединение, уведомляем хост
            has_active_viewers = (rooms[room].get('viewer_main_ws') or rooms[room].get('viewers') or
                                 rooms[room].get('viewer_file_connections') or rooms[room].get('viewer_screen_connections'))
            if not has_active_viewers:
                host_ws = rooms[room].get('host_main_ws') or rooms[room].get('host')
                if host_ws and not host_ws.closed:
                    try:
                        await host_ws.send_str('{"cmd":"viewer_left"}')
                        log(f"Notified host that all viewers left room {room} (after viewer_file close)")
                    except Exception as e:
                        log(f"Error notifying host about viewer_left: {e}")
        elif role == 'viewer_screen':
            if ws in rooms[room].get('viewer_screen_connections', []):
                rooms[room]['viewer_screen_connections'].remove(ws)
                remaining = len(rooms[room]['viewer_screen_connections'])
                log(f"Viewer screen connection removed from room {room}, remaining: {remaining}")
            # Клиента нет — если это было последнее соединение, уведомляем хост (хост перестанет слать кадры)
            has_active_viewers = (rooms[room].get('viewer_main_ws') or rooms[room].get('viewers') or
                                 rooms[room].get('viewer_file_connections') or rooms[room].get('viewer_screen_connections'))
            if not has_active_viewers:
                host_ws = rooms[room].get('host_main_ws') or rooms[room].get('host')
                if host_ws and not host_ws.closed:
                    try:
                        await host_ws.send_str('{"cmd":"viewer_left"}')
                        log(f"Notified host that all viewers left room {room} (after viewer_screen close)")
                    except Exception as e:
                        log(f"Error notifying host about viewer_left: {e}")
        
        # Очищаем комнату если она пуста
        has_host = (rooms[room]['host'] or rooms[room]['host_main_ws'] or 
                    rooms[room]['host_data'] or rooms[room]['host_file_connections'] or 
                    rooms[room]['host_screen_connections'])
        has_viewer = (rooms[room]['viewers'] or rooms[room]['viewer_main_ws'] or 
                     rooms[room]['viewer_file_connections'] or rooms[room]['viewer_screen_connections'])
        
        if not has_host and not has_viewer:
            log_debug(f"Room {room} is empty - cleaning up")  # Используем log_debug вместо log
            if rooms[room].get('file_transfer_task'):
                try:
                    rooms[room]['file_transfer_task'].cancel()
                except Exception:
                    pass  # задача уже завершена или отменена
            del rooms[room]
        else:
            log(f"- {role} left {room}")
    
    return ws

async def _forward_file_data_worker(room):
    """Воркер для пересылки файловых данных из очереди viewer'ам"""
    try:
        room_state = rooms.get(room)
        if not room_state:
            log(f"[{room}] File data worker started but room is missing")
            return
        queue = room_state.get('file_transfer_queue')
        if not queue:
            log(f"[{room}] WARNING: File data worker started but queue is None!")
            return
        
        log(f"[{room}] File data worker started")

        # Диагностика подключенных viewers
        viewer_file_conns = rooms[room].get('viewer_file_connections', [])
        viewers = rooms[room].get('viewers', [])
        total_file_conns = len(viewer_file_conns)
        active_file_conns = len([v for v in viewer_file_conns if v and not v.closed])
        total_viewers = len(viewers)
        active_viewers = len([v for v in viewers if v and not v.closed])
        log(f"[{room}] Worker diagnostics: {total_file_conns} file_conns ({active_file_conns} active), {total_viewers} viewers ({active_viewers} active)")

        last_data_time = time.time()
        while True:
            if room not in rooms:
                log(f"[{room}] Room removed during file transfer - stopping worker")
                break
            try:
                # КРИТИЧНО: Получаем данные из очереди БЕЗ таймаута для максимальной скорости
                # Используем get_nowait для немедленной обработки, если данные есть
                # Если данных нет - используем короткий таймаут
                payload = None
                try:
                    payload = queue.get_nowait()  # Пытаемся получить без ожидания
                except asyncio.QueueEmpty:
                    # Очередь пуста - ждем короткое время (20ms — быстрее реакция на новые чанки, меньше задержка копирования)
                    try:
                        payload = await asyncio.wait_for(queue.get(), timeout=0.02)  # 20ms
                    except asyncio.TimeoutError:
                        # Нет данных - проверяем завершение передачи
                        elapsed = time.time() - last_data_time
                        if elapsed > 60.0 and not rooms.get(room, {}).get('file_transfer', False):
                            log(f"[{room}] File data worker timeout - no data for 60s and file_transfer flag is False")
                            break
                        continue
                
                if payload:
                    data, target_idx, is_file_end_payload = payload  # Распаковываем tuple
                    last_data_time = time.time()  # Обновляем время последних данных

                    # КРИТИЧНО: Логируем только периодически для уменьшения спама
                    if '_queue_get_count' in rooms[room]:
                        rooms[room]['_queue_get_count'] += 1
                    else:
                        rooms[room]['_queue_get_count'] = 1
                    if rooms[room]['_queue_get_count'] % 1000 == 0:  # Каждые 1000 элементов
                        log(f"[{room}] Queue status: got {rooms[room]['_queue_get_count']} items, queue size: {queue.qsize()}")
                else:
                    continue
                
                # КРИТИЧНО: Получаем актуальный список viewers для файлов
                # Используем viewer_file_connections в первую очередь, затем viewers для обратной совместимости
                viewer_file_conns = rooms[room].get('viewer_file_connections', [])
                viewers = rooms[room].get('viewers', [])
                
                # Приоритет 1: viewer_file_connections (специально для файлов)
                if viewer_file_conns:
                    # Фильтруем закрытые соединения
                    active_file_conns = [v for v in viewer_file_conns if v and not v.closed]
                    if active_file_conns:
                        viewers = active_file_conns
                    else:
                        # Нет активных file_connections - используем viewers для обратной совместимости
                        viewers = [v for v in viewers if v and not v.closed]
                else:
                    # Нет viewer_file_connections - используем viewers для обратной совместимости
                    viewers = [v for v in viewers if v and not v.closed]
                
                # КРИТИЧНО: Очищаем закрытые соединения только периодически (каждые 100 элементов) для уменьшения задержек
                # Это позволяет обрабатывать очередь быстрее
                if rooms[room]['_queue_get_count'] % 100 == 0:
                    if '_last_viewer_cleanup' in rooms[room]:
                        if time.time() - rooms[room]['_last_viewer_cleanup'] > 5.0:  # Каждые 5 секунд
                            # Обновляем списки соединений
                            if viewer_file_conns:
                                rooms[room]['viewer_file_connections'] = [v for v in viewer_file_conns if v and not v.closed]
                            rooms[room]['viewers'] = [v for v in rooms[room].get('viewers', []) if v and not v.closed]
                            rooms[room]['_last_viewer_cleanup'] = time.time()
                    else:
                        rooms[room]['_last_viewer_cleanup'] = time.time()
                        # Обновляем списки соединений
                        if viewer_file_conns:
                            rooms[room]['viewer_file_connections'] = [v for v in viewer_file_conns if v and not v.closed]
                        rooms[room]['viewers'] = [v for v in rooms[room].get('viewers', []) if v and not v.closed]

                if not viewers:
                    # Нет viewers - это нормально при переподключении
                    # КРИТИЧНО: НЕ ждем - сразу обрабатываем следующий элемент из очереди
                    # Это позволяет быстро обработать все данные когда viewers появятся
                    if is_file_end_payload:
                        log_debug(f"[{room}] FILE_END received but no viewers - will retry")
                    # НЕ делаем await asyncio.sleep - сразу продолжаем цикл для максимальной скорости
                    continue
                
                # Фильтруем закрытые соединения и проверяем их реальное состояние
                active_viewers = []
                for v in viewers:
                    if v is None:
                        continue
                    try:
                        # Проверяем не только closed, но и реальное состояние соединения
                        if not v.closed:
                            # Дополнительная проверка - пытаемся отправить тестовый пинг (неблокирующий)
                            active_viewers.append(v)
                        else:
                            # Соединение закрыто - удаляем из всех списков
                            if v in rooms[room].get('viewer_file_connections', []):
                                rooms[room]['viewer_file_connections'].remove(v)
                            if v in rooms[room].get('viewers', []):
                                rooms[room]['viewers'].remove(v)
                            remaining = len(rooms[room].get('viewer_file_connections', [])) + len(rooms[room].get('viewers', []))
                            log(f"[{room}] Removed closed viewer from list (remaining: {remaining})")
                    except Exception as e:
                        # Ошибка при проверке - удаляем из всех списков
                        log(f"[{room}] Error checking viewer: {e}, removing from list")
                        if v in rooms[room].get('viewer_file_connections', []):
                            rooms[room]['viewer_file_connections'].remove(v)
                        if v in rooms[room].get('viewers', []):
                            rooms[room]['viewers'].remove(v)

                if not active_viewers:
                    # Нет активных viewers - это нормально при переподключении
                    # НЕ логируем предупреждение - это не ошибка, а нормальная ситуация
                    
                    # Сохраняем данные в буфер для отправки при переподключении
                    buffer = rooms[room].get('pending_data_buffer', [])
                    buffer.append((data, is_file_end_payload))
                    rooms[room]['pending_data_buffer'] = buffer

                    # Ограничиваем размер буфера (последние 100 элементов)
                    if len(buffer) > 100:
                        rooms[room]['pending_data_buffer'] = buffer[-100:]
                        log_debug(f"[{room}] Data buffer overflow, keeping last 100 items")

                    # КРИТИЧНО: НЕ делаем await - сразу продолжаем цикл для максимальной скорости
                    # Это позволяет быстро обработать очередь когда viewers появятся
                    continue
                
                # Отправляем всем активным viewers для увеличения скорости
                # (если несколько соединений клиента)
                sent_count = 0
                errors = []
                # is_file_end уже получен из payload
                
                # КРИТИЧНО: Отправляем чанк ОДНОМУ viewer по target_idx (шардинг). Иначе клиент получает один чанк N раз (дубликаты).
                target_viewer = None
                if active_viewers:
                    idx = target_idx % len(active_viewers)
                    v = active_viewers[idx]
                    if v and not v.closed:
                        target_viewer = v
                tasks = []
                valid_viewers = []
                if target_viewer:
                    try:
                        tasks.append(target_viewer.send_bytes(data))
                        valid_viewers.append(target_viewer)
                    except Exception as e:
                        log(f"[{room}] Error preparing send to viewer: {e}")
                
                if tasks:
                    # КРИТИЧНО: Отправляем параллельно всем соединениям БЕЗ таймаута для максимальной скорости
                    # Используем gather с return_exceptions для быстрой обработки ошибок
                    try:
                        # НЕ используем wait_for - это замедляет отправку
                        results = await asyncio.gather(*tasks, return_exceptions=True)
                        sent_count = sum(1 for r in results if not isinstance(r, Exception))
                        
                        # КРИТИЧНО: Удаляем viewers с ошибками отправки только периодически (каждые 100 элементов) для уменьшения задержек
                        if rooms[room]['_queue_get_count'] % 100 == 0:
                            for i, result in enumerate(results):
                                if isinstance(result, Exception):
                                    viewer = valid_viewers[i] if i < len(valid_viewers) else None
                                    if viewer:
                                        if viewer in rooms[room].get('viewer_file_connections', []):
                                            rooms[room]['viewer_file_connections'].remove(viewer)
                                        if viewer in rooms[room].get('viewers', []):
                                            rooms[room]['viewers'].remove(viewer)
                                        log(f"[{room}] Removed viewer with send error: {result}")
                    except Exception as e:
                        log(f"[{room}] Error in parallel send: {e}")
                        sent_count = 0
                    
                # КРИТИЧНО: Логируем FILE_END для диагностики
                if is_file_end_payload:
                    if sent_count > 0:
                        log(f"[{room}] FILE_END forwarded to {sent_count} viewer(s) successfully")
                    else:
                        log(f"[{room}] WARNING: FILE_END not sent - no active viewers (file transfer may be incomplete)")
                        # НЕ продолжаем - FILE_END должен быть отправлен
                        continue
                
                # Собираем ошибки из результатов
                if tasks:
                    for r in results:
                        if isinstance(r, Exception):
                            err_str = str(r).lower()
                            if not any(x in err_str for x in ['closing', 'closed', 'connection', 'broken']):
                                errors.append(str(r))

                # КРИТИЧНО: Проверяем буфер только периодически (каждые 50 элементов) для уменьшения задержек
                # Это позволяет обрабатывать очередь быстрее и быстрее реагировать на переполнение
                if rooms[room]['_queue_get_count'] % 50 == 0:
                    buffer = rooms[room].get('pending_data_buffer', [])
                    if buffer and sent_count > 0 and active_viewers:
                        log(f"[{room}] Sending {len(buffer)} buffered data items to reconnected viewer")
                        # Отправляем буфер одному viewer (избегаем дубликатов)
                        try:
                            buf_viewer = active_viewers[0]
                            buffer_tasks = []
                            for buffered_data, buffered_is_file_end in buffer:
                                if buf_viewer and not buf_viewer.closed:
                                    buffer_tasks.append(buf_viewer.send_bytes(buffered_data))

                            if buffer_tasks:
                                buffer_results = await asyncio.gather(*buffer_tasks, return_exceptions=True)
                                buffer_sent = sum(1 for r in buffer_results if not isinstance(r, Exception))
                                if buffer_sent > 0:
                                    log(f"[{room}] Buffered data sent to {buffer_sent} viewers")
                        except Exception as e:
                            log(f"[{room}] Error sending buffered data: {e}")

                        # Очищаем буфер после отправки
                        rooms[room]['pending_data_buffer'] = []
                
                # Логируем только если все попытки провалились
                if sent_count == 0 and errors:
                    log_debug(f"[{room}] Error forwarding file data to all viewers: {errors[0] if errors else 'No active viewers'}")
                
                # КРИТИЧНО: Если это FILE_END - НЕ завершаем воркер, а ждем следующий файл
                # Воркер должен продолжать работать для следующих файлов
                if is_file_end_payload:
                    if sent_count > 0:
                        log(f"[{room}] FILE_END forwarded to viewer successfully - waiting for next file")
                        # НЕ сбрасываем флаг передачи файла сразу - ждем следующий файл
                        # КРИТИЧНО: НЕ делаем await - сразу продолжаем цикл для максимальной скорости
                        continue
                    else:
                        log(f"[{room}] WARNING: FILE_END received but failed to send to viewer - will retry on next iteration")
                        # Если не удалось отправить - продолжаем попытки в следующей итерации
                        # КРИТИЧНО: НЕ делаем await - сразу продолжаем цикл для максимальной скорости
                        continue
                
                # КРИТИЧНО: Очередь пуста и есть отложенный folder_done — отправляем клиенту после всех FILE_DATA/FILE_END
                if queue.empty() and room in rooms and rooms[room].get('_pending_folder_done'):
                    pending_data = rooms[room].pop('_pending_folder_done', None)
                    if pending_data:
                        viewer_ws = rooms.get(room, {}).get('viewer_main_ws')
                        viewers_list = rooms.get(room, {}).get('viewers', [])
                        target = viewer_ws if viewer_ws and not viewer_ws.closed else (viewers_list[0] if viewers_list else None)
                        if target:
                            try:
                                await target.send_str(pending_data)
                                log(f"[{room}] Folder done sent to viewer (after queue drained)")
                            except Exception as e:
                                log(f"[{room}] Error sending pending folder_done: {e}")
                    
            except asyncio.TimeoutError:
                # Таймаут - проверяем, не завершена ли передача
                if not rooms.get(room, {}).get('file_transfer', False):
                    if queue.empty():
                        # Перед выходом отправить отложенный folder_done, если есть
                        if room in rooms and rooms[room].get('_pending_folder_done'):
                            pending_data = rooms[room].pop('_pending_folder_done', None)
                            if pending_data:
                                viewer_ws = rooms.get(room, {}).get('viewer_main_ws')
                                viewers_list = rooms.get(room, {}).get('viewers', [])
                                target = viewer_ws if viewer_ws and not viewer_ws.closed else (viewers_list[0] if viewers_list else None)
                                if target:
                                    try:
                                        await target.send_str(pending_data)
                                        log(f"[{room}] Folder done sent to viewer (on worker exit)")
                                    except Exception as e:
                                        log(f"[{room}] Error sending pending folder_done: {e}")
                        log(f"[{room}] File data worker finished (file_transfer flag cleared and queue empty)")
                        break
                    continue
                continue
            except KeyError:
                log(f"[{room}] Room removed during file forwarding - stopping worker")
                break
            except Exception as e:
                log(f"[{room}] Error in file data worker: {e}")
                break
    except Exception as e:
        log(f"[{room}] File data worker error: {e}")
    finally:
        # Очищаем очередь и задачу только если выходящий воркер — текущий (избегаем race при file_download_start)
        if room in rooms and rooms[room].get('file_transfer_task') is asyncio.current_task():
            rooms[room]['file_transfer_queue'] = None
            rooms[room]['file_transfer_task'] = None
            log(f"[{room}] File data worker cleaned up")

async def frame_sender(room):
    """Отправляет кадры всем viewers"""
    t0 = time.time()
    last_frame_time = time.time()
    pending_tasks = set()
    max_pending_per_viewer = 0  # КРИТИЧНО: Установлено в 0 - отправляем синхронно без накопления задач
    skipped = 0
    no_frame_count = 0
    
    async def do_send(viewer, data, info):
        try:
            if viewer and not viewer.closed:
                try:
                    await asyncio.wait_for(viewer.send_bytes(data), timeout=2.0)
                    info['sent'] += 1
                except asyncio.TimeoutError:
                    pass
                except Exception as e:
                    pass
            else:
                # Соединение закрыто - не увеличиваем счетчик sent
                pass
        except Exception as e:
            # Ошибка отправки - не увеличиваем счетчик sent
            # Это нормально при закрытии соединения
            pass
    
    while room in rooms:
        try:
            info = rooms[room]
            # Используем viewer_screen_connections для кадров, если есть, иначе viewers (обратная совместимость)
            viewer_screen_conns = info.get('viewer_screen_connections', [])
            viewer_file_conns = info.get('viewer_file_connections', [])
            viewer_main_ws = info.get('viewer_main_ws')
            viewers = info.get('viewers', [])
            
            # КРИТИЧНО: Для кадров стрима используем ТОЛЬКО viewer_screen_connections
            # НЕ используем viewer_file_connections - они предназначены ТОЛЬКО для файлов
            # Это гарантирует независимость стрима и файлов
            all_viewers = []
            if viewer_screen_conns:
                # КРИТИЧНО: Получаем актуальный список из info, так как viewer_screen_conns может быть устаревшим
                current_screen_conns = info.get('viewer_screen_connections', [])
                all_viewers.extend([v for v in current_screen_conns if v and not v.closed])
            # КРИТИЧНО: НЕ добавляем viewer_file_connections - они для файлов, не для стрима
            # Fallback только на viewer_main_ws для обратной совместимости
            if not all_viewers:
                if viewer_main_ws and not viewer_main_ws.closed:
                    all_viewers.append(viewer_main_ws)
                # КРИТИЧНО: НЕ используем viewers если они содержат file_connections
                # Это предотвращает отправку кадров через файловые соединения
                elif viewers:
                    for v in viewers:
                        if v and not v.closed and v not in all_viewers:
                            # КРИТИЧНО: Проверяем что это НЕ file соединение
                            if v not in viewer_file_conns:
                                all_viewers.append(v)
                                break  # Используем только первое не-file соединение
            
            # Обновляем списки соединений, удаляя закрытые
            info['viewers'] = [v for v in viewers if v and not v.closed] if viewers else []
            if viewer_screen_conns:
                info['viewer_screen_connections'] = [v for v in viewer_screen_conns if v and not v.closed]
            if viewer_file_conns:
                info['viewer_file_connections'] = [v for v in viewer_file_conns if v and not v.closed]
            
            # КРИТИЧНО: Проверяем все возможные варианты хоста
            host = (info.get('host') or 
                   info.get('host_main_ws') or
                   (info.get('host_screen_connections', [])[0] if info.get('host_screen_connections') else None) or
                   (info.get('host_file_connections', [])[0] if info.get('host_file_connections') else None))
            
            # КРИТИЧНО: НЕ выходим из цикла если нет viewers - продолжаем ждать их появления
            # Это позволяет frame_sender продолжать работу при переподключении клиента
            if not host:
                # Нет хоста - выходим, так как кадры не будут приходить
                break
            
            # Если нет активных viewers - продолжаем цикл, но не отправляем кадры
            # Это позволяет frame_sender продолжать работу при переподключении клиента
            if not all_viewers:
                await asyncio.sleep(0.01)  # Минимальная пауза, чтобы не нагружать CPU
                continue
            
            # Проверяем закрыты ли соединения
            try:
                if host and host.closed:
                    break
            except Exception as e:
                # Ошибка проверки состояния соединения - выходим из цикла
                break
        except Exception as e:
            # Ошибка в основном цикле - выходим
            log(f"[{room}] Error in frame_sender loop: {e}")
            break
        
        # Удаляем завершённые задачи - это делается в нескольких местах для предотвращения накопления
        # pending_tasks = {t for t in pending_tasks if not t.done()}  # Перенесено в блок if frame
        
        frame = info.get('frame')
        # КРИТИЧНО: Вычисляем max_total_pending на основе всех активных viewers
        # Это нужно для проверки pending_tasks вне блока if frame
        # Если max_pending_per_viewer = 0, то max_total_pending тоже 0 (синхронная отправка)
        max_total_pending = max_pending_per_viewer * len(all_viewers) if all_viewers and max_pending_per_viewer > 0 else 0
        
        if frame:
            last_frame_time = time.time()
            no_frame_count = 0
            
            # Пропускаем очень большие кадры (> 4MB — защита от утечки памяти)
            # ВАЖНО: H.264 keyframes могут быть 500KB-2MB при высоком скале — НЕ дропаем!
            if len(frame) > 4 * 1024 * 1024:
                info['frame'] = None
                skipped += 1
            else:
                # Отправляем кадр всем viewers параллельно
                # КРИТИЧНО: Используем viewer_screen_connections в первую очередь, затем резервные соединения
                target_viewers = []
                
                # Приоритет 1: viewer_screen_connections (для последовательности кадров)
                # КРИТИЧНО: Получаем актуальный список из info, так как viewer_screen_conns может быть устаревшим
                current_screen_conns = info.get('viewer_screen_connections', [])
                active_screen_conns = [v for v in current_screen_conns if v and not v.closed]
                if active_screen_conns:
                    # КРИТИЧНО: Используем ПЕРВОЕ screen соединение для ВСЕХ кадров
                    # Round-robin ломает порядок H.264 кадров (delta зависят от предыдущих)
                    # WebSocket гарантирует порядок ТОЛЬКО в рамках одного соединения
                    target_viewers = [active_screen_conns[0]]
                else:
                    # КРИТИЧНО: НЕ используем viewer_file_connections для кадров - они для файлов!
                    # Используем viewer_main_ws как резерв для обратной совместимости
                    current_viewer_main = info.get('viewer_main_ws')
                    if current_viewer_main and not current_viewer_main.closed:
                        # КРИТИЧНО: Проверяем что main_ws НЕ является file_connection
                        # Если main_ws это file_connection - не используем его для кадров
                        viewer_file_conns = info.get('viewer_file_connections', [])
                        if current_viewer_main not in viewer_file_conns:
                            target_viewers = [current_viewer_main]
                            log_debug(f"[{room}] Using viewer_main_ws as fallback for frames (no screen_connections)")
                    # КРИТИЧНО: НЕ используем all_viewers если они содержат file_connections
                    # Это гарантирует что кадры не блокируют передачу файлов
                    if not target_viewers and all_viewers:
                        # Проверяем что это НЕ file соединение
                        for v in all_viewers:
                            if v and not v.closed and v not in viewer_file_conns:
                                target_viewers = [v]
                                log_debug(f"[{room}] Using fallback viewer for frames (not a file_connection)")
                                break
                
                # КРИТИЧНО: НЕ используем fallback на file_connections - они для файлов!
                # Если нет screen соединений - логируем и пропускаем кадр
                if not target_viewers:
                    # Нет активных screen соединений - пропускаем кадр
                    # КРИТИЧНО: Логируем только периодически для диагностики
                    if no_frame_count % 10 == 0:  # Каждые 10 пропущенных кадров (чаще для диагностики проблемы)
                        log(f"[{room}] WARNING: No target_viewers for frames! screen_conns: {len(active_screen_conns)}, total_screen: {len(current_screen_conns)}, file_conns: {len(viewer_file_conns)}, main_ws: {1 if current_viewer_main and not current_viewer_main.closed else 0}, all_viewers: {len(all_viewers)}")
                        # КРИТИЧНО: Логируем детали для диагностики
                        if viewer_screen_conns:
                            closed_count = sum(1 for v in viewer_screen_conns if v and v.closed)
                            log(f"[{room}] viewer_screen_connections: total={len(viewer_screen_conns)}, active={len(active_screen_conns)}, closed={closed_count}")
                        if viewer_file_conns:
                            log(f"[{room}] viewer_file_connections count: {len(viewer_file_conns)} (NOT used for frames)")
                        # КРИТИЧНО: Логируем все соединения для диагностики
                        log(f"[{room}] All connections: host_main={1 if info.get('host_main_ws') and not info.get('host_main_ws').closed else 0}, host_screen={len(info.get('host_screen_connections', []))}, host_file={len(info.get('host_file_connections', []))}")
                    skipped += 1
                    info['frame'] = None
                    no_frame_count += 1
                    continue
                
                # КРИТИЧНО: max_total_pending уже вычислен выше на основе all_viewers
                # КРИТИЧНО: Очищаем завершенные задачи перед проверкой (только если используем async)
                if max_pending_per_viewer > 0:
                    pending_tasks = {t for t in pending_tasks if not t.done()}
                
                # При нескольких viewer_screen — больше слотов (2 на канал), иначе 4. Таймаут 15с для медленного канала.
                if max_pending_per_viewer == 0 and target_viewers:
                    info['frame'] = None
                    n_screen = len(active_screen_conns) if active_screen_conns else 1
                    max_pending_sends = min(20, max(4, 2 * n_screen))
                    send_timeout = 15.0
                    pending_key = '_frame_pending_sends'
                    if pending_key not in info:
                        info[pending_key] = 0
                    for viewer in target_viewers:
                        if viewer and not viewer.closed:
                            if info[pending_key] >= max_pending_sends:
                                skipped += 1
                                continue
                            info[pending_key] += 1
                            frame_copy = bytes(frame)  # копия для задачи
                            def _done(t):
                                try:
                                    if room in rooms and pending_key in rooms[room]:
                                        rooms[room][pending_key] = max(0, rooms[room][pending_key] - 1)
                                except Exception:
                                    pass
                            async def _send_one(v, data):
                                try:
                                    await asyncio.wait_for(v.send_bytes(data), timeout=send_timeout)
                                    info['sent'] += 1
                                except asyncio.TimeoutError:
                                    pass  # освобождаем слот, следующий кадр можно слать
                                except Exception:
                                    if v in rooms.get(room, {}).get('viewer_screen_connections', []):
                                        try:
                                            rooms[room]['viewer_screen_connections'].remove(v)
                                        except Exception:
                                            pass
                                finally:
                                    if room in rooms and pending_key in rooms[room]:
                                        rooms[room][pending_key] = max(0, rooms[room][pending_key] - 1)
                            t = asyncio.create_task(_send_one(viewer, frame_copy))
                            t.add_done_callback(_done)
                        else:
                            skipped += 1
                elif len(pending_tasks) < max_total_pending and target_viewers:
                    info['frame'] = None
                    for viewer in target_viewers:
                        # Дополнительная проверка перед отправкой
                        if viewer and not viewer.closed:
                            task = asyncio.create_task(do_send(viewer, frame, info))
                            pending_tasks.add(task)
                        else:
                            # Соединение закрыто - пропускаем
                            skipped += 1
                else:
                    info['frame'] = None
                    if not target_viewers:
                        # Логируем только если нет viewers, чтобы не спамить
                        if time.time() - t0 > 5:  # Логируем только при статистике
                            active_file_count = len(active_file_conns) if 'active_file_conns' in locals() else 0
                            log(f"[{room}] WARNING: No target_viewers available, frame skipped (screen:{len(active_screen_conns)}, file:{active_file_count}, all:{len(all_viewers)})")
                    elif len(pending_tasks) >= max_total_pending:
                        # Очередь переполнена - это нормально, не логируем
                        pass
                    skipped += 1
        else:
            no_frame_count += 1
        
        # Если все задачи висят - очищаем их
        # КРИТИЧНО: Проверяем задачи на зависание и отменяем их
        # КРИТИЧНО: Если max_pending_per_viewer = 0, то pending_tasks всегда пустое (синхронная отправка)
        # Поэтому проверяем зависшие задачи только если max_pending_per_viewer > 0
        if max_pending_per_viewer > 0 and len(pending_tasks) >= max_total_pending:
            done_tasks = {t for t in pending_tasks if t.done()}
            # Удаляем завершенные задачи
            pending_tasks = {t for t in pending_tasks if not t.done()}
            
            # Если все оставшиеся задачи зависли (нет завершенных) - отменяем их
            if len(pending_tasks) >= max_total_pending and not done_tasks:
                hung_count = len(pending_tasks)
                for task in pending_tasks:
                    if not task.done():
                        task.cancel()
                pending_tasks.clear()
                skipped += 1
                log(f"[{room}] WARNING: All {hung_count} pending tasks hung, cleared")
        
        # Проверка зависания
        if room in rooms and not rooms[room].get('file_transfer', False):
            if no_frame_count > 0 and time.time() - last_frame_time > 30.0:
                log(f"[{room}] No frames for 30s - host may be stuck, restarting sender")
                rooms[room]['sending'] = False
                break
        
        await asyncio.sleep(0)  # Только yield — максимальный FPS стрима
        
        # Статистика каждые 5 сек
        if time.time() - t0 > 5:
            r, s, b = info['recv'], info['sent'], info['bytes']
            log(f"[{room}] Recv:{r/5:.0f} Sent:{s/5:.0f} Skip:{skipped/5:.0f} FPS | {b/1024/5:.0f} KB/s | P:{len(pending_tasks)}")
            info['recv'] = info['sent'] = info['bytes'] = 0
            skipped = 0
            t0 = time.time()
    
    # Ждём завершения всех задач
    if pending_tasks:
        await asyncio.gather(*pending_tasks, return_exceptions=True)
    
    # Сбрасываем флаг sending для возможности перезапуска
    if room in rooms:
        rooms[room]['sending'] = False
        # Удаляем задачу из комнаты
        if 'frame_sender_task' in rooms[room]:
            del rooms[room]['frame_sender_task']
    
    log(f"[{room}] sender stopped")

async def health(req):
    return web.json_response({'ok': True})

async def create_room(req):
    """Создаёт новую комнату с паролем"""
    try:
        data = await req.json()
        room_id = data.get('room')
        password_hash = data.get('password_hash')
        admin_key = data.get('admin_key', '')
        
        if not room_id or not password_hash:
            return web.json_response({'error': 'room and password_hash required'}, status=400)
        
        admin_key_env = os.environ.get('ADMIN_KEY', '')
        if admin_key_env and admin_key != admin_key_env:
            return web.json_response({'error': 'Invalid admin key'}, status=403)
        
        config_file = '/opt/signal_server/room_config.json'
        if os.path.exists(config_file):
            try:
                with open(config_file, 'r') as f:
                    room_config = json.load(f)
            except (json.JSONDecodeError, IOError, OSError) as e:
                log(f"Failed to load room config for update: {e}")
                room_config = {}
        else:
            room_config = {}
        
        room_config[room_id] = password_hash
        
        try:
            with open(config_file, 'w') as f:
                json.dump(room_config, f, indent=2)
            os.chmod(config_file, 0o600)
            log(f"Room '{room_id}' created/updated")
            return web.json_response({'ok': True, 'message': f'Room {room_id} created successfully'})
        except Exception as e:
            log(f"Failed to save room config: {e}")
            return web.json_response({'error': f'Failed to save: {e}'}, status=500)
            
    except Exception as e:
        log(f"Create room error: {e}")
        return web.json_response({'error': str(e)}, status=500)

def handle_exception(loop, context):
    """Обрабатывает исключения на уровне event loop - полностью игнорирует ошибки от ботов"""
    exception = context.get('exception')
    if exception:
        err_type = type(exception).__name__
        err_msg = str(exception).lower()
        
        # Полностью игнорируем отмену задач при завершении работы сервера (Ctrl+C)
        if isinstance(exception, (asyncio.CancelledError, KeyboardInterrupt)):
            return  # Не логируем вообще - это нормальное поведение при graceful shutdown
        
        # Полностью игнорируем все ошибки от ботов/некорректных запросов
        if isinstance(exception, (BadHttpMessage, HTTPBadRequest, HTTPNotFound)):
            return  # Не логируем вообще
        if any(x in err_msg for x in ['upgrade', 'pri', 'pause', 'bad http', 'invalid', 'connection reset', 'not found', 'connection closed', 'connection aborted', 'invalid state']):
            return  # Не логируем вообще
        
        # Логируем только реальные ошибки приложения
        log(f"Unhandled exception: {err_type}: {str(exception)}")
    else:
        message = context.get('message', '')
        # Полностью игнорируем протокольные ошибки и ошибки завершения
        if any(x in message for x in ['Pause on PRI/Upgrade', 'BadHttpMessage', 'upgrade', 'pri', 'pause', 'invalid state', 'CancelledError']):
            return  # Не логируем вообще
        log(f"Event loop error: {message}")

app = web.Application(middlewares=[error_handler])
app.router.add_get('/ws', ws_handler)
app.router.add_get('/', health)
app.router.add_post('/api/create_room', create_room)

async def _run_sites():
    """Запуск relay на двух портах: CONTROL_PORT и STREAMING_PORT."""
    runner = web.AppRunner(app)
    await runner.setup()
    site_control = web.TCPSite(runner, HOST, CONTROL_PORT)
    site_streaming = web.TCPSite(runner, HOST, STREAMING_PORT)
    await site_control.start()
    await site_streaming.start()
    print(f"  Control:   ws://{HOST}:{CONTROL_PORT}/ws")
    print(f"  Streaming: ws://{HOST}:{STREAMING_PORT}/ws")
    # Держим процесс запущенным
    try:
        while True:
            await asyncio.sleep(3600)
    except asyncio.CancelledError:
        pass
    finally:
        await runner.cleanup()


if __name__ == '__main__':
    print("=" * 50)
    print("  WS RELAY | Control %d | Streaming %d" % (CONTROL_PORT, STREAMING_PORT))
    print("=" * 50)
    
    load_room_config()
    log("Started")
    
    try:
        loop = asyncio.get_running_loop()
    except RuntimeError:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
    loop.set_exception_handler(handle_exception)
    
    try:
        loop.run_until_complete(_run_sites())
    except KeyboardInterrupt:
        log("Shutting down...")
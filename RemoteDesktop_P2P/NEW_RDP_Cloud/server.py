#!/usr/bin/env python3
"""
RemoteDesktop VPS Server - WebSocket Relay
Bridges C++ host <--> Web client
"""

import asyncio
import websockets
import json
import logging
import hashlib
import secrets
import time
import ssl
import os
import base64
import socket
from typing import Dict, Optional, Set
from dataclasses import dataclass, field
from pathlib import Path

# ─── Logging ────────────────────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s - %(message)s",
    handlers=[
        logging.StreamHandler(),
        logging.FileHandler("vps_server.log"),
    ],
)
log = logging.getLogger("rdp_server")


class _SuppressHandshakeTraceback(logging.Filter):
    """Hide websockets.server ERROR traceback when it's our known proxy (Sec-WebSocket-Key) error."""
    def filter(self, record):
        if record.name != "websockets.server":
            return True
        if record.levelno != logging.ERROR:
            return True
        try:
            msg = record.getMessage()
        except Exception:
            msg = str(record.msg)
        if "opening handshake failed" in msg:
            return False
        return True


logging.getLogger("websockets.server").addFilter(_SuppressHandshakeTraceback())

# ─── Fix Connection: keep-alive → Upgrade for proxies (426) ───────────────────
# Message shown when proxy strips Sec-WebSocket-Key (cannot be fixed in app — proxy must forward headers)
PROXY_WS_HINT = (
    "Missing Sec-WebSocket-Key: your reverse proxy is stripping WebSocket headers. "
    "Configure the proxy to forward: Upgrade, Connection, Sec-WebSocket-Key, Sec-WebSocket-Version. "
    "Example (nginx): proxy_set_header Upgrade $http_upgrade; proxy_set_header Connection \"Upgrade\";"
)


def _install_connection_header_fix():
    """Ensure protocol.process_request sees Connection: Upgrade when proxy sent keep-alive."""
    try:
        from websockets import server as ws_server
        from websockets import headers as ws_headers
        from websockets.http11 import Request
        from websockets.datastructures import Headers
        from websockets.exceptions import InvalidHeader, InvalidHandshake
        orig = ws_server.ServerProtocol.process_request

        def process_request(self, request):
            connection = sum(
                [ws_headers.parse_connection(v) for v in request.headers.get_all("Connection")],
                [],
            )
            # Fix when proxy sends Connection: keep-alive, close, or other (no "upgrade").
            if not any(v.lower() == "upgrade" for v in connection):
                new_headers = Headers()
                for name, value in request.headers.raw_items():
                    new_headers[name] = value
                try:
                    del new_headers["Connection"]
                except KeyError:
                    pass
                new_headers["Connection"] = "Upgrade"
                upgrade_vals = new_headers.get_all("Upgrade")
                if not upgrade_vals or "websocket" not in (upgrade_vals[0] or "").lower():
                    try:
                        del new_headers["Upgrade"]
                    except KeyError:
                        pass
                    new_headers["Upgrade"] = "websocket"
                request = Request(
                    path=request.path,
                    headers=new_headers,
                    _exception=getattr(request, "_exception", None),
                )
            try:
                return orig(self, request)
            except InvalidHeader as e:
                if "Sec-WebSocket-Key" in str(e):
                    log.warning(
                        "WebSocket rejected: proxy is not forwarding Sec-WebSocket-Key. "
                        "Run on server: sudo bash deploy-web.sh && sudo systemctl reload nginx. "
                        "If using Cloudflare: enable WebSockets in Network settings."
                    )
                    raise InvalidHandshake(PROXY_WS_HINT) from None
                raise
        ws_server.ServerProtocol.process_request = process_request
        log.info("WebSocket: Connection header fix installed (proxy keep-alive -> Upgrade)")
    except Exception as e:
        log.warning("WebSocket Connection header fix not installed: %s", e)

_install_connection_header_fix()


def _install_http10_reject():
    """On HTTP/1.0 or invalid request, send 400 and close instead of traceback (expected HTTP/1.1)."""
    from websockets.exceptions import InvalidMessage

    def _send_400_and_close(transport):
        try:
            if transport and not getattr(transport, "is_closing", lambda: False)():
                transport.write(
                    b"HTTP/1.1 400 Bad Request\r\n"
                    b"Connection: close\r\n"
                    b"Content-Type: text/plain; charset=utf-8\r\n"
                    b"Content-Length: 72\r\n\r\n"
                    b"WebSocket requires HTTP/1.1. Use the web page on port 80, not 8080."
                )
                transport.close()
        except Exception:
            pass
        log.warning(
            "Rejected invalid request (HTTP/1.0, HTTP/2, or bad). Use nginx with proxy_http_version 1.1 and forward Sec-WebSocket-Key, Sec-WebSocket-Version, Upgrade, Connection."
        )

    def _is_http10_error(e):
        msg = str(e)
        return (
            ("unsupported protocol" in msg and "expected HTTP/1.1" in msg)
            or "HTTP/1.0" in msg
            or "PRI " in msg
            or "HTTP/2.0" in msg
            or ("did not receive a valid HTTP request" in msg and "expected GET" not in msg and "unsupported HTTP method" not in msg)
        )

    def _is_wrong_method(e):
        msg = str(e)
        return "expected GET" in msg or "unsupported HTTP method" in msg

    def _reject_and_raise(transport):
        _send_400_and_close(transport)
        raise InvalidMessage("HTTP/1.0 not supported")

    # websockets 13+: handshake is on ServerConnection (asyncio.server)
    patched = False
    try:
        from websockets.asyncio import server as ws_async_server
        Conn = getattr(ws_async_server, "ServerConnection", None) or getattr(ws_async_server, "WebSocketServerProtocol", None)
        if Conn and hasattr(Conn, "handshake"):
            _orig = Conn.handshake
            async def _wrap(self, *args, _orig=_orig, **kwargs):
                try:
                    return await _orig(self, *args, **kwargs)
                except InvalidMessage as e:
                    msg = str(e)
                    if _is_wrong_method(e):
                        _send_400_and_close(getattr(self, "transport", None))
                        log.warning("Rejected: WebSocket handshake requires GET (got POST or other).")
                        return
                    if _is_http10_error(e):
                        _reject_and_raise(getattr(self, "transport", None))
                    raise
            Conn.handshake = _wrap
            patched = True
            log.info("WebSocket: HTTP/1.0 rejection handler installed (ServerConnection)")
    except (ImportError, AttributeError):
        pass
    if patched:
        return
    try:
        from websockets.legacy import server as ws_legacy
        Conn = getattr(ws_legacy, "ServerConnection", None) or getattr(ws_legacy, "WebSocketServerProtocol", None)
        if Conn and hasattr(Conn, "handshake"):
            _orig = Conn.handshake
            async def _wrap(self, *args, _orig=_orig, **kwargs):
                try:
                    return await _orig(self, *args, **kwargs)
                except InvalidMessage as e:
                    if _is_wrong_method(e):
                        _send_400_and_close(getattr(self, "transport", None))
                        log.warning("Rejected: WebSocket handshake requires GET (got POST or other).")
                        return
                    if _is_http10_error(e):
                        _reject_and_raise(getattr(self, "transport", None))
                    raise
            Conn.handshake = _wrap
            patched = True
            log.info("WebSocket: HTTP/1.0 rejection handler installed (legacy)")
    except (ImportError, AttributeError):
        pass

    # Fallback: websockets <13 or different layout — ServerProtocol.handshake
    if not patched:
        try:
            from websockets import server as ws_server
            if hasattr(ws_server.ServerProtocol, "handshake"):
                _orig = ws_server.ServerProtocol.handshake
                async def _wrap_proto(self, *args, **kwargs):
                    try:
                        return await _orig(self, *args, **kwargs)
                    except InvalidMessage as e:
                        if _is_http10_error(e):
                            _reject_and_raise(getattr(self, "transport", None))
                        raise
                ws_server.ServerProtocol.handshake = _wrap_proto
                patched = True
                log.info("WebSocket: HTTP/1.0 rejection handler installed (ServerProtocol)")
        except Exception:
            pass

    if not patched:
        log.debug("WebSocket: HTTP/1.0 rejection not installed (no handshake found); asyncio handler will log rejects")


_install_http10_reject()

# ─── Config ─────────────────────────────────────────────────────────────────
HOST = os.environ.get("RDP_HOST", "0.0.0.0")
PORT = int(os.environ.get("RDP_PORT", "8080"))
ADMIN_TOKEN = os.environ.get("RDP_ADMIN_TOKEN", "change-me-admin-token")
MAX_ROOMS = int(os.environ.get("RDP_MAX_ROOMS", "100"))
MAX_CLIENTS_PER_ROOM = int(os.environ.get("RDP_MAX_CLIENTS", "10"))
PING_INTERVAL = 5    # Keep NIC active — short pings prevent adapter power-save
PING_TIMEOUT = 120
SSL_CERT = os.environ.get("RDP_SSL_CERT", "")
SSL_KEY  = os.environ.get("RDP_SSL_KEY", "")

# ─── Data Structures ────────────────────────────────────────────────────────
@dataclass
class Connection:
    ws: object
    role: str           # "host" | "client" | "stream"
    token: str
    user_id: str = ""
    remote: str = ""
    connected_at: float = field(default_factory=time.time)
    bytes_sent: int = 0
    bytes_recv: int = 0
    msg_count: int = 0

@dataclass
class Room:
    token: str
    password_hash: str
    host: Optional[Connection] = None
    clients: Dict[str, Connection] = field(default_factory=dict)
    stream_clients: Dict[str, Connection] = field(default_factory=dict)  # SCRN-only connections (client→receive)
    host_streams: Dict[str, Connection] = field(default_factory=dict)  # Host stream senders (host→send SCRN)
    created_at: float = field(default_factory=time.time)
    last_activity: float = field(default_factory=time.time)
    frame_count: int = 0
    _pending_binary_target: str = ""  # Route next binary from host to this client

    def is_full(self) -> bool:
        return len(self.clients) >= MAX_CLIENTS_PER_ROOM

    def touch(self):
        self.last_activity = time.time()

# In-memory room registry
rooms: Dict[str, Room] = {}
rooms_lock = asyncio.Lock()

# ─── Auth helpers ────────────────────────────────────────────────────────────
def hash_password(pw: str) -> str:
    return hashlib.sha256(pw.encode()).hexdigest()

def check_password(plain: str, hashed: str) -> bool:
    if not hashed:   # no password set → allow
        return True
    return hash_password(plain) == hashed

def new_user_id() -> str:
    return secrets.token_hex(8)

# ─── JSON helpers ─────────────────────────────────────────────────────────────
def make_error(msg: str, req_id: str = "") -> str:
    return json.dumps({"ok": False, "error": msg, "id": req_id})

def make_ok(data, req_id: str = "") -> str:
    return json.dumps({"ok": True, "data": data, "id": req_id})

def make_event(event: str, data) -> str:
    return json.dumps({"event": event, "data": data})

# ─── Room management ─────────────────────────────────────────────────────────
async def get_or_create_room(token: str, password: str = "", role: str = "client") -> Room:
    async with rooms_lock:
        if token not in rooms:
            if len(rooms) >= MAX_ROOMS:
                raise ValueError("Server at room capacity")
            rooms[token] = Room(
                token=token,
                password_hash=hash_password(password) if password else "",
            )
            log.info(f"Room created: {token}")
        elif role == "host" and password:
            # Host reconnects with new password — update the room's password
            rooms[token].password_hash = hash_password(password)
        return rooms[token]

async def cleanup_empty_rooms():
    """Periodically remove stale rooms with no host for >5 min"""
    while True:
        await asyncio.sleep(60)
        async with rooms_lock:
            stale = [
                t for t, r in rooms.items()
                if r.host is None and (time.time() - r.last_activity) > 300
            ]
            for t in stale:
                del rooms[t]
                log.info(f"Room removed (stale): {t}")

# ─── WebSocket handler ───────────────────────────────────────────────────────
async def handler(websocket, path: str):
    remote = websocket.remote_address
    log.info(f"New connection from {remote} path={path}")

    # ── TCP buffer optimization: large buffers + no-delay for throughput ──
    try:
        sock = websocket.transport.get_extra_info("socket")
        if sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 2 * 1024 * 1024)  # 2MB send
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2 * 1024 * 1024)  # 2MB recv
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)              # No Nagle
    except Exception:
        pass

    conn: Optional[Connection] = None
    room: Optional[Room] = None

    try:
        # ── Auth phase ─────────────────────────────────────────────────────
        try:
            raw = await asyncio.wait_for(websocket.recv(), timeout=10)
        except asyncio.TimeoutError:
            await websocket.send(make_error("Auth timeout"))
            return
        
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            await websocket.send(make_error("Invalid JSON"))
            return
        
        if msg.get("cmd") != "auth":
            await websocket.send(make_error("Expected auth command"))
            return
        
        token    = str(msg.get("token", "")).strip()
        password = str(msg.get("password", ""))
        role     = str(msg.get("role", "client"))
        
        if not token:
            await websocket.send(make_error("Missing token"))
            return
        
        if role not in ("host", "client", "stream", "host_stream"):
            await websocket.send(make_error("Invalid role"))
            return
        
        try:
            room = await get_or_create_room(token, password if role == "host" else "", role)
        except ValueError as e:
            await websocket.send(make_error(str(e)))
            return
        
        # Password check for clients and stream connections
        if role in ("client", "stream"):
            if not check_password(password, room.password_hash):
                await websocket.send(make_error("Wrong password"))
                log.warning(f"Auth failed from {remote} (wrong password)")
                return
            if role == "client" and room.is_full():
                await websocket.send(make_error("Room full"))
                return

        # host_stream uses same password as host (already set when host connected)
        if role == "host_stream":
            if not check_password(password, room.password_hash):
                await websocket.send(make_error("Wrong password"))
                log.warning(f"Auth failed from {remote} (host_stream wrong password)")
                return
        
        # Register connection
        user_id = new_user_id()
        conn = Connection(
            ws=websocket,
            role=role,
            token=token,
            user_id=user_id,
            remote=str(remote),
        )
        
        async with rooms_lock:
            if role == "host":
                old_host = room.host
                room.host = conn
                if old_host and old_host.ws != websocket:
                    try:
                        await old_host.ws.send(make_event("replaced", {}))
                        await old_host.ws.close()
                    except:
                        pass
            elif role == "host_stream":
                room.host_streams[user_id] = conn
            elif role == "stream":
                room.stream_clients[user_id] = conn
            else:
                room.clients[user_id] = conn
            room.touch()
        
        # Notify
        await websocket.send(json.dumps({
            "ok": True, "event": "auth_ok",
            "user_id": user_id,
            "role": role,
            "host_online": room.host is not None,
        }))
        
        log.info(f"Auth OK: role={role} token={token[:8]}... id={user_id} from={remote}")
        
        # Notify clients that host came online
        if role == "host":
            await broadcast_to_clients(room, make_event("host_online", {"user_id": user_id}))
        # Notify host that a client joined (not for stream-only connections)
        if role == "client" and room.host:
            try:
                await room.host.ws.send(make_event("client_joined", {"user_id": user_id}))
            except:
                pass
        
        # ── Message relay loop ─────────────────────────────────────────────
        async for raw_msg in websocket:
            conn.msg_count += 1
            room.touch()
            
            if isinstance(raw_msg, bytes):
                conn.bytes_recv += len(raw_msg)
                if role == "client" and room.host:
                    # Binary from client (FILE upload chunks) → forward to host
                    try:
                        await room.host.ws.send(raw_msg)
                        room.host.bytes_sent += len(raw_msg)
                    except:
                        log.warning(f"Failed to forward binary to host")
                elif role == "host_stream":
                    # host_stream ONLY sends SCRN/SCR2 frames → route to stream clients
                    if len(raw_msg) >= 4 and raw_msg[:4] in (b'SCRN', b'SCR2'):
                        enqueue_scrn_to_stream_clients(room, raw_msg)
                elif role == "host":
                    if len(raw_msg) >= 4 and raw_msg[:4] in (b'SCRN', b'SCR2'):
                        # SCRN frames → ONLY to stream_clients, NOT to command clients
                        # Fire-and-forget: never blocks the host handler
                        enqueue_scrn_to_stream_clients(room, raw_msg)
                    else:
                        # FILE chunks → route to specific client if we have a pending target
                        target = room._pending_binary_target
                        room._pending_binary_target = ""
                        if target and target in room.clients:
                            try:
                                await room.clients[target].ws.send(raw_msg)
                                room.clients[target].bytes_sent += len(raw_msg)
                            except:
                                pass
                        else:
                            # Fallback: broadcast to all command clients
                            await broadcast_to_clients(room, raw_msg)
                # role == "stream" sends nothing to host
            else:
                # Text JSON: route by role
                conn.bytes_recv += len(raw_msg.encode())
                if role in ("stream", "host_stream"):
                    continue  # stream/host_stream: binary-only, ignore text
                try:
                    msg = json.loads(raw_msg)
                except:
                    continue
                
                if role == "client":
                    # Client → Host
                    if room.host:
                        try:
                            msg["_from"] = user_id
                            await room.host.ws.send(json.dumps(msg))
                        except:
                            await websocket.send(make_error("Host disconnected"))
                    else:
                        await websocket.send(make_error("Host not connected"))
                
                elif role == "host":
                    # Routing hint: next binary message goes to specific client
                    route_target = msg.get("_route_binary_to", "")
                    if route_target:
                        room._pending_binary_target = route_target
                        continue  # Don't forward routing hints to clients

                    # Host response → route to correct client
                    target_id = msg.get("_to", "")

                    if target_id and target_id in room.clients:
                        try:
                            await room.clients[target_id].ws.send(json.dumps(msg))
                        except:
                            pass
                    else:
                        # Broadcast text to command clients only (not stream connections)
                        await broadcast_to_clients(room, json.dumps(msg))
    
    except websockets.exceptions.ConnectionClosed as e:
        log.info(f"Connection closed: {remote} code={e.code}")
    except Exception as e:
        log.exception(f"Handler error: {e}")
    finally:
        # Cleanup
        if conn and room:
            async with rooms_lock:
                if conn.role == "host" and room.host is conn:
                    room.host = None
                    log.info(f"Host disconnected: token={token[:8]}...")
                elif conn.role == "host_stream":
                    room.host_streams.pop(conn.user_id, None)
                elif conn.role == "client":
                    room.clients.pop(conn.user_id, None)
                elif conn.role == "stream":
                    room.stream_clients.pop(conn.user_id, None)
            
            if conn.role == "host":
                await broadcast_to_clients(room, make_event("host_offline", {}))
            elif conn.role == "client" and room.host:
                try:
                    await room.host.ws.send(make_event("client_left", {"user_id": conn.user_id}))
                except:
                    pass

async def broadcast_to_clients(room: Room, msg):
    """Send text/FILE messages to command clients only (not stream-only connections)."""
    if not room.clients:
        return
    dead = []
    for uid, c in list(room.clients.items()):
        try:
            await c.ws.send(msg)
            c.bytes_sent += len(msg) if isinstance(msg, bytes) else len(msg.encode())
        except:
            dead.append(uid)
    for uid in dead:
        room.clients.pop(uid, None)

def enqueue_scrn_to_stream_clients(room: Room, frame: bytes):
    """Fire-and-forget SCRN frame to all stream clients.
    Each client has a single-slot latest frame — old frames are dropped automatically.
    This NEVER blocks the host handler, preventing backpressure."""
    if not room.stream_clients:
        return
    room.frame_count += 1
    for uid, c in list(room.stream_clients.items()):
        # Store latest frame for this client; sender task picks it up
        c._latest_frame = frame
        c.bytes_sent += len(frame)
        # Start sender task if not already running
        if not getattr(c, '_sender_task', None) or c._sender_task.done():
            c._sender_task = asyncio.create_task(_stream_sender(room, uid, c))


async def _stream_sender(room: Room, uid: str, conn: Connection):
    """Per-client sender coroutine: sends the latest SCRN frame, drops stale ones."""
    try:
        while uid in room.stream_clients:
            frame = getattr(conn, '_latest_frame', None)
            if frame is None:
                await asyncio.sleep(0.01)
                continue
            conn._latest_frame = None  # Consume — if a newer frame arrives, it overwrites
            try:
                await conn.ws.send(frame)
            except Exception:
                break
    except Exception:
        pass
    finally:
        # Clean up dead client
        if uid in room.stream_clients:
            room.stream_clients.pop(uid, None)
            log.info(f"Stream client {uid} removed (send failed)")
            try:
                await conn.ws.close()
            except Exception:
                pass

# ─── Stats endpoint ──────────────────────────────────────────────────────────
async def stats_handler(websocket, path: str):
    """Admin stats websocket at /admin"""
    try:
        raw = await asyncio.wait_for(websocket.recv(), timeout=5)
        msg = json.loads(raw)
        if msg.get("admin_token") != ADMIN_TOKEN:
            await websocket.send(json.dumps({"error": "forbidden"}))
            return
        
        async with rooms_lock:
            data = {
                "rooms": len(rooms),
                "total_hosts": sum(1 for r in rooms.values() if r.host),
                "total_clients": sum(len(r.clients) for r in rooms.values()),
                "room_details": [
                    {
                        "token": t[:8] + "...",
                        "has_host": r.host is not None,
                        "clients": len(r.clients),
                        "frames": r.frame_count,
                        "age_s": int(time.time() - r.created_at),
                    }
                    for t, r in rooms.items()
                ],
            }
        await websocket.send(json.dumps(data))
    except:
        pass

# ─── Proxy compatibility: accept Connection: keep-alive when Upgrade: websocket ───
def _fix_connection_header(connection, request):
    """If proxy sent Connection: keep-alive, set Connection: Upgrade so handshake passes (426 fix)."""
    try:
        from websockets import headers as ws_headers
        headers = request.headers
        connection_options = sum(
            [ws_headers.parse_connection(v) for v in headers.get_all("Connection")],
            [],
        )
        if not any(v.lower() == "upgrade" for v in connection_options):
            upgrade_vals = headers.get_all("Upgrade")
            if upgrade_vals and "websocket" in (upgrade_vals[0] or "").lower():
                try:
                    del headers["Connection"]
                except KeyError:
                    pass
                headers["Connection"] = "Upgrade"
    except Exception as e:
        log.debug("Connection header fix skipped: %s", e)
    return None


# ─── Main ────────────────────────────────────────────────────────────────────
def _loop_exception_handler(loop, ctx):
    from websockets.exceptions import InvalidMessage, InvalidHandshake
    exc = ctx.get("exception")
    msg = str(exc) if exc else ""
    if isinstance(exc, InvalidHandshake) and ("Sec-WebSocket-Key" in msg or "Missing" in msg):
        log.warning("WebSocket rejected: proxy must forward Sec-WebSocket-Key, Sec-WebSocket-Version, Upgrade, Connection. Run: sudo bash deploy-web.sh && sudo systemctl reload nginx")
        return
    if isinstance(exc, InvalidMessage) and ("expected GET" in msg or "unsupported HTTP method" in msg):
        log.warning("Rejected: WebSocket handshake requires GET (got POST or other).")
        return
    if isinstance(exc, InvalidMessage) and ("expected HTTP/1.1" in msg or "unsupported protocol" in msg or "HTTP/1.0" in msg or "PRI " in msg or "HTTP/2.0" in msg or "did not receive a valid HTTP" in msg):
        log.warning("Rejected invalid request (HTTP/1.0, HTTP/2, or bad). Use nginx with proxy_http_version 1.1.")
        return
    asyncio.default_exception_handler(loop, ctx)


async def main():
    try:
        asyncio.get_running_loop().set_exception_handler(_loop_exception_handler)
    except Exception:
        pass
    log.info(f"Starting RemoteDesktop VPS server on {HOST}:{PORT}")
    
    ssl_ctx = None
    if SSL_CERT and SSL_KEY:
        ssl_ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ssl_ctx.load_cert_chain(SSL_CERT, SSL_KEY)
        log.info("SSL enabled")
    
    # Route by path (websockets 13+ passes only ws; path from ws.request.path)
    async def router(ws):
        path = getattr(ws, "path", "") or getattr(getattr(ws, "request", None), "path", "")
        if path == "/admin":
            await stats_handler(ws, path)
        else:
            await handler(ws, path)
    
    asyncio.create_task(cleanup_empty_rooms())
    
    serve_kw = dict(
        ssl=ssl_ctx,
        ping_interval=PING_INTERVAL,
        ping_timeout=PING_TIMEOUT,
        max_size=50 * 1024 * 1024,
        write_limit=2 * 1024 * 1024,  # 2MB write buffer for large file chunks and SCRN frames
        compression=None,
        process_request=_fix_connection_header,
    )
    try:
        server = await websockets.serve(router, HOST, PORT, **serve_kw)
    except TypeError:
        serve_kw.pop("process_request", None)
        server = await websockets.serve(router, HOST, PORT, **serve_kw)
        log.warning("websockets.serve does not support process_request; proxy Connection fix disabled")

    log.info(
        f"Server running. ws{'s' if ssl_ctx else ''}://{HOST}:{PORT}  "
        f"ping_interval={PING_INTERVAL}s ping_timeout={PING_TIMEOUT}s "
        f"max_size={serve_kw.get('max_size',0)//1024//1024}MB "
        f"write_limit={serve_kw.get('write_limit',0)//1024}KB"
    )
    await server.wait_closed()

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        log.info("Server stopped (Ctrl+C)")

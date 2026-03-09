#pragma once
#include "host.h"
#include "logger.h"
#include <queue>
#include <condition_variable>
#include <random>

// WebSocket client (RFC 6455) over plain TCP
// Architecture: 4 threads per connection
//   recv_loop   — reads frames, handles PING/PONG immediately, queues text/binary to cmd_queue
//   cmd_loop    — processes text/binary from cmd_queue (heavy work like file I/O)
//   sender_loop — sends queued outgoing messages (priority + normal queues)
//   keepalive   — sends PING every 15s
//
// CRITICAL: recv_loop NEVER blocks on user callbacks — PING/PONG are always handled instantly.
// This prevents timeout disconnects during heavy file transfers.

class WsClient {
public:
    using TextHandler   = std::function<void(const std::string&)>;
    using BinaryHandler = std::function<void(const std::vector<uint8_t>&)>;
    using CloseHandler  = std::function<void()>;

    TextHandler   on_text;
    BinaryHandler on_binary;
    CloseHandler  on_close;

    ~WsClient() { disconnect(); }

    static std::string make_ws_key() {
        std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<unsigned> dist(0, 255);
        uint8_t raw[16];
        for (auto& b : raw) b = static_cast<uint8_t>(dist(rng));
        static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(24);
        for (int i = 0; i < 15; i += 3) {
            uint32_t v = ((uint32_t)raw[i]<<16)|((uint32_t)raw[i+1]<<8)|raw[i+2];
            out += b64[(v>>18)&63]; out += b64[(v>>12)&63];
            out += b64[(v>>6)&63];  out += b64[v&63];
        }
        uint32_t v = (uint32_t)raw[15] << 16;
        out += b64[(v>>18)&63]; out += b64[(v>>12)&63]; out += "==";
        return out;
    }

    bool connect(const std::string& host, int port, const std::string& path = "/ws") {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2,2), &wsa);

        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) return false;

        // TCP keepalive at OS level
        BOOL tcp_ka = TRUE;
        setsockopt(sock_, SOL_SOCKET, SO_KEEPALIVE, (const char*)&tcp_ka, sizeof(tcp_ka));

        // Increase send buffer to 1MB to reduce blocking on large SCRN frames
        int sndbuf = 1024 * 1024;
        setsockopt(sock_, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));

        // Disable Nagle's algorithm for lower latency
        BOOL nodelay = TRUE;
        setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));

        // Reduce TCP delayed ACK: ACK every packet instead of every 2nd
        #ifndef SIO_TCP_SET_ACK_FREQUENCY
        #define SIO_TCP_SET_ACK_FREQUENCY _WSAIOW(IOC_VENDOR,23)
        #endif
        int freq = 1;
        DWORD bytes_ret = 0;
        WSAIoctl(sock_, SIO_TCP_SET_ACK_FREQUENCY, &freq, sizeof(freq), NULL, 0, &bytes_ret, NULL, NULL);

        // Increase receive buffer to 1MB (matches send buffer)
        int rcvbuf = 1024 * 1024;
        setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, (const char*)&rcvbuf, sizeof(rcvbuf));

        addrinfo hints{}, *res{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0)
            return false;
        bool ok = (::connect(sock_, res->ai_addr, (int)res->ai_addrlen) == 0);
        freeaddrinfo(res);
        if (!ok) return false;

        std::string key = make_ws_key();
        std::string req =
            "GET " + path + " HTTP/1.1\r\n"
            "Host: " + host + ":" + std::to_string(port) + "\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: " + key + "\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";
        send_raw_sync(req.c_str(), (int)req.size());

        std::string resp;
        char buf[4096];
        while (resp.find("\r\n\r\n") == std::string::npos) {
            int n = recv(sock_, buf, sizeof(buf)-1, 0);
            if (n <= 0) return false;
            buf[n]=0; resp += buf;
        }
        if (resp.find("101") == std::string::npos) return false;

        connected_ = true;
        sender_running_ = true;
        cmd_running_ = true;
        last_pong_time_ = std::chrono::steady_clock::now();
        sender_thread_    = std::thread(&WsClient::sender_loop, this);
        recv_thread_      = std::thread(&WsClient::recv_loop, this);
        cmd_thread_       = std::thread(&WsClient::cmd_loop, this);
        keepalive_thread_ = std::thread(&WsClient::keepalive_loop, this);
        return true;
    }

    void disconnect() {
        connected_ = false;
        sender_running_ = false;
        cmd_running_ = false;
        q_cv_.notify_all();
        cmd_cv_.notify_all();
        if (sender_thread_.joinable()) sender_thread_.join();
        if (cmd_thread_.joinable()) cmd_thread_.join();
        if (sock_ != INVALID_SOCKET) {
            SOCKET s = sock_;
            sock_ = INVALID_SOCKET;
            shutdown(s, SD_BOTH);
            closesocket(s);
        }
        if (recv_thread_.joinable()) recv_thread_.join();
        if (keepalive_thread_.joinable()) keepalive_thread_.join();
    }

    bool is_connected() const { return connected_; }

    void send_text(const std::string& text) {
        std::vector<uint8_t> data(text.begin(), text.end());
        enqueue(true, WS_TEXT, std::move(data));
    }

    void send_binary(const std::vector<uint8_t>& data) {
        enqueue(false, WS_BINARY, std::vector<uint8_t>(data.begin(), data.end()));
    }

    void send_binary(const uint8_t* data, size_t size) {
        enqueue(false, WS_BINARY, std::vector<uint8_t>(data, data + size));
    }

    void send_binary_priority(const std::vector<uint8_t>& data) {
        enqueue(true, WS_BINARY, std::vector<uint8_t>(data.begin(), data.end()));
    }

private:
    static const size_t kMaxNormalQueue = 2;
    static const int kPingIntervalSec = 15;  // More frequent pings (was 30)

    struct Outgoing {
        WsOpcode opcode;
        std::vector<uint8_t> data;
    };

    // Incoming command from recv_loop → cmd_loop
    struct Incoming {
        bool is_text;  // true=text, false=binary
        std::string text;
        std::vector<uint8_t> binary;
    };

    SOCKET sock_ = INVALID_SOCKET;
    std::atomic<bool> connected_{false};
    std::atomic<bool> sender_running_{false};
    std::atomic<bool> cmd_running_{false};
    std::thread recv_thread_;
    std::thread sender_thread_;
    std::thread cmd_thread_;
    std::thread keepalive_thread_;

    // Outgoing send queues
    std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::queue<Outgoing> priority_q_;
    std::queue<Outgoing> normal_q_;

    // Incoming command queue (recv_loop → cmd_loop)
    std::mutex cmd_mu_;
    std::condition_variable cmd_cv_;
    std::queue<Incoming> cmd_q_;

    std::mt19937 mask_rng_{std::random_device{}()};
    std::mutex mask_mu_;
    std::atomic<std::chrono::steady_clock::time_point> last_pong_time_{std::chrono::steady_clock::now()};

    void enqueue(bool priority, WsOpcode opcode, std::vector<uint8_t> data) {
        if (!sender_running_ || !connected_) return;
        std::lock_guard<std::mutex> lk(q_mu_);
        if (priority) {
            priority_q_.push(Outgoing{opcode, std::move(data)});
        } else {
            while (normal_q_.size() >= kMaxNormalQueue) {
                (void)normal_q_.front();
                normal_q_.pop();
            }
            normal_q_.push(Outgoing{opcode, std::move(data)});
        }
        q_cv_.notify_one();
    }

    void sender_loop() {
        while (sender_running_ && connected_) {
            Outgoing msg;
            {
                std::unique_lock<std::mutex> lk(q_mu_);
                q_cv_.wait_for(lk, std::chrono::milliseconds(500), [this] {
                    return !sender_running_ || !priority_q_.empty() || !normal_q_.empty();
                });
                if (!sender_running_) break;
                if (!priority_q_.empty()) {
                    msg = std::move(priority_q_.front());
                    priority_q_.pop();
                } else if (!normal_q_.empty()) {
                    msg = std::move(normal_q_.front());
                    normal_q_.pop();
                } else
                    continue;
            }
            if (sock_ == INVALID_SOCKET) break;
            if (!send_frame_to_socket(msg.opcode, msg.data.data(), msg.data.size())) {
                connected_ = false;
                break;
            }
        }
    }

    // Command processing thread — handles on_text/on_binary callbacks
    // This runs heavy work (file I/O, sys_info) without blocking recv_loop
    void cmd_loop() {
        while (cmd_running_ && connected_) {
            Incoming msg;
            {
                std::unique_lock<std::mutex> lk(cmd_mu_);
                cmd_cv_.wait_for(lk, std::chrono::milliseconds(500), [this] {
                    return !cmd_running_ || !cmd_q_.empty();
                });
                if (!cmd_running_) break;
                if (cmd_q_.empty()) continue;
                msg = std::move(cmd_q_.front());
                cmd_q_.pop();
            }
            try {
                if (msg.is_text && on_text)
                    on_text(msg.text);
                else if (!msg.is_text && on_binary)
                    on_binary(msg.binary);
            } catch (...) {}
        }
    }

    // Keepalive: send PING every 15s (was 30s)
    void keepalive_loop() {
        while (connected_) {
            for (int i = 0; i < kPingIntervalSec * 10 && connected_; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!connected_) break;
            enqueue(true, WS_PING, {});
        }
    }

    bool send_raw_sync(const char* data, int len) {
        int sent = 0;
        while (sent < len) {
            SOCKET s = sock_;
            if (s == INVALID_SOCKET) return false;
            int n = ::send(s, data + sent, len - sent, 0);
            if (n <= 0) return false;
            sent += n;
        }
        return true;
    }

    void generate_mask(uint8_t mask[4]) {
        std::lock_guard<std::mutex> lk(mask_mu_);
        std::uniform_int_distribution<unsigned> dist(0, 255);
        for (int i = 0; i < 4; ++i)
            mask[i] = static_cast<uint8_t>(dist(mask_rng_));
    }

    bool send_frame_to_socket(WsOpcode opcode, const uint8_t* payload, size_t len) {
        std::vector<uint8_t> frame;
        frame.reserve(14 + len);
        frame.push_back(0x80 | (uint8_t)opcode);
        if (len < 126) {
            frame.push_back(0x80 | (uint8_t)len);
        } else if (len < 65536) {
            frame.push_back(0x80 | 126);
            frame.push_back((len >> 8) & 0xFF);
            frame.push_back(len & 0xFF);
        } else {
            frame.push_back(0x80 | 127);
            for (int i=7;i>=0;--i) frame.push_back((len>>(i*8))&0xFF);
        }
        uint8_t mask[4];
        generate_mask(mask);
        frame.insert(frame.end(), mask, mask+4);
        size_t base = frame.size();
        frame.resize(base + len);
        // Fast XOR: 4 bytes at a time
        uint8_t* dst = frame.data() + base;
        uint32_t mask32;
        memcpy(&mask32, mask, 4);
        size_t i = 0;
        size_t len4 = len & ~(size_t)3;
        for (; i < len4; i += 4) {
            uint32_t src4;
            memcpy(&src4, payload + i, 4);
            src4 ^= mask32;
            memcpy(dst + i, &src4, 4);
        }
        for (; i < len; ++i)
            dst[i] = payload[i] ^ mask[i & 3];
        return send_raw_sync((const char*)frame.data(), (int)frame.size());
    }

    bool recv_exact(uint8_t* buf, size_t n) {
        size_t got = 0;
        while (got < n) {
            SOCKET s = sock_;
            if (s == INVALID_SOCKET) return false;
            int r = recv(s, (char*)buf+got, (int)(n-got), 0);
            if (r <= 0) return false;
            got += r;
        }
        return true;
    }

    // recv_loop: ONLY reads frames + handles PING/PONG + queues commands
    // NEVER calls user callbacks directly — stays responsive for control frames
    void recv_loop() {
        while (connected_) {
            uint8_t hdr[2];
            if (!recv_exact(hdr, 2)) break;

            bool fin    = (hdr[0] & 0x80) != 0;
            uint8_t op  = hdr[0] & 0x0F;
            bool masked = (hdr[1] & 0x80) != 0;
            uint64_t plen = hdr[1] & 0x7F;

            if (plen == 126) {
                uint8_t ext[2]; if (!recv_exact(ext,2)) break;
                plen = ((uint64_t)ext[0]<<8)|ext[1];
            } else if (plen == 127) {
                uint8_t ext[8]; if (!recv_exact(ext,8)) break;
                plen=0; for(int i=0;i<8;++i) plen=(plen<<8)|ext[i];
            }

            uint8_t mask[4]={};
            if (masked && !recv_exact(mask,4)) break;

            if (plen > 64 * 1024 * 1024) break;

            std::vector<uint8_t> payload(plen);
            if (plen > 0 && !recv_exact(payload.data(), plen)) break;
            if (masked) for (size_t i=0;i<plen;++i) payload[i]^=mask[i%4];

            // Control frames — handle IMMEDIATELY (never queued)
            if (op == WS_PING) {
                enqueue(true, WS_PONG, std::move(payload));
                continue;
            }
            if (op == WS_PONG) {
                last_pong_time_ = std::chrono::steady_clock::now();
                continue;
            }
            if (op == WS_CLOSE) {
                enqueue(true, WS_CLOSE, std::vector<uint8_t>(payload.begin(), payload.end()));
                connected_ = false;
                break;
            }

            // Data frames — queue for cmd_loop (never block recv_loop)
            {
                std::lock_guard<std::mutex> lk(cmd_mu_);
                Incoming in;
                if (op == WS_TEXT) {
                    in.is_text = true;
                    in.text = std::string(payload.begin(), payload.end());
                } else {
                    in.is_text = false;
                    in.binary = std::move(payload);
                }
                cmd_q_.push(std::move(in));
            }
            cmd_cv_.notify_one();
        }
        connected_ = false;
        cmd_running_ = false;
        cmd_cv_.notify_all();
        if (on_close) on_close();
    }
};

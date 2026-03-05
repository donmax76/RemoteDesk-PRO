#pragma once
#include "host.h"
#include "logger.h"
#include <queue>
#include <condition_variable>

// Simple WebSocket client (RFC 6455) over plain TCP
// Outgoing: queue + dedicated sender thread so stream and command responses
// don't block each other. Priority queue for text/file chunks; normal queue for frames (max 2, drop oldest).

class WsClient {
public:
    using TextHandler   = std::function<void(const std::string&)>;
    using BinaryHandler = std::function<void(const std::vector<uint8_t>&)>;
    using CloseHandler  = std::function<void()>;

    TextHandler   on_text;
    BinaryHandler on_binary;
    CloseHandler  on_close;

    ~WsClient() { disconnect(); }

    bool connect(const std::string& host, int port, const std::string& path = "/ws") {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2,2), &wsa);

        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == INVALID_SOCKET) return false;

        addrinfo hints{}, *res{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0)
            return false;
        bool ok = (::connect(sock_, res->ai_addr, (int)res->ai_addrlen) == 0);
        freeaddrinfo(res);
        if (!ok) return false;

        std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
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
        sender_thread_ = std::thread(&WsClient::sender_loop, this);
        recv_thread_ = std::thread(&WsClient::recv_loop, this);
        return true;
    }

    void disconnect() {
        connected_ = false;
        sender_running_ = false;
        q_cv_.notify_all();
        if (sender_thread_.joinable()) sender_thread_.join();
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        if (recv_thread_.joinable()) recv_thread_.join();
    }

    bool is_connected() const { return connected_; }

    // Command responses and file chunks — go to priority queue (sent before frames)
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

    struct Outgoing {
        WsOpcode opcode;
        std::vector<uint8_t> data;
    };

    SOCKET sock_ = INVALID_SOCKET;
    std::atomic<bool> connected_{false};
    std::atomic<bool> sender_running_{false};
    std::thread recv_thread_;
    std::thread sender_thread_;
    std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::queue<Outgoing> priority_q_;
    std::queue<Outgoing> normal_q_;

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
                q_cv_.wait(lk, [this] {
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
            send_frame_to_socket(msg.opcode, msg.data.data(), msg.data.size());
        }
    }

    void send_raw_sync(const char* data, int len) {
        int sent = 0;
        while (sent < len && sock_ != INVALID_SOCKET) {
            int n = ::send(sock_, data + sent, len - sent, 0);
            if (n <= 0) break;
            sent += n;
        }
    }

    void send_frame_to_socket(WsOpcode opcode, const uint8_t* payload, size_t len) {
        std::vector<uint8_t> frame;
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
        uint8_t mask[4] = {0x37, 0x42, 0x13, 0x77};
        frame.insert(frame.end(), mask, mask+4);
        size_t base = frame.size();
        frame.resize(base + len);
        for (size_t i = 0; i < len; ++i)
            frame[base+i] = payload[i] ^ mask[i%4];
        send_raw_sync((const char*)frame.data(), (int)frame.size());
    }

    bool recv_exact(uint8_t* buf, size_t n) {
        size_t got = 0;
        while (got < n) {
            int r = recv(sock_, (char*)buf+got, (int)(n-got), 0);
            if (r <= 0) return false;
            got += r;
        }
        return true;
    }

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

            std::vector<uint8_t> payload(plen);
            if (plen > 0 && !recv_exact(payload.data(), plen)) break;
            if (masked) for (size_t i=0;i<plen;++i) payload[i]^=mask[i%4];

            if (op == WS_PING) { enqueue(true, WS_PONG, std::move(payload)); continue; }
            if (op == WS_CLOSE) { connected_=false; break; }
            if (op == WS_TEXT && on_text)
                on_text(std::string(payload.begin(), payload.end()));
            else if (op == WS_BINARY && on_binary)
                on_binary(payload);
        }
        connected_ = false;
        if (on_close) on_close();
    }
};

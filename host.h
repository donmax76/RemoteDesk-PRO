#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <winbase.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <gdiplus.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <functional>
#include <memory>
#include <map>
#include <sstream>
#include <fstream>
#include <chrono>
#include <condition_variable>
#include <algorithm>
#include <filesystem>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

// ===== WebSocket Frame Types =====
enum WsOpcode : uint8_t {
    WS_CONTINUATION = 0x0,
    WS_TEXT         = 0x1,
    WS_BINARY       = 0x2,
    WS_CLOSE        = 0x8,
    WS_PING         = 0x9,
    WS_PONG         = 0xA
};

// ===== Config =====
struct HostConfig {
    std::string server_address = "127.0.0.1";
    int server_port  = 8080;
    int stream_port  = 8081;
    std::string room_token;
    std::string password;
    int quality      = 75;
    int fps          = 30;
    int scale        = 80;
    int file_connections = 4;
    int screen_connections = 1;
    std::string codec = "jpeg"; // jpeg | h264 | vp8
    std::string log_level = "INFO";
    bool log_to_file = true;
};

// ===== Simple JSON helpers =====
inline std::string json_escape(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (char c : s) {
        if      (c == '"')  r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else                r += c;
    }
    return r;
}

inline std::string json_unescape(const std::string& s) {
    std::string r; r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
                case 'n':  r += '\n'; ++i; break;
                case 'r':  r += '\r'; ++i; break;
                case 't':  r += '\t'; ++i; break;
                case '"':  r += '"';  ++i; break;
                case '\\': r += '\\'; ++i; break;
                default:   r += s[i]; break;
            }
        } else {
            r += s[i];
        }
    }
    return r;
}

inline std::string json_get(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    while (++pos < json.size() && (json[pos]==' '||json[pos]=='\t'));
    if (pos >= json.size()) return "";
    if (json[pos] == '"') {
        auto end = json.find('"', pos+1);
        // Skip escaped quotes (check end > 0 to avoid underflow)
        while (end != std::string::npos && end > 0 && json[end-1]=='\\')
            end = json.find('"', end+1);
        if (end == std::string::npos) return "";
        return json.substr(pos+1, end-pos-1);
    }
    // number / bool
    auto end = json.find_first_of(",}\n", pos);
    std::string val = json.substr(pos, end==std::string::npos ? std::string::npos : end-pos);
    while (!val.empty() && (val.back()==' '||val.back()=='\t')) val.pop_back();
    return val;
}

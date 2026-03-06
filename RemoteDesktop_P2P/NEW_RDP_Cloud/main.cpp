/*
 * RemoteDesktop Host - Main Entry Point
 * Connects to VPS relay server and handles all remote commands
 */

#include "host.h"
#include "logger.h"
#include "ws_client.h"
#include "screen_capture.h"
#include "file_manager.h"
#include "process_manager.h"
#ifdef USE_WEBRTC_STREAM
#include "webrtc_stream.h"
#endif
#include <nlohmann/json.hpp>
#include <pdh.h>
#include <pdhmsg.h>

using json = nlohmann::json;

// ===== Global State =====
static HostConfig g_config;
static Logger& g_log = Logger::get();
static std::atomic<bool> g_running{true};
static std::unique_ptr<WsClient> g_ws;
static ScreenCapture g_screen;
static FileManager g_files;
static ProcessManager g_procs;
static ServiceManager g_services;

// Stream state
static std::atomic<bool> g_streaming{false};
static std::thread g_stream_thread;
static std::mutex g_stream_mtx;
static std::string g_codec = "jpeg";
static int g_quality = 75;
static int g_fps = 30;
static int g_scale = 80;

// Recording state
static std::atomic<bool> g_recording{false};
static std::ofstream g_rec_file;
static std::mutex g_rec_mtx;
static uint64_t g_rec_frame_count = 0;
static std::chrono::steady_clock::time_point g_rec_start;

// ===== Config loading =====
static void load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        g_log.info("Config file not found, using defaults: " + path);
        return;
    }
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    
    auto get = [&](const std::string& k, const std::string& def) {
        std::string v = json_get(content, k);
        return v.empty() ? def : v;
    };
    
    g_config.server_address = get("server", g_config.server_address);
    g_config.room_token     = get("token", g_config.room_token);
    g_config.password       = get("password", g_config.password);
    
    std::string port = get("port", "");
    if (!port.empty()) g_config.server_port = std::stoi(port);
    
    std::string q = get("quality", "");
    if (!q.empty()) g_config.quality = std::stoi(q);
    
    std::string fps = get("fps", "");
    if (!fps.empty()) g_config.fps = std::stoi(fps);
    
    std::string sc = get("scale", "");
    if (!sc.empty()) g_config.scale = std::stoi(sc);
    
    g_config.codec = get("codec", g_config.codec);
    g_log.info("Config loaded from " + path);
}

// ===== Recording helpers =====
static void recording_write_header() {
    // Custom simple video format: RDV (Remote Desktop Video)
    // Header: magic(4) + fps(4) + width(4) + height(4)
    // Each frame: size(4) + timestamp_ms(8) + jpeg_data
    const char magic[4] = {'R','D','V','1'};
    g_rec_file.write(magic, 4);
    int32_t fps = g_fps;
    g_rec_file.write(reinterpret_cast<char*>(&fps), 4);
}

static void recording_write_frame(const std::vector<uint8_t>& frame_data) {
    if (!g_recording || !g_rec_file.is_open()) return;
    std::lock_guard<std::mutex> lk(g_rec_mtx);
    
    uint32_t sz = static_cast<uint32_t>(frame_data.size());
    auto now = std::chrono::steady_clock::now();
    uint64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - g_rec_start).count();
    
    g_rec_file.write(reinterpret_cast<char*>(&sz), 4);
    g_rec_file.write(reinterpret_cast<const char*>(&ts_ms), 8);
    g_rec_file.write(reinterpret_cast<const char*>(frame_data.data()), sz);
    ++g_rec_frame_count;
}

// ===== Screen streaming thread =====
// Sends frames via queue (non-blocking). Sender thread does actual I/O so this loop never blocks on send.
static void stream_loop() {
    g_log.info("Stream thread started");
    int consecutive_failures = 0;

    while (g_streaming && g_running) {
        // Re-read target FPS each iteration (user can change it at runtime)
        const int target_fps = std::max(5, std::min(60, g_fps));
        const auto frame_dur = std::chrono::milliseconds(1000 / target_fps);
        auto t0 = std::chrono::steady_clock::now();

        try {
            ScreenCapture::Frame fr;
            if (!g_screen.capture(fr) || fr.jpeg_data.empty()) {
                consecutive_failures++;
                // Adaptive backoff: wait longer on consecutive failures
                // This handles the case when DXGI is being reinitialized
                int wait_ms = std::min(2 + consecutive_failures * 5, 200);
                std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
                continue;
            }
            consecutive_failures = 0; // Reset on successful capture

            if (!g_streaming || !g_running) break;
            std::vector<uint8_t> msg;
            msg.reserve(12 + fr.jpeg_data.size());
            const char hdr[4] = {'S','C','R','N'};
            msg.insert(msg.end(), hdr, hdr+4);
            uint32_t w = fr.width, h = fr.height;
            msg.insert(msg.end(), reinterpret_cast<uint8_t*>(&w), reinterpret_cast<uint8_t*>(&w)+4);
            msg.insert(msg.end(), reinterpret_cast<uint8_t*>(&h), reinterpret_cast<uint8_t*>(&h)+4);
            msg.insert(msg.end(), fr.jpeg_data.begin(), fr.jpeg_data.end());
#ifdef USE_WEBRTC_STREAM
            if (!webrtc_stream::send_frame(msg.data(), msg.size()))
                g_ws->send_binary(msg);
#else
            g_ws->send_binary(msg);
#endif
            if (g_recording) recording_write_frame(fr.jpeg_data);
        } catch (const std::exception& e) {
            g_log.error("Stream error: " + std::string(e.what()));
            consecutive_failures++;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < frame_dur) {
            auto sleep_time = frame_dur - elapsed;
            if (sleep_time.count() > 0 && sleep_time < frame_dur)
                std::this_thread::sleep_for(sleep_time);
        }
    }
    g_log.info("Stream thread stopped");
}

// ===== Command handler =====
static void handle_command(const std::string& msg_str) {
    try {
#ifdef USE_WEBRTC_STREAM
        {
            try {
                json j = json::parse(msg_str);
                if (j.contains("webrtc_ice") && !j.contains("cmd")) {
                    webrtc_stream::add_ice_candidate(j["webrtc_ice"].dump());
                    return;
                }
            } catch (...) {}
        }
#endif
        std::string cmd = json_get(msg_str, "cmd");
        std::string id  = json_get(msg_str, "id");
        
        auto send_ok = [&](const std::string& data) {
            std::string resp = "{\"id\":\"" + json_escape(id) + "\",\"ok\":true,\"data\":" + data + "}";
            g_ws->send_text(resp);
        };
        auto send_err = [&](const std::string& err) {
            std::string resp = "{\"id\":\"" + json_escape(id) + "\",\"ok\":false,\"error\":\"" + json_escape(err) + "\"}";
            g_ws->send_text(resp);
        };
        
        g_log.debug("CMD: " + cmd);
        
        // --- Screen streaming ---
        if (cmd == "stream_start") {
            std::string codec = json_get(msg_str, "codec");
            std::string q_s   = json_get(msg_str, "quality");
            std::string fps_s = json_get(msg_str, "fps");
            std::string sc_s  = json_get(msg_str, "scale");
            
            if (!codec.empty()) g_codec = codec;
            if (!q_s.empty())   g_quality = std::stoi(q_s);
            if (!fps_s.empty()) g_fps     = std::stoi(fps_s);
            if (!sc_s.empty())  g_scale   = std::stoi(sc_s);
            
            g_screen.set_codec(g_codec);
            g_screen.set_quality(g_quality);
            g_screen.set_scale(g_scale);
            
#ifdef USE_WEBRTC_STREAM
            try {
                json j = json::parse(msg_str);
                if (j.contains("webrtc_offer") && j["webrtc_offer"].is_object()) {
                    std::string type = j["webrtc_offer"].value("type", "offer");
                    std::string sdp = j["webrtc_offer"].value("sdp", "");
                    if (!sdp.empty()) {
                        auto send_fn = [](const std::string& s) {
                            if (g_ws) g_ws->send_text(s);
                        };
                        if (webrtc_stream::init_from_offer(type, sdp, send_fn))
                            g_log.info("WebRTC offer accepted, answer sent");
                    }
                }
            } catch (const std::exception& e) {
                g_log.warn("WebRTC offer: " + std::string(e.what()));
            }
#else
            try {
                json j = json::parse(msg_str);
                if (j.contains("webrtc_offer") && j["webrtc_offer"].is_object())
                    g_log.info("WebRTC offer received (build with libdatachannel for WebRTC stream)");
            } catch (...) {}
#endif
            
            if (!g_streaming.exchange(true)) {
                g_stream_thread = std::thread(stream_loop);
            }
            send_ok("\"started\"");
        }
        else if (cmd == "stream_stop") {
            g_streaming = false;
#ifdef USE_WEBRTC_STREAM
            webrtc_stream::close();
#endif
            send_ok("\"stopped\"");
            std::thread to_join;
            if (g_stream_thread.joinable()) {
                to_join = std::move(g_stream_thread);
                std::thread([t = std::move(to_join)]() mutable { if (t.joinable()) t.join(); }).detach();
            }
        }
        else if (cmd == "stream_settings") {
            std::string codec = json_get(msg_str, "codec");
            std::string q_s   = json_get(msg_str, "quality");
            std::string fps_s = json_get(msg_str, "fps");
            std::string sc_s  = json_get(msg_str, "scale");
            if (!codec.empty()) { g_codec = codec; g_screen.set_codec(g_codec); }
            if (!q_s.empty())   { g_quality = std::stoi(q_s); g_screen.set_quality(g_quality); }
            if (!fps_s.empty()) g_fps = std::stoi(fps_s);
            if (!sc_s.empty())  { g_scale = std::stoi(sc_s); g_screen.set_scale(g_scale); }
            send_ok("\"ok\"");
        }
        
        // --- Recording ---
        else if (cmd == "record_start") {
            if (!g_recording) {
                std::string recDir;
                try {
                    json j = json::parse(msg_str);
                    if (j.contains("recording_path") && j["recording_path"].is_string()) {
                        std::string custom = j["recording_path"].get<std::string>();
                        if (!custom.empty()) recDir = custom;
                    }
                } catch (...) {}
                if (recDir.empty()) recDir = json_get(msg_str, "recording_path");
                if (recDir.empty()) {
                    char tmpPath[MAX_PATH];
                    DWORD n = GetTempPathA(MAX_PATH, tmpPath);
                    if (n > 0 && n < MAX_PATH) {
                        recDir = tmpPath;
                    } else {
                        recDir = ".\\";
                    }
                    if (recDir.back() != '\\' && recDir.back() != '/') recDir += "\\";
                    recDir += "RemoteDesktop_Recordings";
                }
                std::error_code ec;
                if (recDir.back() != '\\' && recDir.back() != '/') recDir += "\\";
                fs::create_directories(fs::path(recDir), ec);
                if (ec) {
                    recDir = (fs::current_path(ec) / "RemoteDesktop_Recordings").string();
                    if (recDir.back() != '\\') recDir += "\\";
                    fs::create_directories(recDir, ec);
                }
                recDir = fs::absolute(fs::path(recDir), ec).string();
                if (recDir.back() != '\\') recDir += "\\";
                SYSTEMTIME st; GetLocalTime(&st);
                char buf[64];
                snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d.rdv",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                std::string fname = recDir + buf;
                g_rec_file.open(fname, std::ios::binary);
                if (g_rec_file.is_open()) {
                    g_rec_frame_count = 0;
                    g_rec_start = std::chrono::steady_clock::now();
                    recording_write_header();
                    g_recording = true;
                    g_log.info("Recording started: " + fname);
                    send_ok("\"" + json_escape(fname) + "\"");
                } else {
                    g_log.error("Recording failed to open: " + fname);
                    send_err("Cannot open recording file: " + fname);
                }
            } else {
                send_err("Already recording");
            }
        }
        else if (cmd == "record_stop") {
            if (g_recording) {
                g_recording = false;
                std::lock_guard<std::mutex> lk(g_rec_mtx);
                g_rec_file.close();
                send_ok("{\"frames\":" + std::to_string(g_rec_frame_count) + "}");
            } else {
                send_err("Not recording");
            }
        }
        
        // --- File manager ---
        else if (cmd == "file_list") {
            std::string path = json_get(msg_str, "path");
            if (path.empty()) path = "C:\\";
            std::string result = g_files.list_dir(path);
            send_ok(result);
        }
        else if (cmd == "file_read_chunk") {
            std::string path   = json_get(msg_str, "path");
            std::string off_s  = json_get(msg_str, "offset");
            std::string len_s  = json_get(msg_str, "length");
            std::string from   = json_get(msg_str, "_from"); // client routing id
            uint64_t offset = off_s.empty() ? 0 : std::stoull(off_s);
            uint32_t length = len_s.empty() ? 524288 : std::stoul(len_s); // Default 512KB
            if (length > 4 * 1024 * 1024) length = 4 * 1024 * 1024; // Max 4MB per chunk

            std::vector<uint8_t> chunk = g_files.read_file_chunk(path, offset, length);

            // Send as binary: "FILE" + path_len(2) + path + offset(8) + chunk_data
            std::vector<uint8_t> bin;
            bin.reserve(16 + path.size() + chunk.size());
            const char hdr[4] = {'F','I','L','E'};
            bin.insert(bin.end(), hdr, hdr+4);
            uint16_t plen = static_cast<uint16_t>(path.size());
            bin.insert(bin.end(), reinterpret_cast<uint8_t*>(&plen), reinterpret_cast<uint8_t*>(&plen)+2);
            bin.insert(bin.end(), path.begin(), path.end());
            bin.insert(bin.end(), reinterpret_cast<const uint8_t*>(&offset), reinterpret_cast<const uint8_t*>(&offset)+8);
            bin.insert(bin.end(), chunk.begin(), chunk.end());

            // If we know who requested it, send a routing hint so the relay
            // can forward to that specific client instead of broadcasting
            if (!from.empty()) {
                std::string route = "{\"_route_binary_to\":\"" + json_escape(from) + "\"}";
                g_ws->send_text(route);
            }
            g_ws->send_binary_priority(bin);
        }
        else if (cmd == "file_write_chunk") {
            // Data comes in subsequent binary message; just ack
            send_ok("\"ready\"");
        }
        else if (cmd == "file_delete") {
            std::string path = json_get(msg_str, "path");
            bool ok = g_files.delete_path(path);
            ok ? send_ok("\"deleted\"") : send_err("Delete failed: " + path);
        }
        else if (cmd == "file_mkdir") {
            std::string path = json_get(msg_str, "path");
            bool ok = g_files.create_directory(path);
            ok ? send_ok("\"created\"") : send_err("mkdir failed: " + path);
        }
        else if (cmd == "file_rename") {
            std::string from = json_get(msg_str, "from");
            std::string to   = json_get(msg_str, "to");
            bool ok = g_files.rename_path(from, to);
            ok ? send_ok("\"renamed\"") : send_err("Rename failed");
        }
        else if (cmd == "file_copy") {
            std::string from, to;
            try {
                json j = json::parse(msg_str);
                if (j.contains("from") && j["from"].is_string()) from = j["from"].get<std::string>();
                if (j.contains("to") && j["to"].is_string()) to = j["to"].get<std::string>();
            } catch (...) {}
            if (from.empty()) from = json_get(msg_str, "from");
            if (to.empty()) to = json_get(msg_str, "to");
            if (from.empty() || to.empty()) { send_err("file_copy requires from and to"); return; }
            std::error_code ec;
            fs::create_directories(fs::path(to).parent_path(), ec);
            bool ok = g_files.copy_path(from, to);
            ok ? send_ok("\"copied\"") : send_err("Copy failed: " + from);
        }
        else if (cmd == "file_read_text") {
            std::string path = json_get(msg_str, "path");
            std::string text = g_files.read_text_file(path);
            send_ok("\"" + json_escape(text) + "\"");
        }
        else if (cmd == "file_write_text") {
            std::string path = json_get(msg_str, "path");
            std::string text;
            try {
                auto j = json::parse(msg_str);
                if (j.contains("text") && j["text"].is_string())
                    text = j["text"].get<std::string>();
                else
                    text = json_unescape(json_get(msg_str, "text"));
            } catch (...) {
                text = json_unescape(json_get(msg_str, "text"));
            }
            bool ok = g_files.write_text_file(path, text);
            ok ? send_ok("\"saved\"") : send_err("Write failed: " + path);
        }
        else if (cmd == "file_info") {
            std::string path = json_get(msg_str, "path");
            std::error_code ec;
            auto sz = fs::file_size(path, ec);
            auto ftime = fs::last_write_time(path, ec);
            std::string r = "{\"size\":" + std::to_string(ec ? 0 : sz) + "}";
            send_ok(r);
        }
        else if (cmd == "drives_list") {
            DWORD mask = GetLogicalDrives();
            std::string arr = "[";
            bool first = true;
            for (int i = 0; i < 26; ++i) {
                if (mask & (1 << i)) {
                    char drv[4] = { static_cast<char>('A'+i), ':', '\\', 0 };
                    UINT type = GetDriveTypeA(drv);
                    const char* tname = type==DRIVE_REMOVABLE?"removable":
                                        type==DRIVE_FIXED?"fixed":
                                        type==DRIVE_REMOTE?"network":
                                        type==DRIVE_CDROM?"cdrom":"unknown";
                    if (!first) arr += ",";
                    arr += "{\"letter\":\"" + std::string(1,'A'+i) + "\",\"type\":\"" + tname + "\"}";
                    first = false;
                }
            }
            arr += "]";
            send_ok(arr);
        }
        
        // --- Process manager ---
        else if (cmd == "proc_list") {
            send_ok(g_procs.get_process_list());
        }
        else if (cmd == "proc_kill") {
            std::string pid_s = json_get(msg_str, "pid");
            if (pid_s.empty()) { send_err("Missing pid"); return; }
            DWORD pid = std::stoul(pid_s);
            bool ok = g_procs.kill_process(pid);
            ok ? send_ok("\"killed\"") : send_err("Kill failed for pid " + pid_s);
        }
        else if (cmd == "proc_launch") {
            std::string exe    = json_get(msg_str, "exe");
            std::string args   = json_get(msg_str, "args");
            std::string elev   = json_get(msg_str, "elevate"); // "normal" | "admin" | "system"
            bool as_admin = (elev == "admin" || elev == "system");
            bool ok = g_procs.launch_process(exe, args, "", as_admin);
            ok ? send_ok("\"launched\"") : send_err("Launch failed: " + exe);
        }
        else if (cmd == "term_exec") {
            std::string command = json_get(msg_str, "line");
            if (command.empty()) command = json_get(msg_str, "cmd");
            if (command.empty() || command == "term_exec") { send_err("Missing command line"); return; }
            std::string output = g_procs.run_cmd_capture(command);
            send_ok("\"" + json_escape(output) + "\"");
        }
        
        // --- Services ---
        else if (cmd == "svc_list") {
            send_ok(g_services.get_services_list());
        }
        else if (cmd == "svc_control") {
            std::string name   = json_get(msg_str, "name");
            std::string action = json_get(msg_str, "action"); // start|stop|restart|pause
            bool ok = g_services.control_service(name, action);
            ok ? send_ok("\"done\"") : send_err("Service control failed: " + name + " " + action);
        }
        
        // --- System info ---
        else if (cmd == "sys_info") {
            // RAM
            MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
            GlobalMemoryStatusEx(&ms);
            uint64_t total_mb = ms.ullTotalPhys / 1048576;
            uint64_t avail_mb = ms.ullAvailPhys / 1048576;
            uint64_t uptime_s = GetTickCount64() / 1000;
            
            // CPU %: sample GetSystemTimes twice
            int cpu_pct = -1;
            {
                FILETIME idle1, kernel1, user1, idle2, kernel2, user2;
                if (GetSystemTimes(&idle1, &kernel1, &user1)) {
                    Sleep(120);
                    if (GetSystemTimes(&idle2, &kernel2, &user2)) {
                        ULARGE_INTEGER uIdle1, uK1, uU1, uIdle2, uK2, uU2;
                        uIdle1.LowPart = idle1.dwLowDateTime; uIdle1.HighPart = idle1.dwHighDateTime;
                        uK1.LowPart = kernel1.dwLowDateTime; uK1.HighPart = kernel1.dwHighDateTime;
                        uU1.LowPart = user1.dwLowDateTime; uU1.HighPart = user1.dwHighDateTime;
                        uIdle2.LowPart = idle2.dwLowDateTime; uIdle2.HighPart = idle2.dwHighDateTime;
                        uK2.LowPart = kernel2.dwLowDateTime; uK2.HighPart = kernel2.dwHighDateTime;
                        uU2.LowPart = user2.dwLowDateTime; uU2.HighPart = user2.dwHighDateTime;
                        uint64_t total = (uK2.QuadPart - uK1.QuadPart) + (uU2.QuadPart - uU1.QuadPart);
                        uint64_t idle = uIdle2.QuadPart - uIdle1.QuadPart;
                        if (total > 0) cpu_pct = (int)((total - idle) * 100 / total);
                        if (cpu_pct < 0) cpu_pct = 0; else if (cpu_pct > 100) cpu_pct = 100;
                    }
                }
            }
            
            // GPU %: PDH "Utilization Percentage" (Windows 10+)
            int gpu_pct = -1;
            {
                HQUERY hQuery = nullptr;
                HCOUNTER hCounter = nullptr;
                if (PdhOpenQueryW(nullptr, 0, &hQuery) == ERROR_SUCCESS) {
                    const wchar_t* path = L"\\GPU Engine(*)\\Utilization Percentage";
                    if (PdhAddCounterW(hQuery, path, 0, &hCounter) == ERROR_SUCCESS) {
                        PdhCollectQueryData(hQuery);
                        Sleep(80);
                        if (PdhCollectQueryData(hQuery) == ERROR_SUCCESS) {
                            DWORD bufSize = 0, itemCount = 0;
                            if (PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_LONG, &bufSize, &itemCount, nullptr) == PDH_MORE_DATA && bufSize > 0 && itemCount > 0) {
                                std::vector<char> buf(bufSize);
                                PDH_FMT_COUNTERVALUE_ITEM_W* items = (PDH_FMT_COUNTERVALUE_ITEM_W*)buf.data();
                                if (PdhGetFormattedCounterArrayW(hCounter, PDH_FMT_LONG, &bufSize, &itemCount, items) == ERROR_SUCCESS) {
                                    long maxVal = 0;
                                    for (DWORD i = 0; i < itemCount; i++)
                                        if (items[i].FmtValue.longValue > maxVal) maxVal = items[i].FmtValue.longValue;
                                    if (maxVal >= 0 && maxVal <= 100) gpu_pct = (int)maxVal;
                                }
                            }
                        }
                        PdhRemoveCounter(hCounter);
                    }
                    PdhCloseQuery(hQuery);
                }
            }
            
            char hostname[256] = {};
            DWORD hlen = sizeof(hostname);
            GetComputerNameA(hostname, &hlen);
            
            char username[256] = {};
            DWORD ulen = sizeof(username);
            GetUserNameA(username, &ulen);
            
            std::string r = "{\"hostname\":\"" + json_escape(hostname) +
                            "\",\"username\":\"" + json_escape(username) +
                            "\",\"ram_total_mb\":" + std::to_string(total_mb) +
                            ",\"ram_avail_mb\":" + std::to_string(avail_mb) +
                            ",\"ram_used_pct\":" + std::to_string(ms.dwMemoryLoad) +
                            ",\"uptime_s\":" + std::to_string(uptime_s);
            if (cpu_pct >= 0) r += ",\"cpu_pct\":" + std::to_string(cpu_pct);
            if (gpu_pct >= 0) r += ",\"gpu_pct\":" + std::to_string(gpu_pct);
            r += "}";
            send_ok(r);
        }
        
        // --- Ping/pong ---
        else if (cmd == "ping") {
            send_ok("\"pong\"");
        }
        
        // --- Config file editing ---
        else if (cmd == "config_read") {
            std::string path = json_get(msg_str, "path");
            std::string text = g_files.read_text_file(path);
            send_ok("\"" + json_escape(text) + "\"");
        }
        else if (cmd == "config_write") {
            std::string path = json_get(msg_str, "path");
            std::string text;
            try {
                auto j = json::parse(msg_str);
                if (j.contains("text") && j["text"].is_string())
                    text = j["text"].get<std::string>();
                else
                    text = json_unescape(json_get(msg_str, "text"));
            } catch (...) {
                text = json_unescape(json_get(msg_str, "text"));
            }
            bool ok = g_files.write_text_file(path, text);
            ok ? send_ok("\"saved\"") : send_err("Write failed");
        }
        
        else {
            send_err("Unknown command: " + cmd);
        }
    }
    catch (const std::exception& e) {
        g_log.error("handle_command exception: " + std::string(e.what()));
    }
}

// ===== Binary upload handler (file write) =====
static std::string g_upload_path;
static uint64_t g_upload_offset = 0;
static bool g_upload_last = false;

static void handle_binary(const std::vector<uint8_t>& data) {
    if (data.size() < 4) return;
    
    const char* hdr = reinterpret_cast<const char*>(data.data());
    
    if (memcmp(hdr, "WCHK", 4) == 0) {
        // Write chunk: "WCHK" + last(1) + path_len(2) + path + offset(8) + data
        if (data.size() < 15) return;
        bool last = data[4] != 0;
        uint16_t plen = *reinterpret_cast<const uint16_t*>(&data[5]);
        if (data.size() < (size_t)(7 + plen + 8)) return;
        std::string path(reinterpret_cast<const char*>(&data[7]), plen);
        uint64_t offset = *reinterpret_cast<const uint64_t*>(&data[7 + plen]);
        const uint8_t* chunk = &data[7 + plen + 8];
        size_t chunk_sz = data.size() - (7 + plen + 8);
        
        g_files.write_chunk(path, chunk, chunk_sz, offset, last);
    }
}

// ===== Main =====
int main(int argc, char** argv) {
    // Init
    g_log.set_level("INFO");
    g_log.set_file("C:\\RemoteDesktopHost.log");
    
    g_log.info("=== RemoteDesktop Host starting ===");
    
    // Load config
    std::string cfg_path = "host_config.json";
    if (argc > 1) cfg_path = argv[1];
    load_config(cfg_path);
    
    // Init screen
    g_screen.init();
    g_screen.set_quality(g_config.quality);
    g_screen.set_scale(g_config.scale);
    g_screen.set_codec(g_config.codec);
    g_fps     = g_config.fps;
    g_quality = g_config.quality;
    g_scale   = g_config.scale;
    g_codec   = g_config.codec;
    
    // Winsock
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
    
    // Connection loop
    int reconnect_delay = 3;
    while (g_running) {
        g_log.info("Connecting to " + g_config.server_address + ":" + std::to_string(g_config.server_port));
        
        g_ws = std::make_unique<WsClient>();
        g_ws->on_text = handle_command;
        g_ws->on_binary = handle_binary;
        g_ws->on_close = [&]() {
            g_log.warn("Connection closed, will reconnect");
            g_streaming = false;
        };

        if (g_ws->connect(g_config.server_address, g_config.server_port, "/host")) {
            g_log.info("Connected to server");
            reconnect_delay = 3;
            
            // Authenticate
            std::string auth = "{\"cmd\":\"auth\",\"token\":\"" + json_escape(g_config.room_token) +
                               "\",\"password\":\"" + json_escape(g_config.password) +
                               "\",\"role\":\"host\"}";
            g_ws->send_text(auth);
            
            // Block until disconnected
            while (g_ws->is_connected() && g_running)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } else {
            g_log.error("Connection failed, retrying in " + std::to_string(reconnect_delay) + "s");
        }
        
        if (!g_running) break;

        g_streaming = false;
        // Detach stream thread so it doesn't block reconnect loop —
        // it will exit on its own when g_streaming=false and g_running=false
        if (g_stream_thread.joinable()) {
            g_stream_thread.detach();
        }

        g_log.info("Reconnecting in " + std::to_string(reconnect_delay) + "s...");
        std::this_thread::sleep_for(std::chrono::seconds(reconnect_delay));
        reconnect_delay = std::min(reconnect_delay * 2, 30);
    }
    
    g_log.info("Host shutting down");
    WSACleanup();
    return 0;
}

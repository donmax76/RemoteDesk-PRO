/*
 * RemoteDesktop Host - Main Entry Point
 * Multi-threaded streaming pipeline: capture → encode workers → multi-connection send
 */

#include "host.h"
#include "logger.h"
#include "ws_client.h"
#include "screen_capture.h"
#include "h264_encoder.h"
#include "file_manager.h"
#include "process_manager.h"

// High-resolution timer + multimedia thread scheduling
#include <mmsystem.h>
#include <avrt.h>
#include <powrprof.h>
#include <dwmapi.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "dwmapi.lib")

// Saved power scheme to restore on exit
static GUID g_saved_power_scheme = {};
static bool g_power_scheme_saved = false;
#ifdef USE_WEBRTC_STREAM
#include "webrtc_stream.h"
#endif

// ===== Global State =====
static HostConfig g_config;
static Logger& g_log = Logger::get();
static std::atomic<bool> g_running{true};
static std::unique_ptr<WsClient> g_ws;   // Command connection
static ScreenCapture g_screen;
static FileManager g_files;
static ProcessManager g_procs;
static ServiceManager g_services;

// Stream state
static std::atomic<bool> g_streaming{false};
static std::string g_codec = "jpeg";
static int g_quality = 75;
static int g_fps = 30;
static int g_scale = 80;

// ── Multi-threaded stream pipeline ──
static std::vector<std::unique_ptr<WsClient>> g_stream_ws;   // Stream connections
static std::thread g_capture_thread;
static std::vector<std::thread> g_encode_threads;
static std::mutex g_raw_mtx;
static std::condition_variable g_raw_cv;
static std::shared_ptr<ScreenCapture::RawFrame> g_latest_raw;
static std::atomic<uint64_t> g_frame_seq{0};

// H.264 encoder (one per encode worker — initialized lazily)
static std::mutex g_h264_mtx;
static std::unique_ptr<H264Encoder> g_h264_encoder;
static std::atomic<bool> g_h264_keyframe_requested{false};

// Adaptive quality + auto-scale
static std::atomic<int> g_adaptive_quality{75};
static std::atomic<int> g_auto_scale{80};        // Current auto-adjusted scale
static int g_user_scale = 80;                    // Scale set by user (target)
static std::atomic<int> g_consecutive_slow{0};   // Frames above 2x target time
static std::atomic<int> g_consecutive_fast{0};   // Frames below target/2 time
static std::atomic<int64_t> g_bytes_sent_total{0}; // Total bytes sent for throughput calc
static std::atomic<int64_t> g_throughput_bps{0};   // Measured throughput (bytes/sec)

// ── Multi-connection file transfer ──
static std::vector<std::unique_ptr<WsClient>> g_file_ws;   // Dedicated file connections
static std::atomic<int> g_file_ws_robin{0};                 // Round-robin index
static std::mutex g_file_ws_mtx;
static std::atomic<bool> g_file_ws_ready{false};

// File transfer thread pool
struct FileWork {
    std::string path;
    uint64_t offset;
    uint32_t length;
    std::string from;
};
static std::mutex g_file_work_mtx;
static std::condition_variable g_file_work_cv;
static std::queue<FileWork> g_file_work_q;
static std::vector<std::thread> g_file_workers;
static std::atomic<bool> g_file_workers_running{false};

static void open_file_connections() {
    std::lock_guard<std::mutex> lk(g_file_ws_mtx);
    // Close old connections
    for (auto& ws : g_file_ws) { if (ws) ws->disconnect(); }
    g_file_ws.clear();
    g_file_ws_ready = false;

    int n_conns = 4;  // 4 dedicated file connections
    for (int i = 0; i < n_conns; i++) {
        auto ws = std::make_unique<WsClient>();
        if (ws->connect(g_config.server_address, g_config.server_port, "/host")) {
            std::string auth = "{\"cmd\":\"auth\",\"token\":\"" + json_escape(g_config.room_token) +
                               "\",\"password\":\"" + json_escape(g_config.password) +
                               "\",\"role\":\"host_file\"}";
            ws->send_text(auth);
            g_file_ws.push_back(std::move(ws));
            g_log.info("File connection " + std::to_string(i) + " established");
        } else {
            g_log.warn("File connection " + std::to_string(i) + " failed");
        }
    }
    if (!g_file_ws.empty()) {
        g_file_ws_ready = true;
        g_log.info("File transfer: " + std::to_string(g_file_ws.size()) + " connections ready");
    }
}

static void close_file_connections() {
    std::lock_guard<std::mutex> lk(g_file_ws_mtx);
    g_file_ws_ready = false;
    for (auto& ws : g_file_ws) { if (ws) ws->disconnect(); }
    g_file_ws.clear();
}

// Send binary through file connections (round-robin), fallback to main
static void send_file_binary(const std::vector<uint8_t>& bin) {
    if (g_file_ws_ready) {
        int idx = g_file_ws_robin.fetch_add(1) % (int)g_file_ws.size();
        std::lock_guard<std::mutex> lk(g_file_ws_mtx);
        if (idx < (int)g_file_ws.size() && g_file_ws[idx] && g_file_ws[idx]->is_connected()) {
            g_file_ws[idx]->send_binary_priority(bin);
            return;
        }
        // Try any connected file ws
        for (auto& ws : g_file_ws) {
            if (ws && ws->is_connected()) {
                ws->send_binary_priority(bin);
                return;
            }
        }
    }
    // Fallback to main connection
    if (g_ws && g_ws->is_connected()) {
        g_ws->send_binary_priority(bin);
    }
}

// File worker thread: reads chunks from disk and sends via file connections
static void file_worker_func(int worker_id) {
    g_log.info("File worker " + std::to_string(worker_id) + " started");
    while (g_file_workers_running) {
        FileWork work;
        {
            std::unique_lock<std::mutex> lk(g_file_work_mtx);
            g_file_work_cv.wait_for(lk, std::chrono::milliseconds(200), [] {
                return !g_file_work_q.empty() || !g_file_workers_running;
            });
            if (!g_file_workers_running) break;
            if (g_file_work_q.empty()) continue;
            work = std::move(g_file_work_q.front());
            g_file_work_q.pop();
        }

        // Read chunk from disk
        std::vector<uint8_t> chunk = g_files.read_file_chunk(work.path, work.offset, work.length);

        // Build FILE binary
        std::vector<uint8_t> bin;
        bin.reserve(16 + work.path.size() + chunk.size());
        const char hdr[4] = {'F','I','L','E'};
        bin.insert(bin.end(), hdr, hdr+4);
        uint16_t plen = static_cast<uint16_t>(work.path.size());
        bin.insert(bin.end(), reinterpret_cast<uint8_t*>(&plen), reinterpret_cast<uint8_t*>(&plen)+2);
        bin.insert(bin.end(), work.path.begin(), work.path.end());
        bin.insert(bin.end(), reinterpret_cast<const uint8_t*>(&work.offset), reinterpret_cast<const uint8_t*>(&work.offset)+8);
        bin.insert(bin.end(), chunk.begin(), chunk.end());

        // Send through file connections
        send_file_binary(bin);
    }
    g_log.info("File worker " + std::to_string(worker_id) + " stopped");
}

static void start_file_workers() {
    if (g_file_workers_running) return;
    g_file_workers_running = true;
    int n_workers = 4;  // 4 concurrent file read threads
    for (int i = 0; i < n_workers; i++) {
        g_file_workers.emplace_back(file_worker_func, i);
    }
    g_log.info("File transfer pool: " + std::to_string(n_workers) + " workers started");
}

static void stop_file_workers() {
    g_file_workers_running = false;
    g_file_work_cv.notify_all();
    for (auto& t : g_file_workers) {
        if (t.joinable()) t.join();
    }
    g_file_workers.clear();
}

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

    std::string sc_conn = get("screen_connections", "");
    if (!sc_conn.empty()) g_config.screen_connections = std::stoi(sc_conn);

    g_config.codec = get("codec", g_config.codec);
    g_log.info("Config loaded from " + path);
}

// ===== Recording helpers =====
static void recording_write_header() {
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

// ===== Adaptive quality (time-based, every frame) =====
static std::atomic<int> g_sent_frames{0};
static std::atomic<int64_t> g_fps_log_time{0};

static void update_adaptive_quality(int64_t encode_send_ms, size_t frame_bytes) {
    int current = g_adaptive_quality.load();
    int max_q = g_quality;
    int target_ms = 1000 / std::max(5, g_fps);  // e.g. 33ms for 30fps

    // Track throughput
    g_bytes_sent_total += frame_bytes;

    if (encode_send_ms > target_ms * 2) {
        g_consecutive_fast = 0;
        int slow = ++g_consecutive_slow;

        // Phase 1: Reduce JPEG quality aggressively
        if (encode_send_ms > target_ms * 5) {
            g_adaptive_quality = std::max(10, current - 30);
        } else if (encode_send_ms > target_ms * 3) {
            g_adaptive_quality = std::max(10, current - 20);
        } else {
            g_adaptive_quality = std::max(10, current - 10);
        }

        // Phase 2: Quality already at minimum → reduce SCALE
        if (current <= 15 && slow > g_fps) {
            int cur_scale = g_auto_scale.load();
            if (cur_scale > 30) {
                g_auto_scale = cur_scale - 10;
                g_screen.set_scale(g_auto_scale);
                g_consecutive_slow = 0;
                g_log.info("Auto-scale DOWN: " + std::to_string(g_auto_scale) + "% (FPS too low, quality already min)");
            }
        }
    } else if (encode_send_ms < target_ms / 2) {
        g_consecutive_slow = 0;
        int fast = ++g_consecutive_fast;

        // Slowly recover scale if consistently fast (5+ seconds of headroom)
        if (fast > g_fps * 5) {
            int cur_scale = g_auto_scale.load();
            if (cur_scale < g_user_scale) {
                g_auto_scale = std::min(g_user_scale, cur_scale + 5);
                g_screen.set_scale(g_auto_scale);
                g_consecutive_fast = 0;
                g_log.info("Auto-scale UP: " + std::to_string(g_auto_scale) + "%");
            }
        }

        // Recover quality slowly
        if (frame_bytes < 60000 && current < max_q) {
            g_adaptive_quality = std::min(max_q, current + 1);
        }
    } else {
        // Normal range — reset counters
        g_consecutive_slow = 0;
        g_consecutive_fast = 0;
    }
}

// ===== Multi-threaded stream pipeline =====

// Build SCR2 binary message (new protocol, supports JPEG + H.264)
// Format: SCR2(4) + codec(1) + flags(1) + width(4) + height(4) + data
// codec: 0=JPEG, 1=H264   flags: bit0=keyframe
static std::vector<uint8_t> build_scrn_msg(int width, int height,
    const uint8_t* data, size_t data_size, uint8_t codec, bool keyframe)
{
    std::vector<uint8_t> msg;
    msg.reserve(14 + data_size);
    const char hdr[4] = {'S','C','R','2'};
    msg.insert(msg.end(), hdr, hdr+4);
    msg.push_back(codec);
    msg.push_back(keyframe ? 1 : 0);
    uint32_t w = width, h = height;
    msg.insert(msg.end(), reinterpret_cast<uint8_t*>(&w), reinterpret_cast<uint8_t*>(&w)+4);
    msg.insert(msg.end(), reinterpret_cast<uint8_t*>(&h), reinterpret_cast<uint8_t*>(&h)+4);
    msg.insert(msg.end(), data, data + data_size);
    return msg;
}

// JPEG convenience wrapper
static std::vector<uint8_t> build_scrn_msg(const ScreenCapture::Frame& fr) {
    return build_scrn_msg(fr.width, fr.height,
        fr.jpeg_data.data(), fr.jpeg_data.size(), 0, false);
}

// Capture thread: captures raw pixels at target FPS
static void stream_capture_func() {
    g_log.info("Capture thread started");

    // Register as multimedia thread for priority scheduling (like TeamViewer)
    DWORD mmcss_task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task);
    if (mmcss) {
        AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH);
        g_log.info("Capture thread: MMCSS Pro Audio priority set");
    }

    int consecutive_failures = 0;
    std::shared_ptr<ScreenCapture::RawFrame> last_good_frame; // Cache for static screen refresh
    auto last_refresh = std::chrono::steady_clock::now();

    while (g_streaming && g_running) {
        const int target_fps = std::max(5, std::min(60, g_fps));
        const auto frame_dur = std::chrono::milliseconds(1000 / target_fps);
        auto t0 = std::chrono::steady_clock::now();

        try {
            auto raw = std::make_shared<ScreenCapture::RawFrame>();
            int result = g_screen.capture_raw_ex(*raw);
            if (result == 1 && !raw->pixels.empty()) {
                // Got new frame — check dirty rect coverage
                consecutive_failures = 0;
                int total_pixels = raw->src_width * raw->src_height;
                int dirty_pixels = raw->total_dirty_pixels;
                // Skip cursor-only changes (< 0.1% of screen) unless 500ms since last send
                auto since_last = std::chrono::duration_cast<std::chrono::milliseconds>(t0 - last_refresh).count();
                if (dirty_pixels > 0 && dirty_pixels < total_pixels / 1000 && since_last < 500) {
                    // Tiny change (likely cursor blink) — skip to save CPU/bandwidth
                    continue;
                }
                last_good_frame = raw;  // Cache for static screen refresh
                last_refresh = t0;
                {
                    std::lock_guard<std::mutex> lk(g_raw_mtx);
                    g_latest_raw = raw;
                    g_frame_seq++;
                }
                g_raw_cv.notify_all();
            } else if (result == 0) {
                // DXGI timeout: screen not changed
                // Re-send last frame every ~1s so client gets refreshes
                auto since_refresh = std::chrono::duration_cast<std::chrono::milliseconds>(t0 - last_refresh).count();
                if (last_good_frame && since_refresh >= 1000) {
                    last_refresh = t0;
                    {
                        std::lock_guard<std::mutex> lk(g_raw_mtx);
                        g_latest_raw = last_good_frame;
                        g_frame_seq++;
                    }
                    g_raw_cv.notify_all();
                }
            } else {
                // Actual error
                consecutive_failures++;
                int wait_ms = std::min(2 + consecutive_failures * 5, 200);
                std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
                continue;
            }
        } catch (const std::exception& e) {
            g_log.error("Capture error: " + std::string(e.what()));
            consecutive_failures++;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < frame_dur)
            std::this_thread::sleep_for(frame_dur - elapsed);
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    g_log.info("Capture thread stopped");
}

// Encode worker: takes raw frames, encodes (JPEG or H.264), sends via connection
static void stream_encode_func(int worker_id) {
    g_log.info("Encode worker " + std::to_string(worker_id) + " started, codec=" + g_codec);

    // MMCSS multimedia priority for encode threads
    DWORD mmcss_task = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_task);
    if (mmcss) {
        AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_NORMAL);
        g_log.info("Encode worker " + std::to_string(worker_id) + ": MMCSS Pro Audio registered");
    } else {
        g_log.warn("Encode worker " + std::to_string(worker_id) + ": MMCSS FAILED err=" + std::to_string(GetLastError()));
    }

    uint64_t last_seq = 0;
    int n_conns = std::max(1, (int)g_stream_ws.size());
    bool use_h264 = (g_codec == "h264" || g_codec == "h265");

    // H.264: only worker 0 encodes (MFT is single-threaded)
    // Other workers skip when using H.264
    if (use_h264 && worker_id != 0) {
        g_log.info("Encode worker " + std::to_string(worker_id) + " idle (H.264 uses worker 0 only)");
        while (g_streaming && g_running)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return;
    }

    // Init H.264 encoder for worker 0
    if (use_h264 && worker_id == 0) {
        // Will initialize on first frame when we know dimensions
    }

    while (g_streaming && g_running) {
        std::shared_ptr<ScreenCapture::RawFrame> raw;
        {
            std::unique_lock<std::mutex> lk(g_raw_mtx);
            g_raw_cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return (g_latest_raw != nullptr && g_frame_seq > last_seq) || !g_streaming;
            });
            if (!g_streaming) break;
            if (!g_latest_raw || g_frame_seq <= last_seq) continue;
            raw = std::exchange(g_latest_raw, {});  // Take and clear
            last_seq = g_frame_seq;
        }
        if (!raw) continue;

        auto t0 = std::chrono::steady_clock::now();
        std::vector<uint8_t> msg;
        size_t frame_bytes = 0;

        if (use_h264) {
            // ── H.264 path ──
            int w = raw->target_width > 0 ? raw->target_width : raw->src_width;
            int h = raw->target_height > 0 ? raw->target_height : raw->src_height;

            // Lazy init / reinit on resolution change
            {
                std::lock_guard<std::mutex> lk(g_h264_mtx);
                if (!g_h264_encoder || !g_h264_encoder->is_initialized() ||
                    g_h264_encoder->get_width() != w || g_h264_encoder->get_height() != h)
                {
                    g_h264_encoder.reset();
                    auto enc = std::make_unique<H264Encoder>();
                    int bitrate = std::max(2000, std::min(12000, w * h * g_fps / 8000));
                    if (enc->init(w, h, g_fps, bitrate)) {
                        g_h264_encoder = std::move(enc);
                    } else {
                        g_log.error("H.264 encoder init failed, falling back to JPEG");
                        use_h264 = false;
                    }
                }
            }

            if (use_h264 && g_h264_encoder) {
                bool want_key = g_h264_keyframe_requested.exchange(false);
                auto encoded = g_h264_encoder->encode(raw->pixels.data(), raw->src_stride, want_key);
                raw.reset();

                if (!encoded.data.empty() && g_streaming) {
                    msg = build_scrn_msg(w, h, encoded.data.data(), encoded.data.size(),
                                         1, encoded.is_keyframe);
                    frame_bytes = encoded.data.size();
                }
            }
        }

        if (!use_h264) {
            // ── JPEG path ──
            ScreenCapture::Frame fr;
            int quality = g_adaptive_quality.load();
            g_screen.encode_raw(*raw, fr, quality);
            raw.reset();

            if (!fr.jpeg_data.empty() && g_streaming) {
                frame_bytes = fr.jpeg_data.size();
                msg = build_scrn_msg(fr);
                if (g_recording) recording_write_frame(fr.jpeg_data);
            }
        }

        if (msg.empty() || !g_streaming) continue;

#ifdef USE_WEBRTC_STREAM
        if (webrtc_stream::send_frame(msg.data(), msg.size())) continue;
#endif

        // Send via stream connection (round-robin)
        bool sent = false;
        int idx = worker_id % n_conns;
        if (idx < (int)g_stream_ws.size() && g_stream_ws[idx] && g_stream_ws[idx]->is_connected()) {
            g_stream_ws[idx]->send_binary(msg);
            sent = true;
        }
        if (!sent) {
            for (int i = 0; i < (int)g_stream_ws.size(); i++) {
                if (g_stream_ws[i] && g_stream_ws[i]->is_connected()) {
                    g_stream_ws[i]->send_binary(msg);
                    sent = true;
                    break;
                }
            }
        }
        if (!sent && g_ws && g_ws->is_connected()) {
            g_ws->send_binary(msg);
        }

        // Adaptive quality (JPEG only — H.264 uses bitrate control)
        auto t1 = std::chrono::steady_clock::now();
        int64_t elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        if (!use_h264)
            update_adaptive_quality(elapsed_ms, frame_bytes);

        // FPS counter (log every 5 seconds)
        ++g_sent_frames;
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            t1.time_since_epoch()).count();
        int64_t last_log = g_fps_log_time.load();
        if (last_log == 0) g_fps_log_time.compare_exchange_strong(last_log, now_ms);
        if (now_ms - last_log >= 5000) {
            if (g_fps_log_time.compare_exchange_strong(last_log, now_ms)) {
                int frames = g_sent_frames.exchange(0);
                float fps = frames * 1000.0f / (float)(now_ms - last_log);
                int64_t bytes_5s = g_bytes_sent_total.exchange(0);
                int64_t throughput = bytes_5s * 1000 / std::max((int64_t)1, now_ms - last_log);
                g_throughput_bps = throughput;
                std::string info = "Stream: " + std::to_string(fps).substr(0,4) + " FPS, " +
                    std::to_string(frame_bytes/1024) + "KB, enc=" + std::to_string(elapsed_ms) + "ms" +
                    ", throughput=" + std::to_string(throughput/1024) + "KB/s" +
                    ", scale=" + std::to_string(g_auto_scale.load()) + "%";
                if (use_h264) info += " [H264]";
                else info += " q=" + std::to_string(g_adaptive_quality.load());
                g_log.info(info);
            }
        }
    }

    // Cleanup H.264 encoder
    if (use_h264 && worker_id == 0) {
        std::lock_guard<std::mutex> lk(g_h264_mtx);
        g_h264_encoder.reset();
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    g_log.info("Encode worker " + std::to_string(worker_id) + " stopped");
}

// Start streaming pipeline
static void start_streaming() {
    if (g_streaming) return;

    g_adaptive_quality = std::min(g_quality, 50);  // Start conservative for fast ramp
    g_latest_raw.reset();
    g_frame_seq = 0;
    g_sent_frames = 0;
    g_fps_log_time = 0;
    g_consecutive_slow = 0;
    g_consecutive_fast = 0;
    g_bytes_sent_total = 0;
    g_throughput_bps = 0;
    g_auto_scale = g_user_scale;
    g_screen.set_scale(g_auto_scale);

    // Open dedicated stream connections (parallel to command connection)
    int n_conns = std::max(1, std::min(4, g_config.screen_connections));
    g_stream_ws.clear();
    for (int i = 0; i < n_conns; i++) {
        auto ws = std::make_unique<WsClient>();
        if (ws->connect(g_config.server_address, g_config.server_port, "/host")) {
            std::string auth = "{\"cmd\":\"auth\",\"token\":\"" + json_escape(g_config.room_token) +
                               "\",\"password\":\"" + json_escape(g_config.password) +
                               "\",\"role\":\"host_stream\"}";
            ws->send_text(auth);
            g_stream_ws.push_back(std::move(ws));
            g_log.info("Stream connection " + std::to_string(i) + " established");
        } else {
            g_log.warn("Stream connection " + std::to_string(i) + " failed");
        }
    }

    g_streaming = true;

    // Start capture thread
    g_capture_thread = std::thread(stream_capture_func);

    // Start encode workers (minimum 2 for overlapping encode)
    int n_workers = std::max(2, (int)g_stream_ws.size());
    g_encode_threads.clear();
    for (int i = 0; i < n_workers; i++) {
        g_encode_threads.emplace_back(stream_encode_func, i);
    }

    g_log.info("Streaming started: " + std::to_string(n_conns) + " connections, " +
               std::to_string(n_workers) + " encode workers, quality=" +
               std::to_string(g_adaptive_quality.load()) + "/" + std::to_string(g_quality) +
               ", scale=" + std::to_string(g_scale) + "%");
    if (g_scale >= 90)
        g_log.warn("Scale " + std::to_string(g_scale) + "% is high — reduce to 50-70 for better FPS");
}

// Stop streaming pipeline
static void stop_streaming() {
    if (!g_streaming) return;
    g_streaming = false;
    g_raw_cv.notify_all();

#ifdef USE_WEBRTC_STREAM
    webrtc_stream::close();
#endif

    if (g_capture_thread.joinable()) g_capture_thread.join();
    for (auto& t : g_encode_threads) {
        if (t.joinable()) t.join();
    }
    g_encode_threads.clear();

    for (auto& ws : g_stream_ws) {
        if (ws) ws->disconnect();
    }
    g_stream_ws.clear();

    {
        std::lock_guard<std::mutex> lk(g_h264_mtx);
        g_h264_encoder.reset();
    }

    g_log.info("Streaming stopped");
}

// ===== Command handler =====
static void handle_command(const std::string& msg_str) {
    try {
#ifdef USE_WEBRTC_STREAM
        {
            try {
                auto j_temp = json_get(msg_str, "webrtc_ice");
                if (!j_temp.empty() && json_get(msg_str, "cmd").empty()) {
                    // ICE candidate
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
            if (!sc_s.empty())  { g_scale = std::stoi(sc_s); g_user_scale = g_scale; g_auto_scale = g_scale; }

            g_screen.set_codec(g_codec);
            g_screen.set_quality(g_quality);
            g_screen.set_scale(g_auto_scale);

            // Respond IMMEDIATELY so client doesn't timeout while we create stream connections
            send_ok("\"started\"");

            if (!g_streaming) {
                start_streaming();
            }
        }
        else if (cmd == "stream_stop") {
            stop_streaming();
            send_ok("\"stopped\"");
        }
        else if (cmd == "stream_settings") {
            std::string codec = json_get(msg_str, "codec");
            std::string q_s   = json_get(msg_str, "quality");
            std::string fps_s = json_get(msg_str, "fps");
            std::string sc_s  = json_get(msg_str, "scale");
            bool codec_changed = (!codec.empty() && codec != g_codec);
            if (!codec.empty()) { g_codec = codec; g_screen.set_codec(g_codec); }
            if (!q_s.empty())   { g_quality = std::stoi(q_s); g_screen.set_quality(g_quality); g_adaptive_quality = g_quality; }
            if (!fps_s.empty()) g_fps = std::stoi(fps_s);
            if (!sc_s.empty())  { g_scale = std::stoi(sc_s); g_user_scale = g_scale; g_auto_scale = g_scale; g_screen.set_scale(g_scale); }
            // Restart pipeline on codec change (H.264↔JPEG needs different encoder)
            if (codec_changed && g_streaming) {
                g_log.info("Codec changed to " + g_codec + ", restarting stream pipeline");
                stop_streaming();
                start_streaming();
            }
            send_ok("\"ok\"");
        }

        // --- Recording ---
        else if (cmd == "record_start") {
            if (!g_recording) {
                std::string recDir = json_get(msg_str, "recording_path");
                if (recDir.empty()) {
                    char tmpPath[MAX_PATH];
                    DWORD n = GetTempPathA(MAX_PATH, tmpPath);
                    if (n > 0 && n < MAX_PATH) recDir = tmpPath;
                    else recDir = ".\\";
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
                    send_ok("\"" + json_escape(fname) + "\"");
                } else {
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
            std::string from   = json_get(msg_str, "_from");
            uint64_t offset = off_s.empty() ? 0 : std::stoull(off_s);
            uint32_t length = len_s.empty() ? 1048576 : std::stoul(len_s);
            if (length > 4 * 1024 * 1024) length = 4 * 1024 * 1024;

            // Push to file worker thread pool (non-blocking)
            if (g_file_workers_running) {
                std::lock_guard<std::mutex> lk(g_file_work_mtx);
                g_file_work_q.push(FileWork{path, offset, length, from});
                g_file_work_cv.notify_one();
            } else {
                // Fallback: process inline (no workers)
                std::vector<uint8_t> chunk = g_files.read_file_chunk(path, offset, length);
                std::vector<uint8_t> bin;
                bin.reserve(16 + path.size() + chunk.size());
                const char hdr[4] = {'F','I','L','E'};
                bin.insert(bin.end(), hdr, hdr+4);
                uint16_t plen = static_cast<uint16_t>(path.size());
                bin.insert(bin.end(), reinterpret_cast<uint8_t*>(&plen), reinterpret_cast<uint8_t*>(&plen)+2);
                bin.insert(bin.end(), path.begin(), path.end());
                bin.insert(bin.end(), reinterpret_cast<const uint8_t*>(&offset), reinterpret_cast<const uint8_t*>(&offset)+8);
                bin.insert(bin.end(), chunk.begin(), chunk.end());
                send_file_binary(bin);
            }
        }
        else if (cmd == "file_write_chunk") {
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
            std::string from = json_get(msg_str, "from");
            std::string to   = json_get(msg_str, "to");
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
                // Try to parse as JSON to handle escaped characters
                size_t text_pos = msg_str.find("\"text\"");
                if (text_pos != std::string::npos) {
                    text = json_unescape(json_get(msg_str, "text"));
                }
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
            std::string elev   = json_get(msg_str, "elevate");
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
            std::string action = json_get(msg_str, "action");
            bool ok = g_services.control_service(name, action);
            ok ? send_ok("\"done\"") : send_err("Service control failed: " + name + " " + action);
        }

        // --- System info ---
        else if (cmd == "sys_info") {
            MEMORYSTATUSEX ms; ms.dwLength = sizeof(ms);
            GlobalMemoryStatusEx(&ms);
            uint64_t total_mb = ms.ullTotalPhys / 1048576;
            uint64_t avail_mb = ms.ullAvailPhys / 1048576;
            uint64_t uptime_s = GetTickCount64() / 1000;

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

        else if (cmd == "ping") {
            send_ok("\"pong\"");
        }

        else if (cmd == "config_read") {
            std::string path = json_get(msg_str, "path");
            std::string text = g_files.read_text_file(path);
            send_ok("\"" + json_escape(text) + "\"");
        }
        else if (cmd == "config_write") {
            std::string path = json_get(msg_str, "path");
            std::string text = json_unescape(json_get(msg_str, "text"));
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
static void handle_binary(const std::vector<uint8_t>& data) {
    if (data.size() < 4) return;
    const char* hdr = reinterpret_cast<const char*>(data.data());
    if (memcmp(hdr, "WCHK", 4) == 0) {
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
    g_log.set_level("INFO");
    g_log.set_file("C:\\RemoteDesktopHost.log");
    g_log.info("=== RemoteDesktop Host starting ===");

    // ── Performance: 1ms timer resolution (like TeamViewer) ──
    // Without this, Sleep/WaitFor* have ~15.6ms granularity, killing FPS and throughput
    timeBeginPeriod(1);

    // ── Elevate process priority for better scheduling ──
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    // ── Disable Windows Network Throttling (like TeamViewer) ──
    // Windows limits non-multimedia network to ~10 packets/ms by default.
    // Setting NetworkThrottlingIndex=0xFFFFFFFF disables this completely.
    // SystemResponsiveness=0 gives 100% CPU to foreground/MMCSS threads.
    {
        HKEY hKey = nullptr;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Multimedia\\SystemProfile",
            0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS)
        {
            DWORD val = 0xFFFFFFFF;
            RegSetValueExA(hKey, "NetworkThrottlingIndex", 0, REG_DWORD, (BYTE*)&val, sizeof(val));
            DWORD resp = 0;  // 0 = give all CPU to MMCSS/foreground
            RegSetValueExA(hKey, "SystemResponsiveness", 0, REG_DWORD, (BYTE*)&resp, sizeof(resp));
            RegCloseKey(hKey);
            g_log.info("Network throttling disabled, SystemResponsiveness=0");
        } else {
            g_log.warn("Cannot set NetworkThrottlingIndex (need admin?)");
        }
    }

    // ── Switch to High Performance power plan (like TeamViewer) ──
    // This prevents CPU/GPU frequency scaling, keeps network adapter at full power
    {
        GUID* currentScheme = nullptr;
        if (PowerGetActiveScheme(NULL, &currentScheme) == ERROR_SUCCESS && currentScheme) {
            g_saved_power_scheme = *currentScheme;
            g_power_scheme_saved = true;
            LocalFree(currentScheme);
        }
        // High Performance GUID: 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c
        GUID highPerf = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
        if (PowerSetActiveScheme(NULL, &highPerf) == ERROR_SUCCESS) {
            g_log.info("Power plan: HIGH PERFORMANCE (GPU/CPU at full clock)");
        } else {
            g_log.warn("Cannot set High Performance power plan");
        }
    }

    // ── Prevent display/system idle throttling ──
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);

    // ── Enable MMCSS for Desktop Window Manager ──
    // Makes DWM compose frames at higher priority → more consistent DXGI capture
    {
        HRESULT hr = DwmEnableMMCSS(TRUE);
        if (SUCCEEDED(hr))
            g_log.info("DWM MMCSS enabled (better DXGI capture)");
    }

    g_log.info("Timer=1ms, priority=HIGH, network throttling=OFF, power=HIGH_PERF");

    std::string cfg_path = "host_config.json";
    if (argc > 1) cfg_path = argv[1];
    load_config(cfg_path);

    g_screen.init();
    g_screen.set_quality(g_config.quality);
    g_screen.set_scale(g_config.scale);
    g_screen.set_codec(g_config.codec);
    g_fps     = g_config.fps;
    g_quality = g_config.quality;
    g_scale   = g_config.scale;
    g_user_scale = g_scale;
    g_auto_scale = g_scale;
    g_codec   = g_config.codec;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    int reconnect_delay = 1;  // Start at 1s for fast reconnect
    while (g_running) {
        g_log.info("Connecting to " + g_config.server_address + ":" + std::to_string(g_config.server_port));

        g_ws = std::make_unique<WsClient>();
        g_ws->on_text = handle_command;
        g_ws->on_binary = handle_binary;
        g_ws->on_close = [&]() {
            g_log.warn("Connection closed, will reconnect");
            g_streaming = false;
            g_raw_cv.notify_all();
        };

        if (g_ws->connect(g_config.server_address, g_config.server_port, "/host")) {
            g_log.info("Connected to server");
            reconnect_delay = 1;  // Reset to 1s on successful connect

            std::string auth = "{\"cmd\":\"auth\",\"token\":\"" + json_escape(g_config.room_token) +
                               "\",\"password\":\"" + json_escape(g_config.password) +
                               "\",\"role\":\"host\"}";
            g_ws->send_text(auth);

            // Open dedicated file transfer connections + workers
            open_file_connections();
            start_file_workers();

            while (g_ws->is_connected() && g_running)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } else {
            g_log.error("Connection failed, retrying in " + std::to_string(reconnect_delay) + "s");
        }

        if (!g_running) break;

        // Stop streaming and file transfer cleanly before reconnecting
        stop_streaming();
        stop_file_workers();
        close_file_connections();

        g_log.info("Reconnecting in " + std::to_string(reconnect_delay) + "s...");
        std::this_thread::sleep_for(std::chrono::seconds(reconnect_delay));
        reconnect_delay = std::min(reconnect_delay * 2, 10);  // Cap at 10s (was 30s)
    }

    stop_streaming();
    stop_file_workers();
    close_file_connections();
    timeEndPeriod(1);

    // Restore original power plan
    if (g_power_scheme_saved) {
        PowerSetActiveScheme(NULL, &g_saved_power_scheme);
        g_log.info("Power plan restored");
    }
    SetThreadExecutionState(ES_CONTINUOUS);  // Reset execution state
    DwmEnableMMCSS(FALSE);

    g_log.info("Host shutting down");
    WSACleanup();
    return 0;
}

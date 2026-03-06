#include "webrtc_stream.h"
#include "host.h"
#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <mutex>

using json = nlohmann::json;

namespace webrtc_stream {

static std::shared_ptr<rtc::PeerConnection> g_pc;
static std::shared_ptr<rtc::DataChannel> g_dc;
static SendTextFn g_send_text;
static std::mutex g_mutex;

static void send_answer(const rtc::Description& answer) {
    if (!g_send_text) return;
    std::string typeStr = answer.typeString();
    std::string sdp = static_cast<std::string>(answer);
    json j;
    j["webrtc_answer"] = json::object();
    j["webrtc_answer"]["type"] = typeStr;
    j["webrtc_answer"]["sdp"] = sdp;
    g_send_text(j.dump());
}

static void send_ice(const rtc::Candidate& candidate) {
    if (!g_send_text) return;
    json j;
    j["webrtc_ice"] = json::object();
    j["webrtc_ice"]["candidate"] = std::string(candidate);
    j["webrtc_ice"]["sdpMid"] = candidate.mid().value_or("");
    j["webrtc_ice"]["sdpMLineIndex"] = 0;
    g_send_text(j.dump());
}

bool init_from_offer(const std::string& type, const std::string& sdp, SendTextFn send_text) {
    std::lock_guard<std::mutex> lock(g_mutex);
    close();
    g_send_text = std::move(send_text);
    try {
        rtc::Configuration config;
        config.iceServers.emplace_back("stun:stun.l.google.com:19302");
        config.disableAutoNegotiation = true;

        g_pc = std::make_shared<rtc::PeerConnection>(config);

        g_pc->onDataChannel([](std::shared_ptr<rtc::DataChannel> dc) {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_dc = std::move(dc);
        });

        g_pc->onLocalDescription([](rtc::Description desc) {
            send_answer(desc);
        });

        g_pc->onLocalCandidate([](rtc::Candidate candidate) {
            send_ice(candidate);
        });

        rtc::Description offer(sdp, type);
        g_pc->setRemoteDescription(offer);
        g_pc->setLocalDescription(rtc::Description::Type::Answer);
        return true;
    } catch (const std::exception& e) {
        if (g_send_text) {
            json err;
            err["webrtc_error"] = e.what();
            g_send_text(err.dump());
        }
        g_pc.reset();
        return false;
    }
}

void add_ice_candidate(const std::string& candidate_json) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_pc) return;
    try {
        json j = json::parse(candidate_json);
        std::string cand = j.value("candidate", "");
        if (cand.empty()) return;
        std::string mid = j.value("sdpMid", "");
        rtc::Candidate c = mid.empty() ? rtc::Candidate(cand) : rtc::Candidate(cand, mid);
        g_pc->addRemoteCandidate(c);
    } catch (...) {}
}

bool send_frame(const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_dc && g_dc->isOpen()) {
        try {
            return g_dc->send(reinterpret_cast<const rtc::byte*>(data), size);
        } catch (...) {}
    }
    return false;
}

bool is_ready() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_dc && g_dc->isOpen();
}

void close() {
    g_dc.reset();
    g_pc.reset();
    g_send_text = nullptr;
}

} // namespace webrtc_stream

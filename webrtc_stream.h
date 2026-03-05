#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

// WebRTC stream: accept offer from client, create answer, send screen frames over DataChannel (UDP).
// Signaling (answer, ICE) is sent via the provided send_text callback.

namespace webrtc_stream {

using SendTextFn = std::function<void(const std::string&)>;

// Initialize from client's offer (SDP string and type "offer"). Creates answer and sends it via send_text.
// send_text will be called with JSON: {"webrtc_answer": {"type":"answer","sdp":"..."}} and {"webrtc_ice": {...}}.
// Returns true if offer was accepted and peer connection is being set up.
bool init_from_offer(const std::string& type, const std::string& sdp, SendTextFn send_text);

// Add remote ICE candidate (from client). Call when receiving {"webrtc_ice": candidate}.
void add_ice_candidate(const std::string& candidate_json);

// Send one binary frame (SCRN format: 4 byte sig + 4 w + 4 h + jpeg). Uses DataChannel if open, else no-op.
// Returns true if sent via WebRTC.
bool send_frame(const uint8_t* data, size_t size);

// Whether the DataChannel is open and we should send frames via WebRTC (not WebSocket).
bool is_ready();

// Close and cleanup (e.g. on stream_stop).
void close();

} // namespace webrtc_stream

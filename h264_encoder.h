#pragma once
/*
 * H.264 Hardware Encoder via Windows Media Foundation
 * Uses NVENC / Intel QuickSync / AMD AMF when available, software fallback otherwise.
 * Input: BGRA pixels, Output: H.264 Annex B NAL units.
 */

#include "host.h"
#include "logger.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <strmif.h>     // ICodecAPI
#include <codecapi.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

class H264Encoder {
public:
    struct EncodedFrame {
        std::vector<uint8_t> data;  // H.264 Annex B (start-code delimited NAL units)
        bool is_keyframe = false;
    };

    H264Encoder() = default;
    ~H264Encoder() { shutdown(); }

    bool init(int width, int height, int fps = 30, int bitrate_kbps = 2000) {
        width_ = width;
        height_ = height;
        fps_ = fps;
        bitrate_ = bitrate_kbps * 1000;

        HRESULT hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) {
            LOG_ERROR("MFStartup failed: 0x" + to_hex(hr));
            return false;
        }
        mf_started_ = true;

        // Try hardware encoder first, then software
        bool found = create_encoder(true);
        if (!found) {
            LOG_WARN("No hardware H.264 encoder, trying software...");
            found = create_encoder(false);
        }
        if (!found) {
            LOG_ERROR("No H.264 encoder found");
            shutdown();
            return false;
        }

        // Unlock async MFT if needed
        {
            ComPtr<IMFAttributes> attrs;
            if (SUCCEEDED(encoder_->GetAttributes(&attrs))) {
                UINT32 is_async = 0;
                if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) && is_async) {
                    attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                    is_async_ = true;
                    LOG_INFO("H264: async MFT unlocked");
                }
            }
        }

        if (!set_output_type()) { LOG_ERROR("H264: set_output_type failed"); shutdown(); return false; }
        if (!set_input_type())  { LOG_ERROR("H264: set_input_type failed");  shutdown(); return false; }

        set_low_latency();

        hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        if (FAILED(hr)) { LOG_ERROR("H264: BEGIN_STREAMING failed"); shutdown(); return false; }
        hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        if (FAILED(hr)) { LOG_ERROR("H264: START_OF_STREAM failed"); shutdown(); return false; }

        encoder_->GetOutputStreamInfo(0, &output_info_);

        // Pre-allocate input buffer
        MFCreateMemoryBuffer(width_ * height_ * 4, &input_buf_);

        initialized_ = true;
        frame_index_ = 0;
        LOG_INFO("H264 encoder initialized: " + std::to_string(width_) + "x" +
                 std::to_string(height_) + " @ " + std::to_string(fps_) + "fps, " +
                 std::to_string(bitrate_/1000) + " kbps" +
                 (hw_encoder_ ? " [HARDWARE]" : " [SOFTWARE]") +
                 (use_nv12_ ? " NV12" : " RGB32"));
        return true;
    }

    EncodedFrame encode(const uint8_t* bgra, int stride, bool request_keyframe = false) {
        EncodedFrame result;
        if (!initialized_ || !encoder_) return result;

        if (request_keyframe) force_keyframe();

        // Fill input sample
        BYTE* buf_ptr = nullptr;
        HRESULT hr = input_buf_->Lock(&buf_ptr, nullptr, nullptr);
        if (FAILED(hr)) return result;

        if (use_nv12_) {
            bgra_to_nv12(bgra, stride, buf_ptr, width_, height_);
            input_buf_->SetCurrentLength(width_ * height_ * 3 / 2);
        } else {
            int dst_stride = width_ * 4;
            for (int y = 0; y < height_; y++)
                memcpy(buf_ptr + y * dst_stride, bgra + y * stride, dst_stride);
            input_buf_->SetCurrentLength(dst_stride * height_);
        }
        input_buf_->Unlock();

        ComPtr<IMFSample> sample;
        MFCreateSample(&sample);
        sample->AddBuffer(input_buf_.Get());
        LONGLONG ts = (LONGLONG)frame_index_ * 10000000LL / fps_;
        sample->SetSampleTime(ts);
        sample->SetSampleDuration(10000000LL / fps_);
        frame_index_++;

        hr = encoder_->ProcessInput(0, sample.Get(), 0);
        if (FAILED(hr)) return result;

        // Drain all available output
        result = drain_output();
        return result;
    }

    void set_bitrate(int bitrate_kbps) {
        bitrate_ = bitrate_kbps * 1000;
        ComPtr<ICodecAPI> api;
        if (encoder_ && SUCCEEDED(encoder_.As(&api))) {
            VARIANT var; var.vt = VT_UI4; var.ulVal = bitrate_;
            api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &var);
        }
    }

    void shutdown() {
        if (encoder_ && initialized_) {
            encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        }
        encoder_.Reset();
        input_buf_.Reset();
        if (mf_started_) { MFShutdown(); mf_started_ = false; }
        initialized_ = false;
    }

    bool is_initialized() const { return initialized_; }
    int get_width()  const { return width_; }
    int get_height() const { return height_; }
    bool is_hardware() const { return hw_encoder_; }

private:
    ComPtr<IMFTransform>    encoder_;
    ComPtr<IMFMediaBuffer>  input_buf_;
    MFT_OUTPUT_STREAM_INFO  output_info_{};
    int  width_ = 0, height_ = 0, fps_ = 30;
    UINT32 bitrate_ = 2000000;
    bool initialized_ = false;
    bool mf_started_  = false;
    bool hw_encoder_  = false;
    bool use_nv12_    = false;
    bool is_async_    = false;
    uint64_t frame_index_ = 0;

    static std::string to_hex(HRESULT hr) {
        char buf[16]; snprintf(buf, sizeof(buf), "%08X", (unsigned)hr);
        return buf;
    }

    bool create_encoder(bool hardware) {
        MFT_REGISTER_TYPE_INFO out_type = { MFMediaType_Video, MFVideoFormat_H264 };
        IMFActivate** ppActivate = nullptr;
        UINT32 count = 0;

        UINT32 flags = MFT_ENUM_FLAG_SORTANDFILTER;
        if (hardware)
            flags |= MFT_ENUM_FLAG_HARDWARE;
        else
            flags |= MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT;

        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags,
                               nullptr, &out_type, &ppActivate, &count);
        if (FAILED(hr) || count == 0) return false;

        hr = ppActivate[0]->ActivateObject(IID_PPV_ARGS(&encoder_));
        for (UINT32 i = 0; i < count; i++) ppActivate[i]->Release();
        CoTaskMemFree(ppActivate);
        if (FAILED(hr)) return false;

        hw_encoder_ = hardware;
        return true;
    }

    bool set_output_type() {
        ComPtr<IMFMediaType> mt;
        MFCreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        mt->SetUINT32(MF_MT_AVG_BITRATE, bitrate_);
        mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, width_, height_);
        MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, fps_, 1);
        mt->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
        mt->SetUINT32(MF_MT_MPEG2_LEVEL, eAVEncH264VLevel3_1);
        return SUCCEEDED(encoder_->SetOutputType(0, mt.Get(), 0));
    }

    bool set_input_type() {
        // Try RGB32 first
        ComPtr<IMFMediaType> mt;
        MFCreateMediaType(&mt);
        mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, width_, height_);
        MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, fps_, 1);

        if (SUCCEEDED(encoder_->SetInputType(0, mt.Get(), 0))) {
            use_nv12_ = false;
            return true;
        }

        // Fallback to NV12 (most hardware encoders require this)
        mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        if (SUCCEEDED(encoder_->SetInputType(0, mt.Get(), 0))) {
            use_nv12_ = true;
            return true;
        }

        // Try IYUV/I420
        mt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_IYUV);
        if (SUCCEEDED(encoder_->SetInputType(0, mt.Get(), 0))) {
            use_nv12_ = true;  // reuse NV12 converter (close enough)
            return true;
        }
        return false;
    }

    void set_low_latency() {
        ComPtr<ICodecAPI> api;
        if (FAILED(encoder_.As(&api))) return;
        VARIANT var;
        // Low latency
        var.vt = VT_BOOL; var.boolVal = VARIANT_TRUE;
        api->SetValue(&CODECAPI_AVLowLatencyMode, &var);
        // CBR
        var.vt = VT_UI4; var.ulVal = eAVEncCommonRateControlMode_CBR;
        api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &var);
        // GOP: keyframe every 2 seconds
        var.vt = VT_UI4; var.ulVal = fps_ * 2;
        api->SetValue(&CODECAPI_AVEncMPVGOPSize, &var);
        // B-frames: 0 (reduce latency)
        var.vt = VT_UI4; var.ulVal = 0;
        api->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &var);
    }

    void force_keyframe() {
        ComPtr<ICodecAPI> api;
        if (SUCCEEDED(encoder_.As(&api))) {
            VARIANT var; var.vt = VT_UI4; var.ulVal = 1;
            api->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &var);
        }
    }

    EncodedFrame drain_output() {
        EncodedFrame result;
        while (true) {
            MFT_OUTPUT_DATA_BUFFER out{};
            out.dwStreamID = 0;

            ComPtr<IMFSample> out_sample;
            ComPtr<IMFMediaBuffer> out_buf;
            if (!(output_info_.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                UINT32 sz = output_info_.cbSize > 0 ? output_info_.cbSize : 512 * 1024;
                MFCreateMemoryBuffer(sz, &out_buf);
                MFCreateSample(&out_sample);
                out_sample->AddBuffer(out_buf.Get());
                out.pSample = out_sample.Get();
                out.pSample->AddRef();
            }

            DWORD status = 0;
            HRESULT hr = encoder_->ProcessOutput(0, 1, &out, &status);

            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                if (out.pSample) out.pSample->Release();
                break;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (out.pSample) out.pSample->Release();
                if (out.pEvents) out.pEvents->Release();
                // Re-negotiate output type
                ComPtr<IMFMediaType> new_type;
                encoder_->GetOutputAvailableType(0, 0, &new_type);
                if (new_type) encoder_->SetOutputType(0, new_type.Get(), 0);
                encoder_->GetOutputStreamInfo(0, &output_info_);
                continue;
            }
            if (FAILED(hr)) {
                if (out.pSample) out.pSample->Release();
                if (out.pEvents) out.pEvents->Release();
                break;
            }

            // Extract H.264 data
            if (out.pSample) {
                ComPtr<IMFMediaBuffer> buf;
                out.pSample->ConvertToContiguousBuffer(&buf);
                if (buf) {
                    BYTE* data = nullptr; DWORD len = 0;
                    buf->Lock(&data, nullptr, &len);
                    if (data && len > 0) {
                        result.data.insert(result.data.end(), data, data + len);
                        // Check keyframe
                        UINT32 clean = 0;
                        if (SUCCEEDED(out.pSample->GetUINT32(MFSampleExtension_CleanPoint, &clean)))
                            result.is_keyframe = result.is_keyframe || (clean != 0);
                    }
                    buf->Unlock();
                }
                out.pSample->Release();
            }
            if (out.pEvents) out.pEvents->Release();
        }
        return result;
    }

    // Convert BGRA to NV12
    static void bgra_to_nv12(const uint8_t* bgra, int src_stride,
                              uint8_t* nv12, int w, int h)
    {
        uint8_t* y_plane  = nv12;
        uint8_t* uv_plane = nv12 + w * h;

        for (int y = 0; y < h; y++) {
            const uint8_t* row = bgra + y * src_stride;
            for (int x = 0; x < w; x++) {
                int b = row[x*4+0], g = row[x*4+1], r = row[x*4+2];
                y_plane[y * w + x] = (uint8_t)(((66*r + 129*g + 25*b + 128) >> 8) + 16);

                if ((y & 1) == 0 && (x & 1) == 0) {
                    int idx = (y/2) * w + x;
                    uv_plane[idx]   = (uint8_t)(((-38*r - 74*g + 112*b + 128) >> 8) + 128);
                    uv_plane[idx+1] = (uint8_t)(((112*r - 94*g - 18*b + 128) >> 8) + 128);
                }
            }
        }
    }
};

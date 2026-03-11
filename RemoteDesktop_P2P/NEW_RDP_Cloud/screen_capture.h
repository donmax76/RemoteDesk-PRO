#pragma once
#include "host.h"
#include "logger.h"

// GDI+ JPEG encoder GUID
static const CLSID CLSID_JPEG = {0x557CF401,0x1A04,0x11D3,{0x9A,0x73,0x00,0x00,0xF8,0x1E,0xF3,0x2E}};
static const CLSID CLSID_PNG  = {0x557CF406,0x1A04,0x11D3,{0x9A,0x73,0x00,0x00,0xF8,0x1E,0xF3,0x2E}};

inline int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    using namespace Gdiplus;
    UINT num=0, size=0;
    GetImageEncodersSize(&num, &size);
    if (size==0) return -1;
    std::vector<uint8_t> buf(size);
    ImageCodecInfo* pImageCodecInfo = (ImageCodecInfo*)buf.data();
    GetImageEncoders(num, size, pImageCodecInfo);
    for (UINT j=0; j<num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format)==0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            return (int)j;
        }
    }
    return -1;
}

class ScreenCapture {
public:
    struct Frame {
        std::vector<uint8_t> jpeg_data;
        int width=0, height=0;
        uint64_t timestamp=0;
    };

    // Raw pixel frame for pipeline (capture and encode on different threads)
    struct RawFrame {
        std::vector<uint8_t> pixels;    // BGRA pixel data
        int src_width = 0, src_height = 0;
        int src_stride = 0;
        int target_width = 0, target_height = 0;
        // Dirty rectangles from DXGI (changed regions)
        struct DirtyRect { int x, y, w, h; };
        std::vector<DirtyRect> dirty_rects;
        int total_dirty_pixels = 0;   // Sum of all dirty rect areas
    };

    ScreenCapture() = default;
    ~ScreenCapture() { stop(); }

    bool init(int quality=75, int scale=80) {
        quality_ = quality;
        scale_   = scale;
        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        Gdiplus::GdiplusStartup(&gdiplusToken_, &gdiplusStartupInput, nullptr);
        if (!init_dxgi()) {
            LOG_WARN("DXGI capture unavailable, falling back to GDI");
            use_gdi_ = true;
        }
        initialized_ = true;
        return true;
    }

    void stop() {
        initialized_ = false;
        staging_.Reset();
        duplication_.Reset();
        d3dDevice_.Reset();
        d3dContext_.Reset();
        if (gdiplusToken_) {
            Gdiplus::GdiplusShutdown(gdiplusToken_);
            gdiplusToken_ = 0;
        }
    }

    // Single-call capture + encode (backward compatible)
    bool capture(Frame& frame) {
        if (!initialized_) return false;
        maybe_retry_dxgi();
        if (!use_gdi_) {
            if (capture_dxgi_legacy(frame)) return true;
        }
        return capture_gdi_legacy(frame);
    }

    // ── Pipeline API ──

    // Step 1: Capture raw pixels (call from ONE thread only — DXGI requirement)
    bool capture_raw(RawFrame& raw) {
        if (!initialized_) return false;
        maybe_retry_dxgi();
        if (!use_gdi_) {
            if (capture_dxgi_raw(raw)) return true;
        }
        return capture_gdi_raw(raw);
    }

    // Extended: returns 1=got frame, 0=no change (DXGI timeout, not an error), -1=error
    int capture_raw_ex(RawFrame& raw) {
        if (!initialized_) return -1;
        maybe_retry_dxgi();
        if (!use_gdi_) {
            return capture_dxgi_raw_ex(raw);
        }
        return capture_gdi_raw(raw) ? 1 : -1;
    }

    // Step 2: Encode raw pixels to JPEG (thread-safe, can run on any thread)
    void encode_raw(const RawFrame& raw, Frame& frame, int quality) {
        if (raw.pixels.empty() || raw.src_width <= 0 || raw.src_height <= 0) return;
        Gdiplus::Bitmap bmp(raw.src_width, raw.src_height, raw.src_stride,
                            PixelFormat32bppARGB, (BYTE*)raw.pixels.data());
        int sw = raw.target_width > 0 ? raw.target_width : raw.src_width;
        int sh = raw.target_height > 0 ? raw.target_height : raw.src_height;
        encode_image_q(bmp, sw, sh, frame, quality);
        frame.width     = sw;
        frame.height    = sh;
        frame.timestamp = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void set_quality(int q) { quality_ = std::max(10, std::min(100, q)); }
    void set_scale  (int s) { scale_   = std::max(10, std::min(100, s)); }
    void set_codec  (const std::string& c) { codec_ = (c == "png" || c == "bmp") ? c : "jpeg"; }
    int  get_scale  () const { return scale_; }

private:
    int quality_ = 75;
    int scale_   = 80;
    std::string codec_ = "jpeg";
    bool initialized_ = false;
    bool use_gdi_     = false;
    ULONG_PTR gdiplusToken_ = 0;

    ComPtr<ID3D11Device>        d3dDevice_;
    ComPtr<ID3D11DeviceContext> d3dContext_;
    ComPtr<IDXGIOutputDuplication> duplication_;
    int screen_w_=0, screen_h_=0;

    // Cached staging texture (avoid re-creating per frame)
    ComPtr<ID3D11Texture2D> staging_;
    D3D11_TEXTURE2D_DESC staging_desc_{};
    // Nearest-neighbor x-coordinate lookup (cached)
    std::vector<int> nn_xmap_;
    int nn_xmap_tw_ = 0, nn_xmap_sw_ = 0;

    int dxgi_fail_count_ = 0;
    int dxgi_retry_interval_s_ = 3;
    std::chrono::steady_clock::time_point last_dxgi_retry_ = std::chrono::steady_clock::now();

    void maybe_retry_dxgi() {
        if (!use_gdi_) return;
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_dxgi_retry_).count();
        if (elapsed >= dxgi_retry_interval_s_) {
            last_dxgi_retry_ = now;
            duplication_.Reset(); d3dDevice_.Reset(); d3dContext_.Reset();
            if (init_dxgi()) {
                use_gdi_ = false; dxgi_fail_count_ = 0; dxgi_retry_interval_s_ = 3;
                LOG_INFO("DXGI re-initialized successfully");
            } else {
                dxgi_retry_interval_s_ = std::min(dxgi_retry_interval_s_ * 2, 30);
            }
        }
    }

    bool init_dxgi() {
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            0, nullptr, 0, D3D11_SDK_VERSION, &d3dDevice_, nullptr, &d3dContext_);
        if (FAILED(hr)) return false;
        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(d3dDevice_.As(&dxgiDevice))) { d3dDevice_.Reset(); d3dContext_.Reset(); return false; }
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(&adapter))) { d3dDevice_.Reset(); d3dContext_.Reset(); return false; }
        ComPtr<IDXGIOutput> output;
        if (FAILED(adapter->EnumOutputs(0, &output))) { d3dDevice_.Reset(); d3dContext_.Reset(); return false; }
        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output.As(&output1))) { d3dDevice_.Reset(); d3dContext_.Reset(); return false; }
        if (FAILED(output1->DuplicateOutput(d3dDevice_.Get(), &duplication_))) { d3dDevice_.Reset(); d3dContext_.Reset(); return false; }
        DXGI_OUTPUT_DESC desc{}; output->GetDesc(&desc);
        screen_w_ = desc.DesktopCoordinates.right  - desc.DesktopCoordinates.left;
        screen_h_ = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        return true;
    }

    void handle_dxgi_lost() {
        ++dxgi_fail_count_;
        LOG_WARN("DXGI ACCESS_LOST (count=" + std::to_string(dxgi_fail_count_) + ")");
        staging_.Reset();
        duplication_.Reset(); d3dDevice_.Reset(); d3dContext_.Reset();
        if (!init_dxgi()) {
            use_gdi_ = true; dxgi_retry_interval_s_ = 3;
            last_dxgi_retry_ = std::chrono::steady_clock::now();
        } else {
            dxgi_fail_count_ = 0;
        }
    }

    // ── Raw capture (for pipeline) ──

    // Return: 1=got frame, 0=no change (timeout), -1=error
    int capture_dxgi_raw_ex(RawFrame& raw) {
        if (!duplication_) return -1;
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = duplication_->AcquireNextFrame(16, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 0;  // No new frame (not an error!)
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) { handle_dxgi_lost(); return -1; }
        if (FAILED(hr)) { use_gdi_ = true; dxgi_retry_interval_s_ = 5; last_dxgi_retry_ = std::chrono::steady_clock::now(); return -1; }

        struct FrameGuard { IDXGIOutputDuplication* dup; ~FrameGuard() { if (dup) dup->ReleaseFrame(); } } fg{duplication_.Get()};

        // Extract dirty rectangles from DXGI
        raw.dirty_rects.clear();
        raw.total_dirty_pixels = 0;
        if (fi.TotalMetadataBufferSize > 0) {
            UINT dirty_buf_size = 0;
            duplication_->GetFrameDirtyRects(0, nullptr, &dirty_buf_size);
            if (dirty_buf_size > 0) {
                std::vector<RECT> rects(dirty_buf_size / sizeof(RECT));
                if (SUCCEEDED(duplication_->GetFrameDirtyRects(dirty_buf_size, rects.data(), &dirty_buf_size))) {
                    int n = dirty_buf_size / sizeof(RECT);
                    for (int i = 0; i < n; i++) {
                        RawFrame::DirtyRect dr;
                        dr.x = rects[i].left;
                        dr.y = rects[i].top;
                        dr.w = rects[i].right - rects[i].left;
                        dr.h = rects[i].bottom - rects[i].top;
                        raw.dirty_rects.push_back(dr);
                        raw.total_dirty_pixels += dr.w * dr.h;
                    }
                }
            }
        }

        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(res.As(&tex))) return false;
        D3D11_TEXTURE2D_DESC td{}; tex->GetDesc(&td);

        // Cache staging texture (avoid GPU alloc per frame)
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        if (!staging_ || staging_desc_.Width != sd.Width || staging_desc_.Height != sd.Height) {
            staging_.Reset();
            if (FAILED(d3dDevice_->CreateTexture2D(&sd, nullptr, &staging_))) return false;
            staging_desc_ = sd;
        }
        d3dContext_->CopyResource(staging_.Get(), tex.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(d3dContext_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

        int src_w = (int)td.Width;
        int src_h = (int)td.Height;
        int tw = src_w * scale_ / 100;
        int th = src_h * scale_ / 100;
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;

        if (scale_ >= 100) {
            // No scaling — tight-copy rows (remove GPU row padding)
            raw.src_width  = src_w;
            raw.src_height = src_h;
            raw.src_stride = src_w * 4;
            raw.target_width  = src_w;
            raw.target_height = src_h;
            size_t row_bytes = (size_t)src_w * 4;
            raw.pixels.resize(row_bytes * src_h);
            const uint8_t* s = (const uint8_t*)mapped.pData;
            uint8_t* d = raw.pixels.data();
            for (int y = 0; y < src_h; y++) {
                memcpy(d, s, row_bytes);
                s += mapped.RowPitch;
                d += row_bytes;
            }
        } else {
            // Nearest-neighbor downscale directly from GPU texture
            raw.src_width  = tw;
            raw.src_height = th;
            raw.src_stride = tw * 4;
            raw.target_width  = tw;
            raw.target_height = th;
            raw.pixels.resize((size_t)tw * th * 4);

            // Cache x-coordinate lookup table
            if (nn_xmap_tw_ != tw || nn_xmap_sw_ != src_w) {
                nn_xmap_.resize(tw);
                for (int x = 0; x < tw; x++)
                    nn_xmap_[x] = x * src_w / tw;
                nn_xmap_tw_ = tw;
                nn_xmap_sw_ = src_w;
            }

            const uint8_t* src = (const uint8_t*)mapped.pData;
            uint32_t* dst = (uint32_t*)raw.pixels.data();
            int src_stride = mapped.RowPitch;

            for (int y = 0; y < th; y++) {
                int sy = y * src_h / th;
                const uint32_t* src_row = (const uint32_t*)(src + (size_t)sy * src_stride);
                for (int x = 0; x < tw; x++) {
                    dst[x] = src_row[nn_xmap_[x]];
                }
                dst += tw;
            }
        }

        d3dContext_->Unmap(staging_.Get(), 0);
        return 1;  // Got frame
    }

    bool capture_dxgi_raw(RawFrame& raw) {
        return capture_dxgi_raw_ex(raw) == 1;
    }

    bool capture_gdi_raw(RawFrame& raw) {
        HDC screenDC = GetDC(nullptr);
        int w  = GetSystemMetrics(SM_CXSCREEN);
        int h  = GetSystemMetrics(SM_CYSCREEN);
        int sw = w * scale_ / 100;
        int sh = h * scale_ / 100;

        HDC memDC    = CreateCompatibleDC(screenDC);
        HBITMAP hBmp = CreateCompatibleBitmap(screenDC, sw, sh);
        HGDIOBJ hOld = SelectObject(memDC, hBmp);
        SetStretchBltMode(memDC, HALFTONE);
        StretchBlt(memDC, 0, 0, sw, sh, screenDC, 0, 0, w, h, SRCCOPY);

        BITMAPINFOHEADER bi{};
        bi.biSize = sizeof(bi); bi.biWidth = sw; bi.biHeight = -sh;
        bi.biPlanes = 1; bi.biBitCount = 32; bi.biCompression = BI_RGB;

        raw.src_width = sw; raw.src_height = sh;
        raw.src_stride = sw * 4;
        raw.target_width = sw; raw.target_height = sh;
        raw.pixels.resize((size_t)sw * sh * 4);
        GetDIBits(memDC, hBmp, 0, sh, raw.pixels.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);

        SelectObject(memDC, hOld);
        DeleteObject(hBmp); DeleteDC(memDC); ReleaseDC(nullptr, screenDC);
        return true;
    }

    // ── Legacy single-call (for backward compat capture()) ──

    bool capture_dxgi_legacy(Frame& frame) {
        RawFrame raw;
        if (!capture_dxgi_raw(raw)) return false;
        encode_raw_internal(raw, frame);
        return true;
    }

    bool capture_gdi_legacy(Frame& frame) {
        RawFrame raw;
        if (!capture_gdi_raw(raw)) return false;
        encode_raw_internal(raw, frame);
        return true;
    }

    void encode_raw_internal(const RawFrame& raw, Frame& frame) {
        Gdiplus::Bitmap bmp(raw.src_width, raw.src_height, raw.src_stride,
                            PixelFormat32bppARGB, (BYTE*)raw.pixels.data());
        encode_image_q(bmp, raw.target_width, raw.target_height, frame, quality_);
        frame.width     = raw.target_width;
        frame.height    = raw.target_height;
        frame.timestamp = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // ── JPEG encoding (thread-safe — only uses local GDI+ objects) ──

    void encode_image_q(Gdiplus::Bitmap& bmp, int sw, int sh, Frame& frame, int quality) {
        using namespace Gdiplus;

        std::unique_ptr<Bitmap> scaled;
        if (sw != (int)bmp.GetWidth() || sh != (int)bmp.GetHeight()) {
            scaled.reset(new Bitmap(sw, sh, PixelFormat24bppRGB));
            Graphics g(scaled.get());
            g.SetInterpolationMode(InterpolationModeBilinear);
            g.DrawImage(&bmp, 0, 0, sw, sh);
        }
        Bitmap* src = scaled ? scaled.get() : &bmp;

        CLSID clsid;
        if (GetEncoderClsid(L"image/jpeg", &clsid) < 0) return;

        IStream* stream = SHCreateMemStream(nullptr, 0);
        if (!stream) return;

        EncoderParameters params{};
        params.Count = 1;
        params.Parameter[0].Guid           = EncoderQuality;
        params.Parameter[0].Type           = EncoderParameterValueTypeLong;
        params.Parameter[0].NumberOfValues = 1;
        ULONG q = (ULONG)std::max(10, std::min(100, quality));
        params.Parameter[0].Value          = &q;
        src->Save(stream, &clsid, &params);

        STATSTG stat{};
        stream->Stat(&stat, STATFLAG_NONAME);
        ULONG size = (ULONG)stat.cbSize.LowPart;
        frame.jpeg_data.resize(size);
        LARGE_INTEGER seek{};
        stream->Seek(seek, STREAM_SEEK_SET, nullptr);
        ULONG read = 0;
        stream->Read(frame.jpeg_data.data(), size, &read);
        stream->Release();
    }
};

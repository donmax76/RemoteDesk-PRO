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

    ScreenCapture() = default;
    ~ScreenCapture() { stop(); }

    bool init(int quality=75, int scale=80) {
        quality_ = quality;
        scale_   = scale;

        // Init GDI+
        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        Gdiplus::GdiplusStartup(&gdiplusToken_, &gdiplusStartupInput, nullptr);

        // Try DXGI
        if (!init_dxgi()) {
            LOG_WARN("DXGI capture unavailable, falling back to GDI");
            use_gdi_ = true;
        }
        initialized_ = true;
        return true;
    }

    void stop() {
        initialized_ = false;
        duplication_.Reset();
        d3dDevice_.Reset();
        d3dContext_.Reset();
        if (gdiplusToken_) {
            Gdiplus::GdiplusShutdown(gdiplusToken_);
            gdiplusToken_ = 0;
        }
    }

    // Returns JPEG bytes for one frame
    bool capture(Frame& frame) {
        if (!initialized_) return false;

        // If DXGI failed previously, periodically try to reinitialize it
        // (TeamViewer/AnyDesk release GPU after closing)
        if (use_gdi_) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_dxgi_retry_).count();
            if (elapsed >= dxgi_retry_interval_s_) {
                last_dxgi_retry_ = now;
                LOG_INFO("Attempting DXGI re-initialization...");
                duplication_.Reset();
                d3dDevice_.Reset();
                d3dContext_.Reset();
                if (init_dxgi()) {
                    use_gdi_ = false;
                    dxgi_fail_count_ = 0;
                    dxgi_retry_interval_s_ = 3;
                    LOG_INFO("DXGI re-initialized successfully, switching back from GDI");
                } else {
                    // Exponential backoff on retries: 3s, 6s, 12s, max 30s
                    dxgi_retry_interval_s_ = std::min(dxgi_retry_interval_s_ * 2, 30);
                }
            }
        }

        if (!use_gdi_) {
            if (capture_dxgi(frame)) return true;
            // DXGI returned false but didn't switch to GDI - might be a timeout, fall through to GDI once
        }
        return capture_gdi(frame);
    }

    void set_quality(int q) { quality_ = std::max(10, std::min(100, q)); }
    void set_scale  (int s) { scale_   = std::max(10, std::min(100, s)); }
    void set_codec  (const std::string& c) { codec_ = (c == "png" || c == "bmp") ? c : "jpeg"; }

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

    // DXGI recovery: periodically retry after falling back to GDI
    int dxgi_fail_count_ = 0;
    int dxgi_retry_interval_s_ = 3;
    std::chrono::steady_clock::time_point last_dxgi_retry_ = std::chrono::steady_clock::now();

    bool init_dxgi() {
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            0, nullptr, 0, D3D11_SDK_VERSION, &d3dDevice_, nullptr, &d3dContext_);
        if (FAILED(hr)) return false;

        ComPtr<IDXGIDevice> dxgiDevice;
        hr = d3dDevice_.As(&dxgiDevice);
        if (FAILED(hr)) { d3dDevice_.Reset(); d3dContext_.Reset(); return false; }

        ComPtr<IDXGIAdapter> adapter;
        hr = dxgiDevice->GetAdapter(&adapter);
        if (FAILED(hr)) { d3dDevice_.Reset(); d3dContext_.Reset(); return false; }

        ComPtr<IDXGIOutput> output;
        hr = adapter->EnumOutputs(0, &output);
        if (FAILED(hr)) { d3dDevice_.Reset(); d3dContext_.Reset(); return false; }

        ComPtr<IDXGIOutput1> output1;
        hr = output.As(&output1);
        if (FAILED(hr)) { d3dDevice_.Reset(); d3dContext_.Reset(); return false; }

        hr = output1->DuplicateOutput(d3dDevice_.Get(), &duplication_);
        if (FAILED(hr)) { d3dDevice_.Reset(); d3dContext_.Reset(); return false; }

        DXGI_OUTPUT_DESC desc{};
        output->GetDesc(&desc);
        screen_w_ = desc.DesktopCoordinates.right  - desc.DesktopCoordinates.left;
        screen_h_ = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        return true;
    }

    bool capture_dxgi(Frame& frame) {
        if (!duplication_) return false;

        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = duplication_->AcquireNextFrame(100, &fi, &res);

        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;

        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
            // Another app (TeamViewer, AnyDesk, RDP) took exclusive GPU access
            // Don't permanently fall back to GDI - try reinit, and if it fails
            // use GDI temporarily with periodic DXGI retry
            ++dxgi_fail_count_;
            LOG_WARN("DXGI ACCESS_LOST (count=" + std::to_string(dxgi_fail_count_) + "), reinitializing...");
            duplication_.Reset();
            d3dDevice_.Reset();
            d3dContext_.Reset();
            if (!init_dxgi()) {
                use_gdi_ = true; // Temporary GDI fallback, will retry DXGI periodically
                dxgi_retry_interval_s_ = 3;
                last_dxgi_retry_ = std::chrono::steady_clock::now();
                LOG_WARN("DXGI reinit failed, using GDI temporarily (will retry every few seconds)");
            } else {
                LOG_INFO("DXGI reinit successful after ACCESS_LOST");
                dxgi_fail_count_ = 0;
            }
            return false;
        }

        if (FAILED(hr)) {
            // Unknown DXGI error - switch to GDI temporarily
            use_gdi_ = true;
            dxgi_retry_interval_s_ = 5;
            last_dxgi_retry_ = std::chrono::steady_clock::now();
            return false;
        }

        // Ensure frame is always released even on error
        struct FrameGuard {
            IDXGIOutputDuplication* dup;
            ~FrameGuard() { if (dup) dup->ReleaseFrame(); }
        } frameGuard{duplication_.Get()};

        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(res.As(&tex))) return false;

        D3D11_TEXTURE2D_DESC texDesc{};
        tex->GetDesc(&texDesc);

        D3D11_TEXTURE2D_DESC stagingDesc = texDesc;
        stagingDesc.Usage          = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags      = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags      = 0;

        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(d3dDevice_->CreateTexture2D(&stagingDesc, nullptr, &staging))) return false;
        d3dContext_->CopyResource(staging.Get(), tex.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(d3dContext_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

        int w = texDesc.Width, h = texDesc.Height;
        int sw = w * scale_ / 100;
        int sh = h * scale_ / 100;

        // DXGI gives BGRA which maps to GDI+ 32bppARGB
        Gdiplus::Bitmap bmp(w, h, mapped.RowPitch, PixelFormat32bppARGB, (BYTE*)mapped.pData);
        encode_image(bmp, sw, sh, frame);

        d3dContext_->Unmap(staging.Get(), 0);
        // frameGuard destructor calls ReleaseFrame()

        frame.width     = sw;
        frame.height    = sh;
        frame.timestamp = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return true;
    }

    bool capture_gdi(Frame& frame) {
        HDC screenDC  = GetDC(nullptr);
        HDC memDC     = CreateCompatibleDC(screenDC);
        int w         = GetSystemMetrics(SM_CXSCREEN);
        int h         = GetSystemMetrics(SM_CYSCREEN);
        int sw        = w * scale_ / 100;
        int sh        = h * scale_ / 100;

        HBITMAP hBmp  = CreateCompatibleBitmap(screenDC, sw, sh);
        HGDIOBJ hOld  = SelectObject(memDC, hBmp);  // save original
        SetStretchBltMode(memDC, HALFTONE);
        StretchBlt(memDC, 0, 0, sw, sh, screenDC, 0, 0, w, h, SRCCOPY);

        Gdiplus::Bitmap bmp(hBmp, nullptr);
        encode_image(bmp, sw, sh, frame);

        SelectObject(memDC, hOld);  // restore original before deleting
        DeleteObject(hBmp);
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);

        frame.width     = sw;
        frame.height    = sh;
        frame.timestamp = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return true;
    }

    void encode_image(Gdiplus::Bitmap& bmp, int sw, int sh, Frame& frame) {
        using namespace Gdiplus;

        std::unique_ptr<Bitmap> scaled;
        if ((int)sw != (int)bmp.GetWidth() || (int)sh != (int)bmp.GetHeight()) {
            scaled.reset(new Bitmap(sw, sh, PixelFormat24bppRGB));
            Graphics g(scaled.get());
            g.SetInterpolationMode(InterpolationModeBilinear);  // Faster than Bicubic for real-time streaming
            g.DrawImage(&bmp, 0, 0, sw, sh);
        }

        Bitmap* src = scaled ? scaled.get() : &bmp;

        const WCHAR* mime = L"image/jpeg";
        if (codec_ == "png") mime = L"image/png";
        else if (codec_ == "bmp") mime = L"image/bmp";

        CLSID clsid;
        if (GetEncoderClsid(mime, &clsid) < 0) {
            GetEncoderClsid(L"image/jpeg", &clsid);
            mime = L"image/jpeg";
        }

        IStream* stream = SHCreateMemStream(nullptr, 0);
        if (!stream) return;

        if (codec_ == "jpeg") {
            EncoderParameters params{};
            params.Count = 1;
            params.Parameter[0].Guid           = EncoderQuality;
            params.Parameter[0].Type           = EncoderParameterValueTypeLong;
            params.Parameter[0].NumberOfValues = 1;
            ULONG q = quality_;
            params.Parameter[0].Value          = &q;
            src->Save(stream, &clsid, &params);
        } else {
            src->Save(stream, &clsid, nullptr);
        }

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

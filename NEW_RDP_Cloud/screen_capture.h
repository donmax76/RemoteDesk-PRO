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
        if (!use_gdi_) {
            if (capture_dxgi(frame)) return true;
            // fallback
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

    bool init_dxgi() {
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            0, nullptr, 0, D3D11_SDK_VERSION, &d3dDevice_, nullptr, &d3dContext_);
        if (FAILED(hr)) return false;

        ComPtr<IDXGIDevice> dxgiDevice;
        d3dDevice_.As(&dxgiDevice);
        ComPtr<IDXGIAdapter> adapter;
        dxgiDevice->GetAdapter(&adapter);
        ComPtr<IDXGIOutput> output;
        adapter->EnumOutputs(0, &output);
        ComPtr<IDXGIOutput1> output1;
        output.As(&output1);

        hr = output1->DuplicateOutput(d3dDevice_.Get(), &duplication_);
        if (FAILED(hr)) return false;

        DXGI_OUTPUT_DESC desc{};
        output->GetDesc(&desc);
        screen_w_ = desc.DesktopCoordinates.right  - desc.DesktopCoordinates.left;
        screen_h_ = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        return true;
    }

    bool capture_dxgi(Frame& frame) {
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        HRESULT hr = duplication_->AcquireNextFrame(100, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
        if (FAILED(hr)) { use_gdi_=true; return false; }

        ComPtr<ID3D11Texture2D> tex;
        res.As(&tex);

        D3D11_TEXTURE2D_DESC texDesc{};
        tex->GetDesc(&texDesc);

        D3D11_TEXTURE2D_DESC stagingDesc = texDesc;
        stagingDesc.Usage          = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags      = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags      = 0;

        ComPtr<ID3D11Texture2D> staging;
        d3dDevice_->CreateTexture2D(&stagingDesc, nullptr, &staging);
        d3dContext_->CopyResource(staging.Get(), tex.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        d3dContext_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);

        int w = texDesc.Width, h = texDesc.Height;
        int sw = w * scale_ / 100;
        int sh = h * scale_ / 100;

        // Convert BGRA to RGB bitmap
        Gdiplus::Bitmap bmp(w, h, mapped.RowPitch, PixelFormat32bppRGB, (BYTE*)mapped.pData);
        encode_image(bmp, sw, sh, frame);

        d3dContext_->Unmap(staging.Get(), 0);
        duplication_->ReleaseFrame();

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
        SelectObject(memDC, hBmp);
        SetStretchBltMode(memDC, HALFTONE);
        StretchBlt(memDC, 0, 0, sw, sh, screenDC, 0, 0, w, h, SRCCOPY);

        Gdiplus::Bitmap bmp(hBmp, nullptr);
        encode_image(bmp, sw, sh, frame);

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
        if (sw != bmp.GetWidth() || sh != bmp.GetHeight()) {
            scaled.reset(new Bitmap(sw, sh, PixelFormat24bppRGB));
            Graphics g(scaled.get());
            g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
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

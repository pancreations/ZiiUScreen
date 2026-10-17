// Desktop capture for ziiu-drc: one Windows display (e.g. an HDMI dummy plug set to
// 1920x1080) through DXGI Desktop Duplication, downscaled on the GPU to the GamePad's
// 854x480 and converted to I420 for x264.
//
// The downscale is an exact area average (box filter over each output pixel's
// footprint in the source), i.e. the desktop is rendered at a higher resolution and
// supersampled down. Desktop Duplication leaves the mouse pointer out of the image;
// the shader draws it in source coordinates before the downscale.
//
// Colour: BT.601 limited range, as the test pattern.

#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

class DesktopCapture {
public:
    // Stats since Open (for ziiu-drc's 5 s report).
    unsigned long desktop_frames = 0, pointer_updates = 0, converts = 0, lost = 0;
    double age_ms_sum = 0, age_ms_max = 0, gpu_ms_sum = 0;
    unsigned long age_n = 0;

    ~DesktopCapture() { Close(); }

    // which: "\\.\DISPLAYn", or a substring of the monitor's hardware ID (e.g.
    // "YOUR_DISPLAY_ID"). outW x outH is the I420 frame; the picture fills visibleW columns,
    // the rest is black.
    bool Open(const char* which, int outW, int outH, int visibleW) {
        out_w_ = outW; out_h_ = outH; vis_w_ = visibleW;
        if (outW % 8 || outH % 2) return false;
        device_name_ = ResolveDisplay(which);
        if (device_name_.empty()) {
            std::fprintf(stderr, "capture: no active display matches \"%s\"\n", which);
            return false;
        }
        if (!CreateDevice()) return false;
        return Duplicate();
    }

    // Fills i420 (outW*outH*3/2) with the current desktop. Returns true if the
    // picture changed since the last call (new desktop image or pointer move).
    bool Grab(uint8_t* i420) {
        if (!dup_) {
            if (GetTickCount64() - last_retry_ < 250) return false;
            last_retry_ = GetTickCount64();
            if (!Duplicate()) return false;
        }
        DXGI_OUTDUPL_FRAME_INFO info;
        IDXGIResource* res = nullptr;
        HRESULT hr = dup_->AcquireNextFrame(0, &info, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
        if (FAILED(hr)) {
            // Mode change, secure desktop (UAC), full-screen exclusive app...
            lost++;
            Release(dup_);
            return false;
        }
        bool changed = false;
        if (info.LastPresentTime.QuadPart) {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex))) {
                ctx_->CopyResource(desk_, tex);
                tex->Release();
                desktop_frames++;
                changed = true;
                LARGE_INTEGER now, f;
                QueryPerformanceCounter(&now);
                QueryPerformanceFrequency(&f);
                double age = (double)(now.QuadPart - info.LastPresentTime.QuadPart) * 1000.0 / (double)f.QuadPart;
                age_ms_sum += age;
                age_n++;
                if (age > age_ms_max) age_ms_max = age;
            }
        }
        if (info.LastMouseUpdateTime.QuadPart) {
            ptr_visible_ = info.PointerPosition.Visible != FALSE;
            ptr_x_ = info.PointerPosition.Position.x;
            ptr_y_ = info.PointerPosition.Position.y;
            pointer_updates++;
            changed = true;
        }
        if (info.PointerShapeBufferSize) {
            shape_buf_.resize(info.PointerShapeBufferSize);
            UINT got = 0;
            DXGI_OUTDUPL_POINTER_SHAPE_INFO si;
            if (SUCCEEDED(dup_->GetFramePointerShape((UINT)shape_buf_.size(), shape_buf_.data(), &got, &si)))
                SetPointerShape(si);
            changed = true;
        }
        res->Release();
        dup_->ReleaseFrame();
        if (!changed && have_frame_) return false;
        Convert(i420);
        have_frame_ = true;
        return true;
    }

    int SourceWidth() const { return src_w_; }
    int SourceHeight() const { return src_h_; }
    const std::string& DeviceName() const { return device_name_; }

    void Close() {
        Release(dup_); Release(desk_srv_); Release(desk_); Release(cur_srv_); Release(cur_);
        Release(out_uav_); Release(out_buf_); Release(staging_); Release(cb_); Release(cs_);
        Release(output_); Release(ctx_); Release(dev_);
    }

private:
    template <class T> static void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

    // Shader parameters (cbuffer P), 16-byte rows.
    struct Params {
        float sx, sy; int src_w, src_h;
        int cur_x, cur_y, cur_w, cur_h;
        int vis_w, out_w, out_h, y_bytes;
    };

    static std::string Narrow(const wchar_t* w) {
        char b[256];
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof(b), nullptr, nullptr);
        return n > 0 ? std::string(b) : std::string();
    }

    static std::string ResolveDisplay(const char* which) {
        if (!std::strncmp(which, "\\\\.\\", 4)) return which;
        for (DWORD i = 0;; i++) {
            DISPLAY_DEVICEW a = {};
            a.cb = sizeof(a);
            if (!EnumDisplayDevicesW(nullptr, i, &a, 0)) break;
            if (!(a.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
            DISPLAY_DEVICEW m = {};
            m.cb = sizeof(m);
            for (DWORD j = 0; EnumDisplayDevicesW(a.DeviceName, j, &m, 0); j++)
                if (Narrow(m.DeviceID).find(which) != std::string::npos) return Narrow(a.DeviceName);
        }
        return std::string();
    }

    bool CreateDevice() {
        IDXGIFactory1* fac = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) return false;
        IDXGIAdapter1* adapter = nullptr;
        for (UINT a = 0; !output_ && fac->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
            IDXGIOutput* o = nullptr;
            for (UINT k = 0; adapter->EnumOutputs(k, &o) != DXGI_ERROR_NOT_FOUND; k++) {
                DXGI_OUTPUT_DESC d;
                o->GetDesc(&d);
                if (Narrow(d.DeviceName) == device_name_ &&
                    SUCCEEDED(o->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output_))) {
                    DXGI_ADAPTER_DESC1 ad;
                    adapter->GetDesc1(&ad);
                    std::printf("capture: %s on %s, desktop %ldx%ld\n", device_name_.c_str(),
                                Narrow(ad.Description).c_str(), d.DesktopCoordinates.right - d.DesktopCoordinates.left,
                                d.DesktopCoordinates.bottom - d.DesktopCoordinates.top);
                    o->Release();
                    break;
                }
                o->Release();
            }
            if (output_) {
                D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
                HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1,
                                               D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
                if (FAILED(hr)) std::fprintf(stderr, "capture: D3D11CreateDevice 0x%08lx\n", (unsigned long)hr);
            }
            adapter->Release();
        }
        fac->Release();
        if (!output_) {
            std::fprintf(stderr, "capture: %s is not a DXGI output\n", device_name_.c_str());
            return false;
        }
        return dev_ && CreatePipeline();
    }

    bool CreatePipeline() {
        ID3DBlob *code = nullptr, *err = nullptr;
        HRESULT hr = D3DCompile(kShader, std::strlen(kShader), "ziiu_scale", nullptr, nullptr, "main", "cs_5_0",
                                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
        if (FAILED(hr)) {
            std::fprintf(stderr, "capture: shader: %s\n", err ? (const char*)err->GetBufferPointer() : "?");
            Release(err);
            return false;
        }
        hr = dev_->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &cs_);
        code->Release();
        if (FAILED(hr)) return false;

        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(Params);
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &cb_))) return false;

        const UINT bytes = (UINT)(out_w_ * out_h_ * 3 / 2);
        bd = {};
        bd.ByteWidth = bytes;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &out_buf_))) return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = bytes / 4;
        ud.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
        if (FAILED(dev_->CreateUnorderedAccessView(out_buf_, &ud, &out_uav_))) return false;
        bd = {};
        bd.ByteWidth = bytes;
        bd.Usage = D3D11_USAGE_STAGING;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &staging_))) return false;

        // No pointer yet: a transparent 1x1.
        const uint32_t clear = 0;
        return MakeCursor(1, 1, &clear) && (cur_w_ = cur_h_ = 0, true);
    }

    bool Duplicate() {
        Release(dup_);
        HRESULT hr = output_->DuplicateOutput(dev_, &dup_);
        if (FAILED(hr)) {
            std::fprintf(stderr, "capture: DuplicateOutput 0x%08lx\n", (unsigned long)hr);
            return false;
        }
        DXGI_OUTDUPL_DESC d;
        dup_->GetDesc(&d);
        if ((int)d.ModeDesc.Width != src_w_ || (int)d.ModeDesc.Height != src_h_ || !desk_) {
            Release(desk_srv_);
            Release(desk_);
            src_w_ = (int)d.ModeDesc.Width;
            src_h_ = (int)d.ModeDesc.Height;
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = (UINT)src_w_;
            td.Height = (UINT)src_h_;
            td.MipLevels = td.ArraySize = 1;
            td.Format = d.ModeDesc.Format;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(dev_->CreateTexture2D(&td, nullptr, &desk_)) ||
                FAILED(dev_->CreateShaderResourceView(desk_, nullptr, &desk_srv_))) {
                std::fprintf(stderr, "capture: desktop texture (format %d)\n", (int)d.ModeDesc.Format);
                Release(dup_);
                return false;
            }
            std::printf("capture: duplicating %dx%d format %d @%u/%u Hz\n", src_w_, src_h_, (int)d.ModeDesc.Format,
                        d.ModeDesc.RefreshRate.Numerator, d.ModeDesc.RefreshRate.Denominator);
        }
        have_frame_ = false;
        return true;
    }

    bool MakeCursor(int w, int h, const uint32_t* rgba) {
        Release(cur_srv_);
        Release(cur_);
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = (UINT)w;
        td.Height = (UINT)h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = {rgba, (UINT)w * 4, 0};
        if (FAILED(dev_->CreateTexture2D(&td, &sd, &cur_)) || FAILED(dev_->CreateShaderResourceView(cur_, nullptr, &cur_srv_)))
            return false;
        cur_w_ = w;
        cur_h_ = h;
        return true;
    }

    // Pointer shape -> RGBA. Alpha 0 with red 255 marks "invert the screen" (XOR).
    void SetPointerShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& si) {
        const uint8_t* s = shape_buf_.data();
        int w = (int)si.Width, h = (int)si.Height;
        if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) h /= 2;   // AND mask, then XOR mask
        if (w <= 0 || h <= 0 || w > 256 || h > 256) return;
        std::vector<uint32_t> px((size_t)w * h);
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                uint32_t r = 0, g = 0, b = 0, a = 0;
                if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
                    int bit = 7 - (x & 7);
                    int andm = (s[y * si.Pitch + x / 8] >> bit) & 1;
                    int xorm = (s[(y + h) * si.Pitch + x / 8] >> bit) & 1;
                    if (!andm) { r = g = b = xorm ? 255 : 0; a = 255; }
                    else if (xorm) r = 255;                       // invert
                } else {
                    const uint8_t* p = s + y * si.Pitch + x * 4;  // BGRA
                    if (si.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
                        b = p[0]; g = p[1]; r = p[2]; a = p[3];
                    } else if (!p[3]) {                           // masked colour: replace
                        b = p[0]; g = p[1]; r = p[2]; a = 255;
                    } else if (p[0] | p[1] | p[2]) {              // XOR: approximate as invert
                        r = 255;
                    }
                }
                px[(size_t)y * w + x] = r | g << 8 | b << 16 | a << 24;
            }
        }
        MakeCursor(w, h, px.data());
    }

    void Convert(uint8_t* i420) {
        LARGE_INTEGER t0, t1, f;
        QueryPerformanceCounter(&t0);
        Params p = {};
        p.sx = (float)src_w_ / (float)vis_w_;
        p.sy = (float)src_h_ / (float)out_h_;
        p.src_w = src_w_; p.src_h = src_h_;
        p.cur_x = ptr_x_; p.cur_y = ptr_y_;
        p.cur_w = ptr_visible_ ? cur_w_ : 0; p.cur_h = ptr_visible_ ? cur_h_ : 0;
        p.vis_w = vis_w_; p.out_w = out_w_; p.out_h = out_h_; p.y_bytes = out_w_ * out_h_;
        ctx_->UpdateSubresource(cb_, 0, nullptr, &p, 0, 0);
        ctx_->CSSetShader(cs_, nullptr, 0);
        ctx_->CSSetConstantBuffers(0, 1, &cb_);
        ID3D11ShaderResourceView* srv[2] = {desk_srv_, cur_srv_};
        ctx_->CSSetShaderResources(0, 2, srv);
        ctx_->CSSetUnorderedAccessViews(0, 1, &out_uav_, nullptr);
        // One thread per 8x2 output block: 16 Y, 4 U, 4 V bytes.
        ctx_->Dispatch((UINT)(out_w_ / 8 + 7) / 8, (UINT)(out_h_ / 2 + 7) / 8, 1);
        ID3D11UnorderedAccessView* nuav = nullptr;
        ctx_->CSSetUnorderedAccessViews(0, 1, &nuav, nullptr);
        ID3D11ShaderResourceView* nsrv[2] = {};
        ctx_->CSSetShaderResources(0, 2, nsrv);
        ctx_->CopyResource(staging_, out_buf_);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx_->Map(staging_, 0, D3D11_MAP_READ, 0, &m))) {
            std::memcpy(i420, m.pData, (size_t)out_w_ * out_h_ * 3 / 2);
            ctx_->Unmap(staging_, 0);
        }
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        gpu_ms_sum += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart;
        converts++;
    }

    static constexpr const char* kShader = R"(
Texture2D<float4> Desk : register(t0);
Texture2D<float4> Cur : register(t1);
RWByteAddressBuffer Out : register(u0);
cbuffer P : register(b0) {
    float2 scale; int2 srcSize;
    int2 curPos; int2 curSize;
    int visW; int outW; int outH; int yBytes;
};

float3 Src(int2 p) {
    float3 c = Desk.Load(int3(p, 0)).rgb;
    int2 q = p - curPos;
    if (all(q >= 0) && all(q < curSize)) {
        float4 k = Cur.Load(int3(q, 0));
        if (k.a > 0) c = lerp(c, k.rgb, k.a);
        else if (k.r > 0.5) c = 1 - c;
    }
    return c;
}

// Exact area average of the source pixels under output pixel (ox, oy).
float3 Px(int ox, int oy) {
    if (ox >= visW) return float3(0, 0, 0);
    float x0 = ox * scale.x, x1 = x0 + scale.x;
    float y0 = oy * scale.y, y1 = y0 + scale.y;
    float3 acc = 0;
    float wsum = 0;
    int ye = min((int)ceil(y1), srcSize.y), xe = min((int)ceil(x1), srcSize.x);
    for (int y = (int)floor(y0); y < ye; y++) {
        float wy = min(y + 1, y1) - max(y, y0);
        for (int x = (int)floor(x0); x < xe; x++) {
            float w = wy * (min(x + 1, x1) - max(x, x0));
            acc += Src(int2(x, y)) * w;
            wsum += w;
        }
    }
    return acc / max(wsum, 1e-6);
}

uint B(float v) { return (uint)clamp(round(v), 0, 255); }
float Yv(float3 c) { return 16 + 65.481 * c.r + 128.553 * c.g + 24.966 * c.b; }
float Uv(float3 c) { return 128 - 37.797 * c.r - 74.203 * c.g + 112.0 * c.b; }
float Vv(float3 c) { return 128 + 112.0 * c.r - 93.786 * c.g - 18.214 * c.b; }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    int bx = id.x, by = id.y;
    if (bx >= outW / 8 || by >= outH / 2) return;
    int x = bx * 8, y = by * 2;
    float3 c[2][8];
    [unroll] for (int r = 0; r < 2; r++)
        [unroll] for (int i = 0; i < 8; i++) c[r][i] = Px(x + i, y + r);
    [unroll] for (int r2 = 0; r2 < 2; r2++)
        [unroll] for (int h = 0; h < 2; h++) {
            uint v = 0;
            [unroll] for (int k = 0; k < 4; k++) v |= B(Yv(c[r2][h * 4 + k])) << (8 * k);
            Out.Store((uint)((y + r2) * outW + x + h * 4), v);
        }
    uint u = 0, vv = 0;
    [unroll] for (int k = 0; k < 4; k++) {
        float3 a = (c[0][2 * k] + c[0][2 * k + 1] + c[1][2 * k] + c[1][2 * k + 1]) * 0.25;
        u |= B(Uv(a)) << (8 * k);
        vv |= B(Vv(a)) << (8 * k);
    }
    int cw = outW / 2;
    Out.Store((uint)(yBytes + by * cw + x / 2), u);
    Out.Store((uint)(yBytes + yBytes / 4 + by * cw + x / 2), vv);
}
)";

    std::string device_name_;
    int out_w_ = 0, out_h_ = 0, vis_w_ = 0, src_w_ = 0, src_h_ = 0;
    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGIOutput1* output_ = nullptr;
    IDXGIOutputDuplication* dup_ = nullptr;
    ID3D11Texture2D* desk_ = nullptr;
    ID3D11ShaderResourceView* desk_srv_ = nullptr;
    ID3D11Texture2D* cur_ = nullptr;
    ID3D11ShaderResourceView* cur_srv_ = nullptr;
    ID3D11Buffer *out_buf_ = nullptr, *staging_ = nullptr, *cb_ = nullptr;
    ID3D11UnorderedAccessView* out_uav_ = nullptr;
    ID3D11ComputeShader* cs_ = nullptr;
    std::vector<uint8_t> shape_buf_;
    int cur_w_ = 0, cur_h_ = 0, ptr_x_ = 0, ptr_y_ = 0;
    bool ptr_visible_ = false, have_frame_ = false;
    ULONGLONG last_retry_ = 0;
};

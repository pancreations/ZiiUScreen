// ziiu-input: the GamePad as a Windows controller and touchscreen.
//
// Receives the GamePad's 128-byte input reports straight from the bridge on
// 127.0.0.1:50022 (ziiu-drc leaves that port alone with ZIIU_INPUT_EXTERNAL=1, so
// input never waits behind a video encode), and:
//   - presents buttons/sticks as a virtual Xbox 360 controller through the ViGEmBus
//     driver (ViGEmClient, MIT, see docs/REFERENCES.md), and sends the game's force
//     feedback to ziiu-drc's control port 127.0.0.1:50200;
//   - injects Windows touch input on the display streamed to the GamePad;
//   - TV button: short press toggles the GamePad's IR sensor bar (ziiu-drc "ZIRT"),
//     hold 2 s opens the calibration screen on the GamePad's display (also "ZCALIBRATE"
//     on 50222, tools/ziiu-input/calibrate.ps1).
// ziiu-drc sends the GamePad's factory touch calibration ("ZCAL" + 8 x s16) to 50222;
// a saved calibration (--calib file) takes precedence.
//
// Report layout and touch math from libdrc input-receiver / device-config (2-clause
// BSD): buttons (d[80] << 16) | (d[2] << 8) | d[3]; sticks LX LY RX RY as u16 LE at
// d[6..13] (libdrc assumes 900..3200; the owner's sticks measured 833..3260, so
// they are calibrated per axis); 10 touch samples at d[36 + 4i] (12-bit x, y),
// pressure bits in the high nibbles of d[37], d[39], d[41], d[43].
//
//   ziiu-input [--display YOUR_DISPLAY_ID] [--calib file] [--labels] [--invert-y]
//     --display   monitor hardware ID part or \\.\DISPLAYn of the streamed display
//                 (touch and the calibration screen are off without it)
//     --calib     calibration file to load and save (sticks, touch)
//     --labels    map by printed letter (GamePad A -> Xbox A). Default is by
//                 position (GamePad B, the bottom button, -> Xbox A), which is
//                 what Windows games' button prompts assume.
//     --invert-y  flip both stick Y axes.
// The virtual controller, any touch contact and the calibration window exist only
// while this process runs.

#include <winsock2.h>
#include <windows.h>

#include <ViGEm/Client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace {

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

const u32 kBtnHome = 0x2, kBtnMinus = 0x4, kBtnPlus = 0x8, kBtnR = 0x10, kBtnL = 0x20,
          kBtnZR = 0x40, kBtnZL = 0x80, kBtnDown = 0x100, kBtnUp = 0x200, kBtnRight = 0x400,
          kBtnLeft = 0x800, kBtnY = 0x1000, kBtnX = 0x2000, kBtnB = 0x4000, kBtnA = 0x8000,
          kBtnTV = 0x200000, kBtnR3 = 0x400000, kBtnL3 = 0x800000;
const int kStickMin = 900, kStickMax = 3200;   // libdrc kDrcStickMin/Max (uncalibrated default)
const float kDeadZone = 0.1f;                  // libdrc kStickDeadZone
const float kDeadZoneCalibrated = 0.06f;       // measured centre: less slack needed
const int kInputPort = 50022, kCalPort = 50222, kControlPort = 50200;

std::atomic<int> g_large{0}, g_small{0};
std::atomic<bool> g_quit{false};

VOID CALLBACK OnX360Notification(PVIGEM_CLIENT, PVIGEM_TARGET, UCHAR large, UCHAR small,
                                 UCHAR, LPVOID) {
    g_large = large;
    g_small = small;
}

BOOL WINAPI OnCtrl(DWORD) {
    g_quit = true;
    return TRUE;
}

u64 NowMs() { return GetTickCount64(); }
u32 Buttons(const u8* d) { return ((u32)d[80] << 16) | (d[2] << 8) | d[3]; }
int StickRaw(const u8* d, int i) { return d[6 + 2 * i] | (d[7 + 2 * i] << 8); }

SOCKET Bind(int port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in sin = {};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = htons((u_short)port);
    if (s == INVALID_SOCKET || bind(s, (sockaddr*)&sin, sizeof(sin)) != 0) {
        std::fprintf(stderr, "cannot bind 127.0.0.1:%d (%d)\n", port, WSAGetLastError());
        return INVALID_SOCKET;
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    return s;
}

void SendControl(SOCKET s, const void* m, int n) {
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    to.sin_port = htons(kControlPort);
    sendto(s, (const char*)m, n, 0, (sockaddr*)&to, sizeof(to));
}

void SendRumble(SOCKET s, int level, int duration_ms) {
    u8 m[3] = {(u8)level, (u8)(duration_ms & 0xff), (u8)(duration_ms >> 8)};
    SendControl(s, m, sizeof(m));
}

// --- Calibration data ---

// libdrc InputReceiver::Recalibrate with no margins: panel pixel = o + raw * scale.
struct TouchCal {
    float ox = 0, w = 1, oy = 0, h = 1;
    void Set(const int16_t c[8]) {   // ref1 x,y  ref2 x,y  raw1 x,y  raw2 x,y
        if (c[6] == c[4] || c[7] == c[5]) return;
        ox = (float)(c[6] * c[0] - c[4] * c[2]) / (c[6] - c[4]);
        w = (float)(c[0] - c[2]) / (c[4] - c[6]);
        oy = (float)(c[7] * c[1] - c[5] * c[3]) / (c[7] - c[5]);
        h = (float)(c[1] - c[3]) / (c[5] - c[7]);
    }
    TouchCal() {
        const int16_t def[8] = {20, 20, 834, 460, 195, 3818, 3877, 373};   // libdrc defaults
        Set(def);
    }
};

struct Calibration {
    bool have_sticks = false, have_touch = false;
    int smin[4] = {}, scen[4] = {}, smax[4] = {};
    TouchCal touch;

    bool Load(const std::string& path) {
        FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "r");
        if (!f) return false;
        char line[256];
        while (std::fgets(line, sizeof(line), f)) {
            int i, a, b, c;
            float ox, w, oy, h;
            if (std::sscanf(line, "stick %d %d %d %d", &i, &a, &b, &c) == 4 && i >= 0 && i < 4 && a < b && b < c) {
                smin[i] = a; scen[i] = b; smax[i] = c;
                have_sticks = true;
            } else if (std::sscanf(line, "touch %f %f %f %f", &ox, &w, &oy, &h) == 4 && w != 0 && h != 0) {
                touch.ox = ox; touch.w = w; touch.oy = oy; touch.h = h;
                have_touch = true;
            }
        }
        std::fclose(f);
        return have_sticks || have_touch;
    }
    bool Save(const std::string& path) const {
        FILE* f = path.empty() ? nullptr : std::fopen(path.c_str(), "w");
        if (!f) return false;
        std::fprintf(f, "# ziiu-input calibration, written by the calibration screen (TV button 2 s).\n"
                        "# stick <0=LX 1=LY 2=RX 3=RY> <min> <centre> <max>  (raw GamePad units)\n"
                        "# touch <x offset> <x scale> <y offset> <y scale>  (raw -> 854x480 panel pixels)\n");
        if (have_sticks)
            for (int i = 0; i < 4; i++) std::fprintf(f, "stick %d %d %d %d\n", i, smin[i], scen[i], smax[i]);
        if (have_touch) std::fprintf(f, "touch %.4f %.6f %.4f %.6f\n", touch.ox, touch.w, touch.oy, touch.h);
        std::fclose(f);
        return true;
    }
};

// --- Controller ---

SHORT Stick(const u8* d, int i, bool invert, const Calibration& cal) {
    const int raw = StickRaw(d, i);
    float v, dz = kDeadZone;
    if (cal.have_sticks) {
        const int c = cal.scen[i];
        v = raw >= c ? (float)(raw - c) / (float)(cal.smax[i] - c) : (float)(raw - c) / (float)(c - cal.smin[i]);
        dz = kDeadZoneCalibrated;
    } else {
        const float mid = (kStickMax - kStickMin) / 2.0f;
        v = (std::max(kStickMin, std::min(kStickMax, raw)) - kStickMin - mid) / mid;
    }
    v = std::max(-1.0f, std::min(1.0f, v));
    if (v > -dz && v < dz) v = 0;
    else v = (v > 0 ? v - dz : v + dz) / (1 - dz);   // no jump at the dead zone edge
    if (invert) v = -v;
    return (SHORT)(v * 32767.0f);
}

XUSB_REPORT ToX360(const u8* d, bool labels, bool invert_y, const Calibration& cal) {
    const u32 b = Buttons(d);
    XUSB_REPORT r;
    XUSB_REPORT_INIT(&r);
    struct { u32 pad; int xusb; } map[] = {
        {kBtnUp, XUSB_GAMEPAD_DPAD_UP},       {kBtnDown, XUSB_GAMEPAD_DPAD_DOWN},
        {kBtnLeft, XUSB_GAMEPAD_DPAD_LEFT},   {kBtnRight, XUSB_GAMEPAD_DPAD_RIGHT},
        {kBtnPlus, XUSB_GAMEPAD_START},       {kBtnMinus, XUSB_GAMEPAD_BACK},
        {kBtnL3, XUSB_GAMEPAD_LEFT_THUMB},    {kBtnR3, XUSB_GAMEPAD_RIGHT_THUMB},
        {kBtnL, XUSB_GAMEPAD_LEFT_SHOULDER},  {kBtnR, XUSB_GAMEPAD_RIGHT_SHOULDER},
        {kBtnHome, XUSB_GAMEPAD_GUIDE},
        {kBtnA, labels ? XUSB_GAMEPAD_A : XUSB_GAMEPAD_B},
        {kBtnB, labels ? XUSB_GAMEPAD_B : XUSB_GAMEPAD_A},
        {kBtnX, labels ? XUSB_GAMEPAD_X : XUSB_GAMEPAD_Y},
        {kBtnY, labels ? XUSB_GAMEPAD_Y : XUSB_GAMEPAD_X},
    };
    for (auto& m : map)
        if (b & m.pad) r.wButtons |= (USHORT)m.xusb;
    r.bLeftTrigger = (b & kBtnZL) ? 255 : 0;    // ZL/ZR are digital on the GamePad
    r.bRightTrigger = (b & kBtnZR) ? 255 : 0;
    r.sThumbLX = Stick(d, 0, false, cal);
    r.sThumbLY = Stick(d, 1, invert_y, cal);
    r.sThumbRX = Stick(d, 2, false, cal);
    r.sThumbRY = Stick(d, 3, invert_y, cal);
    return r;
}

// --- Touch ---

// Raw touch position (10-sample average, 12-bit) if the screen is pressed.
bool TouchRaw(const u8* d, int* x, int* y) {
    const int pressure = ((d[37] >> 4) & 7) | (((d[39] >> 4) & 7) << 3) | (((d[41] >> 4) & 7) << 6) |
                         (((d[43] >> 4) & 7) << 9);
    if (!pressure) return false;
    int sx = 0, sy = 0;
    for (int i = 0; i < 10; i++) {
        sx += ((d[37 + 4 * i] & 0xf) << 8) | d[36 + 4 * i];
        sy += ((d[39 + 4 * i] & 0xf) << 8) | d[38 + 4 * i];
    }
    *x = sx / 10;
    *y = sy / 10;
    return true;
}

// Where the streamed display is on the Windows desktop (physical pixels).
bool FindDisplay(const char* which, RECT* r) {
    DISPLAY_DEVICEA ad = {sizeof(ad)};
    for (DWORD a = 0; EnumDisplayDevicesA(nullptr, a, &ad, 0); a++, ad.cb = sizeof(ad)) {
        if (!(ad.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        bool match = !_stricmp(ad.DeviceName, which);
        DISPLAY_DEVICEA md = {sizeof(md)};
        for (DWORD m = 0; !match && EnumDisplayDevicesA(ad.DeviceName, m, &md, EDD_GET_DEVICE_INTERFACE_NAME);
             m++, md.cb = sizeof(md))
            match = std::strstr(md.DeviceID, which) != nullptr;
        if (!match) continue;
        DEVMODEA dm = {};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsA(ad.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) return false;
        *r = {dm.dmPosition.x, dm.dmPosition.y, dm.dmPosition.x + (LONG)dm.dmPelsWidth,
              dm.dmPosition.y + (LONG)dm.dmPelsHeight};
        return true;
    }
    return false;
}

class Touch {
public:
    bool enabled = false;
    unsigned long downs = 0, fails = 0;
    TouchCal cal;              // factory calibration from the GamePad (ZCAL)
    const TouchCal* user = nullptr;   // saved calibration, if any

    void Init(const char* display) {
        display_ = display;
        if (!display_) return;
        if (!InitializeTouchInjection(1, TOUCH_FEEDBACK_DEFAULT)) {
            std::printf("touch: InitializeTouchInjection failed (%lu), touch off\n", GetLastError());
            return;
        }
        enabled = true;
        Refresh(true);
    }
    // Display position can change (resolution, arrangement): re-read every 2 s.
    void Refresh(bool log = false) {
        RECT r;
        if (!display_) return;
        if (!FindDisplay(display_, &r)) {
            if (have_rect_ || log) std::printf("touch: display %s not found\n", display_);
            have_rect_ = false;
            return;
        }
        if (log || !have_rect_ || std::memcmp(&r, &rect_, sizeof(r)))
            std::printf("touch: display %s at (%ld,%ld) %ldx%ld\n", display_, r.left, r.top, r.right - r.left,
                        r.bottom - r.top);
        rect_ = r;
        have_rect_ = true;
    }
    bool Rect(RECT* r) const { *r = rect_; return have_rect_; }
    void OnReport(const u8* d) {
        if (!enabled || !have_rect_) return;
        int rx, ry;
        if (!TouchRaw(d, &rx, &ry)) {
            // Light presses through a screen protector flicker in and out of contact
            // (owner had to press several times): lift only after 8 unpressed
            // reports (~45 ms), so one press stays one touch.
            if (down_ && ++up_count_ >= 8) Lift();
            return;
        }
        up_count_ = 0;
        const TouchCal& c = user ? *user : cal;
        const float px = c.ox + rx * c.w, py = c.oy + ry * c.h;   // 0..853, 0..479
        const float nx = std::max(0.0f, std::min(1.0f, px / 853.0f));
        const float ny = std::max(0.0f, std::min(1.0f, py / 479.0f));
        POINT pt = {rect_.left + (LONG)(nx * (rect_.right - rect_.left - 1) + 0.5f),
                    rect_.top + (LONG)(ny * (rect_.bottom - rect_.top - 1) + 0.5f)};
        Inject(pt, down_ ? POINTER_FLAG_UPDATE | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT
                         : POINTER_FLAG_DOWN | POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT);
        if (!down_) downs++;
        down_ = true;
        last_ = pt;
    }
    void Lift() {
        if (!down_) return;
        Inject(last_, POINTER_FLAG_UP);
        down_ = false;
        up_count_ = 0;
    }

private:
    const char* display_ = nullptr;
    RECT rect_ = {};
    bool have_rect_ = false, down_ = false;
    int up_count_ = 0;
    POINT last_ = {};

    void Inject(POINT pt, POINTER_FLAGS flags) {
        POINTER_TOUCH_INFO ti = {};
        ti.pointerInfo.pointerType = PT_TOUCH;
        ti.pointerInfo.pointerId = 0;
        ti.pointerInfo.ptPixelLocation = pt;
        ti.pointerInfo.pointerFlags = flags;
        ti.touchFlags = TOUCH_FLAG_NONE;
        ti.touchMask = TOUCH_MASK_CONTACTAREA;
        ti.rcContact = {pt.x - 2, pt.y - 2, pt.x + 2, pt.y + 2};
        if (!InjectTouchInput(1, &ti) && fails++ < 5)
            std::printf("touch: InjectTouchInput failed (%lu) at (%ld,%ld)\n", GetLastError(), pt.x, pt.y);
    }
};

// --- Calibration screen ---
//
// A topmost window on the GamePad's display, driven by the latest input report:
//   1. touch: tap 4 targets near the corners; a least-squares line per axis maps raw
//      touch to panel pixels (the same model as the GamePad's factory calibration);
//   2. sticks: let go (centre averaged over 1 s), then roll both sticks around the
//      edge and press A (min/max per axis).
// B cancels. While it runs, the virtual controller is neutral and touch is not
// injected, so games and the desktop see nothing.

struct Shared {
    std::mutex mu;
    u8 report[128] = {};
    bool have = false;
};

class CalibrationScreen {
public:
    std::atomic<bool> active{false}, done{false};
    Calibration result;   // valid when done && ok
    bool ok = false;

    void Start(const RECT& r, Shared* shared) {
        if (active) return;
        rect_ = r;
        shared_ = shared;
        active = true;
        done = false;
        ok = false;
        std::thread([this] { Run(); }).detach();
    }

private:
    enum Step { kTouch, kLetGo, kRoll, kSaved, kCancelled };
    RECT rect_ = {};
    Shared* shared_ = nullptr;
    HWND hwnd_ = nullptr;
    Step step_ = kTouch;
    int target_ = 0;
    double tsum_x_ = 0, tsum_y_ = 0;
    int tsamples_ = 0, tup_ = 0;
    double raw_x_[4] = {}, raw_y_[4] = {};
    u64 step_ms_ = 0;
    double csum_[4] = {};
    int csamples_ = 0;
    int smin_[4] = {}, smax_[4] = {};
    u32 prev_buttons_ = 0xffffffff;
    std::string message_;

    static const float kTargets[4][2];

    POINT TargetPx(int i) const {
        return {rect_.left + (LONG)(kTargets[i][0] * (rect_.right - rect_.left)),
                rect_.top + (LONG)(kTargets[i][1] * (rect_.bottom - rect_.top))};
    }

    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        CalibrationScreen* self = (CalibrationScreen*)GetWindowLongPtrW(h, GWLP_USERDATA);
        switch (m) {
        case WM_TIMER: if (self) self->Tick(); return 0;
        case WM_PAINT: if (self) self->Paint(); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    void Run() {
        static bool registered = false;
        HINSTANCE inst = GetModuleHandleW(nullptr);
        if (!registered) {
            WNDCLASSW wc = {};
            wc.lpfnWndProc = Proc;
            wc.hInstance = inst;
            wc.lpszClassName = L"ZiiuCalibration";
            wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
            RegisterClassW(&wc);
            registered = true;
        }
        step_ = kTouch;
        target_ = 0;
        tsamples_ = tup_ = csamples_ = 0;
        tsum_x_ = tsum_y_ = 0;
        message_.clear();
        prev_buttons_ = 0xffffffff;
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"ZiiuCalibration", L"ZiiU calibration",
                                WS_POPUP | WS_VISIBLE, rect_.left, rect_.top, rect_.right - rect_.left,
                                // 1 px short of the display: a window covering a monitor exactly
                                // is treated as fullscreen, and opening it stalled the whole PC
                                // (video and audio threads) and set off a resync storm.
                                rect_.bottom - rect_.top - 1, nullptr, nullptr, inst, nullptr);
        if (!hwnd_) {
            std::printf("calibration: cannot open window (%lu)\n", GetLastError());
            active = false;
            return;
        }
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, (LONG_PTR)this);
        SetTimer(hwnd_, 1, 15, nullptr);
        std::printf("calibration: started\n");
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        hwnd_ = nullptr;
        done = true;
        active = false;
    }

    void Next(Step s) {
        step_ = s;
        step_ms_ = NowMs();
        csamples_ = 0;
        for (double& c : csum_) c = 0;
    }

    void Tick() {
        u8 d[128];
        {
            std::lock_guard<std::mutex> lk(shared_->mu);
            if (!shared_->have) return;
            std::memcpy(d, shared_->report, sizeof(d));
        }
        const u32 b = Buttons(d);
        const u32 pressed = prev_buttons_ == 0xffffffff ? 0 : (b & ~prev_buttons_);
        prev_buttons_ = b;
        const u64 now = NowMs();
        if (step_ == kSaved || step_ == kCancelled) {
            if (now - step_ms_ > 1500) DestroyWindow(hwnd_);
            return;
        }
        if (pressed & kBtnB) {
            std::printf("calibration: cancelled\n");
            Next(kCancelled);
            InvalidateRect(hwnd_, nullptr, TRUE);
            return;
        }
        if (step_ == kTouch) {
            int x, y;
            if (TouchRaw(d, &x, &y)) {
                tup_ = 0;
                tsum_x_ += x;
                tsum_y_ += y;
                tsamples_++;
            } else if (tsamples_ && ++tup_ >= 10) {     // released for ~150 ms: one tap done
                if (tsamples_ >= 3) {
                    raw_x_[target_] = tsum_x_ / tsamples_;
                    raw_y_[target_] = tsum_y_ / tsamples_;
                    std::printf("calibration: target %d raw (%.0f, %.0f)\n", target_, raw_x_[target_], raw_y_[target_]);
                    if (++target_ == 4) FinishTouch();
                }
                tsum_x_ = tsum_y_ = 0;
                tsamples_ = tup_ = 0;
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
        } else if (step_ == kLetGo) {
            if (now - step_ms_ > 1000) {                 // first second: time to let go
                for (int i = 0; i < 4; i++) csum_[i] += StickRaw(d, i);
                csamples_++;
            }
            if (now - step_ms_ > 2000 && csamples_) {
                for (int i = 0; i < 4; i++) {
                    result.scen[i] = (int)(csum_[i] / csamples_ + 0.5);
                    smin_[i] = smax_[i] = result.scen[i];
                }
                Next(kRoll);
                InvalidateRect(hwnd_, nullptr, TRUE);
            }
        } else if (step_ == kRoll) {
            for (int i = 0; i < 4; i++) {
                smin_[i] = std::min(smin_[i], StickRaw(d, i));
                smax_[i] = std::max(smax_[i], StickRaw(d, i));
            }
            if (pressed & kBtnA) {
                bool good = true;
                for (int i = 0; i < 4; i++)   // need a real sweep on both sides of centre
                    good = good && result.scen[i] - smin_[i] > 500 && smax_[i] - result.scen[i] > 500;
                if (!good) {
                    message_ = "Not the full range yet: keep rolling both sticks, then press A.";
                } else {
                    for (int i = 0; i < 4; i++) { result.smin[i] = smin_[i]; result.smax[i] = smax_[i]; }
                    result.have_sticks = true;
                    ok = true;
                    std::printf("calibration: sticks");
                    for (int i = 0; i < 4; i++) std::printf(" [%d %d %d]", smin_[i], result.scen[i], smax_[i]);
                    std::printf("\n");
                    Next(kSaved);
                }
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    // Least squares per axis: panel_px = o + raw * s.
    void FinishTouch() {
        double tx[4], ty[4];
        for (int i = 0; i < 4; i++) { tx[i] = kTargets[i][0] * 853.0; ty[i] = kTargets[i][1] * 479.0; }
        auto fit = [](const double* raw, const double* t, float* o, float* s) {
            double mr = 0, mt = 0, srt = 0, srr = 0;
            for (int i = 0; i < 4; i++) { mr += raw[i] / 4; mt += t[i] / 4; }
            for (int i = 0; i < 4; i++) { srt += (raw[i] - mr) * (t[i] - mt); srr += (raw[i] - mr) * (raw[i] - mr); }
            if (srr < 1) return false;
            *s = (float)(srt / srr);
            *o = (float)(mt - *s * mr);
            return true;
        };
        TouchCal c;
        bool good = fit(raw_x_, tx, &c.ox, &c.w) && fit(raw_y_, ty, &c.oy, &c.h);
        double worst = 0;
        for (int i = 0; good && i < 4; i++)
            worst = std::max(worst, std::max(std::fabs(c.ox + raw_x_[i] * c.w - tx[i]), std::fabs(c.oy + raw_y_[i] * c.h - ty[i])));
        std::printf("calibration: touch fit x = %.1f + raw * %.4f, y = %.1f + raw * %.4f, worst error %.1f px\n",
                    c.ox, c.w, c.oy, c.h, worst);
        if (!good || worst > 25) {                        // a missed tap: start over
            message_ = "Those taps did not line up. Tap each cross again, as exactly as you can.";
            target_ = 0;
            return;
        }
        result.touch = c;
        result.have_touch = true;
        message_.clear();
        Next(kLetGo);
    }

    void Text(HDC dc, const std::string& s, int y, int size, COLORREF color) {
        HFONT f = CreateFontW(size, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, f);
        SetTextColor(dc, color);
        SetBkMode(dc, TRANSPARENT);
        RECT r = {0, y, rect_.right - rect_.left, y + size * 3};
        std::wstring w(s.begin(), s.end());
        DrawTextW(dc, w.c_str(), -1, &r, DT_CENTER | DT_WORDBREAK);
        SelectObject(dc, old);
        DeleteObject(f);
    }

    void Paint() {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd_, &ps);
        const int W = rect_.right - rect_.left, H = rect_.bottom - rect_.top, big = H / 14, small = H / 22;
        RECT all = {0, 0, W, H};
        HBRUSH bg = CreateSolidBrush(RGB(16, 18, 24));
        FillRect(dc, &all, bg);
        DeleteObject(bg);
        const COLORREF white = RGB(235, 238, 245), dim = RGB(150, 156, 170), accent = RGB(255, 196, 0);
        if (step_ == kTouch) {
            Text(dc, "Touch calibration " + std::to_string(target_ + 1) + " / 4", H * 38 / 100, big, white);
            Text(dc, "Tap the centre of the cross with the stylus. B cancels.", H * 50 / 100, small, dim);
            POINT p = TargetPx(target_);
            p.x -= rect_.left;
            p.y -= rect_.top;
            HPEN pen = CreatePen(PS_SOLID, std::max(2, H / 300), accent);
            HGDIOBJ old = SelectObject(dc, pen);
            const int a = H / 18;
            MoveToEx(dc, p.x - a, p.y, nullptr); LineTo(dc, p.x + a + 1, p.y);
            MoveToEx(dc, p.x, p.y - a, nullptr); LineTo(dc, p.x, p.y + a + 1);
            SelectObject(dc, GetStockObject(NULL_BRUSH));
            Ellipse(dc, p.x - a / 3, p.y - a / 3, p.x + a / 3, p.y + a / 3);
            SelectObject(dc, old);
            DeleteObject(pen);
        } else if (step_ == kLetGo) {
            Text(dc, "Let go of both sticks", H * 38 / 100, big, white);
            Text(dc, "Measuring the centre...", H * 50 / 100, small, dim);
        } else if (step_ == kRoll) {
            Text(dc, "Roll both sticks around their edge a few times", H * 30 / 100, big, white);
            Text(dc, "then press A. B cancels.", H * 44 / 100, small, dim);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "Left  X %d..%d  Y %d..%d      Right  X %d..%d  Y %d..%d", smin_[0], smax_[0],
                          smin_[1], smax_[1], smin_[2], smax_[2], smin_[3], smax_[3]);
            Text(dc, buf, H * 56 / 100, small, dim);
        } else if (step_ == kSaved) {
            Text(dc, "Calibration saved", H * 42 / 100, big, accent);
        } else {
            Text(dc, "Calibration cancelled: nothing changed", H * 42 / 100, big, white);
        }
        if (!message_.empty()) Text(dc, message_, H * 72 / 100, small, accent);
        EndPaint(hwnd_, &ps);
    }
};
const float CalibrationScreen::kTargets[4][2] = {{0.12f, 0.15f}, {0.88f, 0.15f}, {0.88f, 0.85f}, {0.12f, 0.85f}};

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // MSVC rejects _IOLBF with size 0
    bool labels = false, invert_y = false;
    const char* display = nullptr;
    std::string calib_path;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--labels")) labels = true;
        else if (!std::strcmp(argv[i], "--invert-y")) invert_y = true;
        else if (!std::strcmp(argv[i], "--display") && i + 1 < argc) display = argv[++i];
        else if (!std::strcmp(argv[i], "--calib") && i + 1 < argc) calib_path = argv[++i];
        else {
            std::fprintf(stderr, "usage: ziiu-input [--display ID] [--calib file] [--labels] [--invert-y]\n");
            return 2;
        }
    }
    // Physical pixels for display positions and touch coordinates on scaled displays.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(OnCtrl, TRUE);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    SOCKET in = Bind(kInputPort), calsock = Bind(kCalPort);
    SOCKET out = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (in == INVALID_SOCKET || calsock == INVALID_SOCKET || out == INVALID_SOCKET) return 1;

    PVIGEM_CLIENT client = vigem_alloc();
    if (!client) return 1;
    VIGEM_ERROR err = vigem_connect(client);
    if (!VIGEM_SUCCESS(err)) {
        std::fprintf(stderr, "ViGEmBus not available (0x%08x); is the driver installed?\n", (unsigned)err);
        vigem_free(client);
        return 1;
    }
    PVIGEM_TARGET pad = vigem_target_x360_alloc();
    err = vigem_target_add(client, pad);
    if (!VIGEM_SUCCESS(err)) {
        std::fprintf(stderr, "cannot add virtual Xbox 360 controller (0x%08x)\n", (unsigned)err);
        vigem_target_free(pad);
        vigem_disconnect(client);
        vigem_free(client);
        return 1;
    }
    vigem_target_x360_register_notification(client, pad, &OnX360Notification, nullptr);
    std::printf("ziiu-input: virtual Xbox 360 controller added (%s mapping%s), input port %d, control -> %d\n",
                labels ? "label" : "position", invert_y ? ", Y inverted" : "", kInputPort, kControlPort);
    Calibration cal;
    if (cal.Load(calib_path))
        std::printf("calibration: loaded %s (sticks %s, touch %s)\n", calib_path.c_str(), cal.have_sticks ? "yes" : "no",
                    cal.have_touch ? "yes" : "no");
    else
        std::printf("calibration: none saved yet (hold the TV button 2 s to calibrate)\n");
    Touch touch;
    touch.Init(display);
    if (cal.have_touch) touch.user = &cal.touch;
    Shared shared;
    CalibrationScreen screen;
    FILE* dump = nullptr;
    u64 dump_until = 0;
    bool was_calibrating = false;

    XUSB_REPORT last;
    XUSB_REPORT_INIT(&last);
    bool have_input = false;
    u64 last_input = 0, next_stats = NowMs() + 5000, next_refresh = 0, last_stick_log = 0,
        next_display = NowMs() + 2000, tv_down_ms = 0;
    bool tv_long = false;
    unsigned long reports = 0, updates = 0, rumble_sends = 0;
    int sent_level = 0;
    u32 last_buttons = 0xffffffff, prev_raw_buttons = 0;
    auto start_calibration = [&] {
        RECT r;
        if (!touch.Rect(&r)) {
            std::printf("calibration: needs --display and the display present\n");
            return;
        }
        touch.Lift();
        XUSB_REPORT_INIT(&last);
        vigem_target_x360_update(client, pad, last);
        screen.Start(r, &shared);
    };
    while (!g_quit) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(in, &rd);
        FD_SET(calsock, &rd);
        timeval tv = {0, 20000};
        select(0, &rd, nullptr, nullptr, &tv);
        u8 d[256];
        int n;
        while ((n = recv(calsock, (char*)d, sizeof(d), 0)) >= 0) {
            if (n == 10 && !std::memcmp(d, "ZCALIBRATE", 10)) {
                start_calibration();
            } else if (n == 10 && !std::memcmp(d, "ZTOUCHDUMP", 10)) {
                // Record 10 s of raw reports next to the calibration file, to measure how
                // light a press the panel reports (touch sensitivity through a protector).
                std::string path = calib_path.substr(0, calib_path.find_last_of("\\/") + 1) + "touch-dump.bin";
                if (dump) std::fclose(dump);
                dump = std::fopen(path.c_str(), "wb");
                dump_until = NowMs() + 10000;
                std::printf("touch: recording raw reports for 10 s to %s\n", path.c_str());
            } else if (n == 20 && !std::memcmp(d, "ZCAL", 4)) {
                int16_t c[8];
                std::memcpy(c, d + 4, 16);
                TouchCal t;
                t.Set(c);
                if (std::memcmp(&t, &touch.cal, sizeof(t))) {
                    touch.cal = t;
                    std::printf("touch: GamePad factory calibration ref (%d,%d) (%d,%d) raw (%d,%d) (%d,%d)%s\n", c[0],
                                c[1], c[2], c[3], c[4], c[5], c[6], c[7], cal.have_touch ? " (saved calibration in use)" : "");
                }
            }
        }
        while ((n = recv(in, (char*)d, sizeof(d), 0)) >= 0) {
            if (n != 128) continue;
            const u64 now = NowMs();
            reports++;
            last_input = now;
            if (!have_input) std::printf("input: GamePad reports arriving\n");
            have_input = true;
            if (dump) {
                std::fwrite(d, 1, 128, dump);
                if (now >= dump_until) {
                    std::fclose(dump);
                    dump = nullptr;
                    std::printf("touch: recording done\n");
                }
            }
            {
                std::lock_guard<std::mutex> lk(shared.mu);
                std::memcpy(shared.report, d, 128);
                shared.have = true;
            }
            const u32 b = Buttons(d);
            if (screen.active) { prev_raw_buttons = b; continue; }   // calibration owns input
            // TV button: short press toggles the sensor bar, a 2 s hold calibrates.
            if ((b & kBtnTV) && !(prev_raw_buttons & kBtnTV)) { tv_down_ms = now; tv_long = false; }
            if ((b & kBtnTV) && !tv_long && now - tv_down_ms >= 2000) { tv_long = true; start_calibration(); }
            if (!(b & kBtnTV) && (prev_raw_buttons & kBtnTV) && !tv_long && now - tv_down_ms < 700) {
                SendControl(out, "ZIRT", 4);
                std::printf("input: TV button -> sensor bar toggle\n");
            }
            prev_raw_buttons = b;
            if (screen.active) continue;
            touch.OnReport(d);
            XUSB_REPORT r = ToX360(d, labels, invert_y, cal);
            if (std::memcmp(&r, &last, sizeof(r))) {
                vigem_target_x360_update(client, pad, r);
                updates++;
                if (b != last_buttons) {
                    std::printf("input: buttons=%06x -> xusb=%04x LT=%u RT=%u\n", b, r.wButtons, r.bLeftTrigger,
                                r.bRightTrigger);
                    last_buttons = b;
                }
                // Raw stick values, rate-limited, to check calibration and axis directions.
                if ((r.sThumbLX != last.sThumbLX || r.sThumbLY != last.sThumbLY || r.sThumbRX != last.sThumbRX ||
                     r.sThumbRY != last.sThumbRY) && now - last_stick_log >= 250) {
                    std::printf("sticks: raw L=(%d,%d) R=(%d,%d) -> L=(%d,%d) R=(%d,%d)\n", StickRaw(d, 0), StickRaw(d, 1),
                                StickRaw(d, 2), StickRaw(d, 3), r.sThumbLX, r.sThumbLY, r.sThumbRX, r.sThumbRY);
                    last_stick_log = now;
                }
                last = r;
            }
        }
        // Calibration finished: apply and save.
        if (was_calibrating && screen.done) {
            if (screen.ok) {
                cal = screen.result;
                touch.user = cal.have_touch ? &cal.touch : nullptr;
                std::printf("calibration: %s %s\n", cal.Save(calib_path) ? "saved to" : "applied (not saved, no --calib)",
                            calib_path.c_str());
            }
            last_buttons = 0xffffffff;
            XUSB_REPORT_INIT(&last);
        }
        was_calibrating = screen.active || (was_calibrating && !screen.done);
        if (screen.done) was_calibrating = false;
        const u64 now = NowMs();
        // GamePad gone (link lost or out of range): release everything, never leave a
        // button, stick or touch stuck.
        if (have_input && now - last_input > 250) {
            XUSB_REPORT_INIT(&last);
            vigem_target_x360_update(client, pad, last);
            touch.Lift();
            last_buttons = 0xffffffff;
            have_input = false;
            std::printf("input: no reports for 250 ms, controller and touch released\n");
        }
        // Force feedback: the GamePad has one motor, so use the stronger of the two.
        // Requests carry a 1 s expiry and are refreshed, so the motor stops on its own
        // if this process dies mid-rumble.
        const int level = std::max(g_large.load(), g_small.load());
        if (level != sent_level || (level && now >= next_refresh)) {
            SendRumble(out, level, level ? 1000 : 0);
            rumble_sends++;
            if (level != sent_level)
                std::printf("rumble: game set large=%d small=%d -> level %d\n", g_large.load(), g_small.load(), level);
            sent_level = level;
            next_refresh = now + 400;
        }
        if (now >= next_display) {
            touch.Refresh();
            next_display = now + 2000;
        }
        if (now >= next_stats) {
            u8 vol = 0;
            {
                std::lock_guard<std::mutex> lk(shared.mu);
                vol = shared.report[14];      // GamePad volume slider (libdrc audio_volume)
            }
            std::printf("stats: reports=%lu updates=%lu rumble_sends=%lu level=%d touches=%lu volume_slider=%u\n",
                        reports, updates, rumble_sends, sent_level, touch.downs, vol);
            next_stats += 5000;
        }
    }
    touch.Lift();
    if (sent_level) SendRumble(out, 0, 0);
    vigem_target_x360_unregister_notification(pad);
    vigem_target_remove(client, pad);
    vigem_target_free(pad);
    vigem_disconnect(client);
    vigem_free(client);
    WSACleanup();
    std::printf("ziiu-input: stopped\n");
    return 0;
}

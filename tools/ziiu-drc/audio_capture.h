// audio_capture.h: PC sound for the GamePad speakers.
//
// WASAPI loopback capture of one Windows output device (shared mode, its mix
// format), converted to what the GamePad plays: 48 kHz, stereo, 16-bit PCM
// (libdrc audio-streamer). A capture thread fills a small buffer; ziiu-drc's audio
// tick takes 416 frames per packet. The buffer starts playing at ~17 ms, and
// anything beyond ~60 ms is dropped so latency stays bounded when the PC's audio
// clock runs slightly faster than the GamePad's; on an underrun the packet is
// silence and the buffer refills. Nothing is installed: the device keeps playing
// normally and this only listens to it.
#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class LoopbackAudio {
public:
    std::string device;                  // friendly name of the captured device
    std::atomic<unsigned long> underruns{0}, drops{0};
    bool running = false;

    // target: "default" (the Windows default output) or part of a device name.
    bool Start(const std::string& target) {
        target_ = target;
        running = true;
        thread_ = std::thread([this] { Run(); });
        for (int i = 0; i < 40 && !opened_; i++) Sleep(25);   // report the first open
        return opened_;
    }
    ~LoopbackAudio() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
    }

    // n stereo frames into out (interleaved). False = nothing to play (out untouched).
    bool Pop(int16_t* out, size_t n) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!primed_) {
            if (ring_.size() < kPrimeFrames * 2) return false;
            primed_ = true;
        }
        if (ring_.size() < n * 2) {
            primed_ = false;
            underruns++;
            return false;
        }
        std::copy(ring_.begin(), ring_.begin() + n * 2, out);
        ring_.erase(ring_.begin(), ring_.begin() + n * 2);
        return true;
    }
    double BufferedMs() {
        std::lock_guard<std::mutex> lk(mu_);
        return ring_.size() / 2 / 48.0;
    }

private:
    static const size_t kPrimeFrames = 832;    // 2 GamePad packets, ~17 ms
    static const size_t kMaxFrames = 2880;     // 60 ms: drop back to kPrimeFrames
    std::string target_;
    std::thread thread_;
    std::atomic<bool> stop_{false}, opened_{false};
    std::mutex mu_;
    std::deque<int16_t> ring_;
    bool primed_ = false;
    // Linear resampler state (only used when the device does not run at 48 kHz).
    double pos_ = 0;
    std::vector<float> pending_;               // stereo frames awaiting resampling

    static std::string Narrow(const wchar_t* w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
        return s;
    }
    static std::string Lower(std::string s) {
        for (auto& c : s) c = (char)tolower((unsigned char)c);
        return s;
    }
    static std::string FriendlyName(IMMDevice* dev) {
        // PKEY_Device_FriendlyName, defined here so no extra import library is needed.
        static const PROPERTYKEY kName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
        std::string name = "?";
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(kName, &v)) && v.vt == VT_LPWSTR) name = Narrow(v.pwszVal);
            PropVariantClear(&v);
            ps->Release();
        }
        return name;
    }

    IMMDevice* Find(IMMDeviceEnumerator* en) {
        IMMDevice* dev = nullptr;
        if (Lower(target_) == "default") {
            en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
            return dev;
        }
        IMMDeviceCollection* all = nullptr;
        if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all))) return nullptr;
        UINT count = 0;
        all->GetCount(&count);
        for (UINT i = 0; i < count && !dev; i++) {
            IMMDevice* d = nullptr;
            if (FAILED(all->Item(i, &d))) continue;
            if (Lower(FriendlyName(d)).find(Lower(target_)) != std::string::npos) dev = d;
            else d->Release();
        }
        all->Release();
        return dev;
    }

    void Push(const float* stereo, size_t frames, unsigned rate) {
        std::vector<int16_t> out;
        auto emit = [&](float l, float r) {
            out.push_back((int16_t)std::max(-32768.0f, std::min(32767.0f, l * 32767.0f)));
            out.push_back((int16_t)std::max(-32768.0f, std::min(32767.0f, r * 32767.0f)));
        };
        if (rate == 48000) {
            for (size_t i = 0; i < frames; i++) emit(stereo[2 * i], stereo[2 * i + 1]);
        } else {
            pending_.insert(pending_.end(), stereo, stereo + frames * 2);
            const double step = rate / 48000.0;
            const size_t have = pending_.size() / 2;
            while (pos_ + 1 < have) {
                const size_t i = (size_t)pos_;
                const float f = (float)(pos_ - i);
                emit(pending_[2 * i] * (1 - f) + pending_[2 * i + 2] * f,
                     pending_[2 * i + 1] * (1 - f) + pending_[2 * i + 3] * f);
                pos_ += step;
            }
            const size_t used = std::min((size_t)pos_, have);
            pending_.erase(pending_.begin(), pending_.begin() + used * 2);
            pos_ -= used;
        }
        std::lock_guard<std::mutex> lk(mu_);
        ring_.insert(ring_.end(), out.begin(), out.end());
        if (ring_.size() > kMaxFrames * 2) {
            ring_.erase(ring_.begin(), ring_.end() - kPrimeFrames * 2);
            drops++;
        }
    }

    // One capture session; returns when the device goes away or on stop.
    void Capture(IMMDevice* dev) {
        IAudioClient* ac = nullptr;
        IAudioCaptureClient* cc = nullptr;
        WAVEFORMATEX* wf = nullptr;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) ||
            FAILED(ac->GetMixFormat(&wf)) ||
            FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000, 0, wf, nullptr)) ||
            FAILED(ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc)) || FAILED(ac->Start())) {
            std::printf("audio: cannot capture %s\n", device.c_str());
            if (cc) cc->Release();
            if (ac) ac->Release();
            if (wf) CoTaskMemFree(wf);
            Sleep(2000);
            return;
        }
        // Sample type: float or integer PCM. For WAVE_FORMAT_EXTENSIBLE the format tag
        // is the first field of the subformat GUID.
        WORD tag = wf->wFormatTag;
        if (tag == WAVE_FORMAT_EXTENSIBLE) tag = (WORD)((WAVEFORMATEXTENSIBLE*)wf)->SubFormat.Data1;
        const bool is_float = tag == 3 /* WAVE_FORMAT_IEEE_FLOAT */ && wf->wBitsPerSample == 32;
        const unsigned ch = wf->nChannels, bits = wf->wBitsPerSample, rate = wf->nSamplesPerSec;
        std::printf("audio: capturing \"%s\" (%u Hz, %u ch, %u-bit %s)\n", device.c_str(), rate, ch, bits,
                    is_float ? "float" : "PCM");
        opened_ = true;
        std::vector<float> st;
        while (!stop_) {
            Sleep(3);
            UINT32 pkt = 0;
            HRESULT hr = cc->GetNextPacketSize(&pkt);
            while (SUCCEEDED(hr) && pkt) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = cc->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) break;
                st.assign((size_t)frames * 2, 0.0f);
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && ch) {
                    for (UINT32 i = 0; i < frames; i++) {
                        for (unsigned c = 0; c < 2; c++) {
                            const unsigned src = std::min(c, ch - 1);   // mono -> both sides
                            const BYTE* s = data + ((size_t)i * ch + src) * (bits / 8);
                            float v = 0;
                            if (is_float) v = *(const float*)s;
                            else if (bits == 16) v = *(const int16_t*)s / 32768.0f;
                            else if (bits == 24) v = (int32_t)((s[0] << 8) | (s[1] << 16) | (s[2] << 24)) / 2147483648.0f;
                            else if (bits == 32) v = *(const int32_t*)s / 2147483648.0f;
                            st[2 * i + c] = v;
                        }
                    }
                }
                cc->ReleaseBuffer(frames);
                Push(st.data(), frames, rate);
                hr = cc->GetNextPacketSize(&pkt);
            }
            if (FAILED(hr)) {
                std::printf("audio: device lost (0x%08lx), reopening\n", (unsigned long)hr);
                break;
            }
        }
        ac->Stop();
        cc->Release();
        ac->Release();
        CoTaskMemFree(wf);
    }

    void Run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!stop_) {
            IMMDeviceEnumerator* en = nullptr;
            IMMDevice* dev = nullptr;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                           __uuidof(IMMDeviceEnumerator), (void**)&en)))
                dev = Find(en);
            if (dev) {
                device = FriendlyName(dev);
                Capture(dev);
                dev->Release();
            } else {
                Sleep(2000);
            }
            if (en) en->Release();
        }
        CoUninitialize();
    }
};

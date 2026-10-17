// Read-only endpoint/loopback diagnostic. Does not play or change audio.
// Build: g++ -O2 -std=c++17 audio_probe.cpp -lole32 -static -o audio-probe.exe
#define NOMINMAX
#include "audio_capture.h"
#include <memory>

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (FAILED(hr)) { std::printf("enumerator failed: %08lx\n", (unsigned long)hr); return 1; }
    IMMDevice* def = nullptr;
    LPWSTR default_id = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &def))) {
        def->GetId(&default_id);
        def->Release();
    }
    IMMDeviceCollection* all = nullptr;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all))) return 1;
    UINT count = 0;
    all->GetCount(&count);
    struct Endpoint {
        std::string name;
        std::unique_ptr<LoopbackAudio> capture;
        int peak = 0;
        unsigned packets = 0;
        bool is_default = false;
    };
    std::vector<Endpoint> endpoints;
    static const PROPERTYKEY name_key = {{0xa45c254e,0xdf1c,0x4efd,{0x80,0x20,0x67,0xd1,0x46,0xa8,0x50,0xe0}},14};
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* d = nullptr;
        if (FAILED(all->Item(i, &d))) continue;
        Endpoint ep;
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id))) {
            ep.is_default = default_id && !wcscmp(id, default_id);
            CoTaskMemFree(id);
        }
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(name_key, &v)) && v.vt == VT_LPWSTR) {
                char name[1024] = {};
                WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, name, sizeof(name), nullptr, nullptr);
                ep.name = name;
            }
            PropVariantClear(&v);
            ps->Release();
        }
        ep.capture.reset(new LoopbackAudio);
        ep.capture->Start(ep.name);
        endpoints.push_back(std::move(ep));
        d->Release();
    }
    // Use the production capture code concurrently, without binding any stream port.
    const ULONGLONG end = GetTickCount64() + 5000;
    while (GetTickCount64() < end) {
        for (auto& ep : endpoints) {
            int16_t samples[832];
            while (ep.capture->Pop(samples, 416)) {
                ++ep.packets;
                for (int16_t s : samples) ep.peak = std::max(ep.peak, std::abs((int)s));
            }
        }
        Sleep(5);
    }
    for (auto& ep : endpoints) {
        std::printf("%s packets=%u PCM peak=%d/32767 %s\n", ep.is_default ? "DEFAULT" : "       ", ep.packets, ep.peak, ep.name.c_str());
    }
    endpoints.clear();
    CoTaskMemFree(default_id);
    all->Release();
    en->Release();
    CoUninitialize();
}

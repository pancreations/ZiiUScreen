// ziiu-monitor: plug the ZiiUScreen virtual monitor into Windows while it runs.
//
//   ziiu-monitor [seconds] [frame.bmp] [--activate]     (admin; default 30 s)
//
// --activate: if Windows keeps the new monitor inactive, turn on only its display
// path (SetDisplayConfig with the current active paths plus ours, not saved to the
// display database). Other monitors keep their configuration.
//
// Creates the software device that driver/ziiu_display (IddCx) binds to, so Windows
// sees a new monitor ("Wii U GamePad"). The device is removed as soon as this
// process exits (SwDeviceClose, or the handle closing if it is killed): nothing
// stays attached. Every second it reports the frames the driver handed over through
// shared memory (ziiu_display_shared.h); at the end it saves the latest one as a BMP.
//
// Device creation follows Microsoft's IddSampleApp (Windows-driver-samples, MS-PL).
#include <windows.h>
#include <swdevice.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../../driver/include/ziiu_display_shared.h"

static HANDLE g_stop;

static BOOL WINAPI OnCtrl(DWORD)
{
    SetEvent(g_stop);
    return TRUE;
}

static VOID WINAPI OnCreated(HSWDEVICE, HRESULT hr, PVOID ctx, PCWSTR id)
{
    std::printf("device %s (0x%08lx) %ls\n", SUCCEEDED(hr) ? "created" : "FAILED", (unsigned long)hr, id ? id : L"");
    SetEvent(*(HANDLE*)ctx);
}

// Copy the latest complete frame (see the seqlock rule in ziiu_display_shared.h).
static bool LatestFrame(const ZIIU_FRAME_HEADER* h, const BYTE* base, std::vector<BYTE>& px, UINT& w, UINT& hgt)
{
    for (int tries = 0; tries < 4; tries++) {
        int64_t seq = h->Latest;
        if (!seq) return false;
        const ZIIU_FRAME_SLOT_INFO& s = h->Slot[seq % ZIIU_FRAME_SLOTS];
        if (s.Seq != seq) continue;
        w = s.Width;
        hgt = s.Height;
        px.assign(base + h->DataOffset + (size_t)(seq % ZIIU_FRAME_SLOTS) * h->SlotBytes,
                  base + h->DataOffset + (size_t)(seq % ZIIU_FRAME_SLOTS) * h->SlotBytes + (size_t)w * hgt * 4);
        if (s.Seq == seq) return true;
    }
    return false;
}

// The driver's stage log (see Diag in Driver.cpp): print lines not shown yet.
static void PrintDriverStages()
{
    static long long shown = 0;
    FILE* f = nullptr;
    if (_wfopen_s(&f, L"C:\\ProgramData\\ZiiUScreen\\display.log", L"rb") || !f) {
        std::printf("driver log: none yet\n");
        return;
    }
    _fseeki64(f, shown, SEEK_SET);
    char line[256];
    while (fgets(line, sizeof(line), f)) std::printf("  driver: %s", line);
    shown = _ftelli64(f);
    fclose(f);
}

// Find the GamePad monitor's display path. Returns true if found; *active tells
// whether Windows has it in the desktop.
static bool FindPath(bool activate)
{
    UINT32 np = 0, nm = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &np, &nm) != ERROR_SUCCESS) return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(nm);
    if (QueryDisplayConfig(QDC_ALL_PATHS, &np, paths.data(), &nm, modes.data(), nullptr) != ERROR_SUCCESS) return false;
    paths.resize(np);
    int ours = -1;
    for (UINT32 i = 0; i < np; i++) {
        DISPLAYCONFIG_TARGET_DEVICE_NAME tn = {};
        tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        tn.header.size = sizeof(tn);
        tn.header.adapterId = paths[i].targetInfo.adapterId;
        tn.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) continue;
        if (wcscmp(tn.monitorFriendlyDeviceName, L"Wii U GamePad") != 0) continue;
        bool act = (paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE) != 0;
        if (act) {
            const DISPLAYCONFIG_MODE_INFO* sm = paths[i].sourceInfo.modeInfoIdx < nm ? &modes[paths[i].sourceInfo.modeInfoIdx] : nullptr;
            if (sm && sm->infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
                std::printf("GamePad monitor: ACTIVE, desktop %ux%u at (%d,%d)\n", sm->sourceMode.width, sm->sourceMode.height,
                            sm->sourceMode.position.x, sm->sourceMode.position.y);
            else
                std::printf("GamePad monitor: ACTIVE\n");
            return true;
        }
        if (ours < 0) ours = (int)i;
    }
    if (ours < 0) {
        std::printf("GamePad monitor: not in the display configuration\n");
        return false;
    }
    DISPLAYCONFIG_PATH_INFO cand = paths[ours];
    std::printf("GamePad monitor: connected but INACTIVE (targetAvailable=%d statusFlags=0x%x)\n",
                (int)cand.targetInfo.targetAvailable, (unsigned)cand.targetInfo.statusFlags);
    if (!activate) return true;
    // Active paths plus ours, with explicit 1920x1080 @ 59.94 source and target modes to
    // the right of the current desktop. SDC_VALIDATE first, to get Windows' verdict.
    UINT32 ap = 0, am = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &ap, &am) != ERROR_SUCCESS) return true;
    std::vector<DISPLAYCONFIG_PATH_INFO> act(ap + 1);
    std::vector<DISPLAYCONFIG_MODE_INFO> amodes(am + 2);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &ap, act.data(), &am, amodes.data(), nullptr) != ERROR_SUCCESS) return true;
    LONG right = 0;
    for (UINT32 i = 0; i < am; i++)
        if (amodes[i].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
            right = max(right, amodes[i].sourceMode.position.x + (LONG)amodes[i].sourceMode.width);
    // A source on our adapter that no active path uses.
    int pick = -1;
    for (UINT32 i = 0; i < np && pick < 0; i++) {
        if (memcmp(&paths[i].targetInfo.adapterId, &cand.targetInfo.adapterId, sizeof(LUID)) ||
            paths[i].targetInfo.id != cand.targetInfo.id) continue;
        bool used = false;
        for (UINT32 k = 0; k < ap; k++)
            if (!memcmp(&act[k].sourceInfo.adapterId, &paths[i].sourceInfo.adapterId, sizeof(LUID)) &&
                act[k].sourceInfo.id == paths[i].sourceInfo.id) used = true;
        if (!used) pick = (int)i;
    }
    if (pick < 0) {
        std::printf("activate: no free source for the GamePad target\n");
        return true;
    }
    DISPLAYCONFIG_PATH_INFO p = paths[pick];
    p.flags = DISPLAYCONFIG_PATH_ACTIVE;
    DISPLAYCONFIG_MODE_INFO sm = {}, tm = {};
    sm.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    sm.adapterId = p.sourceInfo.adapterId;
    sm.id = p.sourceInfo.id;
    sm.sourceMode.width = 1920;
    sm.sourceMode.height = 1080;
    sm.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
    sm.sourceMode.position.x = right;
    tm.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
    tm.adapterId = p.targetInfo.adapterId;
    tm.id = p.targetInfo.id;
    DISPLAYCONFIG_VIDEO_SIGNAL_INFO& si = tm.targetMode.targetVideoSignalInfo;
    si.activeSize.cx = si.totalSize.cx = 1920;
    si.activeSize.cy = si.totalSize.cy = 1080;
    si.vSyncFreq.Numerator = 60000; si.vSyncFreq.Denominator = 1001;
    si.hSyncFreq.Numerator = 60000 * 1080; si.hSyncFreq.Denominator = 1001;
    si.pixelRate = 60000ull * 1920 * 1080 / 1001;
    si.videoStandard = 255;
    si.AdditionalSignalInfo.videoStandard = 255;
    si.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    amodes[am] = sm;
    amodes[am + 1] = tm;
    p.sourceInfo.modeInfoIdx = am;
    p.targetInfo.modeInfoIdx = am + 1;
    act[ap] = p;
    const UINT32 flags = SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES;
    LONG v = SetDisplayConfig(ap + 1, act.data(), am + 2, amodes.data(), flags | SDC_VALIDATE);
    std::printf("activate: validate (explicit 1920x1080) -> %ld\n", v);
    LONG v2 = SetDisplayConfig(ap + 1, act.data(), am + 2, amodes.data(), SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_VALIDATE);
    std::printf("activate: validate (no ALLOW_CHANGES) -> %ld\n", v2);
    LONG r = SetDisplayConfig(ap + 1, act.data(), am + 2, amodes.data(), flags | SDC_APPLY);
    std::printf("activate: apply -> %ld\n", r);
    return true;
}

static bool SaveBmp(const char* path, const std::vector<BYTE>& px, UINT w, UINT h)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") || !f) return false;
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof(ih);
    ih.biWidth = (LONG)w;
    ih.biHeight = -(LONG)h;   // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + (DWORD)px.size();
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
    return true;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    unsigned seconds = argc > 1 ? (unsigned)std::atoi(argv[1]) : 30;
    const char* bmp = argc > 2 && std::strcmp(argv[2], "--activate") ? argv[2] : nullptr;
    bool activate = false;
    for (int i = 1; i < argc; i++)
        if (!std::strcmp(argv[i], "--activate")) activate = true;
    if (!seconds) seconds = 30;
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetConsoleCtrlHandler(OnCtrl, TRUE);

    HANDLE created = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    SW_DEVICE_CREATE_INFO info = {};
    info.cbSize = sizeof(info);
    info.pszInstanceId = L"ZiiUScreenDisplay";
    info.pszzHardwareIds = L"ZiiUScreenDisplay\0\0";
    info.pszzCompatibleIds = L"ZiiUScreenDisplay\0\0";
    info.pszDeviceDescription = L"ZiiUScreen GamePad Display";
    info.CapabilityFlags = SWDeviceCapabilitiesRemovable | SWDeviceCapabilitiesSilentInstall |
                           SWDeviceCapabilitiesDriverRequired;
    HSWDEVICE dev = nullptr;
    HRESULT hr = SwDeviceCreate(L"ZiiUScreenDisplay", L"HTREE\\ROOT\\0", &info, 0, nullptr, OnCreated, &created, &dev);
    if (FAILED(hr)) {
        std::printf("SwDeviceCreate failed 0x%08lx (admin? driver package installed?)\n", (unsigned long)hr);
        return 1;
    }
    if (WaitForSingleObject(created, 10000) != WAIT_OBJECT_0) {
        std::printf("device creation timed out\n");
        SwDeviceClose(dev);
        return 1;
    }

    HANDLE section = nullptr;
    const BYTE* base = nullptr;
    const ZIIU_FRAME_HEADER* h = nullptr;
    int64_t last = 0;
    for (unsigned t = 0; t < seconds; t++) {
        if (WaitForSingleObject(g_stop, 1000) == WAIT_OBJECT_0) break;
        PrintDriverStages();
        if (t == 1 || t == 3 || t == 7) FindPath(activate && t == 1);
        if (!h) {
            section = OpenFileMappingW(FILE_MAP_READ, FALSE, ZIIU_FRAME_SECTION);
            if (section) {
                base = (const BYTE*)MapViewOfFile(section, FILE_MAP_READ, 0, 0, ZIIU_FRAME_SECTION_BYTES);
                if (base && ((const ZIIU_FRAME_HEADER*)base)->Magic == ZIIU_FRAME_MAGIC) h = (const ZIIU_FRAME_HEADER*)base;
            }
        }
        if (!h) {
            std::printf("t=%2us: no frames yet (driver section not created)\n", t + 1);
            continue;
        }
        int64_t seq = h->Latest;
        const ZIIU_FRAME_SLOT_INFO& s = h->Slot[seq % ZIIU_FRAME_SLOTS];
        std::printf("t=%2us: %lld frames/s, total %lld, %ux%u, dropped %lld\n", t + 1, (long long)(seq - last),
                    (long long)seq, s.Width, s.Height, (long long)h->Dropped);
        last = seq;
    }
    if (bmp && h) {
        std::vector<BYTE> px;
        UINT w = 0, ht = 0;
        if (LatestFrame(h, base, px, w, ht) && SaveBmp(bmp, px, w, ht))
            std::printf("saved %ux%u frame to %s\n", w, ht, bmp);
        else
            std::printf("no complete frame to save\n");
    }
    if (base) UnmapViewOfFile(base);
    if (section) CloseHandle(section);
    SwDeviceClose(dev);
    std::printf("device removed\n");
    return 0;
}

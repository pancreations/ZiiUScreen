// ziiuctl - command-line access to ziiu_wifi.sys (run as administrator).
//
//   ziiuctl info              driver state, never touches the chip
//   ziiuctl chipid            ownership handshake + read chip ID (first hardware access)
//   ziiuctl clear-latch       re-enable hardware operations after a crash was investigated
//   ziiuctl fw-load [dir]     reset Wi-Fi subsystem, start DMA, download ROM patch + WM
//                             firmware, query NIC capabilities (milestone 2)
//   ziiuctl sniff <ch|hop|wiiu|wiiu-lock> [s] [file.pcap]
//                             fw-load + monitor mode (receive only) on a 5 GHz channel, or
//                             hopping until a WiiU... network is heard; print who's
//                             transmitting, optionally save every frame (milestone 3)
//   ziiuctl ap-pair [s] [ch]  (max 60 s, 3 dBm) fw-load + broadcast a Wii U pairing network (open AP,
//                             WiiU..._STA1, channel 36) and print frames (milestone 4)
//   ziiuctl ap-normal <ch> [s]  (max 60 s, 3 dBm) advertise the saved console's hidden,
//                             Nintendo-WPA2 network; diagnostic only (no association yet)
//   ziiuctl dma-stop          stop DMA, reset the Wi-Fi subsystem (firmware back to ROM)
//   ziiuctl reg <addr>        read a chip register (hex address)
//   ziiuctl tsf-probe         fw-load + receive-only monitor; find the counting TSF registers
//
// Firmware download follows mt76 (mt792x_load_firmware, mt76_connac2_load_patch,
// mt76_connac2_load_ram, mt7925_mcu_fill_message; BSD-3-Clause-Clear, MediaTek Inc.).

#include <windows.h>
#include <cfgmgr32.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "ziiu_wifi_ioctl.h"

static const wchar_t* kStreamStopEvent = L"Local\\ZiiUScreen.StreamStop";
static HANDLE streamStopEvent = nullptr;
static BOOL WINAPI StreamConsoleStop(DWORD kind)
{
    if (kind == CTRL_C_EVENT || kind == CTRL_BREAK_EVENT || kind == CTRL_CLOSE_EVENT ||
        kind == CTRL_LOGOFF_EVENT || kind == CTRL_SHUTDOWN_EVENT) {
        if (streamStopEvent) SetEvent(streamStopEvent);
        return TRUE; // cleanup remains on the main thread, never in this callback
    }
    return FALSE;
}

static const char* OpName(ULONG op)
{
    switch (op) {
    case ZiiuHwOpNone: return "none";
    case ZiiuHwOpReadChipId: return "read chip id";
    case ZiiuHwOpDmaStart: return "DMA start";
    case ZiiuHwOpDmaStop: return "DMA stop";
    case ZiiuHwOpMcuCmd: return "MCU command";
    case ZiiuHwOpFwScatter: return "firmware download";
    case ZiiuHwOpPollReg: return "register read";
    case ZiiuHwOpRxRead: return "RX read";
    case ZiiuHwOpWriteReg: return "register write";
    case ZiiuHwOpTxFrame: return "TX frame";
    case ZiiuHwOpRadioSession: return "radio session (TX)";
    default: return "unknown";
    }
}

static HANDLE OpenDevice()
{
    ULONG len = 0;
    if (CM_Get_Device_Interface_List_SizeW(&len, (LPGUID)&GUID_DEVINTERFACE_ZIIU_WIFI, nullptr,
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || len <= 1) {
        return INVALID_HANDLE_VALUE;
    }
    std::vector<wchar_t> list(len);
    if (CM_Get_Device_Interface_ListW((LPGUID)&GUID_DEVINTERFACE_ZIIU_WIFI, nullptr, list.data(), len,
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) {
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileW(list.data(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
}

static void PrintLastError(const char* what)
{
    DWORD err = GetLastError();
    char msg[256] = {};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, msg, sizeof(msg), nullptr);
    std::fprintf(stderr, "%s failed: %lu %s", what, err, msg);
}

static bool Ioctl(HANDLE h, DWORD code, const void* in, DWORD inLen, void* out, DWORD outLen, DWORD* got = nullptr)
{
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(h, code, const_cast<void*>(in), inLen, out, outLen, &bytes, nullptr);
    if (got) {
        *got = bytes;
    }
    return ok != FALSE;
}

// ---------------------------------------------------------------------------
// Little helpers for building and parsing MCU messages
// ---------------------------------------------------------------------------

static void Put16(std::vector<uint8_t>& b, size_t at, uint16_t v) { b[at] = (uint8_t)v; b[at + 1] = (uint8_t)(v >> 8); }
static void Put32(std::vector<uint8_t>& b, size_t at, uint32_t v) { for (int i = 0; i < 4; i++) b[at + i] = (uint8_t)(v >> (8 * i)); }
static uint16_t Le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t Be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// connac3 TXD for MCU commands (MT_TXD0_*, MT_TXD1_HDR_FORMAT in mt76_connac3_mac.h)
static void PutTxd(std::vector<uint8_t>& b)
{
    const uint32_t MT_TX_TYPE_CMD = 2, MT_TX_MCU_PORT_RX_Q0 = 0x20, MT_HDR_FORMAT_CMD = 1;
    Put32(b, 0, ((uint32_t)b.size() & 0xffff) | (MT_TX_TYPE_CMD << 23) | (MT_TX_MCU_PORT_RX_Q0 << 25));
    Put32(b, 4, MT_HDR_FORMAT_CMD << 14);
}

// MCU command ids (mt76_connac_mcu.h)
enum : uint8_t {
    MCU_CMD_TARGET_ADDRESS_LEN_REQ = 0x01,
    MCU_CMD_FW_START_REQ = 0x02,
    MCU_CMD_NIC_POWER_CTRL = 0x04,
    MCU_CMD_PATCH_START_REQ = 0x05,
    MCU_CMD_PATCH_FINISH_REQ = 0x07,
    MCU_CMD_PATCH_SEM_CONTROL = 0x10,
};
enum : uint16_t { MCU_UNI_CMD_CHIP_CONFIG = 0x0e };
enum { PATCH_NOT_DL_SEM_FAIL, PATCH_IS_DL, PATCH_NOT_DL_SEM_SUCCESS, PATCH_REL_SEM_SUCCESS };

const size_t EVT_HDR = 44;  // struct mt7925_mcu_rxd

class Mcu {
public:
    explicit Mcu(HANDLE h) : h_(h) {}

    // Legacy (non-unified) command: struct mt76_connac2_mcu_txd, 64-byte header.
    bool Cmd(uint8_t cid, const std::vector<uint8_t>& payload, bool wait, std::vector<uint8_t>* evt = nullptr)
    {
        std::vector<uint8_t> m(64 + payload.size());
        uint8_t seq = NextSeq();
        std::memcpy(m.data() + 64, payload.data(), payload.size());
        PutTxd(m);
        Put16(m, 32, (uint16_t)(m.size() - 32));               // len
        Put16(m, 34, (1u << 15) | (0x20u << 10));              // pq_id = MCU_PQ_ID(MCU port, RX_Q0)
        m[36] = cid;
        m[37] = 0xa0;                                          // MCU_PKT_ID
        m[38] = 3;                                             // set_query = MCU_Q_NA
        m[39] = seq;
        // ext_cid 0, s2d_index MCU_S2D_H2N
        return Send(m, seq, wait, evt);
    }

    // Unified command: struct mt76_connac2_mcu_uni_txd, 48-byte header.
    bool UniCmd(uint16_t cid, uint8_t option, const std::vector<uint8_t>& payload, bool wait,
                std::vector<uint8_t>* evt = nullptr)
    {
        std::vector<uint8_t> m(48 + payload.size());
        uint8_t seq = NextSeq();
        std::memcpy(m.data() + 48, payload.data(), payload.size());
        PutTxd(m);
        Put16(m, 32, (uint16_t)(m.size() - 32));
        Put16(m, 34, cid);
        m[37] = 0xa0;
        m[39] = seq;
        m[43] = option;
        return Send(m, seq, wait, evt);
    }

    bool Scatter(const uint8_t* data, size_t len)
    {
        while (len > 0) {
            DWORD n = (DWORD)(len < ZIIU_FW_SCATTER_MAX ? len : ZIIU_FW_SCATTER_MAX);
            if (!Ioctl(h_, IOCTL_ZIIU_FW_SCATTER, data, n, nullptr, 0)) {
                PrintLastError("FW_SCATTER");
                return false;
            }
            data += n;
            len -= n;
        }
        return true;
    }

private:
    uint8_t NextSeq()
    {
        seq_ = (uint8_t)((seq_ + 1) & 0xf);
        if (seq_ == 0) {
            seq_ = 1;
        }
        return seq_;
    }

    bool Send(const std::vector<uint8_t>& m, uint8_t seq, bool wait, std::vector<uint8_t>* evt)
    {
        std::vector<uint8_t> in(sizeof(ZIIU_MCU_CMD) + m.size());
        ZIIU_MCU_CMD hdr = {};
        hdr.Flags = wait ? ZIIU_MCU_WAIT_RESPONSE : 0;
        hdr.Seq = seq;
        hdr.TimeoutMs = 3000;  // mt7925e: mcu.timeout = 3 * HZ
        hdr.Length = (ULONG)m.size();
        std::memcpy(in.data(), &hdr, sizeof(hdr));
        std::memcpy(in.data() + sizeof(hdr), m.data(), m.size());
        std::vector<uint8_t> out(wait ? ZIIU_MCU_MAX_EVENT : 0);
        DWORD got = 0;
        if (!Ioctl(h_, IOCTL_ZIIU_MCU_CMD, in.data(), (DWORD)in.size(), out.empty() ? nullptr : out.data(),
                   (DWORD)out.size(), &got)) {
            char what[64];
            std::snprintf(what, sizeof(what), "MCU command 0x%02x/0x%02x (seq %u)", m[36], m[34], seq);
            PrintLastError(what);
            return false;
        }
        if (evt) {
            out.resize(got);
            *evt = std::move(out);
        }
        return true;
    }

    HANDLE h_;
    uint8_t seq_ = 0;
};

static bool PollReg(HANDLE h, ULONG addr, ULONG mask, ULONG value, ULONG timeoutMs, ULONG* last)
{
    ZIIU_POLL_REG req = { addr, mask, value, timeoutMs };
    ZIIU_POLL_RESULT res = {};
    if (!Ioctl(h, IOCTL_ZIIU_POLL_REG, &req, sizeof(req), &res, sizeof(res))) {
        PrintLastError("POLL_REG");
        return false;
    }
    if (last) {
        *last = res.Value;
    }
    return res.Matched != 0;
}

static bool ReadFile(const std::wstring& path, std::vector<uint8_t>& data)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 16 * 1024 * 1024;
    if (ok) {
        data.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = ::ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr) && got == data.size();
    }
    CloseHandle(f);
    return ok;
}

// ---------------------------------------------------------------------------
// Firmware download
// ---------------------------------------------------------------------------

const uint32_t MT_CONN_ON_MISC = 0x7c0600f0;

// mt76_connac_mcu_init_download: PATCH_START_REQ for patch-type addresses on mt7925.
static bool InitDownload(Mcu& mcu, uint32_t addr, uint32_t len, uint32_t mode)
{
    std::vector<uint8_t> p(12);
    Put32(p, 0, addr);
    Put32(p, 4, len);
    Put32(p, 8, mode);
    bool patchAddr = addr == 0x200000 || addr == 0x900000 || addr == 0xe0002800;
    return mcu.Cmd(patchAddr ? MCU_CMD_PATCH_START_REQ : MCU_CMD_TARGET_ADDRESS_LEN_REQ, p, true);
}

static bool SemCtrl(Mcu& mcu, bool get, int* result)
{
    std::vector<uint8_t> p(4), evt;
    Put32(p, 0, get ? 1 : 0);  // PATCH_SEM_GET / PATCH_SEM_RELEASE
    if (!mcu.Cmd(MCU_CMD_PATCH_SEM_CONTROL, p, true, &evt) || evt.size() < EVT_HDR) {
        return false;
    }
    *result = evt[40];  // mt7925_mcu_parse_response: status after sizeof(rxd) - 4
    return true;
}

// mt76_connac2_load_patch
static bool LoadPatch(Mcu& mcu, const std::vector<uint8_t>& fw)
{
    int sem = 0;
    if (!SemCtrl(mcu, true, &sem)) {
        return false;
    }
    if (sem == PATCH_IS_DL) {
        std::printf("  patch already loaded\n");
        return true;
    }
    if (sem != PATCH_NOT_DL_SEM_SUCCESS) {
        std::printf("  failed to get patch semaphore (%d)\n", sem);
        return false;
    }

    bool ok = true;
    const size_t HDR = 96, SEC = 64;
    uint32_t nRegion = fw.size() >= HDR ? Be32(&fw[32 + 12]) : 0;
    if (fw.size() < HDR || nRegion == 0 || nRegion > 16 || fw.size() < HDR + nRegion * SEC) {
        std::printf("  invalid patch file\n");
        ok = false;
    }
    if (ok) {
        std::printf("  patch build %.16s, hw/sw ver 0x%08x, %u sections\n", (const char*)&fw[0], Be32(&fw[20]), nRegion);
    }
    for (uint32_t i = 0; ok && i < nRegion; i++) {
        const uint8_t* s = &fw[HDR + i * SEC];
        uint32_t type = Be32(s), offs = Be32(s + 4), addr = Be32(s + 12), len = Be32(s + 16), info = Be32(s + 20);
        if ((type & 0xffff) != 2 || (uint64_t)offs + len > fw.size()) {  // PATCH_SEC_TYPE_INFO
            std::printf("  invalid patch section %u\n", i);
            ok = false;
            break;
        }
        // mt76_connac2_get_data_mode
        uint32_t mode = 1u << 31;  // DL_MODE_NEED_RSP
        if (info != 0xffffffff) {
            switch (info >> 24) {
            case 0: break;                                                        // plain
            case 1: mode |= 1 | (((info & 0xff) << 1) & 0x6) | (1 << 3); break;   // AES
            case 2: mode |= 1 | (1 << 6) | (1 << 3); break;                       // scramble
            default: std::printf("  unsupported patch encryption %u\n", info >> 24); ok = false; break;
            }
        }
        if (!ok) {
            break;
        }
        std::printf("  patch section %u: addr 0x%08x len 0x%x mode 0x%08x\n", i, addr, len, mode);
        ok = InitDownload(mcu, addr, len, mode) && mcu.Scatter(&fw[offs], len);
    }
    if (ok) {
        std::vector<uint8_t> p(4), evt;  // check_crc = 0
        ok = mcu.Cmd(MCU_CMD_PATCH_FINISH_REQ, p, true, &evt) && evt.size() >= EVT_HDR;
        if (ok && evt[40] != 0) {
            std::printf("  patch finish returned %u\n", evt[40]);
            ok = false;
        }
    }
    if (!SemCtrl(mcu, false, &sem) || sem != PATCH_REL_SEM_SUCCESS) {
        std::printf("  failed to release patch semaphore (%d)\n", sem);
        ok = false;
    }
    return ok;
}

// mt76_connac2_load_ram + mt76_connac_mcu_send_ram_firmware (WM only)
static bool LoadRam(Mcu& mcu, const std::vector<uint8_t>& fw)
{
    const size_t TRAILER = 36, REGION = 40;
    if (fw.size() < TRAILER) {
        std::printf("  invalid RAM file\n");
        return false;
    }
    const uint8_t* t = &fw[fw.size() - TRAILER];
    uint32_t nRegion = t[2];
    if (nRegion == 0 || fw.size() < TRAILER + nRegion * REGION) {
        std::printf("  invalid RAM file\n");
        return false;
    }
    std::printf("  WM firmware version %.10s, build %.15s, %u regions\n", (const char*)t + 7, (const char*)t + 17, nRegion);

    uint32_t offset = 0, override = 0;
    for (uint32_t i = 0; i < nRegion; i++) {
        const uint8_t* r = t - (nRegion - i) * REGION;
        uint32_t addr = Le32(r + 16), len = Le32(r + 20);
        uint8_t fs = r[24];
        if ((uint64_t)offset + len > fw.size() - TRAILER - nRegion * REGION) {
            std::printf("  invalid RAM region %u\n", i);
            return false;
        }
        if (fs & 0x40) {  // FW_FEATURE_NON_DL
            std::printf("  region %u: addr 0x%08x len 0x%x (not downloaded)\n", i, addr, len);
            offset += len;
            continue;
        }
        if (fs & 0x20) {  // FW_FEATURE_OVERRIDE_ADDR
            override = addr;
        }
        // mt76_connac_mcu_gen_dl_mode
        uint32_t mode = (1u << 31) | (((fs >> 1) & 3) << 1);
        if (fs & 0x01) mode |= 1 | (1 << 3);  // encrypted: ENCRYPT | RESET_SEC_IV
        if (fs & 0x10) mode |= 1 << 6;        // DL_CONFIG_ENCRY_MODE_SEL
        std::printf("  region %u: addr 0x%08x len 0x%x mode 0x%08x\n", i, addr, len, mode);
        if (!InitDownload(mcu, addr, len, mode) || !mcu.Scatter(&fw[offset], len)) {
            return false;
        }
        offset += len;
    }
    std::vector<uint8_t> p(8);
    Put32(p, 0, override ? 1 : 0);  // FW_START_OVERRIDE
    Put32(p, 4, override);
    return mcu.Cmd(MCU_CMD_FW_START_REQ, p, true);
}

// mt7925_mcu_get_nic_capability: proves the WM firmware answers unified commands.
static bool NicCapability(Mcu& mcu, uint8_t mac[6] = nullptr)
{
    std::vector<uint8_t> p(8), evt;
    Put16(p, 4, 3);  // UNI_CHIP_CONFIG_NIC_CAPA
    Put16(p, 6, 4);
    // option: MCU_CMD_UNI_EXT_ACK without MCU_CMD_ACK (mt7925 does that for CHIP_CONFIG)
    if (!mcu.UniCmd(MCU_UNI_CMD_CHIP_CONFIG, 0x06, p, true, &evt) || evt.size() < EVT_HDR + 4) {
        return false;
    }
    const uint8_t* d = evt.data() + EVT_HDR;
    size_t left = evt.size() - EVT_HDR - 4;
    uint16_t n = Le16(d);
    d += 4;
    std::printf("  NIC capability: %u elements\n", n);
    for (uint16_t i = 0; i < n && left >= 4; i++) {
        uint16_t tag = Le16(d), len = Le16(d + 2);
        if (len < 4 || len > left) {
            break;
        }
        if (tag == 7 && len >= 10) {  // MT_NIC_CAP_MAC_ADDR
            std::printf("  MAC address: %02x:%02x:%02x:%02x:%02x:%02x\n", d[4], d[5], d[6], d[7], d[8], d[9]);
            if (mac) {
                std::memcpy(mac, d + 4, 6);
            }
        } else if (tag == 8 && len >= 4 + 11) {  // MT_NIC_CAP_PHY
            std::printf("  PHY: nss %u, bands%s%s\n", d[4 + 4], (d[4 + 10] & 1) ? " 2.4GHz" : "",
                        (d[4 + 10] & 2) ? " 5GHz" : "");
        }
        d += len;
        left -= len;
    }
    return true;
}

static std::wstring RepoPath(const wchar_t* rel)
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir = dir.substr(0, dir.find_last_of(L'\\'));
    return dir + L"\\..\\..\\..\\" + rel;  // build\ziiuctl\Release -> repo root
}

static std::wstring DefaultFirmwareDir()
{
    return RepoPath(L"driver\\firmware");
}

// The owner's console as the GamePad remembers it, read from the ZiiU Probe output
// (tools/wiiu-probe; kept in .local\wiiu\, never committed: it holds the WPA2 key).
struct Console {
    std::string ssid;
    uint8_t bssid[6];
    uint8_t gamepad[6];
    std::string psk;  // 64 hex characters (the PMK)
};

static bool ParseMac(const char* s, uint8_t mac[6])
{
    unsigned m[6];
    if (sscanf_s(s, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) mac[i] = (uint8_t)m[i];
    return true;
}

static bool LoadConsole(const std::wstring& path, Console* c, bool quiet = false)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || !f) {
        if (!quiet) {
            std::fwprintf(stderr, L"can't read %ls (run ZiiU Probe on the Wii U, see docs/SETUP.md)\n", path.c_str());
        }
        return false;
    }
    std::vector<uint8_t> ee;
    bool drc0 = false, haveGamepad = false;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        unsigned at = 0;
        int used = 0;
        if (!std::strncmp(line, "--- ", 4)) {
            drc0 = std::strstr(line, "DRC0") != nullptr;
        } else if (drc0 && !std::strncmp(line, "mac: r=0", 8) && std::strstr(line, "dev=2 ")) {
            haveGamepad = ParseMac(std::strstr(line, "dev=2 ") + 6, c->gamepad);
        } else if (sscanf_s(line, "%3x:%n", &at, &used) == 1 && used == 4 && at == ee.size()) {
            for (const char* p = line + used; *p;) {
                unsigned v;
                int n = 0;
                if (sscanf_s(p, " %2x%n", &v, &n) != 1) break;
                ee.push_back((uint8_t)v);
                p += n;
            }
        }
    }
    std::fclose(f);
    // UIC EEPROM pairing block: SSID (32) @1, SSID length @0x21, PSK (64) @0x25, PSK
    // length @0x65, BSSID @0x7f.
    if (ee.size() < 0x85 || !haveGamepad || ee[0x21] == 0 || ee[0x21] > 32 || ee[0x65] != 64) {
        std::fprintf(stderr, "ziiu-probe.txt has no GamePad pairing data\n");
        return false;
    }
    c->ssid.assign((const char*)&ee[1], ee[0x21]);
    c->psk.assign((const char*)&ee[0x25], 64);
    std::memcpy(c->bssid, &ee[0x7f], 6);
    return true;
}

static int FwLoad(HANDLE h, const std::wstring& dir, uint8_t mac[6] = nullptr)
{
    std::vector<uint8_t> patch, ram;
    std::wstring patchPath = dir + L"\\WIFI_MT7925_PATCH_MCU_1_1_hdr.bin";
    std::wstring ramPath = dir + L"\\WIFI_RAM_CODE_MT7925_1_1.bin";
    if (!ReadFile(patchPath, patch) || !ReadFile(ramPath, ram)) {
        std::fwprintf(stderr, L"Firmware not found in %ls\nRun tools\\fetch-firmware.ps1 first.\n", dir.c_str());
        return 2;
    }

    std::printf("starting DMA (Wi-Fi subsystem reset)...\n");
    if (!Ioctl(h, IOCTL_ZIIU_DMA_START, nullptr, 0, nullptr, 0)) {
        PrintLastError("DMA_START");
        return 1;
    }
    Mcu mcu(h);
    ULONG v = 0;

    // mt792x_load_firmware
    std::vector<uint8_t> pwr(4);
    pwr[0] = 1;  // NIC_POWER_CTRL power_mode = 1 (restart)
    if (!mcu.Cmd(MCU_CMD_NIC_POWER_CTRL, pwr, false)) {
        return 1;
    }
    if (!PollReg(h, MT_CONN_ON_MISC, 0x7, 0x1, 1000, &v)) {  // FW_STATE == FW_PWR_ON
        std::printf("  warning: MCU not ready for firmware download (MISC 0x%08lx)\n", v);
    }

    std::printf("loading ROM patch...\n");
    if (!LoadPatch(mcu, patch)) {
        std::printf("ROM patch download FAILED\n");
        return 1;
    }
    std::printf("loading WM firmware...\n");
    if (!LoadRam(mcu, ram)) {
        std::printf("WM firmware download FAILED\n");
        return 1;
    }
    if (!PollReg(h, MT_CONN_ON_MISC, 0x3, 0x3, 1500, &v)) {  // MT_TOP_MISC2_FW_N9_RDY
        std::printf("firmware did not report ready (MISC 0x%08lx)\n", v);
        return 1;
    }
    std::printf("firmware running (MISC 0x%08lx)\n", v);

    std::printf("querying NIC capability...\n");
    if (!NicCapability(mcu, mac)) {
        std::printf("NIC capability query FAILED\n");
        return 1;
    }
    std::printf("milestone 2 OK: firmware is up and answering commands\n");
    return 0;
}

// ---------------------------------------------------------------------------
// Monitor mode (milestone 3): the minimum of mt76's start + monitor interface path
// ---------------------------------------------------------------------------

enum : uint16_t {
    MCU_UNI_CMD_DEV_INFO_UPDATE = 0x01,
    MCU_UNI_CMD_BSS_INFO_UPDATE = 0x02,
    MCU_UNI_CMD_STA_REC_UPDATE = 0x03,
    MCU_UNI_CMD_BAND_CONFIG = 0x08,
    MCU_UNI_CMD_SET_DOMAIN_INFO = 0x15,
    MCU_UNI_CMD_SNIFFER = 0x24,
    MCU_UNI_CMD_EFUSE_CTRL = 0x2d,
};
const uint8_t UNI_ACK = 0x07;  // MCU_CMD_UNI_EXT_ACK

static bool UniStatus(const char* what, bool sent, const std::vector<uint8_t>& evt, bool hasStatus)
{
    if (!sent) {
        std::printf("  %s: FAILED (no reply)\n", what);
        return false;
    }
    if (hasStatus && evt.size() >= EVT_HDR + 8) {  // struct mt7925_mcu_uni_event
        uint32_t st = Le32(&evt[EVT_HDR + 4]);
        if (st != 0) {
            std::printf("  %s: firmware status %u\n", what, st);
            return false;
        }
    }
    std::printf("  %s: ok\n", what);
    return true;
}

// Country for regulatory setup: the Windows region (ISO 3166 alpha-2), "00" if unknown.
static std::string Country()
{
    wchar_t iso[8] = {};
    GEOID geo = GetUserGeoID(GEOCLASS_NATION);
    if (geo == GEOID_NOT_AVAILABLE || GetGeoInfoW(geo, GEO_ISO2, iso, 8, 0) != 3) {
        return "00";
    }
    return std::string{ (char)towupper(iso[0]), (char)towupper(iso[1]) };
}

// mt7925_mcu_read_eeprom: one byte of the chip's EEPROM (efuse) image.
static bool ReadEeprom(Mcu& mcu, uint32_t offset, uint8_t* val)
{
    const uint32_t block = offset & ~15u;  // MT7925_EEPROM_BLOCK_SIZE
    std::vector<uint8_t> p(4 + 4 + 8 + 16), evt;
    Put16(p, 4, 1);  // UNI_EFUSE_ACCESS
    Put16(p, 6, (uint16_t)(p.size() - 4));
    Put32(p, 8, block);
    // option: MCU_CMD_UNI_QUERY_ACK
    if (!mcu.UniCmd(MCU_UNI_CMD_EFUSE_CTRL, 0x03, p, true, &evt) || evt.size() < EVT_HDR + 48 + 16 ||
        Le32(&evt[EVT_HDR + 12]) != block) {
        return false;
    }
    *val = evt[EVT_HDR + 48 + (offset & 15)];
    return true;
}

// __mt7925_mcu_set_clc for one CLC table: send every segment whose rule matches alpha2.
static int SendClc(Mcu& mcu, const uint8_t* c, uint32_t clen, const char* alpha2, uint8_t env)
{
    const uint8_t* data = c + 16;                // struct mt7925_clc: len, idx, ver, 10-byte type
    const uint8_t* end = c + clen;
    uint8_t idx = c[4], ver = c[5], nrSeg = c[8];
    if (data + 16 * nrSeg > end || (nrSeg > 0 && data + 8 > end)) {
        return -1;
    }
    const uint8_t* pos = data + 16 * nrSeg;      // rules follow the segment table
    const uint8_t* last = data + Le32(data + 4); // ...up to the first segment's data
    if (last > end) {
        return -1;
    }
    int sent = 0;
    for (; pos + 8 <= last; pos += 8) {          // struct mt7925_clc_rule
        if (pos[0] != (uint8_t)alpha2[0] || pos[1] != (uint8_t)alpha2[1] || pos[4] == 0 || pos[4] > nrSeg) {
            continue;
        }
        const uint8_t* seg = data + 16 * (pos[4] - 1);  // struct mt7925_clc_segment
        uint32_t segOff = Le32(seg + 4), segLen = Le32(seg + 8);
        if (data + segOff + segLen > end || 48 + 84 + segLen > ZIIU_MCU_MAX_MSG) {
            return -1;
        }
        std::vector<uint8_t> p(84 + segLen), evt;
        Put16(p, 4, 3);                          // tag
        Put16(p, 6, (uint16_t)(84 + segLen - 4));
        p[8] = ver;
        Put16(p, 10, (uint16_t)segLen);
        p[12] = idx;
        p[13] = env;
        p[16] = pos[0];
        p[17] = pos[1];
        p[18] = pos[2];                          // type
        p[19] = pos[3];
        std::memcpy(&p[84], data + segOff, segLen);
        if (!mcu.UniCmd(0x2c, UNI_ACK, p, true, &evt)) {  // MCU_UNI_CMD_SET_POWER_LIMIT
            return -1;
        }
        sent++;
    }
    return sent;
}

// mt7925_load_clc + mt7925_mcu_set_clc: the regulatory (CLC) power tables live in a
// non-downloaded region of the RAM firmware file.
static bool SetClc(Mcu& mcu, const std::vector<uint8_t>& ram, const std::string& alpha2)
{
    uint8_t hwType = 0;
    if (!ReadEeprom(mcu, 0xa71, &hwType)) {      // MT_EE_HW_TYPE
        std::printf("  regulatory: EEPROM read FAILED\n");
        return false;
    }
    const uint8_t encap = hwType & 3;            // MT_EE_HW_TYPE_ENCAP
    const size_t TRAILER = 36, REGION = 40;
    const uint8_t* t = &ram[ram.size() - TRAILER];
    uint32_t nRegion = t[2], offset = 0, len = 0;
    const uint8_t* base = nullptr;
    for (uint32_t i = 0; i < nRegion; i++) {
        const uint8_t* r = t - (nRegion - i) * REGION;
        len = Le32(r + 20);
        if ((uint64_t)offset + len > ram.size()) {
            break;
        }
        if ((r[24] & 0x40) && r[25] == 2) {      // FW_FEATURE_NON_DL, FW_TYPE_CLC
            base = &ram[offset];
            break;
        }
        offset += len;
    }
    if (!base) {
        std::printf("  regulatory: no CLC in firmware\n");
        return false;
    }
    bool have[4] = {};
    int total = 0;
    for (uint32_t off = 0; off + 16 <= len;) {
        const uint8_t* c = base + off;
        uint32_t clen = Le32(c);
        uint8_t idx = c[4];
        if (idx >= 4 || clen < 16 || (uint64_t)off + clen > len) {
            break;
        }
        off += clen;
        if (have[idx] || (idx == 2 && (c[6] & 3) != encap) || (c[7] & 3) != encap) {
            continue;
        }
        have[idx] = true;
        if (idx >= 2) {                          // BE_CTRL, REGD: not power tables
            continue;
        }
        int n = SendClc(mcu, c, clen, alpha2.c_str(), 0);  // ENVIRON_ANY
        if (n == 0) {
            n = SendClc(mcu, c, clen, "00", 1);  // not found: world, ENVIRON_INDOOR
        }
        if (n < 0) {
            std::printf("  regulatory: CLC table %u FAILED\n", idx);
            return false;
        }
        total += n;
    }
    std::printf("  regulatory: country %s, %d CLC segments\n", alpha2.c_str(), total);
    return total > 0;
}

static bool MonitorSetup(Mcu& mcu, const uint8_t mac[6])
{
    std::vector<uint8_t> evt;

    {   // mt7925_mcu_set_eeprom: efuse buffer mode, whole format
        std::vector<uint8_t> p(12);
        Put16(p, 4, 2);   // UNI_EFUSE_BUFFER_MODE
        Put16(p, 6, 8);
        p[8] = 0;         // EE_MODE_EFUSE
        p[9] = 1;         // EE_FORMAT_WHOLE
        if (!UniStatus("eeprom mode", mcu.UniCmd(MCU_UNI_CMD_EFUSE_CTRL, UNI_ACK, p, true, &evt), evt, false)) return false;
    }
    {   // mt7925_mcu_set_channel_domain: world domain, all common channels (RX only)
        static const uint8_t ch2[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 };
        std::vector<uint8_t> ch5;                // every 5 GHz channel number (sniff scan-wide)
        for (int c = 32; c <= 177; c++) ch5.push_back((uint8_t)c);
        size_t n = sizeof(ch2) + ch5.size();
        std::vector<uint8_t> p(8 + 8 + n * 8);
        p[0] = '0'; p[1] = '0';
        p[4] = 0; p[5] = 3; p[6] = 3;            // bw_2g 20/40, bw_5g/6g up to 160
        Put16(p, 8, 2);                          // tag
        Put16(p, 10, (uint16_t)(8 + n * 8));     // len
        p[12] = sizeof(ch2);
        p[13] = (uint8_t)ch5.size();
        size_t at = 16;
        for (uint8_t c : ch2) { Put16(p, at, c); at += 8; }
        for (uint8_t c : ch5) { Put16(p, at, c); at += 8; }
        if (!UniStatus("channel domain", mcu.UniCmd(MCU_UNI_CMD_SET_DOMAIN_INFO, UNI_ACK, p, true, &evt), evt, false)) return false;
    }
    {   // mt7925_mcu_set_rts_thresh
        std::vector<uint8_t> p(16);
        Put16(p, 4, 0x08);  // UNI_BAND_CONFIG_RTS_THRESHOLD
        Put16(p, 6, 12);
        Put32(p, 8, 0x92b);
        Put32(p, 12, 2);
        if (!UniStatus("rts threshold", mcu.UniCmd(MCU_UNI_CMD_BAND_CONFIG, UNI_ACK, p, true, &evt), evt, false)) return false;
    }
    // mt76_connac_mcu_uni_add_dev for a monitor vif: idx 0, omac 0, band 0xff, wcid 19
    {
        std::vector<uint8_t> p(16);
        p[0] = 0;         // omac_idx
        p[1] = 0xff;      // band_idx (mt7925: auto)
        Put16(p, 4, 0);   // DEV_INFO_ACTIVE
        Put16(p, 6, 12);
        p[8] = 1;         // active
        p[9] = 0;         // link_idx
        std::memcpy(&p[10], mac, 6);
        if (!UniStatus("add device", mcu.UniCmd(MCU_UNI_CMD_DEV_INFO_UPDATE, UNI_ACK, p, true, &evt), evt, true)) return false;
    }
    {
        std::vector<uint8_t> p(4 + 32);
        p[0] = 0;                  // bss_idx
        Put16(p, 4, 0);            // UNI_BSS_INFO_BASIC
        Put16(p, 6, 32);
        p[8] = 1;                  // active
        p[9] = 0;                  // omac_idx
        p[10] = 0;                 // hw_bss_idx
        p[11] = 0xff;              // band_idx
        Put32(p, 12, 0x10002);     // CONNECTION_INFRA_AP (monitor uses AP type)
        p[16] = 1;                 // conn_state
        p[17] = 0;                 // wmm_idx
        Put16(p, 24, 19);          // bmc_tx_wlan_idx = MT792x_WTBL_RESERVED - idx
        Put16(p, 30, 19);          // sta_idx
        if (!UniStatus("add bss", mcu.UniCmd(MCU_UNI_CMD_BSS_INFO_UPDATE, UNI_ACK, p, true, &evt), evt, true)) return false;
    }
    {   // mt7925_mcu_set_sniffer(enable)
        std::vector<uint8_t> p(12);
        Put16(p, 4, 0);   // UNI_SNIFFER_ENABLE
        Put16(p, 6, 8);
        p[8] = 1;
        if (!UniStatus("sniffer on", mcu.UniCmd(MCU_UNI_CMD_SNIFFER, UNI_ACK, p, true, &evt), evt, false)) return false;
    }
    {   // mt7925_mcu_set_deep_sleep(false): chip config string "KeepFullPwr 1", no reply
        const char* s = "KeepFullPwr 1";
        std::vector<uint8_t> p(8 + 8 + 320);
        Put16(p, 4, 2);                          // UNI_CHIP_CONFIG_CHIP_CFG
        Put16(p, 6, (uint16_t)(p.size() - 4));
        Put16(p, 12, (uint16_t)(std::strlen(s) + 1));  // data_size
        std::memcpy(&p[16], s, std::strlen(s) + 1);
        if (!mcu.UniCmd(MCU_UNI_CMD_CHIP_CONFIG, 0x06, p, false)) return false;
        std::printf("  full power: sent\n");
    }
    {   // mt7925_configure_filter: enable + other BSS + control frames
        std::vector<uint8_t> p(72);
        Put16(p, 4, 0x0c);  // UNI_BAND_CONFIG_SET_MAC80211_RX_FILTER
        Put16(p, 6, 68);
        p[8] = 0;           // mode: fif
        Put32(p, 12, (1u << 31) | (1u << 5) | (1u << 6));
        if (!UniStatus("rx filter", mcu.UniCmd(MCU_UNI_CMD_BAND_CONFIG, UNI_ACK, p, true, &evt), evt, false)) return false;
    }
    {   // mt7925_mcu_set_beacon_filter(false) + true promiscuous RX: clear MT_WF_RFCR drop
        // bits. Without OTHER_UC/OTHER_BSS/A2/A3 cleared, unicast traffic between other
        // devices (e.g. a console and its GamePad) never reaches us, only broadcasts.
        std::vector<uint8_t> p(72);
        Put16(p, 4, 0x0c);
        Put16(p, 6, 68);
        p[8] = 1;                                           // mode: bit op
        Put32(p, 16, (1u << 4) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) |  // PROBEREQ, A3_MAC,
                         (1u << 17) | (1u << 18) | (1u << 19));  // A3/A2_BSSID, OTHER_BEACON/BSS/UC/TIM
        p[20] = 2;                                          // MT7925_FIF_BIT_CLR
        if (!UniStatus("rx filter bits", mcu.UniCmd(MCU_UNI_CMD_BAND_CONFIG, UNI_ACK, p, true, &evt), evt, false)) return false;
    }
    return true;
}

// mt7925_mcu_config_sniffer: 20 MHz on one channel
static bool SetChannel(Mcu& mcu, uint8_t ch, bool quiet)
{
    std::vector<uint8_t> p(4 + 16), evt;
    Put16(p, 4, 1);           // UNI_SNIFFER_CONFIG
    Put16(p, 6, 16);
    p[10] = ch > 14 ? 2 : 1;  // ch_band
    p[11] = 0;                // bw 20 MHz
    p[12] = ch;               // control_ch
    p[13] = 0;                // sco
    p[14] = ch;               // center_ch
    p[16] = 1;                // drop_err (FCS errors)
    bool ok = mcu.UniCmd(MCU_UNI_CMD_SNIFFER, UNI_ACK, p, true, &evt);
    if (!quiet || !ok) {
        std::printf("  channel %u: %s\n", ch, ok ? "ok" : "FAILED");
    }
    return ok;
}

struct Seen {
    uint8_t addr2[6];
    uint8_t type, subtype;
    std::string ssid;
    uint8_t ch;
    int rssi;
    unsigned count;
};

static std::string Ssid(const uint8_t* ie, size_t len)
{
    while (len >= 2 && (size_t)ie[1] + 2 <= len) {
        if (ie[0] == 0) {
            std::string s;
            for (int i = 0; i < ie[1]; i++) {
                s += (ie[2 + i] >= 0x20 && ie[2 + i] < 0x7f) ? (char)ie[2 + i] : '?';
            }
            return s.empty() ? "<hidden/any>" : s;
        }
        len -= ie[1] + 2;
        ie += ie[1] + 2;
    }
    return "";
}

static const char* FrameName(uint8_t type, uint8_t sub)
{
    if (type == 0) {
        static const char* m[16] = { "assoc-req", "assoc-resp", "reassoc-req", "reassoc-resp", "probe-req",
                                     "probe-resp", "timing-adv", "mgmt-7", "beacon", "atim", "disassoc",
                                     "auth", "deauth", "action", "action-noack", "mgmt-15" };
        return m[sub];
    }
    return type == 1 ? "control" : type == 2 ? "data" : "ext";
}

static uint8_t g_self[6];    // our BSSID when acting as AP; frames to it get marked
static bool g_haveSelf = false;
static uint8_t g_gamepad[6]; // owner's GamePad (from ziiu-probe.txt); its frames get marked
static bool g_haveGamepad = false;
static uint8_t g_console[6]; // already-paired console BSSID (normal mode uses a hidden SSID)
static bool g_haveConsole = false;
static uint8_t g_wiiuCh = 0; // channel of a Wii U network or the owner's known radios
static uint8_t g_csaCh = 0;  // channel the saved console announced it is switching to
static ULONGLONG g_consoleSeen = 0; // tick of the saved console's latest frame
static FILE* g_pcap = nullptr;

// New channel from a Channel Switch (37) or Extended Channel Switch (60) element, or 0.
static uint8_t CsaChannel(const uint8_t* ie, size_t len)
{
    while (len >= 2 && (size_t)ie[1] + 2 <= len) {
        if (ie[0] == 37 && ie[1] >= 3) return ie[3];
        if (ie[0] == 60 && ie[1] >= 4) return ie[4];
        len -= ie[1] + 2;
        ie += ie[1] + 2;
    }
    return 0;
}

// Capture file: pcap with a minimal radiotap header (channel, signal), Wireshark-readable.
static FILE* PcapOpen(const char* path)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) {
        return nullptr;
    }
    const uint32_t hdr[6] = { 0xa1b2c3d4, 0x00040002, 0, 0, 65535, 127 };  // LINKTYPE_IEEE802_11_RADIOTAP
    std::fwrite(hdr, sizeof(hdr), 1, f);
    return f;
}

static void PcapWrite(const uint8_t* frame, size_t len, uint8_t ch, int rssi)
{
    uint8_t rt[13] = { 0, 0, 13, 0, 0x28, 0, 0, 0 };  // version, pad, length; present: channel | dBm signal
    uint16_t freq = (uint16_t)(ch > 14 ? 5000 + 5 * ch : 2407 + 5 * ch);
    uint16_t flags = ch > 14 ? 0x0140 : 0x0080;       // 5 GHz + OFDM / 2 GHz
    std::memcpy(rt + 8, &freq, 2);
    std::memcpy(rt + 10, &flags, 2);
    rt[12] = (uint8_t)(int8_t)rssi;
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t us = ((((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ull) / 10;
    uint32_t rec[4] = { (uint32_t)(us / 1000000), (uint32_t)(us % 1000000), (uint32_t)(len + 13),
                        (uint32_t)(len + 13) };
    std::fwrite(rec, sizeof(rec), 1, g_pcap);
    std::fwrite(rt, sizeof(rt), 1, g_pcap);
    std::fwrite(frame, len, 1, g_pcap);
}

// Parse one received buffer (mt7925_mac_fill_rx): RXD + groups + 802.11 frame.
static void HandleFrame(const uint8_t* b, size_t len, std::vector<Seen>& seen, unsigned* other)
{
    if (len < 32) {
        return;
    }
    uint32_t rxd0 = Le32(b), rxd1 = Le32(b + 4), rxd2 = Le32(b + 8), rxd3 = Le32(b + 12);
    if (((rxd0 >> 27) & 0x1f) != 2) {  // PKT_TYPE_NORMAL
        (*other)++;
        return;
    }
    size_t off = 32;
    int rssi = 0;
    if (rxd1 & (1u << 19)) off += 16;  // group 4
    if (rxd1 & (1u << 16)) off += 16;  // group 1
    if (rxd1 & (1u << 17)) off += 16;  // group 2
    if (rxd1 & (1u << 18)) {           // group 3 (P-RXV)
        if (off + 16 <= len) {
            rssi = ((int)(Le32(b + off + 12) & 0xff) - 220) / 2;
        }
        off += 16;
        if (rxd1 & (1u << 20)) off += 96;  // group 5
    }
    off += 2 * ((rxd2 >> 13) & 7);     // header padding
    if ((rxd2 & (1u << 7)) || off + 24 > len) {  // 802.3 translated, or too short
        (*other)++;
        return;
    }
    const uint8_t* f = b + off;
    size_t flen = len - off;
    uint16_t fc = Le16(f);
    uint8_t type = (fc >> 2) & 3, sub = (fc >> 4) & 0xf;
    uint8_t ch = (rxd3 >> 8) & 0xff;
    if (g_pcap) {
        PcapWrite(f, flen, ch, rssi);
    }
    std::string ssid;
    if (type == 0 && (sub == 4 || sub == 5 || sub == 8)) {
        size_t body = 24 + (sub == 4 ? 0 : 12);
        if (flen > body) {
            ssid = Ssid(f + body, flen - body);
        }
    }
    // Any frame involving a saved radio address, in any of the three MAC slots.
    bool linkFrame = (g_haveConsole && (!std::memcmp(f + 4, g_console, 6) ||
                                        !std::memcmp(f + 10, g_console, 6) ||
                                        !std::memcmp(f + 16, g_console, 6))) ||
                     (g_haveGamepad && (!std::memcmp(f + 4, g_gamepad, 6) ||
                                        !std::memcmp(f + 10, g_gamepad, 6) ||
                                        !std::memcmp(f + 16, g_gamepad, 6)));
    if (linkFrame) {
        g_consoleSeen = GetTickCount64();  // last tick we heard the Wii U link, for re-hop
    }
    // Lock hopping onto the console's channel: a frame the console sent, a frame the
    // GamePad sent to the console, or a WiiU... beacon/probe response. Not the
    // GamePad's probe requests: it probes every channel while searching (a probe
    // once locked a capture to an empty channel). Normal-mode beacons have an empty
    // SSID. Ignore noise-floor strays (a lone weak frame once pinned us to an empty
    // channel).
    bool fromConsole = g_haveConsole && !std::memcmp(f + 10, g_console, 6);
    bool padToConsole = g_haveConsole && g_haveGamepad && !std::memcmp(f + 10, g_gamepad, 6) &&
                        !std::memcmp(f + 4, g_console, 6);
    bool wiiuBeacon = type == 0 && (sub == 5 || sub == 8) && ssid.rfind("WiiU", 0) == 0;
    if (!g_wiiuCh && ch && rssi > -85 && (fromConsole || padToConsole || wiiuBeacon)) {
        g_wiiuCh = ch;
    }
    // The console moves channels at runtime (seen: spectrum-management CSA action,
    // count 0). Note announcements so a locked capture can follow it.
    if (g_haveConsole && !std::memcmp(f + 10, g_console, 6)) {
        g_consoleSeen = GetTickCount64();
        uint8_t to = 0;
        if (type == 0 && sub == 8 && flen > 36) {
            to = CsaChannel(f + 36, flen - 36);
        } else if (type == 0 && sub == 13 && flen >= 26) {
            if (f[24] == 0 && f[25] == 4) {                    // spectrum mgmt: Channel Switch
                to = CsaChannel(f + 26, flen - 26);
            } else if (f[24] == 4 && f[25] == 4 && flen >= 30) {  // public: Extended Channel Switch
                to = f[28];
            }
        }
        if (to && to != ch) {
            g_csaCh = to;
        }
    }
    for (Seen& s : seen) {
        if (!std::memcmp(s.addr2, f + 10, 6) && s.type == type && s.subtype == sub && s.ssid == ssid) {
            s.count++;
            s.rssi = rssi;
            return;
        }
    }
    Seen s = {};
    std::memcpy(s.addr2, f + 10, 6);
    s.type = type;
    s.subtype = sub;
    s.ssid = ssid;
    s.ch = ch;
    s.rssi = rssi;
    s.count = 1;
    seen.push_back(s);
    bool wiiu = ssid.rfind("WiiU", 0) == 0 || (g_haveConsole && !std::memcmp(f + 10, g_console, 6)) ||
                (g_haveSelf && !std::memcmp(f + 4, g_self, 6)) ||
                (g_haveGamepad && !std::memcmp(f + 10, g_gamepad, 6));
    static ULONGLONG t0 = GetTickCount64();
    std::printf("%s %5.1fs ch%-3u %4d dBm  %02x:%02x:%02x:%02x:%02x:%02x  %-12s %s\n", wiiu ? ">>" : "  ",
                (GetTickCount64() - t0) / 1000.0, ch, rssi,
                s.addr2[0], s.addr2[1], s.addr2[2], s.addr2[3], s.addr2[4], s.addr2[5], FrameName(type, sub),
                ssid.c_str());
    std::fflush(stdout);
}

// Receive and print frames for a while (shared by sniff and ap-pair).
static unsigned ListenLoop(HANDLE h, unsigned seconds, const std::function<void()>& tick)
{
    std::vector<uint8_t> buf(256 * 1024);
    std::vector<Seen> seen;
    unsigned other = 0, frames = 0;
    ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < seconds * 1000ull) {
        if (tick) {
            tick();
        }
        ZIIU_RX_READ req = { 200 };
        DWORD got = 0;
        if (!Ioctl(h, IOCTL_ZIIU_RX_READ, &req, sizeof(req), buf.data(), (DWORD)buf.size(), &got)) {
            PrintLastError("RX_READ");
            break;
        }
        for (DWORD at = 0; at + 4 <= got;) {
            uint32_t n = Le32(&buf[at]);
            if (at + 4 + n > got) {
                break;
            }
            HandleFrame(&buf[at + 4], n, seen, &other);
            frames++;
            at += 4 + ((n + 3) & ~3u);
        }
    }
    std::printf("done: %u frames, %zu distinct senders/types, %u non-802.11 buffers\n", frames, seen.size(), other);
    return frames;
}

static bool WriteReg(HANDLE h, ULONG addr, ULONG value)
{
    ZIIU_WRITE_REG req = { addr, value };
    if (!Ioctl(h, IOCTL_ZIIU_WRITE_REG, &req, sizeof(req), nullptr, 0)) {
        char what[48];
        std::snprintf(what, sizeof(what), "WRITE_REG 0x%08lx", addr);
        PrintLastError(what);
        return false;
    }
    return true;
}

static bool ReadReg(HANDLE h, ULONG addr, ULONG* v)
{
    return PollReg(h, addr, 0, 0, 0, v);
}

// MT7925 MAC setup, following mt7925_mac_init / mt792x_mac_init_band.
static bool MacInit(HANDLE h)
{
    // mt76_rates: 4 CCK (mode 0, idx 0-3), 8 OFDM (mode 1, hw idx 11,15,10,14,9,13,8,12)
    static const uint16_t rates[12] = { 0x000, 0x001, 0x002, 0x003, 0x10b, 0x10f, 0x10a, 0x10e,
                                        0x109, 0x10d, 0x108, 0x10c };
    const ULONG ITCR = 0x820d43b0, ITDR0 = 0x820d43b8, ITDR1 = 0x820d43bc;  // MT_WTBL_*
    for (int i = 0; i < 12; i++) {
        ULONG rate = ((rates[i] >> 8) << 6) | (rates[i] & 0x3f);  // MT_TX_RATE_MODE | MT_TX_RATE_IDX
        ULONG idx = 14 + i;                                       // MT792x_BASIC_RATES_TBL + i
        if (!WriteReg(h, ITDR0, rate) || !WriteReg(h, ITDR1, 1u << 6) ||  // MT_WTBL_SPE_IDX_SEL
            !WriteReg(h, ITCR, (1u << 16) | (1u << 31) | idx)) {          // WR | EXEC | idx
            return false;
        }
    }
    ULONG v = 0;
    const ULONG MDP_DCR0 = 0x820cc800, MDP_DCR1 = 0x820cc804;
    if (!ReadReg(h, MDP_DCR1, &v) || !WriteReg(h, MDP_DCR1, (v & ~0xfff8u) | (1536u << 3))) return false;
    if (!ReadReg(h, MDP_DCR0, &v) || !WriteReg(h, MDP_DCR0, v | (1u << 15))) return false;  // DAMSDU_EN
    for (ULONG idx = 0; idx < 20; ++idx) {
        const ULONG update = 0x820d4380;
        if (!ReadReg(h, update, &v) ||
            !WriteReg(h, update, (v & ~0xfffu) | idx | (1u << 14)) ||
            !PollReg(h, update, 1u << 31, 0, 5000, nullptr)) return false;
    }
    for (ULONG band = 0; band < 2; ++band) {
        const ULONG offset = band * 0x10000;
        const auto modify = [&](ULONG address, ULONG clear, ULONG set) {
            ULONG old = 0;
            return ReadReg(h, address + offset, &old) &&
                   WriteReg(h, address + offset, (old & ~clear) | set);
        };
        if (!modify(0x820e40f4, 0x3f, 0x3f | (1u << 17) | (1u << 18)) ||
            !modify(0x820e53c4, 0, 1u << 30) ||
            !modify(0x820e5380, 0, 1u << 30) ||
            !modify(0x820ed004, 0, (1u << 8) | (1u << 9)) ||
            !modify(0x820e7000, 0xfff8u | (1u << 23), 1536u << 3) ||
            !modify(0x820e9008, 0xc3000000, 0x03000000)) return false;
    }
    std::printf("  mac init: ok\n");
    return true;
}

static void PutIe(std::vector<uint8_t>& b, uint8_t id, const std::vector<uint8_t>& body)
{
    b.push_back(id);
    b.push_back((uint8_t)body.size());
    b.insert(b.end(), body.begin(), body.end());
}

static void PutWpsAttr(std::vector<uint8_t>& b, uint16_t type, const std::vector<uint8_t>& v)
{
    b.push_back((uint8_t)(type >> 8));
    b.push_back((uint8_t)type);
    b.push_back((uint8_t)(v.size() >> 8));
    b.push_back((uint8_t)v.size());
    b.insert(b.end(), v.begin(), v.end());
}

// Beacon body from the capability field on (beacon offload "type 1"), modelled on
// drc-hostap's wiiu_ap_pairing.conf: open, channel 36, DTIM 3, WMM, HT, WPS (PIN).
static std::vector<uint8_t> PairingBeacon(const std::string& ssid, uint8_t ch, uint16_t* timPos)
{
    std::vector<uint8_t> b = { 0x01, 0x00 };  // capability: ESS
    PutIe(b, 0, std::vector<uint8_t>(ssid.begin(), ssid.end()));
    PutIe(b, 1, { 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c });  // 6(B) 9 12(B) 18 24(B) 36 48 54
    PutIe(b, 3, { ch });                                              // DS parameter set
    *timPos = (uint16_t)b.size();
    PutIe(b, 5, { 0, 3, 0, 0 });                                      // TIM: DTIM count 0, period 3
    {   // HT capabilities: SM power save disabled, 2 streams MCS 0-15
        std::vector<uint8_t> ht(26, 0);
        ht[0] = 0x0c;
        ht[2] = 0x17;              // A-MPDU: 64k, 8 us spacing
        ht[3] = 0xff;
        ht[4] = 0xff;
        PutIe(b, 45, ht);
    }
    {   // HT operation: primary channel, 20 MHz
        std::vector<uint8_t> op(22, 0);
        op[0] = ch;
        PutIe(b, 61, op);
    }
    {   // WMM parameter element (wmm_ac_* from wiiu_ap_pairing.conf), U-APSD advertised
        std::vector<uint8_t> w = { 0x00, 0x50, 0xf2, 0x02, 0x01, 0x01, 0x80, 0x00 };
        struct { uint8_t aci, aifs, cwmin, cwmax; uint16_t txop; } ac[4] = {
            { 0, 2, 4, 5, 47 }, { 1, 7, 4, 10, 0 }, { 2, 3, 4, 5, 94 }, { 3, 3, 4, 5, 47 } };
        for (auto& a : ac) {
            w.push_back((uint8_t)((a.aci << 5) | a.aifs));
            w.push_back((uint8_t)((a.cwmax << 4) | a.cwmin));
            w.push_back((uint8_t)a.txop);
            w.push_back((uint8_t)(a.txop >> 8));
        }
        PutIe(b, 221, w);
    }
    {   // WPS: configured, PIN registrar active (as hostapd after "wps_pin")
        std::vector<uint8_t> w = { 0x00, 0x50, 0xf2, 0x04 };
        PutWpsAttr(w, 0x104a, { 0x10 });          // Version 1.0
        PutWpsAttr(w, 0x1044, { 0x02 });          // WPS State: configured
        PutWpsAttr(w, 0x1041, { 0x01 });          // Selected Registrar
        PutWpsAttr(w, 0x1012, { 0x00, 0x00 });    // Device Password ID: PIN
        PutWpsAttr(w, 0x1053, { 0x00, 0x84 });    // SR config methods: label, push button
        PutWpsAttr(w, 0x1049, { 0x00, 0x37, 0x2a, 0x00, 0x01, 0x20 });  // WFA ext: Version2 2.0
        PutIe(b, 221, w);
    }
    return b;
}

// Observed from the owner's real console on 2026-10-14. Its beacon has a zero-filled
// SSID field of the real SSID's length (rather than a zero-length field), a Nintendo
// RSN IE, 100 TU interval and DTIM period 3. The saved SSID is never put on air here.
static std::vector<uint8_t> NormalBeacon(size_t ssidLen, uint8_t ch, uint16_t* timPos)
{
    std::vector<uint8_t> b = { 0x11, 0x00 };  // ESS | privacy
    PutIe(b, 0, std::vector<uint8_t>(ssidLen, 0));
    PutIe(b, 1, { 0x8c, 0x12, 0x98, 0x24, 0xb0, 0x48, 0x60, 0x6c });
    *timPos = (uint16_t)b.size();
    PutIe(b, 5, { 1, 3, 0, 0 });  // firmware advances the DTIM count
    PutIe(b, 48, { 1, 0,                         // RSN version
                   0xa4, 0xc0, 0xe1, 4,          // Nintendo group CCMP
                   1, 0, 0xa4, 0xc0, 0xe1, 4,    // one pairwise suite: Nintendo CCMP
                   1, 0, 0xa4, 0xc0, 0xe1, 2,    // one AKM suite: Nintendo PSK
                   8, 0 });                      // RSN capabilities (observed)
    PutIe(b, 45, { 0x1c, 0, 0x19, 0xff, 0xff, 0, 0, 0, 0, 0, 0, 0, 0,
                    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 });
    std::vector<uint8_t> htOp(22, 0);
    htOp[0] = ch;
    htOp[1] = 0x08;
    htOp[2] = 0x04;
    PutIe(b, 61, htOp);
    PutIe(b, 221, { 0x00, 0x10, 0x18, 0x02, 0x01, 0x00, 0x04, 0x00, 0x00 });
    PutIe(b, 221, { 0x00, 0x50, 0xf2, 0x02, 0x01, 0x01, 0x82, 0x00,
                    0x02, 0x54, 0x2f, 0x00, 0x27, 0xa4, 0x00, 0x00,
                    0x43, 0x54, 0x5e, 0x00, 0x63, 0x54, 0x2f, 0x00 });
    return b;
}

// mt7925_mcu_rate_txpower_band for 5 GHz with one flat limit for every rate
// (no regulatory database here; halfDbm is in 0.5 dB units).
static bool SetTxPower5G(Mcu& mcu, uint8_t halfDbm, const std::string& alpha2)
{
    static const uint8_t chans[] = { 36, 38, 40, 42, 44, 46, 48, 50, 52, 54, 56, 58, 60, 62, 64, 100,
                                     102, 104, 106, 108, 110, 112, 114, 116, 118, 120, 122, 124, 126,
                                     128, 132, 134, 136, 138, 140, 142, 144, 149, 151, 153, 155, 157,
                                     159, 161, 165, 167 };
    const size_t HDR = 52, SKU = 1 + 449;  // mt7925_tx_power_limit_tlv, mt7925_sku_tlv
    for (size_t i = 0; i < sizeof(chans); i += 3) {
        size_t n = sizeof(chans) - i < 3 ? sizeof(chans) - i : 3;
        std::vector<uint8_t> p(HDR + n * SKU, 0);
        Put16(p, 4, 1);                          // tag
        Put16(p, 6, (uint16_t)(p.size() - 4));   // len
        p[12] = (uint8_t)n;                      // n_chan
        p[13] = 2;                               // band: 5 GHz
        p[14] = (i + n == sizeof(chans)) ? 1 : 0;  // last_msg
        p[16] = alpha2[0];
        p[17] = alpha2[1];
        for (size_t j = 0; j < n; j++) {
            uint8_t* s = &p[HDR + j * SKU];
            s[0] = chans[i + j];
            std::memset(s + 1, halfDbm, 449);
        }
        std::vector<uint8_t> evt;
        if (!mcu.UniCmd(0x2c, UNI_ACK, p, true, &evt)) {  // MCU_UNI_CMD_SET_POWER_LIMIT
            std::printf("  tx power: FAILED\n");
            return false;
        }
    }
    std::printf("  tx power: %u.%u dBm on all 5 GHz channels\n", halfDbm / 2, (halfDbm & 1) * 5);
    return true;
}

// mt7925_start_ap + link_info_changed(beacon) + set_tx, on bss/omac 0.
static bool StartAp(Mcu& mcu, const uint8_t mac[6], uint8_t ch, const std::string& ssid, bool normal)
{
    std::vector<uint8_t> evt;
    {   // add_interface (AP): device + basic BSS
        std::vector<uint8_t> p(16);
        p[1] = 0xff;
        Put16(p, 4, 0);
        Put16(p, 6, 12);
        p[8] = 1;
        std::memcpy(&p[10], mac, 6);
        if (!UniStatus("add device", mcu.UniCmd(MCU_UNI_CMD_DEV_INFO_UPDATE, UNI_ACK, p, true, &evt), evt, true)) return false;
    }
    {   // second half of mt76_connac_mcu_uni_add_dev: basic BSS, AP type, not yet connected
        std::vector<uint8_t> p(4 + 32);
        Put16(p, 4, 0);
        Put16(p, 6, 32);
        p[8] = 1;
        p[11] = 0xff;
        Put32(p, 12, 0x10002);
        p[16] = 1;
        Put16(p, 24, 19);
        Put16(p, 30, 19);
        if (!UniStatus("add bss", mcu.UniCmd(MCU_UNI_CMD_BSS_INFO_UPDATE, UNI_ACK, p, true, &evt), evt, true)) return false;
    }
    {   // Configure RX for diagnostics. Like MonitorSetup, clear the drop bits so
        // probe/auth frames still reach user mode if firmware MAC filtering has not
        // adopted the cloned BSSID yet. We only observe these frames here.
        std::vector<uint8_t> p(72);
        Put16(p, 4, 0x0c);
        Put16(p, 6, 68);
        Put32(p, 12, (1u << 31) | (1u << 6));
        if (!UniStatus("rx filter", mcu.UniCmd(MCU_UNI_CMD_BAND_CONFIG, UNI_ACK, p, true, &evt), evt, false)) return false;
        std::vector<uint8_t> q(72);
        Put16(q, 4, 0x0c);
        Put16(q, 6, 68);
        q[8] = 1;
        Put32(q, 16, (1u << 4) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) |
                         (1u << 17) | (1u << 18) | (1u << 19));
        q[20] = 2;              // clear
        if (!UniStatus("rx filter bits", mcu.UniCmd(MCU_UNI_CMD_BAND_CONFIG, UNI_ACK, q, true, &evt), evt, false)) return false;
    }
    {   // start_ap: mt7925_mcu_add_bss_info with all TLVs (bss_idx 0)
        std::vector<uint8_t> p = { 0, 0, 0, 0 };
        uint16_t ntlv = 0;
        auto tlv = [&](uint16_t tag, std::vector<uint8_t> body) {
            std::vector<uint8_t> t(4);
            Put16(t, 0, tag);
            Put16(t, 2, (uint16_t)(4 + body.size()));
            p.insert(p.end(), t.begin(), t.end());
            p.insert(p.end(), body.begin(), body.end());
            ntlv++;
        };
        {   // UNI_BSS_INFO_BASIC
            std::vector<uint8_t> v(28, 0);
            v[0] = 1;                   // active
            v[3] = 0xff;                // band_idx
            Put32(v, 4, 0x10002);       // CONNECTION_INFRA_AP
            v[8] = 0;                   // conn_state: connected
            std::memcpy(&v[10], mac, 6);
            Put16(v, 16, 19);           // bmc_tx_wlan_idx
            Put16(v, 18, 100);          // beacon interval
            v[20] = 3;                  // dtim
            v[21] = 0x11;               // phymode A | AN
            Put16(v, 22, 19);           // sta_idx
            Put16(v, 24, 3);            // nonht_basic_phy = PHY_TYPE_OFDM_INDEX
            tlv(0, v);
        }
        tlv(16, normal ? std::vector<uint8_t>{ 7, 6, 4, 0 }           // SEC: WPA2-PSK/CCMP (mt76)
                       : std::vector<uint8_t>{ 0, 1, 0, 0 });          // SEC: open
        {
            std::vector<uint8_t> v(12, 0);                               // RATE
            Put16(v, 2, 0x0540);                                         // OFDM_BASIC_RATE
            v[9] = 18;                                                   // bc_fixed_rate (6M)
            v[10] = 18;                                                  // mc_fixed_rate
            tlv(11, v);
        }
        tlv(15, { 1, 0, 0, 0 });                                         // QBSS: qos
        {
            std::vector<uint8_t> v(16, 0);                               // MLD (not MLD)
            v[0] = 0xff;
            v[1] = 32;
            std::memcpy(&v[2], mac, 6);
            v[8] = 0xff;
            v[9] = 0xff;
            tlv(26, v);
        }
        {
            std::vector<uint8_t> v(16, 0);                               // IFS: slot 9 us
            v[0] = 1;
            Put16(v, 4, 9);
            tlv(23, v);
        }
        tlv(2, { ch, ch, 0, 0, 2, 2, 4, 0, 2, 0, 0, 0 });               // RLM: mt7925 HT-op flag, 20 MHz, 5 GHz
        tlv(6, { 0, 0, 0, 0 });                                          // 11V_MBSSID
        Put16(p, 2, ntlv);
        if (!UniStatus("start ap (bss info)", mcu.UniCmd(MCU_UNI_CMD_BSS_INFO_UPDATE, UNI_ACK, p, true, &evt), evt, true)) return false;
    }
    {   // mt7925_mcu_set_bss_pm(enable): PM off, then beacon timing
        std::vector<uint8_t> p(8);
        Put16(p, 4, 27);  // UNI_BSS_INFO_PM_DISABLE
        Put16(p, 6, 4);
        if (!UniStatus("bss pm off", mcu.UniCmd(MCU_UNI_CMD_BSS_INFO_UPDATE, UNI_ACK, p, true, &evt), evt, true)) return false;
        std::vector<uint8_t> q(16);
        Put16(q, 4, 22);  // UNI_BSS_INFO_BCNFT
        Put16(q, 6, 12);
        Put16(q, 8, 100);
        q[10] = 3;
        if (!UniStatus("beacon timing", mcu.UniCmd(MCU_UNI_CMD_BSS_INFO_UPDATE, UNI_ACK, q, true, &evt), evt, true)) return false;
    }
    {   // mt7925_mcu_sta_update for the broadcast wcid: header translation off
        std::vector<uint8_t> p(8 + 8);
        p[0] = 0;          // bss_idx
        p[1] = 19;         // wlan_idx_lo
        Put16(p, 2, 1);    // tlv_num
        p[4] = 1;          // is_tlv_append
        p[5] = 0xe;        // muar_idx: no station
        Put16(p, 8, 0x2b); // STA_REC_HDR_TRANS
        Put16(p, 10, 8);
        p[12] = 1;         // from_ds (AP)
        p[14] = 1;         // dis_rx_hdr_tran
        if (!UniStatus("bcast station", mcu.UniCmd(MCU_UNI_CMD_STA_REC_UPDATE, UNI_ACK, p, true, &evt), evt, true)) return false;
    }
    {   // mt7925_mcu_uni_add_beacon_offload
        uint16_t tim = 0;
        std::vector<uint8_t> body = normal ? NormalBeacon(ssid.size(), ch, &tim)
                                           : PairingBeacon(ssid, ch, &tim);
        std::vector<uint8_t> p(4 + 524);
        Put16(p, 4, 7);    // UNI_BSS_INFO_BCN_CONTENT
        Put16(p, 6, 524);
        Put16(p, 8, tim);
        p[14] = 1;         // enable
        p[15] = 1;         // type: from capability field
        Put16(p, 16, (uint16_t)body.size());
        std::memcpy(&p[18], body.data(), body.size());
        if (!UniStatus("beacon", mcu.UniCmd(MCU_UNI_CMD_BSS_INFO_UPDATE, UNI_ACK, p, true, &evt), evt, true)) return false;
    }
    {   // mt7925_mcu_set_tx: EDCA per access class (BE, BK, VI, VO)
        std::vector<uint8_t> p = { 0, 0, 4, 0 };
        struct { uint8_t aifs, cwmin, cwmax; uint16_t txop; } ac[4] = {
            { 2, 4, 5, 47 }, { 7, 4, 10, 0 }, { 3, 4, 5, 94 }, { 3, 4, 5, 47 } };
        for (uint8_t i = 0; i < 4; i++) {
            std::vector<uint8_t> e(12, 0);
            Put16(e, 0, 0);
            Put16(e, 2, 12);
            e[4] = i;      // queue (ACI)
            e[5] = 0xf;    // set aifs, cw_min, cw_max, txop
            e[6] = ac[i].cwmin;
            e[7] = ac[i].cwmax;
            Put16(e, 8, ac[i].txop);
            e[10] = ac[i].aifs;
            p.insert(p.end(), e.begin(), e.end());
        }
        if (!UniStatus("edca", mcu.UniCmd(0x04, UNI_ACK, p, true, &evt), evt, false)) return false;
    }
    {   // Re-apply the RX filter last: with it only set before BSS/beacon setup, a
        // 20 s hostapd run on 2026-10-15 got zero RX records in AP mode.
        std::vector<uint8_t> p(72);
        Put16(p, 4, 0x0c);
        Put16(p, 6, 68);
        Put32(p, 12, (1u << 31) | (1u << 6));
        if (!UniStatus("rx filter (final)", mcu.UniCmd(MCU_UNI_CMD_BAND_CONFIG, UNI_ACK, p, true, &evt), evt, false)) return false;
        std::vector<uint8_t> q(72);
        Put16(q, 4, 0x0c);
        Put16(q, 6, 68);
        q[8] = 1;
        Put32(q, 16, (1u << 4) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) |
                         (1u << 17) | (1u << 18) | (1u << 19));
        q[20] = 2;              // clear
        if (!UniStatus("rx filter bits (final)", mcu.UniCmd(MCU_UNI_CMD_BAND_CONFIG, UNI_ACK, q, true, &evt), evt, false)) return false;
    }
    // AP-mode RX needs the sniffer path. Measured 2026-10-15 on channel 149, 20 s each:
    // AP mode alone (and with full power) delivered 0 frames; with sniffer on it
    // delivered ~30/s while beacon TX airtime stayed the same (~700 per 5 s).
    {
        const char* s = "KeepFullPwr 1";
        std::vector<uint8_t> p(8 + 8 + 320);
        Put16(p, 4, 2);                          // UNI_CHIP_CONFIG_CHIP_CFG
        Put16(p, 6, (uint16_t)(p.size() - 4));
        Put16(p, 12, (uint16_t)(std::strlen(s) + 1));
        std::memcpy(&p[16], s, std::strlen(s) + 1);
        if (!mcu.UniCmd(MCU_UNI_CMD_CHIP_CONFIG, 0x06, p, false)) return false;
        std::printf("  full power: sent\n");
    }
    {
        std::vector<uint8_t> p(12);
        Put16(p, 4, 0);   // UNI_SNIFFER_ENABLE
        Put16(p, 6, 8);
        p[8] = 1;
        if (!UniStatus("sniffer rx on", mcu.UniCmd(MCU_UNI_CMD_SNIFFER, UNI_ACK, p, true, &evt), evt, false)) return false;
        if (!SetChannel(mcu, ch, false)) return false;
    }
    return true;
}

struct DmaStopGuard {
    HANDLE h;
    ~DmaStopGuard()
    {
        if (Ioctl(h, IOCTL_ZIIU_DMA_STOP, nullptr, 0, nullptr, 0)) {
            std::printf("DMA stopped; radio no longer broadcasting\n");
        } else {
            PrintLastError("DMA_STOP");
        }
    }
};

static int RunLinkWorker(HANDLE h, unsigned seconds, uint8_t ch, bool checkOnly = false, bool txCheck = false)
{
    HANDLE childHandle = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &childHandle,
                         0, TRUE, DUPLICATE_SAME_ACCESS)) {
        PrintLastError("DuplicateHandle"); return 1;
    }
    std::wstring command = L"py.exe -3 -u \"" + RepoPath(L"tools\\gamepad-link\\connect.py") +
        L"\" --handle " + std::to_wstring((uintptr_t)childHandle) + L" --seconds " +
        std::to_wstring(seconds) + L" --channel " + std::to_wstring(ch);
    if (checkOnly) command += L" --check";
    if (txCheck) command += L" --tx-check";
    STARTUPINFOW startup = {}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(childHandle);
    if (!created) { PrintLastError("Start link worker"); return 1; }
    CloseHandle(process.hThread);
    DWORD wait = WaitForSingleObject(process.hProcess, (seconds + 10) * 1000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    return code == 0 ? 0 : 1;
}

// drc-hostap (tools/hostapd-win) as the connection worker. hostapd answers probes,
// authentication and association and runs the Nintendo WPA2 handshake; its ziiu
// backend moves frames through this process's inherited driver handle. The config
// holds the pairing secrets, so it is written only under .local.
static int RunHostapd(HANDLE h, unsigned seconds, uint8_t ch, const Console& con)
{
    const std::wstring dir = RepoPath(L".local\\gamepad-link");
    const std::wstring exe = RepoPath(L".local\\build\\hostap\\hostapd\\hostapd.exe");
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::fprintf(stderr, "hostapd.exe not built (see tools\\hostapd-win)\n");
        return 2;
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    char bssid[18], peer[18];
    std::snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x", con.bssid[0], con.bssid[1],
                  con.bssid[2], con.bssid[3], con.bssid[4], con.bssid[5]);
    std::snprintf(peer, sizeof(peer), "%02x:%02x:%02x:%02x:%02x:%02x", con.gamepad[0], con.gamepad[1],
                  con.gamepad[2], con.gamepad[3], con.gamepad[4], con.gamepad[5]);
    // tools/hostapd-win mirrors drc-hostap's wiiu_ap_normal.conf (pinned revision).
    std::string conf =
        "interface=ziiu0\ndriver=ziiu\nlogger_stdout=-1\nlogger_stdout_level=0\n"
        "hw_mode=a\nchannel=" + std::to_string(ch) + "\nbeacon_int=100\ndtim_period=3\n"
        "macaddr_acl=0\nauth_algs=3\nwmm_enabled=1\nuapsd_advertisement_enabled=1\n"
        "wmm_ac_be_acm=0\nwmm_ac_be_aifs=2\nwmm_ac_be_cwmin=4\nwmm_ac_be_cwmax=5\nwmm_ac_be_txop_limit=47\n"
        "wmm_ac_bk_acm=0\nwmm_ac_bk_aifs=7\nwmm_ac_bk_cwmin=4\nwmm_ac_bk_cwmax=10\nwmm_ac_bk_txop_limit=0\n"
        "wmm_ac_vi_acm=0\nwmm_ac_vi_aifs=3\nwmm_ac_vi_cwmin=4\nwmm_ac_vi_cwmax=5\nwmm_ac_vi_txop_limit=94\n"
        "wmm_ac_vo_acm=0\nwmm_ac_vo_aifs=3\nwmm_ac_vo_cwmin=4\nwmm_ac_vo_cwmax=5\nwmm_ac_vo_txop_limit=47\n"
        // [GF] and the Broadcom vendor element match the console's own beacon
        // (live-link.pcap); drc-hostap left GF off only for its own driver.
        "ieee80211n=1\nht_capab=[GF]\nvendor_elements=dd09001018020100040000\n"
        "bssid=" + std::string(bssid) + "\nssid=" + con.ssid + "\nignore_broadcast_ssid=2\n"
        "wpa=2\nwpa_psk=" + con.psk + "\nwpa_key_mgmt=WPA-PSK\nwpa_pairwise=CCMP\n";
    const std::wstring confPath = dir + L"\\hostapd.conf";
    FILE* f = nullptr;
    if (_wfopen_s(&f, confPath.c_str(), L"wb") != 0 || !f) {
        std::fprintf(stderr, "cannot write hostapd.conf\n");
        return 2;
    }
    std::fwrite(conf.data(), 1, conf.size(), f);
    std::fclose(f);

    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), nullptr, TRUE };
    HANDLE log = CreateFileW((dir + L"\\hostapd.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) { PrintLastError("hostapd.log"); return 2; }
    HANDLE childHandle = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &childHandle, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        PrintLastError("DuplicateHandle"); CloseHandle(log); return 1;
    }
    SetEnvironmentVariableW(L"ZIIU_HANDLE", std::to_wstring((uintptr_t)childHandle).c_str());
    SetEnvironmentVariableA("ZIIU_PEER", peer);
    SetEnvironmentVariableW(L"ZIIU_LOG_DIR", dir.c_str());
    // -dd: debug log; never -K (that would print key material).
    std::wstring command = L"\"" + exe + L"\" -dd -t \"" + confPath + L"\"";
    STARTUPINFOW startup = {}; startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION process = {};
    BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(childHandle);
    CloseHandle(log);
    SetEnvironmentVariableW(L"ZIIU_HANDLE", nullptr);
    if (!created) { PrintLastError("Start hostapd"); return 1; }
    CloseHandle(process.hThread);
    streamStopEvent = CreateEventW(nullptr, TRUE, FALSE, kStreamStopEvent);
    if (!streamStopEvent) {
        PrintLastError("Create stream stop event");
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hProcess);
        return 1;
    }
    ResetEvent(streamStopEvent);
    SetConsoleCtrlHandler(StreamConsoleStop, TRUE);
    if (seconds)
        std::printf("hostapd running on channel %u for %u s (log: .local\\gamepad-link\\hostapd.log)\n", ch, seconds);
    else
        std::printf("hostapd running on channel %u until stream-stop or Ctrl+C (no time limit)\n", ch);
    int rc = 0;
    HANDLE waits[2] = {process.hProcess, streamStopEvent};
    const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, seconds ? seconds * 1000 : INFINITE);
    if (wait == WAIT_OBJECT_0) {
        DWORD code = 1; GetExitCodeProcess(process.hProcess, &code);
        std::printf("hostapd exited early (code %lu); see hostapd.log\n", code);
        rc = 1;
    } else {
        TerminateProcess(process.hProcess, 0);
        WaitForSingleObject(process.hProcess, 5000);
        std::printf("%s; hostapd stopped\n", wait == WAIT_TIMEOUT ? "time limit reached" : "stop requested");
        if (wait == WAIT_FAILED) rc = 1;
    }
    SetConsoleCtrlHandler(StreamConsoleStop, FALSE);
    CloseHandle(streamStopEvent);
    streamStopEvent = nullptr;
    CloseHandle(process.hProcess);
    return rc;
}

static int RunAp(HANDLE h, const std::wstring& dir, unsigned seconds, uint8_t ch, bool normal, bool connect = false, bool txCheck = false, bool hostapd = false)
{
    if (connect) {
        ZIIU_INFO info = {};
        if (!Ioctl(h, IOCTL_ZIIU_GET_INFO, nullptr, 0, &info, sizeof(info)) || info.InterfaceVersion != ZIIU_WIFI_INTERFACE_VERSION) {
            std::fprintf(stderr, "ap-connect needs driver interface %d; no radio setup attempted\n", ZIIU_WIFI_INTERFACE_VERSION);
            return 2;
        }
    }
    Console con = {};
    if (normal && !LoadConsole(RepoPath(L".local\\wiiu\\ziiu-probe.txt"), &con)) {
        return 2;
    }
    uint8_t mac[6] = {};
    int rc = FwLoad(h, dir, mac);
    if (rc != 0) {
        return rc;
    }
    DmaStopGuard stop = { h };
    std::vector<uint8_t> ram;
    if (!ReadFile(dir + L"\\WIFI_RAM_CODE_MT7925_1_1.bin", ram)) {
        return 2;
    }
    const std::string country = Country();
    Mcu mcu(h);
    std::vector<uint8_t> evt;
    std::printf("access point setup...\n");
    {   // same base init as monitor mode
        std::vector<uint8_t> p(12);
        Put16(p, 4, 2);
        Put16(p, 6, 8);
        p[9] = 1;
        if (!UniStatus("eeprom mode", mcu.UniCmd(MCU_UNI_CMD_EFUSE_CTRL, UNI_ACK, p, true, &evt), evt, false)) return 1;
    }
    // mt7925_mcu_apply_regd: CLC, then channel domain, then TX power, all for one country
    if (!SetClc(mcu, ram, country)) {
        return 1;
    }
    {
        static const uint8_t ch5[] = { 36, 40, 44, 48, 149, 153, 157, 161, 165 };
        std::vector<uint8_t> p(16 + sizeof(ch5) * 8);
        p[0] = country[0]; p[1] = country[1];
        p[5] = 3; p[6] = 3;
        Put16(p, 8, 2);
        Put16(p, 10, (uint16_t)(8 + sizeof(ch5) * 8));
        p[13] = sizeof(ch5);
        for (size_t i = 0; i < sizeof(ch5); i++) Put16(p, 16 + i * 8, ch5[i]);
        if (!UniStatus("channel domain", mcu.UniCmd(MCU_UNI_CMD_SET_DOMAIN_INFO, UNI_ACK, p, true, &evt), evt, false)) return 1;
    }
    // Keep both diagnostic AP modes at low power.
    if (!MacInit(h) || !SetTxPower5G(mcu, 6, country)) {  // 3 dBm
        return 1;
    }

    const uint8_t* apMac = mac;
    std::string ssid;
    if (normal) {
        apMac = con.bssid;
        ssid = con.ssid;
        std::memcpy(g_gamepad, con.gamepad, 6);
        g_haveGamepad = true;
    } else {
        // Pairing SSID: "WiiU" + mac2 + mac (truncated to 32 characters) + "_STA1"
        uint8_t mac2[6];
        std::memcpy(mac2, mac, 6);
        mac2[5]++;
        char hex[32];
        std::snprintf(hex, sizeof(hex), "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x", mac2[0], mac2[1], mac2[2],
                      mac2[3], mac2[4], mac2[5], mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        ssid = "WiiU" + std::string(hex, 23) + "_STA1";
    }

    if (!StartAp(mcu, apMac, ch, ssid, normal)) {
        std::printf("access point setup FAILED\n");
        return 1;
    }
    std::memcpy(g_self, apMac, 6);
    g_haveSelf = true;
    if (connect && hostapd) {
        return RunHostapd(h, seconds, ch, con); // DmaStopGuard stops the radio afterwards
    }
    if (connect) {
        std::printf("Starting bounded GamePad connection worker on channel %u for %u s\n", ch, seconds);
        return RunLinkWorker(h, seconds, ch, false, txCheck); // DmaStopGuard also runs if the worker fails
    }
    if (normal) {
        std::printf("advertising the saved console as a hidden WPA2 AP on channel %u for %u s\n"
                    "(diagnostic only: association and WPA2 handshake are not implemented)\n", ch, seconds);
    } else {
        std::printf("broadcasting '%s' on channel %u for %u s (>> = frame to us or asking for WiiU)...\n",
                    ssid.c_str(), ch, seconds);
    }
    // Report TX airtime (MT_MIB_SDR36, band 0) every 5 s: beacons should show up there.
    ULONGLONG lastReport = GetTickCount64();
    ListenLoop(h, seconds, [&]() {
        if (GetTickCount64() - lastReport >= 5000) {
            ULONG tx = 0, rx = 0;
            ReadReg(h, 0x820ed054, &tx);
            ReadReg(h, 0x820ed058, &rx);  // MT_MIB_SDR37: RX airtime
            std::printf("  [airtime counters: tx 0x%06lx rx 0x%06lx]\n", tx & 0xffffff, rx & 0xffffff);
            lastReport = GetTickCount64();
        }
    });
    return 0;
}

// Receive only: where does the Wi-Fi clock (TSF) count? mt792x_get_tsf sets
// LPON_TCR(0, omac) to software-read mode and reads LPON UTTR0/1; in our AP sessions
// that always gave 0. Dumps the LPON block (0x820eb000-0x820eb3ff) twice per setup
// stage, prints the words that changed, and tries the mt792x read for omac 0-3.
static void LponDump(HANDLE h, const char* stage)
{
    const ULONG base = 0x820eb000, n = 0x400 / 4;
    std::vector<ULONG> a(n), b(n);
    ULONGLONG t0 = GetTickCount64();
    for (ULONG i = 0; i < n; i++) ReadReg(h, base + i * 4, &a[i]);
    ULONGLONG t1 = GetTickCount64();
    for (ULONG i = 0; i < n; i++) ReadReg(h, base + i * 4, &b[i]);
    std::printf("[%s] LPON words that changed between two dumps (%llu ms apart):\n", stage,
                (unsigned long long)(t1 - t0));
    int changed = 0;
    for (ULONG i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            std::printf("  0x%08lx: 0x%08lx -> 0x%08lx (%+ld)\n", base + i * 4, a[i], b[i], (long)(b[i] - a[i]));
            changed++;
        }
    }
    if (!changed) std::printf("  none\n");
    // Two layouts: mt792x (TCR 0xa8 + 4n, UTTR 0x080/0x084) and connac3 / mt7996
    // (TCR 0xa8 + 0x40n, UTTR 0x360/0x364). Live counters seen at 0xa0 + 0x40n.
    struct Layout { const char* name; ULONG stride, uttr; } layouts[] = {
        { "mt792x ", 4, 0x080 }, { "connac3", 0x40, 0x360 } };
    for (const Layout& L : layouts) {
        for (ULONG omac = 0; omac < 4; omac++) {
            ULONG tcr = 0, lo = 0, hi = 0, lo2 = 0, hi2 = 0, live = 0, liveHi = 0;
            ULONG tcrAddr = base + 0xa8 + omac * L.stride;
            ReadReg(h, tcrAddr, &tcr);
            WriteReg(h, tcrAddr, (tcr & ~3u) | 3);
            ReadReg(h, base + L.uttr, &lo);
            ReadReg(h, base + L.uttr + 4, &hi);
            ReadReg(h, base + 0xa0 + omac * 0x40, &live);
            ReadReg(h, base + 0xa4 + omac * 0x40, &liveHi);
            Sleep(50);
            WriteReg(h, tcrAddr, (tcr & ~3u) | 3);
            ReadReg(h, base + L.uttr, &lo2);
            ReadReg(h, base + L.uttr + 4, &hi2);
            unsigned long long t = ((unsigned long long)hi << 32) | lo, t2 = ((unsigned long long)hi2 << 32) | lo2;
            std::printf("[%s] %s read, omac %lu: tcr=0x%08lx tsf=%llu then %llu (%+lld); live 0x%03lx=%lu hi=%lu\n",
                        stage, L.name, omac, tcr, t, t2, (long long)(t2 - t), 0xa0 + omac * 0x40, live, liveHi);
        }
    }
}

static int TsfProbe(HANDLE h, const std::wstring& dir)
{
    uint8_t mac[6] = {};
    int rc = FwLoad(h, dir, mac);
    if (rc != 0) return rc;
    LponDump(h, "firmware");
    Mcu mcu(h);
    if (!MacInit(h) || !MonitorSetup(mcu, mac) || !SetChannel(mcu, 161, false)) {
        std::printf("monitor setup FAILED\n");
        return 1;
    }
    LponDump(h, "monitor ch161");
    return 0;
}

// Receive only: never transmits. With "hop", stays on the first channel a WiiU... network
// is heard on, so a console's pairing and the GamePad's replies are captured in full.
// With "scan", visits every 5 GHz channel once for `seconds` each and reports traffic.
static int Sniff(HANDLE h, const std::wstring& dir, const std::string& chArg, unsigned seconds)
{
    // "wiiu": the GamePad link is 5 GHz only and the console radio is silent while idle,
    // so cover just its channels with a tight dwell to catch a brief reconnect burst.
    static const uint8_t wiiuHopCh[] = { 149, 153, 157, 161, 165 };
    static const uint8_t fullHop[] = { 36, 40, 44, 48, 149, 153, 157, 161, 165 };
    static const uint8_t all[] = { 36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128,
                                   132, 136, 140, 144, 149, 153, 157, 161, 165 };
    std::vector<uint8_t> wide;
    for (int c = 32; c <= 177; c++) wide.push_back((uint8_t)c);
    bool scanWide = chArg == "scan-wide";
    bool scanning = chArg == "scan" || scanWide;
    // "wiiu-lock": like "wiiu", but once the console is heard it never hops again, so a
    // GamePad power cycle (silent for ~10 s) still has its handshake captured.
    bool lockForever = chArg == "wiiu-lock";
    bool wiiuHop = chArg == "wiiu" || lockForever;
    bool hopping = chArg == "hop" || wiiuHop;
    // wiiu-lock sweeps both non-DFS bands: the console picks its channel at boot
    // (seen 153/157/161, but a 149-165-only sweep once missed it entirely).
    bool upperOnly = wiiuHop && !lockForever;
    const uint8_t* hop = upperOnly ? wiiuHopCh : fullHop;
    size_t hopLen = upperOnly ? sizeof(wiiuHopCh) : sizeof(fullHop);
    // Dwell must exceed the RX_READ wait (200 ms) or the radio hops before a read
    // completes and receives nothing. 300 ms gives at least one full read per channel.
    unsigned hopMs = wiiuHop ? 300 : 250;
    uint8_t ch = hopping || scanning ? hop[0] : (uint8_t)std::atoi(chArg.c_str());
    if (!hopping && !scanning && (ch == 0 || ch > 177)) {
        std::fprintf(stderr, "bad channel '%s'\n", chArg.c_str());
        return 2;
    }

    uint8_t mac[6] = {};
    int rc = FwLoad(h, dir, mac);
    if (rc != 0) {
        return rc;
    }
    // If ZiiU Probe has been run, mark the owner's GamePad frames (>>) so the real
    // console <-> GamePad link stands out in the capture.
    Console con = {};
    if (LoadConsole(RepoPath(L".local\\wiiu\\ziiu-probe.txt"), &con, true)) {
        std::memcpy(g_gamepad, con.gamepad, 6);
        g_haveGamepad = true;
        std::memcpy(g_console, con.bssid, 6);
        g_haveConsole = true;
        std::printf("tracking GamePad %02x:%02x:%02x:%02x:%02x:%02x (paired to console %02x:%02x:%02x:%02x:%02x:%02x)\n",
                    con.gamepad[0], con.gamepad[1], con.gamepad[2], con.gamepad[3], con.gamepad[4], con.gamepad[5],
                    con.bssid[0], con.bssid[1], con.bssid[2], con.bssid[3], con.bssid[4], con.bssid[5]);
    }
    Mcu mcu(h);
    std::printf("monitor mode setup...\n");
    if (!MacInit(h) || !MonitorSetup(mcu, mac) || !SetChannel(mcu, ch, false)) {
        std::printf("monitor setup FAILED\n");
        return 1;
    }
    if (scanning) {
        unsigned total = 0;
        std::vector<uint8_t> list = scanWide ? wide : std::vector<uint8_t>(all, all + sizeof(all));
        for (uint8_t c : list) {
            if (!SetChannel(mcu, c, true)) {
                std::printf("channel %u: FAILED\n", c);
                continue;
            }
            std::printf("--- channel %u\n", c);
            unsigned n = ListenLoop(h, seconds, nullptr);
            total += n;
        }
        return total > 0 ? 0 : 1;
    }
    std::printf("listening %u s %s (>> = Wii U network name)...\n", seconds,
                wiiuHop ? "hopping 149-165 (Wii U 5 GHz)" : hopping ? "hopping 36-48,149-165" : "");

    unsigned hopIdx = 0;
    ULONGLONG lastHop = GetTickCount64();
    // Follow the console's runtime channel switches whether we started by hopping
    // or pinned to a channel; a pinned start that then goes silent falls back to hopping.
    bool following = hopping || g_haveConsole;
    if (!hopping) {
        g_wiiuCh = ch;
    }
    unsigned frames = ListenLoop(h, seconds, [&]() {
        if (hopping && g_wiiuCh) {
            hopping = false;
            std::printf("  Wii U radio heard on channel %u: staying there\n", g_wiiuCh);
            SetChannel(mcu, g_wiiuCh, false);
        }
        if (following && !hopping && g_csaCh) {
            std::printf("  console announced a switch %u -> %u: following\n", g_wiiuCh, g_csaCh);
            g_wiiuCh = g_csaCh;
            g_csaCh = 0;
            g_consoleSeen = GetTickCount64();
            SetChannel(mcu, g_wiiuCh, false);
        }
        if (following && !hopping && !lockForever && g_haveConsole && g_consoleSeen &&
            GetTickCount64() - g_consoleSeen > 1500) {
            std::printf("  link silent 1.5 s on channel %u: hopping again\n", g_wiiuCh);
            g_wiiuCh = 0;
            g_consoleSeen = 0;
            hopping = true;
            lastHop = 0;
        }
        if (hopping && GetTickCount64() - lastHop >= hopMs) {
            hopIdx = (hopIdx + 1) % hopLen;
            SetChannel(mcu, hop[hopIdx], true);
            lastHop = GetTickCount64();
        }
    });
    return frames > 0 ? 0 : 1;
}

int main(int argc, char** argv)
{
    // Readiness/errors must appear immediately when launched through a hidden
    // elevated helper with redirected output.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::string cmd = argc > 1 ? argv[1] : "info";

    // Signal the running owner's process; no device handle or hardware access.
    if (cmd == "stream-stop") {
        HANDLE stop = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStreamStopEvent);
        if (!stop) { std::printf("No controllable stream is running.\n"); return 1; }
        const BOOL ok = SetEvent(stop);
        CloseHandle(stop);
        std::printf("Stream stop requested.\n");
        return ok ? 0 : 1;
    }

    HANDLE h = OpenDevice();
    if (h == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "ZiiUScreen driver not found or access denied.\n"
                             "Is the card in GamePad mode (tools\\wifi-mode.ps1 GamePad) and is this an admin prompt?\n");
        return 2;
    }

    int rc = 0;
    if (cmd == "info") {
        ZIIU_INFO info = {};
        DWORD got = 0;
        if (!Ioctl(h, IOCTL_ZIIU_GET_INFO, nullptr, 0, &info, sizeof(info), &got) || got != sizeof(info)) {
            PrintLastError("GET_INFO");
            rc = 1;
        } else {
            std::printf("interface version : %lu\n", info.InterfaceVersion);
            std::printf("BAR0              : %s, 0x%lx bytes\n", info.BarMapped ? "mapped" : "NOT mapped", info.BarLength);
            std::printf("crash latch       : %s\n", OpName(info.LatchedOp));
            std::printf("DMA               : %s\n", info.DmaActive ? "running" : "stopped");
            if (info.PciCommand) {
                std::printf("PCI command       : 0x%04lx (bus master %s)\n", info.PciCommand,
                            (info.PciCommand & 4) ? "on" : "off");
            }
            std::printf("events skipped    : %lu\n", info.EventsSkipped);
            std::printf("rx frames/dropped : %lu / %lu\n", info.RxFrames, info.RxDropped);
            if (info.LatchedOp) {
                std::printf("  The PC crashed during '%s'. Hardware operations are disabled until\n"
                            "  you run 'ziiuctl clear-latch' (after checking C:\\Windows\\Minidump).\n",
                            OpName(info.LatchedOp));
            }
        }
    } else if (cmd == "chipid") {
        ZIIU_CHIP_ID id = {};
        DWORD got = 0;
        if (!Ioctl(h, IOCTL_ZIIU_READ_CHIP_ID, nullptr, 0, &id, sizeof(id), &got) || got != sizeof(id)) {
            PrintLastError("READ_CHIP_ID");
            rc = 1;
        } else if (id.Status != 0) {
            std::printf("handshake failed: NTSTATUS 0x%08lx\n", id.Status);
            rc = 1;
        } else {
            std::printf("chip id 0x%04lx, hw rev 0x%02lx%s\n", id.ChipId, id.HwRev & 0xff,
                        id.ChipId == 0x7925 ? "  (MT7925 confirmed)" : "  (UNEXPECTED)");
            rc = id.ChipId == 0x7925 ? 0 : 1;
        }
    } else if (cmd == "clear-latch") {
        if (!Ioctl(h, IOCTL_ZIIU_CLEAR_LATCH, nullptr, 0, nullptr, 0)) {
            PrintLastError("CLEAR_LATCH");
            rc = 1;
        } else {
            std::printf("crash latch cleared\n");
        }
    } else if (cmd == "fw-load") {
        std::wstring dir = DefaultFirmwareDir();
        if (argc > 2) {
            int n = MultiByteToWideChar(CP_ACP, 0, argv[2], -1, nullptr, 0);
            dir.assign(n > 0 ? n - 1 : 0, L'\0');
            MultiByteToWideChar(CP_ACP, 0, argv[2], -1, dir.data(), n);
        }
        rc = FwLoad(h, dir);
    } else if (cmd == "sniff" && argc > 2) {
        unsigned seconds = argc > 3 ? (unsigned)std::atoi(argv[3]) : 20;
        if (argc > 4 && !(g_pcap = PcapOpen(argv[4]))) {
            std::fprintf(stderr, "can't create %s\n", argv[4]);
            rc = 2;
        } else {
            rc = Sniff(h, DefaultFirmwareDir(), argv[2], seconds ? seconds : 20);
        }
        if (g_pcap) {
            std::fclose(g_pcap);
            std::printf("capture written to %s\n", argv[4]);
        }
    } else if (cmd == "link-check") {
        rc = RunLinkWorker(h, 1, 157, true);
    } else if (cmd == "ap-pair") {
        // The pairing network is open and visible to everyone nearby: keep it short.
        unsigned seconds = argc > 2 ? (unsigned)std::atoi(argv[2]) : 60;
        if (seconds > 60) {
            seconds = 60;
        }
        int ch = argc > 3 ? std::atoi(argv[3]) : 36;
        bool chOk = ch == 36 || ch == 40 || ch == 44 || ch == 48;  // UNII-1: no radar detection needed
        if (!chOk) {
            std::fprintf(stderr, "ap-pair only uses channels 36, 40, 44, 48\n");
            rc = 2;
        } else {
            rc = RunAp(h, DefaultFirmwareDir(), seconds ? seconds : 60, (uint8_t)ch, false);
        }
    } else if (cmd == "ap-normal" || cmd == "ap-connect" || cmd == "ap-tx-check" || cmd == "ap-hostapd") {
        int ch = argc > 2 ? std::atoi(argv[2]) : 0;
        bool chOk = ch == 36 || ch == 40 || ch == 44 || ch == 48 || ch == 149 ||
                    ch == 153 || ch == 157 || ch == 161 || ch == 165;
        if (!chOk) {
            std::fprintf(stderr, "ap-normal needs a non-DFS 5 GHz channel (36/40/44/48/149/153/157/161/165)\n");
            rc = 2;
        } else {
            unsigned seconds = argc > 3 ? (unsigned)std::atoi(argv[3]) : 30;
            // Explicit zero is continuous only for the normal streaming host.
            // Keep pairing and other diagnostic commands bounded as before.
            if (cmd != "ap-hostapd" && (!seconds || seconds > 60)) seconds = 60;
            if (cmd == "ap-hostapd" && seconds > 86400) {
                std::fprintf(stderr, "ap-hostapd duration must be 0..86400 seconds\n");
                CloseHandle(h);
                return 2;
            }
            rc = RunAp(h, DefaultFirmwareDir(), seconds, (uint8_t)ch, true, cmd != "ap-normal", cmd == "ap-tx-check",
                       cmd == "ap-hostapd");
        }
    } else if (cmd == "tsf-probe") {
        rc = TsfProbe(h, DefaultFirmwareDir());
    } else if (cmd == "dma-stop") {
        if (!Ioctl(h, IOCTL_ZIIU_DMA_STOP, nullptr, 0, nullptr, 0)) {
            PrintLastError("DMA_STOP");
            rc = 1;
        } else {
            std::printf("DMA stopped, Wi-Fi subsystem reset\n");
        }
    } else if (cmd == "reg" && argc > 2) {
        ULONG v = 0;
        ULONG addr = std::strtoul(argv[2], nullptr, 16);
        if (!PollReg(h, addr, 0, 0, 0, &v)) {
            rc = 1;
        } else {
            std::printf("0x%08lx = 0x%08lx\n", addr, v);
        }
    } else {
        std::fprintf(stderr, "usage: ziiuctl [info|chipid|clear-latch|fw-load [dir]|sniff <channel|hop> [seconds]|"
                             "ap-pair [seconds] [channel]|ap-normal <channel> [seconds]|"
                             "link-check|ap-connect <channel> [seconds]|ap-hostapd <channel> [seconds, 0=continuous]|stream-stop|ap-tx-check <channel> [seconds]|"
                             "dma-stop|reg <hexaddr>]\n");
        rc = 2;
    }
    CloseHandle(h);
    return rc;
}

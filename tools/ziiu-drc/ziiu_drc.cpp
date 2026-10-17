// ziiu-drc: the console side of the GamePad protocol, for Windows.
//
// Port of the parts of libdrc (Copyright (c) 2013, Mema Hacking, 2-clause BSD,
// see docs/REFERENCES.md) that a console runs: H.264 video (DRH-mode x264),
// audio stream, command protocol (UIC config, UVC/UAC state), resync messages
// and the 180 Hz input report. It talks to the GamePad through the bridge in
// tools/hostapd-win (ziiu_net.c) on 127.0.0.1:
//
//   GamePad -> console   50010 msg, 50022 input, 50023 cmd replies   (we bind)
//   console -> GamePad   50120 video, 50121 audio, 50123 cmd queries (we send)
//   bridge  -> us        50190 Wi-Fi TSF samples ("ZTSF", tsf, qpc, qpf)
//   local   -> us        50200 rumble control (ZIIU_RUMBLE[_TEST]; bound on request)
//   us      -> local     50222 touch calibration "ZCAL" for tools/ziiu-input, which owns
//                        the 50022 input port when ZIIU_INPUT_EXTERNAL=1
//
// It streams a test pattern, or with ZIIU_CAPTURE set a Windows display (see
// desktop_capture.h), and logs input/commands.
//   ziiu-drc [seconds] [x264 preset]       (defaults: 60, "slow" as libdrc; 0 = continuous)
//   ZIIU_CAPTURE=YOUR_DISPLAY_ID or \\.\DISPLAY3   display to stream (monitor hardware ID
//                                          substring, or GDI device name)
//   ZIIU_CAPTURE_DUMP=frames.yuv            write captured I420 frames (864x480) from frame
//                                          120 on, ZIIU_CAPTURE_DUMP_FRAMES of them (default 1)
//   ZIIU_DRC_DELAY_US=0..16684             release a frame this long after its timestamp
//                                          (or when encoded, if later); unset = next tick
//   ZIIU_AUDIO=default | <name part> | off PC sound to the GamePad speakers (WASAPI
//                                          loopback of that output device; audio_capture.h)
//   ZIIU_INPUT_EXTERNAL=1                  leave the input port to tools/ziiu-input
//   ZIIU_SENSOR_BAR=1                      GamePad IR sensor bar on at start; toggle with
//                                          "ZIR1"/"ZIR0"/"ZIRT" on 50200 (tools/ziiu-drc/ir.ps1)
//   ZIIU_RUMBLE=1                          bind control port 50200 (1 byte level,
//                                          or level + u16 LE duration ms; level 0 = stop)
//   ZIIU_RUMBLE_TEST=1                     also drive the motor from held GamePad buttons
//                                          (A full, B half, Y quarter) for a manual A/B test

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <x264.h>
}

#include "audio_capture.h"
#include "desktop_capture.h"

namespace {

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

const int kWidth = 864, kHeight = 480;          // libdrc screen.h
const int kVisibleWidth = 854;                  // the GamePad shows 854 of the 864 columns
const int kChunksPerFrame = 5;                  // 6 macroblock rows per chunk
const size_t kMaxVstrmPayload = 1400;
const double kFrameUs = 1000000.0 / 59.94;
// Audio as the owner's Wii U sends it (decrypted capture, 2026-10-15): 416 stereo
// samples (1664 bytes) per packet, timestamps 8667 us apart. libdrc used 384.
const int kAudioSamples = 416;

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

int EnvInt(const char* name, int def) {
    const char* v = std::getenv(name);
    return v && *v ? std::atoi(v) : def;
}

u64 QpcNow() { LARGE_INTEGER v; QueryPerformanceCounter(&v); return (u64)v.QuadPart; }
u64 QpcFreq() { LARGE_INTEGER v; QueryPerformanceFrequency(&v); return (u64)v.QuadPart; }

u8 InsertBits(u8 v, u8 ins, int b, int e) {
    u8 mask = (u8)(((1 << e) - 1) ^ ((1 << b) - 1));
    return (u8)((v & ~mask) | ((ins << b) & mask));
}

// Offline checks use a separate localhost port range, so they cannot feed the
// running radio bridge or take the active stream's sockets.
u16 LocalPort(u16 port) {
    return port ? (u16)(port + EnvInt("ZIIU_TEST_PORT_OFFSET", 0)) : 0;
}

SOCKET LocalSocket(u16 port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    BOOL off = FALSE;
    DWORD got = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &got, nullptr, nullptr);
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    sockaddr_in sin = {};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sin.sin_port = htons(LocalPort(port));
    if (bind(s, (sockaddr*)&sin, sizeof(sin)) != 0) {
        std::fprintf(stderr, "cannot bind 127.0.0.1:%u (%d)\n", port, WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

// --- Clock: Wi-Fi TSF (microseconds) from the bridge, extrapolated with QPC ---
// Read by the audio thread while the main loop takes samples: guarded by mu.
struct Clock {
    mutable std::mutex mu;
    bool have = false;
    u64 tsf0 = 0, qpc0 = 0, qpf = QpcFreq();
    u64 tsf_first = 0, qpc_first = 0;          // for the rate check in the report
    unsigned long samples = 0, zero = 0;
    void Sample(const u8* m) {
        u64 tsf, qpc, f;
        std::memcpy(&tsf, m + 4, 8);
        std::memcpy(&qpc, m + 12, 8);
        std::memcpy(&f, m + 20, 8);
        if (!f) return;
        if (samples++ < 3) std::printf("TSF from bridge: %llu us\n", (unsigned long long)tsf);
        if (!tsf) { zero++; return; }          // read before the BSS runs (or failed)
        std::lock_guard<std::mutex> lk(mu);
        if (!tsf_first) { tsf_first = tsf; qpc_first = qpc; }
        tsf0 = tsf; qpc0 = qpc; qpf = f; have = true;
    }
    // TSF microseconds per QPC microsecond since the first sample (should be ~1).
    double Rate() const {
        if (!have || qpc0 == qpc_first) return 0;
        return (double)(tsf0 - tsf_first) / ((double)(qpc0 - qpc_first) * 1e6 / (double)qpf);
    }
    u32 Now() const {
        std::lock_guard<std::mutex> lk(mu);
        u64 q = QpcNow();
        if (!have) return (u32)(q * 1000000 / qpf);
        return (u32)(tsf0 + (u64)((double)(q - qpc0) * 1e6 / (double)qpf));
    }
    // Stamp for a given QPC time (from cf811ae). Scheduled ticks can precede the most
    // recent bridge sample.
    u32 At(u64 q) const {
        std::lock_guard<std::mutex> lk(mu);
        if (!have) return (u32)((q / qpf) * 1000000 + (q % qpf) * 1000000 / qpf);
        const double delta = q >= qpc0 ? (double)(q - qpc0) : -(double)(qpc0 - q);
        return (u32)tsf0 + (u32)(int64_t)(delta * 1e6 / (double)qpf);
    }
};

// --- Command protocol (libdrc cmd-packet / cmd-protocol) ---
enum CmdType { kQuery = 0, kQueryAck = 1, kReply = 2, kReplyAck = 3 };
enum CmdQuery { kGeneric = 0, kUvcUac = 1, kTime = 2 };

struct CmdInFlight {
    u16 query_type;
    std::vector<u8> payload;
    int state;          // packet type we expect next
    int retries;
    u64 deadline_qpc;
    std::string what;
};

std::vector<u8> CmdPacket(int type, int qtype, u16 seq, const std::vector<u8>& payload) {
    std::vector<u8> p(8 + payload.size());
    p[0] = (u8)type; p[1] = 0;
    p[2] = (u8)qtype; p[3] = 0;
    p[4] = (u8)payload.size(); p[5] = (u8)(payload.size() >> 8);
    p[6] = (u8)seq; p[7] = (u8)(seq >> 8);
    std::copy(payload.begin(), payload.end(), p.begin() + 8);
    return p;
}

std::vector<u8> GenericCmd(u16 tid, u8 service, u8 method, const std::vector<u8>& body) {
    std::vector<u8> p(12 + body.size(), 0);
    p[0] = 0x7e; p[1] = 0x01;          // magic
    p[2] = (u8)(tid >> 4);             // transaction id: the Wii U increments it
    p[3] = (u8)(((tid & 0xf) << 4) | 0x08);   // ... and final fragment
    p[5] = 0x40;                       // query flag
    p[6] = service; p[7] = method;
    p[10] = (u8)(body.size() >> 8); p[11] = (u8)body.size();
    std::copy(body.begin(), body.end(), p.begin() + 12);
    return p;
}

// UVC/UAC state exactly as the owner's Wii U sends it every second (constant in a
// 100 s decrypted capture; libdrc sent zeros and a big-endian mic rate).
std::vector<u8> UvcUacState() {
    static const u8 real[48] = {
        0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x00, 0x19, 0x00, 0x00, 0x80, 0x3e, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x02, 0x9e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00,
        0x40, 0x40, 0x03, 0x00, 0x2d, 0x00, 0x00, 0x01, 0x80, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00};
    return std::vector<u8>(real, real + sizeof(real));
}

// Time command (query type 2), the Wii U's first query after connecting: Modified
// Julian Day and seconds since local midnight, both u32 LE (capture: 61328 and
// 15119 = 2026-10-15 04:11:59 local).
std::vector<u8> TimePayload() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    int a = (14 - st.wMonth) / 12, y = st.wYear + 4800 - a, m = st.wMonth + 12 * a - 3;
    u32 jdn = st.wDay + (153 * m + 2) / 5 + 365 * y + y / 4 - y / 100 + y / 400 - 32045;
    u32 mjd = jdn - 2400001, secs = st.wHour * 3600u + st.wMinute * 60u + st.wSecond;
    std::vector<u8> p(8);
    std::memcpy(p.data(), &mjd, 4);
    std::memcpy(p.data() + 4, &secs, 4);
    return p;
}

// Sent by the Wii U every ~1.03 s from port 50025 to 50125, one way: a command-style
// header (query, type 0, size 88, own sequence) and a constant 88-byte body.
const u8 kStatus50125[88] = {
    0x5a, 0xa5, 0x58, 0x00, 0x00, 0x03, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x10, 0x00, 0x10, 0x00, 0x18, 0x00, 0x20, 0x00, 0x28, 0x00,
    0x30, 0x00, 0x38, 0x00, 0x40, 0x00, 0x48, 0x00, 0x50, 0x00, 0x58, 0x00, 0x60, 0x00, 0x68, 0x00,
    0x70, 0x00, 0x78, 0x00, 0x80, 0x00, 0x88, 0x00, 0x90, 0x00, 0x98, 0x00, 0xa0, 0x00, 0xa8, 0x00,
    0xb0, 0x00, 0xb8, 0x00, 0xc0, 0x00, 0xc8, 0x00, 0xd0, 0x00, 0xd8, 0x00, 0xe0, 0x00, 0xe8, 0x00,
    0xeb, 0x00, 0xeb, 0x00, 0xeb, 0x00, 0x00, 0x00};

// --- H.264 (libdrc h264-encoder with drc-x264's DRH mode) ---
struct Encoder {
    x264_t* enc = nullptr;
    std::array<std::vector<u8>, kChunksPerFrame> chunks;
    // Where x264 reported each chunk. Copied only after x264_encoder_encode
    // returns: when a chunk is reported its last CABAC bytes may still be
    // outstanding (carry), as libdrc relies on by keeping pointers.
    std::array<const u8*, kChunksPerFrame> at{};
    std::array<int, kChunksPerFrame> size{};
    int got = 0;

    static void OnNal(x264_t*, x264_nal_t* nal, void* opaque) {
        Encoder* self = (Encoder*)opaque;
        if (nal->i_type == NAL_SEI) return;
        const int mb_per_chunk = (kWidth / 16) * (kHeight / 16) / kChunksPerFrame;
        int idx = nal->i_first_mb / mb_per_chunk;
        if (idx < 0 || idx >= kChunksPerFrame) return;
        self->at[idx] = nal->p_payload;
        self->size[idx] = nal->i_payload;
        self->got++;
    }

    bool Open(const char* preset) {
        x264_param_t p;
        if (x264_param_default_preset(&p, preset, "zerolatency") < 0) return false;
        p.i_width = kWidth;
        p.i_height = kHeight;
        p.analyse.inter &= ~X264_ANALYSE_PSUB16x16;
        // Motion vectors within what the real Wii U sends (mv_stats.py on the owner's
        // capture: max 70 px across, 55 px down). x264 otherwise searched to 193x128 px
        // when windows were dragged, and the GamePad left those blocks smeared.
        // i_mv_range clamps the search on both axes (CLIP_FMV in analyse.c).
        p.analyse.i_mv_range = EnvInt("ZIIU_MV_RANGE", 48);
        // The owner's Wii U (decrypted capture, mb_stats.py): no intra-refresh sweep, an
        // IDR only when the GamePad asks (4 in 3000 frames), and per-macroblock QP from 1
        // to 43. libdrc used a sweeping intra refresh and a flat QP 32, which shows as a
        // moving column and blocking on desktop content. ZIIU_INTRA_REFRESH=1 and
        // ZIIU_CRF=0 bring libdrc's behaviour back for A/B runs.
        const bool intra_refresh = EnvInt("ZIIU_INTRA_REFRESH", 0) != 0;
        const int crf = EnvInt("ZIIU_CRF", 12);   // A/B offline: 44.7 dB scrolling vs 36.6 at CRF 20
        // Frame size cap: the Wii U's IDRs are 45-46 KB (capture), so allow about that
        // for one frame (360 kbit buffer) at up to 20 Mbit/s sustained.
        const int vbv_kbps = EnvInt("ZIIU_VBV_KBPS", 20000);
        const int vbv_buf = EnvInt("ZIIU_VBV_BUF_KBIT", 360);
        p.i_fps_num = 60000;                 // without this x264 assumes 25 fps for rate control
        p.i_fps_den = 1001;
        p.b_vfr_input = 0;
        p.i_keyint_min = intra_refresh ? 10 : X264_KEYINT_MAX_INFINITE;
        p.i_keyint_max = intra_refresh ? 30 : X264_KEYINT_MAX_INFINITE;
        p.i_scenecut_threshold = -1;
        p.i_csp = X264_CSP_I420;
        p.b_cabac = 1;
        p.b_interlaced = 0;
        p.i_bframe = 0;
        p.i_bframe_pyramid = 0;
        p.i_frame_reference = 1;
        p.b_constrained_intra = 1;
        p.b_intra_refresh = intra_refresh;
        p.analyse.i_weighted_pred = 0;
        p.analyse.b_weighted_bipred = 0;
        p.analyse.b_transform_8x8 = 0;
        p.analyse.i_chroma_qp_offset = 0;
        if (crf > 0) {
            // drc-x264 writes no slice header and encodes every macroblock's QP as a
            // delta from the GamePad's implicit 32, so any rate control works. The VBV
            // keeps each frame within about two frame times of airtime.
            p.rc.i_rc_method = X264_RC_CRF;
            p.rc.f_rf_constant = (float)crf;
            p.rc.i_qp_min = 1;
            p.rc.i_qp_max = 51;
            p.rc.i_vbv_max_bitrate = vbv_kbps;
            p.rc.i_vbv_buffer_size = vbv_buf;
        } else {
            p.rc.i_rc_method = X264_RC_CQP;  // libdrc: QP 32 everywhere
            p.rc.i_qp_constant = p.rc.i_qp_min = p.rc.i_qp_max = 32;
            p.rc.f_ip_factor = 1.0;
        }
        std::printf("x264: %s, intra refresh %s\n",
                    crf > 0 ? ("CRF " + std::to_string(crf) + ", VBV " + std::to_string(vbv_kbps) + " kbit/s, buffer " +
                               std::to_string(vbv_buf) + " kbit").c_str()
                            : "CQP 32 (libdrc)",
                    intra_refresh ? "on (libdrc)" : "off (Wii U)");
        p.b_repeat_headers = 0;              // the GamePad has its own SPS/PPS
        p.b_aud = 0;
        p.b_drh_mode = 1;                    // macroblock-row chunks, no slice header
        p.i_threads = 1;
        p.b_sliced_threads = 0;
        p.i_slice_count = 1;
        p.nalu_process = OnNal;
        p.i_log_level = X264_LOG_WARNING;
        x264_param_apply_profile(&p, "main");
        enc = x264_encoder_open(&p);
        // The GamePad's own PPS has chroma_qp_index_offset 0, but x264 lowers its offset
        // by 1-2 when psy-rd/psy-trellis are on (encoder.c), so colour was encoded at a
        // different QP than the GamePad decodes with: chroma stuck at ~32.5 dB on a
        // PowerShell-blue screen while luma reached 52 dB. Cancel x264's adjustment.
        if (enc) {
            x264_param_t got;
            x264_encoder_parameters(enc, &got);
            if (got.analyse.i_chroma_qp_offset != 0) {
                x264_encoder_close(enc);
                p.analyse.i_chroma_qp_offset = -got.analyse.i_chroma_qp_offset;
                enc = x264_encoder_open(&p);
                if (enc) x264_encoder_parameters(enc, &got);
            }
            std::printf("x264: chroma QP offset %d (GamePad PPS: 0)\n", enc ? got.analyse.i_chroma_qp_offset : -99);
            if (enc && got.analyse.i_chroma_qp_offset != 0) return false;
        }
        return enc && x264_encoder_maximum_delayed_frames(enc) == 0;
    }

    bool Encode(u8* yuv, bool idr) {
        x264_picture_t in, out;
        x264_picture_init(&in);
        in.opaque = this;
        in.img.i_csp = X264_CSP_I420;
        in.img.i_plane = 3;
        in.img.i_stride[0] = kWidth;
        in.img.i_stride[1] = in.img.i_stride[2] = kWidth / 2;
        in.img.plane[0] = yuv;
        in.img.plane[1] = yuv + kWidth * kHeight;
        in.img.plane[2] = in.img.plane[1] + kWidth * kHeight / 4;
        in.i_type = idr ? X264_TYPE_IDR : X264_TYPE_P;
        for (auto& c : chunks) c.clear();
        at.fill(nullptr);
        size.fill(0);
        got = 0;
        x264_nal_t* nals;
        int n;
        x264_encoder_encode(enc, &nals, &n, &in, &out);
        if (got != kChunksPerFrame) return false;
        for (int i = 0; i < kChunksPerFrame; i++) {
            if (!at[i] || size[i] <= 0) return false;
            chunks[i].assign(at[i], at[i] + size[i]);
        }
        return true;
    }
};

// --- Test pattern: colour bars, a moving square and a grey ramp ---
void DrawPattern(std::vector<u8>& f, int frame) {
    static const u8 bars[8][3] = {   // Y, U, V (BT.601 limited) white..black
        {235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34},
        {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}};
    u8* Y = f.data();
    u8* U = Y + kWidth * kHeight;
    u8* V = U + kWidth * kHeight / 4;
    int sq = (frame * 4) % (kWidth - 64);
    for (int y = 0; y < kHeight; y++) {
        for (int x = 0; x < kWidth; x++) {
            u8 yy, uu, vv;
            if (y < 320) {
                const u8* b = bars[x * 8 / kWidth];
                yy = b[0]; uu = b[1]; vv = b[2];
            } else {
                yy = (u8)(16 + x * 219 / kWidth); uu = vv = 128;
            }
            if (y >= 360 && y < 424 && x >= sq && x < sq + 64) { yy = 235; uu = vv = 128; }
            Y[y * kWidth + x] = yy;
            if (!(x & 1) && !(y & 1)) {
                U[(y / 2) * (kWidth / 2) + x / 2] = uu;
                V[(y / 2) * (kWidth / 2) + x / 2] = vv;
            }
        }
    }
}

struct App {
    SOCKET msg = INVALID_SOCKET, hid = INVALID_SOCKET, cmd = INVALID_SOCKET,
           tsf = INVALID_SOCKET, out = INVALID_SOCKET, rctl = INVALID_SOCKET;
    Clock clock;
    Encoder enc;
    std::vector<u8> frame = std::vector<u8>(kWidth * kHeight * 3 / 2);
    u16 vseq = 0, aseq = 0, cmd_seq = 0;
    bool vinit = false, resync = true;
    // Resync requests that arrive while an IDR is still on its way are stale: the
    // GamePad asks once per frame until one arrives (3 requests, 1 IDR from the real
    // Wii U). Ignore them for kIdrGuardFrames after an IDR is encoded.
    static const unsigned long kIdrGuardFrames = 3;
    unsigned long idr_guard_until = 0, resyncs_ignored = 0;
    // Storm backoff. In a resync storm the GamePad asks every frame, so the 3-frame
    // guard sent a ~40 KB IDR every 3 frames. That alone filled the radio path (about
    // 1550 packets/s through the bridge: busy waits 2500-4300 per 5 s, live 2026-10-16
    // and range-freeze), every frame went out late and was rejected again, and the storm
    // never ended until the GamePad was power-cycled. Now a request right after the
    // guard ends doubles the guard (3, 6, 12, 24, 30 frames), so the load drops and the
    // next IDR can arrive in time; an isolated request still gets an IDR after 3 frames.
    // ZIIU_IDR_BACKOFF=0 restores the fixed guard; ZIIU_IDR_GUARD_MAX caps it (frames).
    bool idr_backoff = true;
    unsigned long idr_guard_max = 30, idr_guard = kIdrGuardFrames, last_idr_frame = 0;
    unsigned long idr_guard_peak = 0;   // for the 5 s report
    std::map<u16, CmdInFlight> inflight;
    struct QueuedCmd { u16 qtype; std::vector<u8> payload; std::string what; };
    std::deque<QueuedCmd> cmd_queue;
    u16 generic_tid = 0x5b, status_seq = 0;
    u64 qpf = QpcFreq();
    unsigned long frames = 0, vpackets = 0, hid_packets = 0, enc_fail = 0, resyncs = 0;
    std::atomic<unsigned long> apackets{0};
    SOCKET aout = INVALID_SOCKET;           // audio thread's own send socket
    std::atomic<bool> audio_stop{false};
    double enc_ms_total = 0;
    u32 last_buttons = 0xffffffff;
    // Rumble: the GamePad's motor is a single GPIO toggled by the astrm header's
    // "vibrate" bit (libdrc sc-astrm; GPIO4 in re/gpio.rst). It carries no amplitude,
    // so we PWM the bit across audio packets (~115/s) to approximate intensity.
    // Off by default: rumble_level 0 leaves the audio stream byte-identical to before.
    bool rumble_enabled = false;   // bind the control port / arm the feature
    bool rumble_test = false;      // hold A/B/Y on the GamePad to drive the motor
    bool input_external = false;   // ZIIU_INPUT_EXTERNAL: ziiu-input owns port 50022
    // Touch calibration from the UIC config (libdrc device-config: 0x244/0x153/0x1d3
    // in the 0x300 blob, CRC16-checked). Only these 8 numbers leave this process; the
    // blob also holds the Wi-Fi key. Sent to ziiu-input on 50222 every 2 s.
    bool have_cal = false;
    unsigned uvc_count = 0;
    int16_t cal[8] = {};
    LoopbackAudio audio;
    unsigned long skipped_ticks = 0, idr_enc_n = 0, idr_enc_late = 0;
    std::atomic<unsigned long> audio_packets{0};
    std::atomic<int> audio_peak{0};         // max |sample| since the last report (32767 = full scale)
    double idr_enc_max = 0, p_enc_max = 0;
    std::atomic<int> rumble_level{0};   // current target intensity 0..255 (set here, read by audio thread)
    u32 rumble_acc = 0;            // PWM accumulator over audio packets
    std::atomic<u64> rumble_until{0};   // QPC deadline for a timed request (0 = until changed)
    std::atomic<unsigned long> rumble_on_packets{0};
    unsigned long rumble_cmds = 0;
    DesktopCapture cap;
    bool capturing = false;
    const char* dump_path = nullptr;
    unsigned long changed_frames = 0;
    u64 bytes_sum = 0;
    size_t bytes_max = 0, idr_max = 0;

    void Send(u16 port, const u8* d, size_t n) {
        sockaddr_in to = {};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to.sin_port = htons(LocalPort(port));
        sendto(out, (const char*)d, (int)n, 0, (sockaddr*)&to, sizeof(to));
    }

    // Commands
    // The Wii U runs one query at a time (query, ack, reply, reply-ack) in a 40 ms
    // burst after connecting; queued here and sent in that order.
    void Query(u16 qtype, const std::vector<u8>& payload, const std::string& what) {
        cmd_queue.push_back({qtype, payload, what});
        PumpCmds();
    }
    void Generic(u8 service, u8 method, const std::vector<u8>& body, const std::string& what) {
        Query(kGeneric, GenericCmd(generic_tid++ & 0xfff, service, method, body), what);
    }
    void PumpCmds() {
        if (!inflight.empty() || cmd_queue.empty()) return;
        QueuedCmd q = cmd_queue.front();
        cmd_queue.pop_front();
        StartQuery(q.qtype, q.payload, q.what);
    }
    void StartQuery(u16 qtype, const std::vector<u8>& payload, const std::string& what) {
        u16 seq = cmd_seq++;
        CmdInFlight& c = inflight[seq];
        c.query_type = qtype;
        c.payload = payload;
        c.retries = 0;
        c.what = what;
        SendQuery(seq, c);
    }
    void SendQuery(u16 seq, CmdInFlight& c) {
        auto p = CmdPacket(kQuery, c.query_type, seq, c.payload);
        c.state = kQueryAck;
        c.deadline_qpc = QpcNow() + qpf;     // 1 s, up to 10 tries (libdrc)
        Send(50123, p.data(), p.size());
    }
    void CmdTimeouts() {
        u64 now = QpcNow();
        for (auto it = inflight.begin(); it != inflight.end();) {
            if (now < it->second.deadline_qpc) { ++it; continue; }
            if (++it->second.retries >= 10) {
                std::printf("cmd %s: no reply after 10 tries\n", it->second.what.c_str());
                it = inflight.erase(it);
                PumpCmds();
            } else {
                SendQuery(it->first, it->second);
                ++it;
            }
        }
    }
    static u16 Crc16(const u8* p, size_t n) {          // libdrc device-config CRC16
        u16 crc = 0xffff;
        for (size_t i = 0; i < n; i++) {
            crc ^= p[i];
            for (int j = 0; j < 8; j++) crc = (crc & 1) ? (u16)((crc >> 1) ^ 0x8408) : (u16)(crc >> 1);
        }
        return crc;
    }
    void ParseCalibration(const u8* blob) {
        for (size_t off : {0x244, 0x153, 0x1d3}) {
            if (Crc16(blob + off, 16) != (u16)(blob[off + 16] | (blob[off + 17] << 8))) continue;
            for (int i = 0; i < 8; i++) cal[i] = (int16_t)(blob[off + 2 * i] | (blob[off + 2 * i + 1] << 8));
            have_cal = true;
            std::printf("touch: calibration ref (%d,%d) (%d,%d) raw (%d,%d) (%d,%d)\n", cal[0], cal[1], cal[2],
                        cal[3], cal[4], cal[5], cal[6], cal[7]);
            return;
        }
        std::printf("touch: no valid calibration in UIC config, ziiu-input uses libdrc defaults\n");
    }
    void SendCalibration() {
        if (!have_cal) return;
        u8 m[4 + 16];
        std::memcpy(m, "ZCAL", 4);
        std::memcpy(m + 4, cal, 16);
        Send(50222, m, sizeof(m));
    }

    void OnCmd(const u8* d, size_t n) {
        if (n < 8) return;
        int type = d[0] | (d[1] << 8), qtype = d[2] | (d[3] << 8);
        size_t len = d[4] | (d[5] << 8);
        u16 seq = (u16)(d[6] | (d[7] << 8));
        if (8 + len > n) len = n - 8;
        if (type == kReply) {
            auto ack = CmdPacket(kReplyAck, qtype, seq, {});
            Send(50123, ack.data(), ack.size());
        }
        auto it = inflight.find(seq);
        if (it == inflight.end() || it->second.state != type) {
            if (type == kQuery)
                std::printf("cmd: GamePad sent a query (type %d, %zu bytes), not handled\n", qtype, len);
            return;
        }
        if (type == kQueryAck) {
            it->second.state = kReply;
            it->second.deadline_qpc = QpcNow() + qpf;
            return;
        }
        if (type == kReply) {
            if (it->second.what == "UIC config (5.6)" && len == 0x310) ParseCalibration(d + 8 + 0x10);
            static std::map<std::string, int> logged;
            if (logged[it->second.what]++ < 2) {
                std::printf("cmd %s: reply %zu bytes:", it->second.what.c_str(), len);
                for (size_t i = 0; i < std::min<size_t>(len, 32); i++) std::printf(" %02x", d[8 + i]);
                std::printf("\n");
            }
            inflight.erase(it);
            PumpCmds();
        }
    }

    // libdrc's VideoStreamer sends frame N's packets at tick N+1, one frame period
    // after its timestamp, and the GamePad depends on it: sent as soon as encoded,
    // ~2 of 3 frames were dropped with a resync request (283-589 per 15 s); delayed,
    // 0 (live A/B, 2026-10-15). ZIIU_DRC_DELAY=0 sends immediately, =ab alternates
    // every 15 s.
    bool delay_send = true;
    int variant_lock = 1;
    std::vector<std::pair<u16, std::vector<u8>>> held;
    unsigned long resyncs_at_switch = 0;
    // ZIIU_DRC_DELAY_US=n releases a held frame n us after its timestamp, or as soon
    // as it is encoded if that is later, instead of at the next tick (~16.7 ms). The
    // owner judged 4000 us smooth on the earlier `fast` profile (2026-10-15).
    int delay_us = -1;            // -1: legacy next-tick release
    int capture_offset_us = 0;    // opt-in: capture later within the same wire slot
    u64 held_deadline = 0;
    u64 held_captured_at = 0;
    double capture_to_send_sum = 0, capture_to_send_max = 0;
    unsigned long sent_frames = 0;
    // Stamp to send: how long after its timestamp a frame leaves (the GamePad drops
    // frames that arrive too soon after their stamp).
    bool stamp_after_grab = false;
    u64 held_stamp_qpc = 0;
    double stamp_to_send_sum = 0, stamp_to_send_min = 1e9;
    unsigned long stamp_early = 0;   // sent < 12 ms after the stamp
    void Out(u16 port, const u8* d, size_t n) {
        if (delay_send) held.emplace_back(port, std::vector<u8>(d, d + n));
        else Send(port, d, n);
    }
    void FlushHeld() {
        if (!held.empty() && held_captured_at) {
            const double ms = (double)(QpcNow() - held_captured_at) * 1000.0 / qpf;
            capture_to_send_sum += ms;
            capture_to_send_max = std::max(capture_to_send_max, ms);
            sent_frames++;
            const double st = (double)(int64_t)(QpcNow() - held_stamp_qpc) * 1000.0 / qpf;
            stamp_to_send_sum += st;
            stamp_to_send_min = std::min(stamp_to_send_min, st);
            if (st < 12.0) stamp_early++;
        }
        for (auto& h : held) Send(h.first, h.second.data(), h.second.size());
        held.clear();
        held_deadline = 0;
        held_captured_at = 0;
    }

    // Video: libdrc GenerateVstrmPackets + GenerateAstrmPacket (video format)
    void SendFrame(bool idr, u32 ts) {
        // Byte for byte as the owner's Wii U sends it (libdrc: header ts 0x00100000,
        // size 24, 16000 x4): header ts 0x00010000, size field 0, then the frame's
        // timestamp and 16000, 16000, 21000, 21000.
        u8 ap[8 + 24] = {};
        ap[0] = InsertBits(0, 1, 2, 3);       // astrm type 1: video format
        u32 fmt_ts = 0x00010000;
        std::memcpy(ap + 4, &fmt_ts, 4);
        std::memcpy(ap + 8, &ts, 4);
        const u32 rates[4] = {16000, 16000, 21000, 21000};
        std::memcpy(ap + 12, rates, sizeof(rates));
        Out(50121, ap, sizeof(ap));

        bool init = !vinit;
        vinit = true;
        for (int c = 0; c < kChunksPerFrame; c++) {
            const std::vector<u8>& ch = enc.chunks[c];
            size_t off = 0;
            bool first_packet = true;
            do {
                size_t sz = std::min(kMaxVstrmPayload, ch.size() - off);
                bool last_packet = off + sz == ch.size();
                u8 p[16 + kMaxVstrmPayload] = {};
                u16 seq = vseq;
                vseq = (u16)((vseq + 1) % 1024);
                p[0] = (u8)(0xf0 | (seq >> 8));
                p[1] = (u8)seq;
                p[2] = (u8)(0x08 | (init << 7) | ((c == 0 && first_packet) << 6) |
                            (last_packet << 5) | ((c == kChunksPerFrame - 1 && last_packet) << 4) |
                            ((sz >> 8) & 7));
                p[3] = (u8)sz;
                p[4] = (u8)(ts >> 24); p[5] = (u8)(ts >> 16); p[6] = (u8)(ts >> 8); p[7] = (u8)ts;
                int o = 8;                     // option order as the Wii U sends them
                if (idr) p[o++] = 0x80;        // IDR
                p[o++] = 0x82; p[o++] = 0;     // 59.94 Hz
                p[o++] = 0x83;                 // force decoding
                p[o++] = 0x85; p[o++] = 6;     // 6 macroblock rows per chunk
                std::memcpy(p + 16, ch.data() + off, sz);
                Out(50120, p, 16 + sz);
                vpackets++;
                off += sz;
                first_packet = false;
            } while (off < ch.size());
        }
    }

    // Resync handling as the owner's Wii U does it (decrypted capture, all 3 IDRs):
    // after the GamePad's request it sends no other frame until the IDR (IDR stamp 2-3
    // slots after the previous frame), and the slot after every IDR stays empty (next
    // stamp +33.4 ms). Before, one PC hiccup (fullscreen switch, closing a big window)
    // set off a resync storm that lasted minutes (2026-10-15).
    // A first attempt (reverted) dropped the held frame but left a gap in the vstrm
    // sequence numbers, which looks like packet loss to the GamePad, and stormed
    // nonstop. Dropped packets now give their sequence numbers back.
    // Live switch: "ZRS1" / "ZRS0" on the control port; ZIIU_RESYNC=0 at start.
    bool wiiu_resync = false, held_idr = false, skip_after_idr = false;
    unsigned long dropped_for_idr = 0, slots_after_idr = 0;
    void DropHeld() {
        int n = 0;
        for (auto& h : held) if (h.first == 50120) n++;
        vseq = (u16)((vseq + 1024 - n) % 1024);
        vpackets -= (unsigned long)n;
        held.clear();
        held_deadline = 0;
        held_captured_at = 0;
    }

    void VideoTick(u64 scheduled_qpc) {
        if (resync && frames < idr_guard_until) {
            resync = false;
            resyncs_ignored++;
        }
        if (wiiu_resync && resync && !held.empty() && !held_idr) {
            DropHeld();                       // nothing between the request and the IDR
            dropped_for_idr++;
        }
        FlushHeld();
        if (wiiu_resync && skip_after_idr) {  // the slot after an IDR stays empty
            skip_after_idr = false;
            slots_after_idr++;
            return;
        }
        skip_after_idr = false;
        u64 t0 = QpcNow();
        if (capturing) {
            if (cap.Grab(frame.data())) changed_frames++;
            const unsigned long dump_n = (unsigned long)EnvInt("ZIIU_CAPTURE_DUMP_FRAMES", 1);
            if (dump_path && frames >= 120 && frames < 120 + dump_n) {
                if (FILE* f = std::fopen(dump_path, frames == 120 ? "wb" : "ab")) {
                    std::fwrite(frame.data(), 1, frame.size(), f);
                    std::fclose(f);
                    if (frames == 120 + dump_n - 1) std::printf("capture: wrote %s (%lu frames)\n", dump_path, dump_n);
                }
            }
        } else {
            DrawPattern(frame, (int)frames);
        }
        bool idr = resync;
        if (idr) {
            // Asked again within 6 frames of the last guard ending: that IDR didn't take.
            const bool again = frames > 0 && last_idr_frame && frames <= last_idr_frame + idr_guard + 6;
            idr_guard = idr_backoff && again ? std::min(idr_guard * 2, idr_guard_max) : kIdrGuardFrames;
            idr_guard_peak = std::max(idr_guard_peak, idr_guard);
            idr_guard_until = frames + idr_guard;
            last_idr_frame = frames;
        }
        // Deadline mode stamps the nominal tick, as the version the owner judged smooth
        // did (cf811ae); stamping the actual capture moment while releasing early put
        // the GamePad in a resync storm ("could not connect", 2026-10-15).
        // Legacy mode now also stamps the scheduled tick, before the grab, as libdrc stamps
        // before latching and encoding: the frame then goes out a full period after its
        // stamp even when this tick started late.
        // Stamping after the grab shortened that to 6-12 ms whenever a game loaded the
        // GPU (scale+readback 4-11 ms instead of 0.2), and frames sent ~5 ms after their
        // stamp are dropped with a resync request (live A/B, 2026-10-15): endless storms
        // while gaming (2026-10-16). ZIIU_STAMP_AFTER_GRAB=1 restores the old stamp.
        const u64 ts_qpc = delay_us >= 0 || !stamp_after_grab ? scheduled_qpc : QpcNow();
        held_stamp_qpc = ts_qpc;
        u32 ts = clock.At(ts_qpc);
        if (!enc.Encode(frame.data(), idr)) { enc_fail++; return; }
        const double enc_ms = (double)(QpcNow() - t0) * 1000.0 / (double)qpf;
        enc_ms_total += (double)(QpcNow() - t0) * 1000.0 / (double)qpf;
        // Resync storms with no encode failures: is a slow IDR encode making the IDR
        // late? Track encode time per frame type (reported every 5 s).
        if (idr) { idr_enc_max = std::max(idr_enc_max, enc_ms); idr_enc_n++; if (enc_ms > kFrameUs / 1000) idr_enc_late++; }
        else p_enc_max = std::max(p_enc_max, enc_ms);
        resync = false;
        size_t bytes = 0;
        for (auto& c : enc.chunks) bytes += c.size();
        bytes_sum += bytes;
        bytes_max = std::max(bytes_max, bytes);
        if (idr) idr_max = std::max(idr_max, bytes);
        SendFrame(idr, ts);
        held_captured_at = t0;
        held_idr = idr;
        if (idr) skip_after_idr = true;
        if (delay_send && delay_us >= 0) {
            held_deadline = ts_qpc + (u64)delay_us * qpf / 1000000;
            if (QpcNow() >= held_deadline) FlushHeld();   // encode took longer: send now
        }
        frames++;
    }

    // Audio runs on its own thread (as in libdrc's AudioStreamer): from the main loop,
    // packets waited behind 3-10 ms video encodes and carried their late send time,
    // and the GamePad played PC audio choppy (owner, 2026-10-15). The real Wii U
    // stamps packets exactly 8667 us apart, so each packet carries its nominal slot.
    void AudioTick(u64 slot_qpc) {
        u8 p[8 + kAudioSamples * 4] = {};    // PCM 48 kHz stereo 16-bit; silence if no PC audio
        if (audio.Pop((int16_t*)(p + 8), kAudioSamples)) {
            audio_packets++;
            int peak = 0;                     // loudest sample, so the log tells sound from silence
            for (int i = 0; i < kAudioSamples * 2; i++) peak = std::max(peak, std::abs((int)((int16_t*)(p + 8))[i]));
            if (peak > audio_peak) audio_peak = peak;
        }
        p[0] = (u8)(InsertBits(0, 1, 5, 8) | (aseq >> 8));
        p[1] = (u8)aseq;
        aseq = (u16)((aseq + 1) & 0x3ff);
        p[2] = (kAudioSamples * 4) >> 8; p[3] = (kAudioSamples * 4) & 0xff;
        u32 ts = clock.At(slot_qpc);
        std::memcpy(p + 4, &ts, 4);
        // Vibrate bit (bit 3 of byte 0): PWM the single-bit motor. A timed request
        // expires here so a dropped "stop" can never leave the motor stuck on.
        int level = rumble_level;
        if (level && rumble_until && QpcNow() >= rumble_until) rumble_level = level = 0;
        if (level) {
            rumble_acc += (u32)level;
            if (rumble_acc >= 255) {
                rumble_acc -= 255;
                p[0] = InsertBits(p[0], 1, 3, 4);
                rumble_on_packets++;
            }
        } else {
            rumble_acc = 0;
        }
        sockaddr_in to = {};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to.sin_port = htons(LocalPort(50121));
        sendto(aout, (const char*)p, (int)sizeof(p), 0, (sockaddr*)&to, sizeof(to));
        apackets++;
    }
    void AudioLoop() {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);   // above x264's workers
        const u64 start = QpcNow();
        u64 k = 0;
        while (!audio_stop) {
            const u64 slot = start + (u64)((double)k * (double)qpf * kAudioSamples / 48000.0);
            const u64 now = QpcNow();
            if (now < slot) {
                if (slot - now > qpf / 500) Sleep(1);             // > 2 ms away
                continue;                                          // then spin to the slot
            }
            if (now > slot + qpf / 10) {                           // stalled > 100 ms: restart the grid
                k = (u64)((double)(now - start) * 48000.0 / kAudioSamples / (double)qpf);
                continue;
            }
            AudioTick(slot);
            k++;
        }
    }

    void OnHid(const u8* d, size_t n) {
        if (n != 128) return;
        hid_packets++;
        u32 buttons = ((u32)d[80] << 16) | (d[2] << 8) | d[3];
        if (buttons != last_buttons) {
            std::printf("input: buttons=%06x battery=%u power=%02x\n", buttons, d[5], d[4]);
            last_buttons = buttons;
        }
        // Loopback A/B test: hold a face button on the GamePad to drive its own motor.
        // A = full, B = half, Y = quarter, so a run also shows PWM intensity by feel.
        if (rumble_test) {
            u8 lvl = (buttons & 0x8000) ? 255 : (buttons & 0x4000) ? 128 :
                     (buttons & 0x1000) ? 64 : 0;
            if (lvl != rumble_level) {
                rumble_level = lvl;
                rumble_until = 0;
            }
        }
    }

    // Sensor bar: generic command service 5, method 0x01, payload 01/00 switches the
    // GamePad's IR LEDs via a GPIO (libdrc sc-cmd.rst, GPIO5 in gpio.rst), e.g. as the
    // sensor bar for a Wii Remote in Dolphin. Re-sent every 10 s while on, so it comes
    // back after the GamePad reconnects.
    bool sensor_bar = false;
    void SetSensorBar(bool on) {
        sensor_bar = on;
        Generic(5, 0x01, {(u8)(on ? 1 : 0)}, on ? "sensor bar on" : "sensor bar off");
        std::printf("sensor bar: %s\n", on ? "on" : "off");
    }

    // Control port (127.0.0.1:50200): 1 byte level, or level + u16 LE duration (ms).
    // The hook the virtual-controller feeder will drive from a game's force feedback.
    // Also takes "ZIR1" / "ZIR0" / "ZIRT": GamePad sensor bar (IR LEDs) on, off, toggle.
    void OnRumbleCtl(const u8* d, size_t n) {
        if (n < 1) return;
        if (n == 4 && !std::memcmp(d, "ZIR", 3)) {
            SetSensorBar(d[3] == 'T' ? !sensor_bar : d[3] == '1');
            return;
        }
        if (n == 4 && !std::memcmp(d, "ZRS", 3)) {
            wiiu_resync = d[3] == '1';
            std::printf("resync handling: %s\n", wiiu_resync ? "wiiu (drop + empty slot)" : "legacy");
            return;
        }
        rumble_cmds++;
        rumble_level = d[0];
        u16 dur = n >= 3 ? (u16)(d[1] | (d[2] << 8)) : 0;
        rumble_until = (rumble_level && dur) ? QpcNow() + (u64)dur * qpf / 1000 : 0;
        std::printf("rumble: level=%d duration=%ums\n", rumble_level.load(), dur);
    }

    void Drain(SOCKET s, void (App::*fn)(const u8*, size_t)) {
        u8 b[2048];
        for (;;) {
            int n = recv(s, (char*)b, sizeof(b), 0);
            if (n < 0) return;
            (this->*fn)(b, (size_t)n);
        }
    }
    void OnMsg(const u8* d, size_t n) {
        if (n == 4 && !std::memcmp(d, "\1\0\0\0", 4)) {
            resync = true;                    // GamePad asks for an IDR frame
            if (resyncs++ < 5) std::printf("msg: resync requested\n");
        } else {
            std::printf("msg: %zu bytes\n", n);
        }
    }
    void OnTsf(const u8* d, size_t n) {
        if (n == 28 && !std::memcmp(d, "ZTSF", 4)) clock.Sample(d);
    }

    int Run(unsigned seconds, const char* preset) {
        if (EnvInt("ZIIU_TEST_PORT_OFFSET", 0) < -49000 || EnvInt("ZIIU_TEST_PORT_OFFSET", 0) > 10000)
            return 1;
        // With ZIIU_INPUT_EXTERNAL=1 tools/ziiu-input owns the input port, so button
        // presses reach Windows without waiting behind a video encode in this loop.
        input_external = EnvInt("ZIIU_INPUT_EXTERNAL", 0) != 0;
        msg = LocalSocket(50010);
        if (!input_external) hid = LocalSocket(50022);
        cmd = LocalSocket(50023);
        tsf = LocalSocket(50190);
        out = LocalSocket(0);
        if (msg == INVALID_SOCKET || (!input_external && hid == INVALID_SOCKET) || cmd == INVALID_SOCKET ||
            tsf == INVALID_SOCKET || out == INVALID_SOCKET)
            return 1;
        rumble_test = EnvInt("ZIIU_RUMBLE_TEST", 0) != 0 && !input_external;
        rumble_enabled = rumble_test || EnvInt("ZIIU_RUMBLE", 0) != 0;
        if (const char* v = std::getenv("ZIIU_DRC_DELAY_US")) {
            char* end = nullptr;
            const long us = std::strtol(v, &end, 10);
            if (!*v || *end || us < 0 || us > 16684) {
                std::fprintf(stderr, "ZIIU_DRC_DELAY_US must be 0..16684\n");
                return 1;
            }
            delay_us = (int)us;
        }
        capture_offset_us = EnvInt("ZIIU_CAPTURE_OFFSET_US", 0);
        if (capture_offset_us < 0 || (capture_offset_us &&
            (delay_us < 0 || capture_offset_us >= delay_us))) {
            std::fprintf(stderr, "capture offset requires 0 <= offset < explicit release delay\n");
            return 1;
        }
        if (const char* a = std::getenv("ZIIU_AUDIO")) {
            if (std::strcmp(a, "off") && !audio.Start(a)) std::printf("audio: no matching output device yet, retrying\n");
        }
        rctl = LocalSocket(50200);            // rumble + sensor bar control
        if (rctl == INVALID_SOCKET) return 1;
        if (rumble_enabled)
            std::printf("rumble: enabled (%s, control port 50200)\n",
                        rumble_test ? "hold A/B/Y on the GamePad to test" : "external control");
        if (!enc.Open(preset)) {
            std::fprintf(stderr, "x264 open failed (preset %s)\n", preset);
            return 1;
        }
        if (const char* c = std::getenv("ZIIU_CAPTURE")) {
            if (!cap.Open(c, kWidth, kHeight, kVisibleWidth)) return 1;
            capturing = true;
            std::memset(frame.data(), 16, (size_t)kWidth * kHeight);       // black until the first grab
            std::memset(frame.data() + kWidth * kHeight, 128, (size_t)kWidth * kHeight / 2);
            dump_path = std::getenv("ZIIU_CAPTURE_DUMP");
        }
        std::printf("ziiu-drc: %s %dx%d @59.94, preset %s, %u s\n", capturing ? "display" : "test pattern",
                    kWidth, kHeight, preset, seconds);

        // The Wii U's startup sequence (decrypted capture): time, UVC/UAC state,
        // generic 0.0 (versions), 4.4, 5.22 with 02 (twice), 5.6 (UIC config).
        Query(kTime, TimePayload(), "time");
        Query(kUvcUac, UvcUacState(), "UVC/UAC state");
        Generic(0, 0, {}, "version (0.0)");
        Generic(4, 4, {}, "4.4");
        Generic(5, 0x16, {0x02}, "5.22");
        Generic(5, 0x16, {0x02}, "5.22");
        Generic(5, 0x06, {}, "UIC config (5.6)");
        if (EnvInt("ZIIU_SENSOR_BAR", 0)) SetSensorBar(true);
        // Off by default: live A/B in one session (2026-10-15) gave ~300 requests per 5 s
        // with it on (from connect, also with sequence numbers given back) and 0 within
        // 5 s of switching it off ("ZRS0"). Kept as a switch for future tests.
        wiiu_resync = EnvInt("ZIIU_RESYNC", 0) != 0;
        std::printf("resync handling: %s\n", wiiu_resync ? "wiiu (drop + empty slot)" : "legacy");
        stamp_after_grab = EnvInt("ZIIU_STAMP_AFTER_GRAB", 0) != 0;
        idr_backoff = EnvInt("ZIIU_IDR_BACKOFF", 1) != 0;
        idr_guard_max = (unsigned long)std::max(EnvInt("ZIIU_IDR_GUARD_MAX", 30), (int)kIdrGuardFrames);
        std::printf("IDR backoff: %s (guard %lu..%lu frames)\n", idr_backoff ? "on" : "off", kIdrGuardFrames,
                    idr_backoff ? idr_guard_max : kIdrGuardFrames);
        if (const char* v = std::getenv("ZIIU_DRC_DELAY"))
            variant_lock = !std::strcmp(v, "ab") ? -1 : v[0] == '1';
        delay_send = variant_lock != 0;
        if (capture_offset_us && !delay_send) {
            std::fprintf(stderr, "capture offset requires delayed video release\n");
            return 1;
        }
        int reports = 0;
        timeBeginPeriod(1);
        aout = LocalSocket(0);
        if (aout == INVALID_SOCKET) return 1;
        std::thread audio_thread([this] { AudioLoop(); });
        u64 start = QpcNow(), next_video = start, next_uvc = start + qpf,
            next_report = start + 5 * qpf, next_status = start;
        const u64 frame_q = (u64)(kFrameUs * (double)qpf / 1e6);
        const u64 capture_offset_q = (u64)capture_offset_us * qpf / 1000000;
        next_video += capture_offset_q;
        while (!seconds || QpcNow() - start < (u64)seconds * qpf) {
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(msg, &rd); FD_SET(cmd, &rd); FD_SET(tsf, &rd);
            if (hid != INVALID_SOCKET) FD_SET(hid, &rd);
            if (rctl != INVALID_SOCKET) FD_SET(rctl, &rd);
            timeval tv = {0, 1000};
            select(0, &rd, nullptr, nullptr, &tv);
            Drain(tsf, &App::OnTsf);
            Drain(msg, &App::OnMsg);
            if (hid != INVALID_SOCKET) Drain(hid, &App::OnHid);
            Drain(cmd, &App::OnCmd);
            if (rctl != INVALID_SOCKET) Drain(rctl, &App::OnRumbleCtl);

            u64 now = QpcNow();
            if (held_deadline && now >= held_deadline) FlushHeld();
            if (now >= next_video && (delay_us < 0 || held.empty())) {
                // A slow encode must not be followed by a burst of catch-up encodes
                // (seen as hitching): capture only the latest due tick, keep the grid.
                const u64 missed = (now - next_video) / frame_q;
                skipped_ticks += (unsigned long)missed;
                next_video += missed * frame_q;
                VideoTick(next_video - capture_offset_q);
                next_video += frame_q;
            }
            if (now >= next_status) {
                u8 sp[8 + sizeof(kStatus50125)];
                auto hdr = CmdPacket(kQuery, 0, status_seq++, {});
                std::memcpy(sp, hdr.data(), 8);
                sp[4] = sizeof(kStatus50125);
                std::memcpy(sp + 8, kStatus50125, sizeof(kStatus50125));
                Send(50125, sp, sizeof(sp));
                next_status += qpf * 103 / 100;
            }
            if (now >= next_uvc) {
                Query(kUvcUac, UvcUacState(), "UVC/UAC state");
                ++uvc_count;
                if (input_external && uvc_count % 2 == 0) SendCalibration();
                if (sensor_bar && uvc_count % 10 == 0)
                    Generic(5, 0x01, {1}, "sensor bar on");
                next_uvc += qpf;
            }
            CmdTimeouts();
            if (now >= next_report) {
                std::printf("frames=%lu (enc avg %.1f ms, fail %lu) vstrm=%lu astrm=%lu input=%lu tsf=%s "
                            "resyncs=%lu (ignored %lu)\n",
                            frames, frames ? enc_ms_total / frames : 0.0, enc_fail, vpackets,
                            apackets.load(), hid_packets, clock.have ? "yes" : "no", resyncs, resyncs_ignored);
                if (rumble_enabled)
                    std::printf("rumble: level=%d on_packets=%lu cmds=%lu\n",
                                rumble_level.load(), rumble_on_packets.load(), rumble_cmds);
                std::printf("timing: release %s, skipped ticks %lu; encode max IDR %.1f ms (%lu IDRs, %lu over a "
                            "frame period), P %.1f ms; resync %s: dropped before IDR %lu, empty slots %lu; "
                            "IDR guard max %lu frames\n",
                            delay_us >= 0 ? "deadline" : "next tick", skipped_ticks, idr_enc_max, idr_enc_n,
                            idr_enc_late, p_enc_max, wiiu_resync ? "wiiu" : "legacy", dropped_for_idr, slots_after_idr,
                            idr_guard_peak);
                idr_guard_peak = 0;
                idr_enc_max = p_enc_max = 0;
                idr_enc_n = idr_enc_late = 0;
                std::printf("video latency: capture-to-send avg %.2f max %.2f ms (%lu frames); capture offset %d us\n",
                            sent_frames ? capture_to_send_sum / sent_frames : 0.0,
                            capture_to_send_max, sent_frames, capture_offset_us);
                std::printf("video stamp: stamp-to-send avg %.2f min %.2f ms, %lu frames under 12 ms (%s)\n",
                            sent_frames ? stamp_to_send_sum / sent_frames : 0.0,
                            sent_frames ? stamp_to_send_min : 0.0, stamp_early,
                            stamp_after_grab && delay_us < 0 ? "after grab" : "scheduled tick");
                stamp_to_send_sum = 0;
                stamp_to_send_min = 1e9;
                stamp_early = 0;
                capture_to_send_sum = capture_to_send_max = 0;
                sent_frames = 0;
                if (audio.running)
                    std::printf("audio: %s, packets from PC %lu, peak %d of 32767 (0 = silence), underruns %lu, "
                                "drops %lu, buffered %.1f ms\n",
                                audio.device.c_str(), audio_packets.load(), audio_peak.exchange(0),
                                audio.underruns.load(), audio.drops.load(), audio.BufferedMs());
                if (capturing) {
                    std::printf("capture: desktop frames=%lu pointer=%lu changed=%lu lost=%lu age avg %.1f max %.1f ms, "
                                "scale+readback avg %.2f ms (grab+encode avg %.1f ms)\n",
                                cap.desktop_frames, cap.pointer_updates, changed_frames, cap.lost,
                                cap.age_n ? cap.age_ms_sum / cap.age_n : 0.0, cap.age_ms_max,
                                cap.converts ? cap.gpu_ms_sum / cap.converts : 0.0,
                                frames ? enc_ms_total / frames : 0.0);
                    cap.age_ms_max = 0;
                }
                std::printf("video: avg %.1f KB/frame (%.1f Mbit/s), max %.1f KB, IDR max %.1f KB\n",
                            frames ? bytes_sum / 1024.0 / frames : 0.0, frames ? bytes_sum * 8 * 59.94 / 1e6 / frames : 0.0,
                            bytes_max / 1024.0, idr_max / 1024.0);
                bytes_max = idr_max = 0;
                std::printf("clock: stamp now=%u last tsf=%llu samples=%lu zero=%lu rate=%.6f\n",
                            clock.Now(), (unsigned long long)clock.tsf0, clock.samples, clock.zero,
                            clock.Rate());
                // Alternate the send timing every 15 s so one run compares both.
                if (variant_lock < 0 && ++reports % 3 == 0) {
                    std::printf("variant %s: %lu resyncs in 15 s\n", delay_send ? "delayed (libdrc)" : "immediate",
                                resyncs - resyncs_at_switch);
                    resyncs_at_switch = resyncs;
                    FlushHeld();
                    delay_send = !delay_send;
                    std::printf("variant -> %s\n", delay_send ? "delayed (libdrc)" : "immediate");
                }
                std::fflush(stdout);
                next_report += 5 * qpf;
            }
        }
        audio_stop = true;
        audio_thread.join();
        timeEndPeriod(1);
        x264_encoder_close(enc.enc);
        std::printf("done\n");
        return 0;
    }
};

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    unsigned seconds = argc > 1 ? (unsigned)std::atoi(argv[1]) : 60;
    const char* preset = argc > 2 ? argv[2] : "slow";
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    App app;
    int rc = app.Run(seconds ? seconds : 60, preset);
    WSACleanup();
    return rc;
}

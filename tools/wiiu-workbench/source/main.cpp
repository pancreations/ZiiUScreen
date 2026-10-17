// SPDX-License-Identifier: GPL-3.0-or-later
// ZiiU Workbench: an optional Aroma plugin, not a foreground homebrew app.
#include <wups.h>
#include <wups/config/WUPSConfigItemBoolean.h>
#include <wups/config/WUPSConfigItemStub.h>
#include <coreinit/cache.h>
#include <coreinit/dynload.h>
#include <coreinit/memorymap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <coreinit/title.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/surface.h>
#include <memory/mappedmemory.h>
#include <nsysccr/cdc.h>
#include <sysapp/launch.h>
#include <vpad/input.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include "input_mailbox.h"

WUPS_PLUGIN_NAME("ZiiU Workbench");
static_assert(sizeof(WUPS_VERSION_STR) == 6 && WUPS_VERSION_STR[0] == '0' &&
              WUPS_VERSION_STR[2] == '9' && WUPS_VERSION_STR[4] == '0',
              "Use pinned WUPS 0.9.0 for the owner's PluginBackend v0.3.6");
WUPS_PLUGIN_DESCRIPTION("Remote inspection, screenshots and control. Disable here to stop all access.");
WUPS_PLUGIN_VERSION("0.1.2");
WUPS_PLUGIN_AUTHOR("ZiiUScreen");
WUPS_PLUGIN_LICENSE("GPL-3.0-or-later");
WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_STORAGE("ziiu_workbench");

static constexpr int PORT = 49426;
static constexpr const char *TOKEN_PATH = "fs:/vol/external01/wiiu/ziiu-workbench.token";
static bool enabled = false, started = false, appActive = false;
static std::atomic<bool> initialized{false};
static const char *serverStatus = "Disabled; enable here after reaching the Menu";
static std::atomic<bool> running{false}, foreground{false};
static OSThread worker;
alignas(16) static unsigned char workerStack[64 * 1024];
static char token[65];
static std::atomic<unsigned> inputCalls{0};
static std::atomic<unsigned> inputSamples{0};
static InputMailbox inputMailbox;
static std::atomic<uint32_t> previousMask{0};
static std::atomic<int> shotState{0}; // idle, requested, copying, ready, unsupported
static GX2ScanTarget shotTarget = GX2_SCAN_TARGET_TV;
static unsigned shotWidth, shotHeight;
static unsigned char shotRGB[640 * 480 * 3];

static void releaseInput() {
    inputMailbox.cancel();
}

DECL_FUNCTION(int32_t, VPADRead, VPADChan chan, VPADStatus *samples, uint32_t count, VPADReadError *error) {
    int32_t got = real_VPADRead(chan, samples, count, error);
    if (!initialized || (!running && previousMask == 0)) return got;
    if (chan != VPAD_CHAN_0 || !samples || !count) return got;
    inputCalls.fetch_add(1, std::memory_order_relaxed);
    RemoteInput input;
    const bool active = running && foreground && inputMailbox.read(OSGetTime(), input);
    const uint32_t mask = active ? input.mask : 0;
    if (got <= 0 || (uint32_t)got > count || (error && *error != VPAD_READ_SUCCESS)) {
        return got; // never invent a connected GamePad or consume physical input
    }
    // VPAD samples are newest first. Generate edges in chronological order.
    for (int32_t i = got - 1; i >= 0; --i) {
        overlayButtons(samples[i], mask, previousMask.exchange(mask));
        if (active) {
            inputSamples.fetch_add(1, std::memory_order_relaxed);
            if (input.sx != 0 || input.sy != 0) samples[i].leftStick = {input.sx / 1000.0f, input.sy / 1000.0f};
            if (input.x >= 0) {
                VPADTouchData touch = {(uint16_t)input.x, (uint16_t)input.y, 1, VPAD_VALID};
                samples[i].tpNormal = samples[i].tpFiltered1 = samples[i].tpFiltered2 = touch;
            }
        }
    }
    return got;
}
WUPS_MUST_REPLACE(VPADRead, WUPS_LOADER_LIBRARY_VPAD, VPADRead);

// Capture only on request and from the rendering thread. Use mapped GPU memory,
// a linear surface and a completed GPU copy before reading pixels on the CPU.
// See ScreenshotWUPS (GPL-3.0) for the underlying Cafe GX2 capture technique.
DECL_FUNCTION(void, GX2CopyColorBufferToScanBuffer, const GX2ColorBuffer *color, GX2ScanTarget target) {
    int expected = 1;
    if (running && foreground && color && shotState.load() == 1 && target == shotTarget &&
        shotState.compare_exchange_strong(expected, 2)) {
        const auto &src = color->surface;
        bool ok = false;
        if (src.width > 0 && src.width <= 1920 && src.height > 0 && src.height <= 1080 &&
            src.aa == GX2_AA_MODE1X && color->viewMip == 0 &&
            (src.format == GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8 ||
             src.format == GX2_SURFACE_FORMAT_SRGB_R8_G8_B8_A8)) {
            GX2Surface dst = {};
            dst.dim = GX2_SURFACE_DIM_TEXTURE_2D;
            dst.width = src.width; dst.height = src.height; dst.depth = 1;
            dst.mipLevels = 1; dst.format = src.format; dst.aa = GX2_AA_MODE1X;
            dst.use = (GX2SurfaceUse)(GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER);
            dst.tileMode = GX2_TILE_MODE_LINEAR_ALIGNED;
            GX2CalcSurfaceSizeAndAlignment(&dst);
            dst.image = MEMAllocFromMappedMemoryForGX2Ex(dst.imageSize, dst.alignment);
            if (dst.image) {
                GX2Invalidate(GX2_INVALIDATE_MODE_CPU, dst.image, dst.imageSize);
                GX2CopySurface(&src, 0, color->viewFirstSlice, &dst, 0, 0);
                GX2Invalidate(GX2_INVALIDATE_MODE_COLOR_BUFFER, dst.image, dst.imageSize);
                GX2DrawDone();
                DCInvalidateRange(dst.image, dst.imageSize);
                unsigned scale = (src.width + 639) / 640;
                if ((src.height + scale - 1) / scale > 480) scale = (src.height + 479) / 480;
                shotWidth = (src.width + scale - 1) / scale;
                shotHeight = (src.height + scale - 1) / scale;
                auto *pixels = (unsigned char *)dst.image;
                for (unsigned y = 0; y < shotHeight; ++y)
                    for (unsigned x = 0; x < shotWidth; ++x)
                        memcpy(shotRGB + (y * shotWidth + x) * 3,
                               pixels + (y * scale * dst.pitch + x * scale) * 4, 3);
                MEMFreeToMappedMemory(dst.image);
                ok = true;
            }
        }
        shotState.store(ok ? 3 : 4);
    }
    real_GX2CopyColorBufferToScanBuffer(color, target);
}
WUPS_MUST_REPLACE(GX2CopyColorBufferToScanBuffer, WUPS_LOADER_LIBRARY_GX2, GX2CopyColorBufferToScanBuffer);

static bool waitSocket(int fd, bool write, OSTime deadline) {
    while (running && foreground && OSGetTime() < deadline) {
        fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
        timeval timeout = {0, 100000};
        int n = select(fd + 1, write ? nullptr : &fds, write ? &fds : nullptr, nullptr, &timeout);
        if (n > 0) return true;
        if (n < 0 && errno != EINTR) return false;
    }
    return false;
}
static bool sendAll(int fd, const void *buf, size_t size) {
    const char *p = (const char *)buf;
    OSTime deadline = OSGetTime() + OSMillisecondsToTicks(3000);
    while (size) {
        if (!waitSocket(fd, true, deadline)) return false;
        size_t chunk = size > 8192 ? 8192 : size;
        int n = send(fd, p, chunk, 0);
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) return false;
        size -= n; p += n;
    }
    return true;
}
static void reply(int fd, const char *format, ...) {
    char buf[2048];
    va_list args; va_start(args, format);
    int n = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    if (n > 0 && n < (int)sizeof(buf)) sendAll(fd, buf, n);
}
static void errorReply(int fd, const char *message) {
    reply(fd, "{\"ok\":false,\"error\":\"%s\"}\n", message);
}
static bool readLine(int fd, char *out, size_t size) {
    size_t used = 0;
    OSTime deadline = OSGetTime() + OSMillisecondsToTicks(2000);
    while (used + 1 < size && waitSocket(fd, false, deadline)) {
        char c;
        int n = recv(fd, &c, 1, 0);
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR)) continue;
        if (n != 1) return false;
        if (c == '\n') { out[used] = 0; return true; }
        if (c < 32 || c > 126) return false;
        out[used++] = c;
    }
    return false;
}
static bool number(const char *s, unsigned long long &value, int base = 0) {
    if (!s || !*s || *s == '-') return false;
    char *end; errno = 0; value = strtoull(s, &end, base);
    return errno == 0 && *end == 0;
}
static void status(int fd) {
    reply(fd, "{\"ok\":true,\"version\":1,\"name\":\"ZiiU Workbench\","
              "\"build\":\"0.1.2\",\"title_id\":\"%016llx\",\"foreground\":%s,\"vpad_calls\":%u,"
              "\"input_samples\":%u,\"remote_buttons\":%u,"
              "\"capabilities\":[\"radio\",\"input\",\"screenshot\",\"launch\",\"symbol\",\"memory_read\"]}\n",
          (unsigned long long)OSGetTitleID(), foreground ? "true" : "false", inputCalls.load(),
          inputSamples.load(), previousMask.load());
}
static void radio(int fd) {
    CCRCDCDrhState drh = {}; CCRCDCDrcState drc = {};
    CCRCDCWpsStatusType wps = {}; uint8_t count = 0;
    int a = CCRCDCSysGetDrhState(&drh);
    int b = CCRCDCSysGetDrcState(CCR_CDC_DESTINATION_DRC0, &drc);
    int c = CCRCDCWpsStatus(&wps), d = CCRCDCGetMultiDrc(&count);
    reply(fd, "{\"ok\":true,\"drh_rc\":%d,\"drh_state\":%u,\"drc_rc\":%d,"
              "\"drc_state\":%u,\"wps_rc\":%d,\"wps_status\":%u,\"count_rc\":%d,\"max_drc\":%u}\n",
          a, (unsigned)drh.state, b, (unsigned)drc.state, c, (unsigned)wps, d, count);
}
static void snapshot(int fd, const char *target) {
    if (!target || (strcmp(target, "tv") && strcmp(target, "gamepad"))) { errorReply(fd, "Use tv or gamepad"); return; }
    if (shotState == 2) { errorReply(fd, "Previous GPU copy still busy"); return; }
    shotTarget = !strcmp(target, "tv") ? GX2_SCAN_TARGET_TV : GX2_SCAN_TARGET_DRC0;
    shotState = 1;
    OSTime deadline = OSGetTime() + OSMillisecondsToTicks(2000);
    while (running && foreground && shotState < 3 && OSGetTime() < deadline)
        OSSleepTicks(OSMillisecondsToTicks(10));
    if (shotState == 3) {
        char header[64];
        int size = snprintf(header, sizeof(header), "P6\n%u %u\n255\n", shotWidth, shotHeight);
        reply(fd, "{\"ok\":true,\"bytes\":%u,\"format\":\"ppm\"}\n", size + shotWidth * shotHeight * 3);
        if (sendAll(fd, header, size)) sendAll(fd, shotRGB, shotWidth * shotHeight * 3);
    } else errorReply(fd, shotState == 4 ? "Unsupported surface or no GPU memory" : "No frame received within 2 seconds");
    int expected = 1; shotState.compare_exchange_strong(expected, 0);
}
static void memoryRead(int fd, const char *addrText, const char *lengthText) {
    unsigned long long addr, length;
    if (!number(addrText, addr) || !number(lengthText, length) || length == 0 || length > 4096 ||
        !((addr >= 0x01000000 && addr + length <= 0x01800000) ||
          (addr >= 0x02000000 && addr + length <= 0x40000000))) { errorReply(fd, "Read must be 1..4096 bytes in user code/data RAM"); return; }
    // Never dereference MMIO, kernel addresses or an unmapped page.
    for (uint32_t p = (uint32_t)addr & ~0xFFFu; p < addr + length; p += 4096)
        if (!OSIsAddressValid(p)) { errorReply(fd, "Unmapped address"); return; }
    unsigned char bytes[4096];
    memcpy(bytes, (void *)(uintptr_t)addr, (size_t)length);
    reply(fd, "{\"ok\":true,\"bytes\":%u,\"format\":\"binary\"}\n", (unsigned)length);
    sendAll(fd, bytes, length);
}
static void symbol(int fd, char *moduleName, char *exportName, char *kind) {
    if (!moduleName || !exportName || !kind || (strcmp(kind, "function") && strcmp(kind, "data"))) {
        errorReply(fd, "SYMBOL module export function|data"); return;
    }
    OSDynLoad_Module module = nullptr; void *address = nullptr;
    int rc = OSDynLoad_IsModuleLoaded(moduleName, &module);
    if (!rc) rc = OSDynLoad_FindExport(module, !strcmp(kind, "data") ? OS_DYNLOAD_EXPORT_DATA : OS_DYNLOAD_EXPORT_FUNC, exportName, &address);
    reply(fd, "{\"ok\":%s,\"rc\":%d,\"address\":\"0x%08x\"}\n", rc ? "false" : "true", rc, (unsigned)(uintptr_t)address);
}
static void command(int fd, char *line) {
    char *save = nullptr;
    char *auth = strtok_r(line, " ", &save);
    char *cmd = strtok_r(nullptr, " ", &save);
    if (!auth || strlen(auth) != 64 || !cmd) { errorReply(fd, "Authentication required"); return; }
    unsigned diff = 0;
    for (unsigned i = 0; i < 64; ++i) diff |= auth[i] ^ token[i];
    if (diff) { errorReply(fd, "Authentication failed"); return; }
    char *args[8] = {}; unsigned argc = 0;
    while (argc < 8 && (args[argc] = strtok_r(nullptr, " ", &save))) ++argc;
    if (!strcmp(cmd, "STATUS") && argc == 0) status(fd);
    else if (!strcmp(cmd, "RADIO") && argc == 0) radio(fd);
    else if (!strcmp(cmd, "RELEASE") && argc == 0) { releaseInput(); reply(fd, "{\"ok\":true}\n"); }
    else if (!strcmp(cmd, "SCREENSHOT") && argc == 1) snapshot(fd, args[0]);
    else if (!strcmp(cmd, "READ") && argc == 2) memoryRead(fd, args[0], args[1]);
    else if (!strcmp(cmd, "SYMBOL") && argc == 3) symbol(fd, args[0], args[1], args[2]);
    else if (!strcmp(cmd, "TITLES")) errorReply(fd, "Title browsing uses FTP in the PC client; console MCP enumeration is disabled");
    else if (!strcmp(cmd, "MENU") && argc == 0) { reply(fd, "{\"ok\":true,\"accepted\":true}\n"); releaseInput(); SYSLaunchMenu(); }
    else if (!strcmp(cmd, "LAUNCH") && argc == 1) {
        unsigned long long id;
        if (!number(args[0], id, 16) || strlen(args[0]) != 16) { errorReply(fd, "Expected 16 digit installed title ID"); return; }
        unsigned type = (unsigned)(id >> 32);
        if (type != 0x00050000 && type != 0x00050010) { errorReply(fd, "Only Wii U game or system application title types are supported"); return; }
        // PC client verifies installation through FTP; never invoke hooked MCP APIs here.
        reply(fd, "{\"ok\":true,\"accepted\":true}\n"); releaseInput(); SYSLaunchTitle(id);
    } else if (!strcmp(cmd, "INPUT") && argc == 6) {
        unsigned long long mask, ms;
        char *endX, *endY, *endSX, *endSY;
        long x = strtol(args[2], &endX, 10), y = strtol(args[3], &endY, 10);
        long sx = strtol(args[4], &endSX, 10), sy = strtol(args[5], &endSY, 10);
        if (!number(args[0], mask) || !number(args[1], ms) || mask > 0x7ffff || (mask & VPAD_BUTTON_SYNC) ||
            ms < 20 || ms > 2000 || *endX || *endY || *endSX || *endSY ||
            x < -1 || x > 4095 || y < -1 || y > 4095 || ((x == -1) != (y == -1)) ||
            sx < -1000 || sx > 1000 || sy < -1000 || sy > 1000) {
            errorReply(fd, "Invalid input; duration 20..2000 ms, raw touch 0..4095, stick -1000..1000"); return;
        }
        RemoteInput input;
        input.mask = mask; input.x = x; input.y = y; input.sx = sx; input.sy = sy;
        input.expires = OSGetTime() + OSMillisecondsToTicks(ms);
        inputMailbox.publish(input);
        reply(fd, "{\"ok\":true,\"accepted\":true,\"duration_ms\":%u}\n", (unsigned)ms);
    } else errorReply(fd, "Unknown command or wrong argument count");
}
static int32_t serve(int32_t, const char **) {
    int server = -1;
    while (running) {
        if (!foreground) { OSSleepTicks(OSMillisecondsToTicks(100)); continue; }
        if (server < 0) {
            server = socket(AF_INET, SOCK_STREAM, 0);
            if (server >= 0) {
                fcntl(server, F_SETFL, O_NONBLOCK);
                int yes = 1; setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
                sockaddr_in addr = {}; addr.sin_family = AF_INET;
                addr.sin_port = htons(PORT); addr.sin_addr.s_addr = htonl(INADDR_ANY);
                if (bind(server, (sockaddr *)&addr, sizeof(addr)) < 0 || listen(server, 2) < 0) {
                    close(server); server = -1;
                }
            }
            if (server < 0) { OSSleepTicks(OSMillisecondsToTicks(500)); continue; }
        }
        if (!waitSocket(server, false, OSGetTime() + OSMillisecondsToTicks(200))) continue;
        int client = accept(server, nullptr, nullptr);
        if (client < 0) continue;
        if (fcntl(client, F_SETFL, O_NONBLOCK) < 0) { close(client); continue; }
        char line[512];
        if (readLine(client, line, sizeof(line))) command(client, line);
        close(client);
    }
    if (server >= 0) close(server);
    return 0;
}
static void start() {
    if (!initialized || started || !enabled || !appActive) return;
    FILE *f = fopen(TOKEN_PATH, "rb");
    if (!f) { serverStatus = "Missing SD:/wiiu/ziiu-workbench.token"; return; }
    size_t n = fread(token, 1, 64, f); fclose(f); token[64] = 0;
    if (n != 64 || strspn(token, "0123456789abcdef") != 64) { serverStatus = "Invalid authentication token"; return; }
    releaseInput();
    running = true;
    if (OSCreateThread(&worker, serve, 0, nullptr, workerStack + sizeof(workerStack),
                       sizeof(workerStack), 16, OS_THREAD_ATTRIB_AFFINITY_CPU2)) {
        started = true; serverStatus = "Worker started; TCP 49426";
        OSSetThreadName(&worker, "ZiiU Workbench"); OSResumeThread(&worker);
    } else { running = false; serverStatus = "OSCreateThread failed"; }
}
static void stop() {
    running = false; releaseInput();
    int expected = 1; shotState.compare_exchange_strong(expected, 0);
    if (started) { OSJoinThread(&worker, nullptr); started = false; }
    while (shotState == 2) OSSleepTicks(OSMillisecondsToTicks(1));
    memset(token, 0, sizeof(token));
    serverStatus = "Disabled; listener stopped";
}
static void enabledChanged(ConfigItemBoolean *, bool value) {
    enabled = value;
    // Session-only activation: a power cycle always recovers to disabled.
    if (!value) stop(); else start();
}
static WUPSConfigAPICallbackStatus configOpened(WUPSConfigCategoryHandle root) {
    WUPSConfigItemBoolean_AddToCategory(root, "enabled", "Enable remote workbench (this session)", false, enabled, enabledChanged);
    WUPSConfigItemStub_AddToCategory(root, serverStatus);
    WUPSConfigItemStub_AddToCategory(root, "Build 0.1.2 - title browsing handled over FTP");
    WUPSConfigItemStub_AddToCategory(root, "TCP 49426; token required. FTP remains separate.");
    WUPSConfigItemStub_AddToCategory(root, "Disable stops remote access and releases buttons.");
    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}
static void configClosed() {}
INITIALIZE_PLUGIN() {
    initialized = true;
    enabled = false; // do not trust an enabled setting left by a previous build
    WUPSConfigAPIOptionsV1 options = {.name = "ZiiU Workbench"};
    WUPSConfigAPI_Init(options, configOpened, configClosed);
}
ON_APPLICATION_START() { appActive = true; foreground = true; previousMask = 0; start(); }
ON_RELEASE_FOREGROUND() { foreground = false; releaseInput(); }
ON_ACQUIRED_FOREGROUND() { foreground = true; }
ON_APPLICATION_REQUESTS_EXIT() { stop(); }
ON_APPLICATION_ENDS() { appActive = false; foreground = false; stop(); }
DEINITIALIZE_PLUGIN() { appActive = false; stop(); initialized = false; }

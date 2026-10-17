// ziiu-probe: read-only dump of the Wii U's GamePad radio (DRH) and GamePad (DRC) state.
//
// Runs on the owner's console (Aroma) to learn how the real console talks to the
// GamePad. Only query calls; nothing on the console or GamePad is changed. Output is
// shown on screen and saved to SD:/ziiu-probe.txt (fetched over FTP).

#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nsysccr/cdc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/proc.h>

static FILE* out;

static void Out(const char* fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    WHBLogPrint(line);
    WHBLogConsoleDraw();
    if (out) {
        fprintf(out, "%s\n", line);
    }
}

static const char* DestName(CCRCDCDestination d)
{
    switch (d) {
    case CCR_CDC_DESTINATION_DRH: return "DRH (console radio)";
    case CCR_CDC_DESTINATION_DRC0: return "DRC0 (GamePad 1)";
    case CCR_CDC_DESTINATION_DRC1: return "DRC1 (GamePad 2)";
    default: return "?";
    }
}

static void Device(CCRCDCDestination d)
{
    Out("--- %s", DestName(d));
    Out("ping: %d", (int)CCRCDCDevicePing(d));

    CCRCDCMacAddress mac;
    memset(&mac, 0, sizeof(mac));
    int32_t r = CCRCDCGetMacAddress(d, &mac);
    Out("mac: r=%d dev=%u %02x:%02x:%02x:%02x:%02x:%02x", (int)r, mac.device, mac.address[0], mac.address[1],
        mac.address[2], mac.address[3], mac.address[4], mac.address[5]);

    CCRCDCSysInfo info;
    memset(&info, 0, sizeof(info));
    r = CCRCDCSysGetInfo(d, &info);
    Out("sysinfo: r=%d board=%u chip=0x%x lvc=0x%08x umi=0x%08x unk=0x%08x sdCis=0x%08x spl=0x%08x", (int)r,
        (unsigned)info.boardVersion, (unsigned)info.chipVersion, (unsigned)info.lvcVersion,
        (unsigned)info.umiVersion, (unsigned)info.unknown, (unsigned)info.sdCis, (unsigned)info.splId);

    if (d != CCR_CDC_DESTINATION_DRH) {
        CCRCDCDrcState st;
        memset(&st, 0, sizeof(st));
        r = CCRCDCSysGetDrcState(d, &st);
        Out("drc state: r=%d state=%u", (int)r, st.state);
        CCRCDCSoftwareVersion v;
        memset(&v, 0, sizeof(v));
        r = CCRCDCSoftwareGetVersion(d, &v);
        Out("firmware: r=%d running=0x%08x active=0x%08x", (int)r, (unsigned)v.runningVersion,
            (unsigned)v.activeVersion);
    }
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    WHBProcInit();
    WHBLogConsoleInit();

    out = fopen("fs:/vol/external01/ziiu-probe.txt", "w");
    Out("ziiu-probe (read-only)  file: %s", out ? "SD:/ziiu-probe.txt" : "SD write FAILED");

    uint8_t num = 0;
    int32_t r = CCRCDCGetMultiDrc(&num);
    Out("multi drc: r=%d num=%u", (int)r, num);
    CCRCDCDrhState drh;
    memset(&drh, 0, sizeof(drh));
    r = CCRCDCSysGetDrhState(&drh);
    Out("drh state: r=%d state=0x%02x", (int)r, drh.state);
    CCRCDCWpsStatusType wps = 0xffffffff;
    r = CCRCDCWpsStatus(&wps);
    Out("wps status: r=%d status=%u (0 paired, 1 searching, 2 pairing)", (int)r, (unsigned)wps);

    Device(CCR_CDC_DESTINATION_DRH);
    Device(CCR_CDC_DESTINATION_DRC0);
    Device(CCR_CDC_DESTINATION_DRC1);

    static CCRCDCEepromData ee;
    memset(&ee, 0, sizeof(ee));
    r = CCRCDCPerGetUicEeprom(CCR_CDC_DESTINATION_DRC0, &ee);
    Out("--- GamePad 1 EEPROM: r=%d version=0x%08x", (int)r, (unsigned)ee.version);
    if (out && r == 0) {
        for (unsigned i = 0; i < sizeof(ee.data); i += 16) {
            fprintf(out, "%03x:", i);
            for (unsigned j = 0; j < 16; j++) {
                fprintf(out, " %02x", ee.data[i + j]);
            }
            fprintf(out, "\n");
        }
    }

    if (out) {
        fclose(out);
        out = NULL;
    }
    Out("done. Press HOME to exit.");
    while (WHBProcIsRunning()) {
        WHBLogConsoleDraw();
        OSSleepTicks(OSMillisecondsToTicks(100));
    }
    WHBLogConsoleFree();
    WHBProcShutdown();
    return 0;
}

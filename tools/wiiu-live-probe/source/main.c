// Foreground, read-only Wii U DRH/GamePad state server. No EEPROM or key export.
// Listens only while this app is open; HOME closes the listener.
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <nsysccr/cdc.h>
#include <nsysnet/_socket.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/proc.h>

#define LIVE_PORT 49425

static void SendStatus(int fd)
{
    CCRCDCDrhState drh = {0};
    CCRCDCDrcState drc = {0};
    CCRCDCWpsStatusType wps = 0;
    uint8_t count = 0;
    int drh_rc = CCRCDCSysGetDrhState(&drh);
    int drc_rc = CCRCDCSysGetDrcState(CCR_CDC_DESTINATION_DRC0, &drc);
    int wps_rc = CCRCDCWpsStatus(&wps);
    int count_rc = CCRCDCGetMultiDrc(&count);
    int drh_ping = CCRCDCDevicePing(CCR_CDC_DESTINATION_DRH);
    int drc_ping = CCRCDCDevicePing(CCR_CDC_DESTINATION_DRC0);
    char reply[320];
    int n = snprintf(reply, sizeof(reply),
        "{\"version\":1,\"drh_rc\":%d,\"drh_state\":%u,"
        "\"drc_rc\":%d,\"drc_state\":%u,"
        "\"wps_rc\":%d,\"wps_status\":%u,"
        "\"count_rc\":%d,\"max_drc\":%u,"
        "\"drh_ping\":%d,\"drc_ping\":%d}\n",
        drh_rc, (unsigned)drh.state, drc_rc, (unsigned)drc.state,
        wps_rc, (unsigned)wps, count_rc, (unsigned)count,
        drh_ping, drc_ping);
    if (n > 0 && n < (int)sizeof(reply)) {
        (void)send(fd, reply, (size_t)n, 0);
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    WHBProcInit();
    WHBLogConsoleInit();
    socket_lib_init();
    int socket_error = 0;
    int bind_error = 0;
    int listen_error = 0;
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) socket_error = errno;
    if (server >= 0) {
        int yes = 1;
        (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(LIVE_PORT);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(server, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
            bind_error = errno;
            close(server);
            server = -1;
        } else if (listen(server, 1) < 0) {
            listen_error = errno;
            close(server);
            server = -1;
        }
    }
    WHBLogPrintf("ZiiU Live Probe: %s", server >= 0 ? "ready on TCP 49425" : "network listener failed");
    if (server < 0) WHBLogPrintf("socket=%d bind=%d listen=%d", socket_error, bind_error, listen_error);
    FILE *diag = fopen("fs:/vol/external01/ziiu-live-probe-status.txt", "w");
    if (diag) {
        fprintf(diag, "ready=%d socket_error=%d bind_error=%d listen_error=%d\n",
                server >= 0, socket_error, bind_error, listen_error);
        fclose(diag);
    }
    WHBLogPrint("Read-only radio/GamePad state. HOME exits.");
    while (WHBProcIsRunning()) {
        WHBLogConsoleDraw();
        if (server < 0) {
            usleep(200000);
            continue;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server, &fds);
        struct timeval timeout = {0, 200000};
        if (select(server + 1, &fds, NULL, NULL, &timeout) <= 0) continue;
        int client = accept(server, NULL, NULL);
        if (client < 0) continue;
        FD_ZERO(&fds);
        FD_SET(client, &fds);
        struct timeval recv_timeout = {1, 0};
        if (select(client + 1, &fds, NULL, NULL, &recv_timeout) > 0) {
            char command[16] = {0};
            int n = recv(client, command, sizeof(command) - 1, 0);
            if (n >= 6 && !memcmp(command, "STATUS", 6)) SendStatus(client);
        }
        close(client);
    }
    if (server >= 0) close(server);
    socket_lib_finish();
    WHBLogConsoleFree();
    WHBProcShutdown();
    return 0;
}

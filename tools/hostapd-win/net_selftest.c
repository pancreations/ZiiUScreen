/* Exercise the real localhost bridge with a fake radio callback. No radio access,
 * no production ports. Build with the drc-hostap utils headers and ws2_32. */
#include "ziiu_net.c"

void * os_zalloc(size_t size) { return calloc(1, size); }

struct received {
    unsigned count;
    u16 ports[8];
    size_t lengths[8];
    u8 payloads[8][1700];
    SOCKET out, audio, video;
    int inject;
};

static int enqueue(SOCKET out, SOCKET in, const u8 *payload, int len);

static int record(void *ctx, const u8 *dst, u16 type, const u8 *body, size_t len)
{
    struct received *r = ctx;
    unsigned i = r->count++;
    (void)dst;
    if (i >= 8 || type != ETH_P_IP || len < 28 || len - 28 > 1700) return -1;
    r->ports[i] = WPA_GET_BE16(body + 22);
    r->lengths[i] = len - 28;
    memcpy(r->payloads[i], body + 28, len - 28);
    if (r->ports[i] == 50120 && r->inject) {
        u8 format[32] = {4}, video[100] = {0xf0, 0, 0x40};
        WPA_PUT_LE32(format + 8, 3);
        WPA_PUT_BE32(video + 4, 3);
        r->inject = 0;
        /* Simulate the next frame arriving during the video drain. */
        enqueue(r->out, r->audio, format, sizeof(format));
        enqueue(r->out, r->video, video, sizeof(video));
    }
    return 0;
}

static int enqueue(SOCKET out, SOCKET in, const u8 *payload, int len)
{
    struct sockaddr_in to;
    int size = sizeof(to);
    if (getsockname(in, (struct sockaddr *)&to, &size)) return 0;
    return sendto(out, (const char *)payload, len, 0, (struct sockaddr *)&to, size) == len;
}

int main(void)
{
    WSADATA wsa;
    const u8 own[6] = {2,0,0,0,0,1}, peer[6] = {2,0,0,0,0,2};
    struct received received = {0};
    struct ziiu_net *net;
    SOCKET out;
    u8 format[32] = {4}, video[100] = {0xf0, 0, 0x40}, pcm[1672] = {0x20};
    int ok, i;
    if (WSAStartup(MAKEWORD(2,2), &wsa)) return 1;
    net = ziiu_net_init(own, peer, record, &received, 0);
    if (!net) return 1;
    net->link_up = net->bound = 1;
    net->down[0] = ziiu_local_socket(0);
    net->down[1] = ziiu_local_socket(0);
    out = ziiu_local_socket(0);
    received.out = out;
    received.audio = net->down[1];
    received.video = net->down[0];
    received.inject = 1;
    WPA_PUT_LE32(format + 8, 1);
    WPA_PUT_BE32(video + 4, 1);
    for (i = 8; i < sizeof(pcm); ++i) pcm[i] = (u8)(i * 17);
    ok = enqueue(out, net->down[1], format, sizeof(format)) &&
         enqueue(out, net->down[0], video, sizeof(video)) &&
         enqueue(out, net->down[1], pcm, sizeof(pcm));
    WPA_PUT_LE32(format + 8, 2);
    WPA_PUT_BE32(video + 4, 2);
    ok = enqueue(out, net->down[1], format, sizeof(format)) &&
         enqueue(out, net->down[0], video, sizeof(video)) && ok;
    ziiu_net_poll(net);
    printf("bridge order:");
    for (i = 0; i < received.count && i < 8; ++i)
        printf(" %u/%u", received.ports[i], (unsigned)received.lengths[i]);
    puts("");
    ok = ok && received.count == 7 && received.ports[0] == 50121 &&
         received.lengths[0] == sizeof(pcm) &&
         !memcmp(received.payloads[0], pcm, sizeof(pcm));
    for (i = 1; i <= 3; ++i)
        ok = ok && received.ports[2*i-1] == 50121 && received.lengths[2*i-1] == 32 &&
             WPA_GET_LE32(received.payloads[2*i-1] + 8) == i &&
             received.ports[2*i] == 50120 && WPA_GET_BE32(received.payloads[2*i] + 4) == i;
    ok = ok && net->video_format_count == 0 && net->video_format_missing == 0;
    puts(ok ? "PASS: timing header precedes video; full PCM payload preserved" :
              "FAIL: timing header must precede video, with PCM preserved");
    closesocket(out);
    ziiu_net_deinit(net);
    WSACleanup();
    return ok ? 0 : 1;
}

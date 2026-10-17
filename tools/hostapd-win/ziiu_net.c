/*
 * Minimal IPv4 side of the console for the one GamePad on the link.
 *
 * - DHCP server with a single lease (the GamePad).
 * - ARP replies for the console address only.
 * - Everything else is counted and logged (first packet per UDP port pair).
 * - Bridge for the console-side service (tools/ziiu-drc, a libdrc port): UDP from
 *   the GamePad to the console address goes to 127.0.0.1:<same port>; datagrams
 *   sent to 127.0.0.1:50120/50121/50123 (video, audio, commands) go to the
 *   GamePad on that port. The chip's TSF (Wi-Fi clock, used for A/V timestamps)
 *   is published to 127.0.0.1:50190 every 100 ms.
 * - Two TX queues: control (ARP, DHCP, commands) always goes first; bulk (video,
 *   audio) is short so a slow link drops stream packets instead of delaying the
 *   control traffic behind them. The bridge only forwards to the GamePad once it
 *   holds its DHCP lease (it gives up on a network that never answers DHCP).
 *
 * Addresses: libdrc's convention is console 192.168.1.10, GamePad 192.168.1.11.
 * A GamePad that remembers its lease asks for its old address in DHCPREQUEST
 * (option 50); the log shows it, and we ACK it if it is in the console's /24.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include "includes.h"

#include "common.h"
#include "ziiu_net.h"

#define ZIIU_IP(a, b, c, d) (((u32) (a) << 24) | ((u32) (b) << 16) | ((u32) (c) << 8) | (u32) (d))
#define ZIIU_CONSOLE_IP ZIIU_IP(192, 168, 1, 10)
#define ZIIU_PAD_IP ZIIU_IP(192, 168, 1, 11)
#define ZIIU_NETMASK ZIIU_IP(255, 255, 255, 0)
#define ZIIU_LEASE_S 86400
#define ZIIU_MAX_FLOWS 24
#define ZIIU_TXQ_CTRL 64                 /* packets waiting for a TX ring slot */
#define ZIIU_TXQ_BULK 64
#define ZIIU_TSF_PORT 50190
#define ZIIU_DOWN_MAX 1700    /* largest datagram to the GamePad: astrm audio 8 + 1664 */
/* vid, aud, cmd, status (the Wii U's one-way message every ~1 s from 50025) */
static const u16 ziiu_down_ports[4] = { 50120, 50121, 50123, 50125 };

#define ETH_P_IP 0x0800
#define ETH_P_ARP 0x0806

struct ziiu_flow {
	u32 dst_ip;
	u16 sport, dport;
	unsigned long packets, bytes;
};

struct ziiu_txq_ent {
	u16 ethertype;
	u16 len;
	u8 data[1800];            /* IP packet: 20 + 8 + up to ZIIU_DOWN_MAX */
};

struct ziiu_txq {
	struct ziiu_txq_ent *ent;
	unsigned int cap, head, count;
	unsigned long full;
};

enum { ZIIU_Q_CTRL, ZIIU_Q_BULK };

struct ziiu_net {
	u8 own[ETH_ALEN], peer[ETH_ALEN];
	ziiu_net_tx_fn tx;
	void *ctx;
	u32 console_ip, pad_ip;
	u16 ip_id;
	int link_up, bound;
	struct ziiu_flow flows[ZIIU_MAX_FLOWS];
	unsigned long arp, dhcp, tx_ok, tx_fail, other_ip, other_type, malformed;
	SOCKET up, down[4];
	unsigned long up_fwd, down_fwd[4], down_dropped, down_unbound, tx_busy_waits;
	struct ziiu_txq q[2];
	/* Timing headers wait for their matching frame, never for an arbitrary
	 * socket-drain order. Bounded to the same number of entries as bulk TX. */
	u8 video_formats[ZIIU_TXQ_BULK][32];
	unsigned int video_format_count;
	unsigned long video_format_missing;
};

static const char * ip_str(u32 ip, char *buf)
{
	os_snprintf(buf, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
	return buf;
}

static u32 csum_add(u32 sum, const u8 *p, size_t len)
{
	while (len > 1) {
		sum += WPA_GET_BE16(p);
		p += 2;
		len -= 2;
	}
	if (len)
		sum += (u32) p[0] << 8;
	return sum;
}

static u16 csum_fold(u32 sum)
{
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (u16) ~sum;
}

/* Send queued packets, control first, until the driver's TX ring is full (-2). */
static void ziiu_net_flush(struct ziiu_net *net)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(net->q); i++) {
		struct ziiu_txq *q = &net->q[i];

		while (q->count) {
			struct ziiu_txq_ent *e = &q->ent[q->head];
			int r = net->tx(net->ctx, net->peer, e->ethertype, e->data, e->len);

			if (r == -2) {
				net->tx_busy_waits++;
				return;
			}
			if (r < 0)
				net->tx_fail++;
			else
				net->tx_ok++;
			q->head = (q->head + 1) % q->cap;
			q->count--;
		}
	}
}

static int ziiu_net_send(struct ziiu_net *net, int queue, u16 ethertype, const u8 *body,
			 size_t len)
{
	struct ziiu_txq *q = &net->q[queue];
	struct ziiu_txq_ent *e;

	if (len > sizeof(e->data))
		return -1;
	if (q->count == q->cap) {
		q->full++;
		return -1;
	}
	e = &q->ent[(q->head + q->count) % q->cap];
	e->ethertype = ethertype;
	e->len = (u16) len;
	os_memcpy(e->data, body, len);
	q->count++;
	ziiu_net_flush(net);
	return 0;
}

static int ziiu_udp_send(struct ziiu_net *net, int queue, u32 dst_ip, u16 sport, u16 dport,
			 const u8 *payload, size_t len)
{
	u8 *p;
	u8 pseudo[12];
	u32 sum;
	size_t total = 20 + 8 + len;
	int ret;

	p = os_zalloc(total);
	if (!p)
		return -1;
	p[0] = 0x45;
	/* IP TOS as the owner's Wii U marks its flows (decrypted capture): video 0xa0,
	 * audio 0xe0, commands and the status message 0x20. */
	p[1] = dport == 50120 ? 0xa0 : dport == 50121 ? 0xe0 : dport >= 50123 && dport <= 50125 ? 0x20 : 0;
	WPA_PUT_BE16(p + 2, (u16) total);
	WPA_PUT_BE16(p + 4, net->ip_id++);
	p[8] = 64;
	p[9] = 17;
	WPA_PUT_BE32(p + 12, net->console_ip);
	WPA_PUT_BE32(p + 16, dst_ip);
	WPA_PUT_BE16(p + 10, csum_fold(csum_add(0, p, 20)));

	WPA_PUT_BE16(p + 20, sport);
	WPA_PUT_BE16(p + 22, dport);
	WPA_PUT_BE16(p + 24, (u16) (8 + len));
	os_memcpy(p + 28, payload, len);
	os_memcpy(pseudo, p + 12, 8);
	pseudo[8] = 0;
	pseudo[9] = 17;
	WPA_PUT_BE16(pseudo + 10, (u16) (8 + len));
	sum = csum_add(csum_add(0, pseudo, sizeof(pseudo)), p + 20, 8 + len);
	sum = csum_fold(sum);
	WPA_PUT_BE16(p + 26, sum ? sum : 0xffff);

	ret = ziiu_net_send(net, queue, ETH_P_IP, p, total);
	os_free(p);
	return ret;
}

/* --- ARP ----------------------------------------------------------------- */

static void ziiu_arp_rx(struct ziiu_net *net, const u8 *p, size_t len)
{
	u8 r[28];
	u16 op;
	u32 sip, tip;
	char a[16], b[16];

	if (len < 28 || WPA_GET_BE16(p) != 1 || WPA_GET_BE16(p + 2) != ETH_P_IP ||
	    p[4] != 6 || p[5] != 4) {
		net->malformed++;
		return;
	}
	net->arp++;
	op = WPA_GET_BE16(p + 6);
	sip = WPA_GET_BE32(p + 14);
	tip = WPA_GET_BE32(p + 24);
	wpa_printf(MSG_INFO, "ziiu-net: ARP op=%u sender=%s target=%s", op,
		   ip_str(sip, a), ip_str(tip, b));
	if (op != 1 || tip != net->console_ip || os_memcmp(p + 8, net->peer, ETH_ALEN) != 0)
		return;
	os_memcpy(r, p, 6);
	r[6] = 0;
	r[7] = 2;
	os_memcpy(r + 8, net->own, ETH_ALEN);
	WPA_PUT_BE32(r + 14, net->console_ip);
	os_memcpy(r + 18, p + 8, 10);
	ziiu_net_send(net, ZIIU_Q_CTRL, ETH_P_ARP, r, sizeof(r));
	wpa_printf(MSG_INFO, "ziiu-net: ARP reply %s is-at " MACSTR,
		   ip_str(net->console_ip, a), MAC2STR(net->own));
}

/* --- DHCP ---------------------------------------------------------------- */

static const char * dhcp_type_str(int t)
{
	static const char *names[] = { "?", "DISCOVER", "OFFER", "REQUEST", "DECLINE",
				       "ACK", "NAK", "RELEASE", "INFORM" };
	return t > 0 && t < (int) ARRAY_SIZE(names) ? names[t] : "?";
}

static void ziiu_dhcp_reply(struct ziiu_net *net, const u8 *req, int type, u32 yiaddr)
{
	u8 b[300];
	u8 *o = b + 240;
	u32 ciaddr = WPA_GET_BE32(req + 12), dst;
	u16 flags = WPA_GET_BE16(req + 10);
	char a[16], c[16];

	os_memset(b, 0, sizeof(b));
	b[0] = 2;
	b[1] = 1;
	b[2] = 6;
	os_memcpy(b + 4, req + 4, 4);             /* xid */
	WPA_PUT_BE16(b + 10, flags);
	if (type == 5)
		WPA_PUT_BE32(b + 12, ciaddr);
	if (type != 6)
		WPA_PUT_BE32(b + 16, yiaddr);
	os_memcpy(b + 24, req + 24, 4 + 16);      /* giaddr, chaddr */
	WPA_PUT_BE32(b + 236, 0x63825363);
	*o++ = 53; *o++ = 1; *o++ = (u8) type;
	*o++ = 54; *o++ = 4; WPA_PUT_BE32(o, net->console_ip); o += 4;
	if (type != 6) {
		*o++ = 51; *o++ = 4; WPA_PUT_BE32(o, ZIIU_LEASE_S); o += 4;
		*o++ = 1; *o++ = 4; WPA_PUT_BE32(o, ZIIU_NETMASK); o += 4;
	}
	*o++ = 255;

	/* RFC 2131 4.1. Layer 2 is always unicast to the GamePad (pairwise key). */
	if (type == 6 || (!ciaddr && (flags & 0x8000)))
		dst = 0xffffffff;
	else if (ciaddr)
		dst = ciaddr;
	else
		dst = yiaddr;
	if (ziiu_udp_send(net, ZIIU_Q_CTRL, dst, 67, 68, b, sizeof(b)) != 0)
		return;
	if (type == 5 && !net->bound) {
		net->bound = 1;
		wpa_printf(MSG_INFO, "ziiu-net: GamePad bound to %s, bridge to GamePad open",
			   ip_str(yiaddr, a));
	}
	wpa_printf(MSG_INFO, "ziiu-net: DHCP %s yiaddr=%s to %s", dhcp_type_str(type),
			   ip_str(type == 6 ? 0 : yiaddr, a), ip_str(dst, c));
}

static void ziiu_dhcp_rx(struct ziiu_net *net, const u8 *p, size_t len)
{
	int type = 0;
	u32 req_ip = 0, server = 0, ciaddr, ip;
	char params[64 * 4] = "", host[64] = "", a[16], b[16], c[16];
	size_t i;

	if (len < 240 || p[0] != 1 || p[1] != 1 || p[2] != 6 ||
	    WPA_GET_BE32(p + 236) != 0x63825363) {
		net->malformed++;
		return;
	}
	if (os_memcmp(p + 28, net->peer, ETH_ALEN) != 0)
		return;
	net->dhcp++;
	ciaddr = WPA_GET_BE32(p + 12);
	for (i = 240; i < len && p[i] != 255;) {
		u8 opt = p[i], olen;

		if (opt == 0) {
			i++;
			continue;
		}
		if (i + 2 > len || i + 2 + p[i + 1] > len)
			break;
		olen = p[i + 1];
		if (opt == 53 && olen == 1)
			type = p[i + 2];
		else if (opt == 50 && olen == 4)
			req_ip = WPA_GET_BE32(p + i + 2);
		else if (opt == 54 && olen == 4)
			server = WPA_GET_BE32(p + i + 2);
		else if (opt == 12)
			os_memcpy(host, p + i + 2, olen < sizeof(host) - 1 ? olen : sizeof(host) - 1);
		else if (opt == 55) {
			size_t k, at = 0;

			for (k = 0; k < olen && at + 5 < sizeof(params); k++)
				at += os_snprintf(params + at, sizeof(params) - at, "%s%u",
						  k ? "," : "", p[i + 2 + k]);
		}
		i += 2 + olen;
	}
	wpa_printf(MSG_INFO, "ziiu-net: DHCP %s from GamePad xid=%08x flags=%04x ciaddr=%s "
		   "requested=%s server=%s params=[%s] host='%s'",
		   dhcp_type_str(type), WPA_GET_BE32(p + 4), WPA_GET_BE16(p + 10),
		   ip_str(ciaddr, a), ip_str(req_ip, b), ip_str(server, c), params, host);

	switch (type) {
	case 1: /* DISCOVER */
		ip = req_ip && (req_ip & ZIIU_NETMASK) == (net->console_ip & ZIIU_NETMASK) &&
			req_ip != net->console_ip ? req_ip : net->pad_ip;
		ziiu_dhcp_reply(net, p, 2, ip);
		break;
	case 3: /* REQUEST */
		if (server && server != net->console_ip) {
			wpa_printf(MSG_INFO, "ziiu-net: DHCP REQUEST for another server, ignored");
			break;
		}
		ip = req_ip ? req_ip : ciaddr;
		if (!ip || (ip & ZIIU_NETMASK) != (net->console_ip & ZIIU_NETMASK) ||
		    ip == net->console_ip) {
			ziiu_dhcp_reply(net, p, 6, 0);
			break;
		}
		net->pad_ip = ip;
		ziiu_dhcp_reply(net, p, 5, ip);
		break;
	default:
		break;
	}
}

/* --- IPv4 ------------------------------------------------------------------ */

static void ziiu_flow_note(struct ziiu_net *net, u32 dst_ip, u16 sport, u16 dport,
			   const u8 *payload, size_t len)
{
	struct ziiu_flow *f = NULL;
	size_t i;
	char a[16];

	for (i = 0; i < ZIIU_MAX_FLOWS; i++) {
		if (net->flows[i].packets && net->flows[i].sport == sport &&
		    net->flows[i].dport == dport && net->flows[i].dst_ip == dst_ip) {
			f = &net->flows[i];
			break;
		}
		if (!net->flows[i].packets && !f)
			f = &net->flows[i];
	}
	if (!f)
		return;
	if (!f->packets) {
		f->dst_ip = dst_ip;
		f->sport = sport;
		f->dport = dport;
		wpa_printf(MSG_INFO, "ziiu-net: first UDP %u -> %s:%u len=%u",
			   sport, ip_str(dst_ip, a), dport, (unsigned) len);
		wpa_hexdump(MSG_INFO, "ziiu-net: first bytes", payload, len < 48 ? len : 48);
	}
	f->packets++;
	f->bytes += len;
}

static void ziiu_ip_rx(struct ziiu_net *net, const u8 *p, size_t len)
{
	size_t ihl, total;
	u32 dst;
	u16 sport, dport, ulen;
	char a[16];

	if (len < 20 || (p[0] >> 4) != 4) {
		net->malformed++;
		return;
	}
	ihl = (p[0] & 15) * 4;
	total = WPA_GET_BE16(p + 2);
	if (ihl < 20 || total < ihl || total > len) {
		net->malformed++;
		return;
	}
	dst = WPA_GET_BE32(p + 16);
	if (p[9] != 17) {
		if (net->other_ip++ < 8)
			wpa_printf(MSG_INFO, "ziiu-net: IPv4 proto %u to %s len=%u",
				   p[9], ip_str(dst, a), (unsigned) total);
		return;
	}
	if (total < ihl + 8) {
		net->malformed++;
		return;
	}
	sport = WPA_GET_BE16(p + ihl);
	dport = WPA_GET_BE16(p + ihl + 2);
	ulen = WPA_GET_BE16(p + ihl + 4);
	if (ulen < 8 || ihl + ulen > total) {
		net->malformed++;
		return;
	}
	if (sport == 68 && dport == 67) {
		ziiu_dhcp_rx(net, p + ihl + 8, ulen - 8);
		return;
	}
	ziiu_flow_note(net, dst, sport, dport, p + ihl + 8, ulen - 8);
	if (dst == net->console_ip && net->up != INVALID_SOCKET) {
		struct sockaddr_in to;

		os_memset(&to, 0, sizeof(to));
		to.sin_family = AF_INET;
		to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		to.sin_port = htons(dport);
		if (sendto(net->up, (const char *) p + ihl + 8, ulen - 8, 0,
			   (struct sockaddr *) &to, sizeof(to)) >= 0)
			net->up_fwd++;
	}
}

void ziiu_net_rx(struct ziiu_net *net, const u8 *src, const u8 *dst, u16 ethertype,
		 const u8 *body, size_t len)
{
	if (!net || os_memcmp(src, net->peer, ETH_ALEN) != 0)
		return;
	if (ethertype == ETH_P_ARP)
		ziiu_arp_rx(net, body, len);
	else if (ethertype == ETH_P_IP)
		ziiu_ip_rx(net, body, len);
	else if (net->other_type++ < 8)
		wpa_printf(MSG_INFO, "ziiu-net: ethertype 0x%04x len=%u to " MACSTR,
			   ethertype, (unsigned) len, MAC2STR(dst));
}

void ziiu_net_report(struct ziiu_net *net)
{
	char a[16], b[16], c[16];
	size_t i;

	if (!net)
		return;
	wpa_printf(MSG_INFO, "ziiu-net: console=%s pad=%s arp=%lu dhcp=%lu tx=%lu/%lu "
		   "other_ip=%lu other_type=%lu malformed=%lu",
		   ip_str(net->console_ip, a), ip_str(net->pad_ip, b), net->arp, net->dhcp,
		   net->tx_ok, net->tx_ok + net->tx_fail, net->other_ip, net->other_type,
		   net->malformed);
	wpa_printf(MSG_INFO, "ziiu-net: bridge up=%lu down vid=%lu aud=%lu cmd=%lu status=%lu dropped=%lu "
		   "before_lease=%lu txq ctrl=%u/full %lu bulk=%u/full %lu busy_waits=%lu",
		   net->up_fwd, net->down_fwd[0], net->down_fwd[1], net->down_fwd[2],
		   net->down_fwd[3], net->down_dropped, net->down_unbound, net->q[ZIIU_Q_CTRL].count,
		   net->q[ZIIU_Q_CTRL].full, net->q[ZIIU_Q_BULK].count, net->q[ZIIU_Q_BULK].full,
		   net->tx_busy_waits);
	wpa_printf(MSG_INFO, "ziiu-net: video timing pending=%u missing=%lu",
		   net->video_format_count, net->video_format_missing);
	for (i = 0; i < ZIIU_MAX_FLOWS; i++) {
		struct ziiu_flow *f = &net->flows[i];

		if (f->packets)
			wpa_printf(MSG_INFO, "ziiu-net:   udp %u -> %s:%u packets=%lu bytes=%lu",
				   f->sport, ip_str(f->dst_ip, c), f->dport, f->packets, f->bytes);
	}
}

/* Localhost-only, non-blocking UDP socket; port 0 = any. ICMP "port unreachable"
 * from an absent service must not break later receives (SIO_UDP_CONNRESET). */
static SOCKET ziiu_local_socket(u16 port)
{
	SOCKET sk = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	struct sockaddr_in sin;
	u_long nb = 1;
	BOOL off = FALSE;
	DWORD got = 0;

	if (sk == INVALID_SOCKET)
		return sk;
	os_memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sin.sin_port = htons(port);
	WSAIoctl(sk, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &got, NULL, NULL);
	if (ioctlsocket(sk, FIONBIO, &nb) != 0 ||
	    bind(sk, (struct sockaddr *) &sin, sizeof(sin)) != 0) {
		wpa_printf(MSG_ERROR, "ziiu-net: cannot bind 127.0.0.1:%u (%d)", port,
			   WSAGetLastError());
		closesocket(sk);
		return INVALID_SOCKET;
	}
	return sk;
}

void ziiu_net_set_link(struct ziiu_net *net, int up)
{
	if (!net)
		return;
	if (net->link_up && !up && net->bound) {
		net->bound = 0;
		net->video_format_count = 0;
		wpa_printf(MSG_INFO, "ziiu-net: link down, bridge to GamePad closed");
	}
	net->link_up = up;
}

/* Drain one service socket. The audio socket also carries each video's timing
 * header. An encoder submits that header before its video packets, but draining
 * all of port 50120 first used to reverse the order at the radio boundary. */
static void ziiu_net_drain(struct ziiu_net *net, size_t i)
{
	u8 buf[2048];
	int n, budget = 128;

	if (net->down[i] == INVALID_SOCKET)
		return;
	while (budget-- > 0) {
		struct sockaddr_in from;
		int fl = sizeof(from);

		n = recvfrom(net->down[i], (char *) buf, sizeof(buf), 0,
			     (struct sockaddr *) &from, &fl);
		if (n < 0) {
			if (WSAGetLastError() == WSAEMSGSIZE) {
				net->down_dropped++;  /* truncated: never forward */
				continue;
			}
			break;
		}
		if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK) || n > ZIIU_DOWN_MAX) {
			net->down_dropped++;  /* vstrm 1416, astrm audio 1672 */
			continue;
		}
		if (!net->link_up || !net->bound) {
			net->down_unbound++;
			continue;
		}
		/* Recheck after receiving each video packet: the next frame's timing
		 * header may have arrived while we were transmitting this batch. */
		if (i == 0)
			ziiu_net_drain(net, 1);
		if (i == 1 && n == 32 && (buf[0] & 4)) {
			if (net->video_format_count == ZIIU_TXQ_BULK) {
				memmove(net->video_formats, net->video_formats + 1,
					(ZIIU_TXQ_BULK - 1) * 32);
				net->video_format_count--;
				net->down_dropped++;
			}
			memcpy(net->video_formats[net->video_format_count++], buf, 32);
			continue;
		}
		if (i == 0 && n >= 16 && (buf[2] & 0x40)) {
			unsigned int f;
			for (f = 0; f < net->video_format_count; f++)
				if (WPA_GET_LE32(net->video_formats[f] + 8) == WPA_GET_BE32(buf + 4))
					break;
			if (f < net->video_format_count) {
				if (ziiu_udp_send(net, ZIIU_Q_BULK, net->pad_ip, 50021, 50121,
						  net->video_formats[f], 32) == 0)
					net->down_fwd[1]++;
				else
					net->down_dropped++;
				if (f + 1 < net->video_format_count)
					memmove(net->video_formats[f], net->video_formats[f + 1],
						(net->video_format_count - f - 1) * 32);
				net->video_format_count--;
			} else {
				net->video_format_missing++;
			}
		}
		if (ziiu_udp_send(net, i >= 2 ? ZIIU_Q_CTRL : ZIIU_Q_BULK, net->pad_ip,
				  ziiu_down_ports[i] - 100,
				  ziiu_down_ports[i], buf, n) == 0)
			net->down_fwd[i]++;
		else
			net->down_dropped++;
	}
}

/* Service -> GamePad. Called from the driver's 1 ms poll after RX was drained. */
void ziiu_net_poll(struct ziiu_net *net)
{
	if (!net)
		return;
	ziiu_net_drain(net, 1); /* PCM goes now; timing headers wait for matching video */
	ziiu_net_drain(net, 0);
	ziiu_net_drain(net, 2);
	ziiu_net_drain(net, 3);
	ziiu_net_flush(net);
}

/* Wi-Fi TSF for the service's A/V timestamps: "ZTSF", tsf, QPC, QPC frequency. */
void ziiu_net_publish_tsf(struct ziiu_net *net, u64 tsf)
{
	u8 m[28];
	LARGE_INTEGER qpc, qpf;
	struct sockaddr_in to;

	if (!net || net->up == INVALID_SOCKET)
		return;
	QueryPerformanceCounter(&qpc);
	QueryPerformanceFrequency(&qpf);
	os_memcpy(m, "ZTSF", 4);
	WPA_PUT_LE64(m + 4, tsf);
	WPA_PUT_LE64(m + 12, (u64) qpc.QuadPart);
	WPA_PUT_LE64(m + 20, (u64) qpf.QuadPart);
	os_memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	to.sin_port = htons(ZIIU_TSF_PORT);
	sendto(net->up, (const char *) m, sizeof(m), 0, (struct sockaddr *) &to, sizeof(to));
}

struct ziiu_net * ziiu_net_init(const u8 *own_mac, const u8 *peer_mac,
				ziiu_net_tx_fn tx, void *ctx, int bridge)
{
	struct ziiu_net *net = os_zalloc(sizeof(*net));
	size_t i;

	if (!net)
		return NULL;
	net->q[ZIIU_Q_CTRL].cap = ZIIU_TXQ_CTRL;
	net->q[ZIIU_Q_BULK].cap = ZIIU_TXQ_BULK;
	for (i = 0; i < ARRAY_SIZE(net->q); i++) {
		net->q[i].ent = os_calloc(net->q[i].cap, sizeof(struct ziiu_txq_ent));
		if (!net->q[i].ent) {
			os_free(net->q[0].ent);
			os_free(net);
			return NULL;
		}
	}
	os_memcpy(net->own, own_mac, ETH_ALEN);
	os_memcpy(net->peer, peer_mac, ETH_ALEN);
	net->tx = tx;
	net->ctx = ctx;
	net->console_ip = ZIIU_CONSOLE_IP;
	net->pad_ip = ZIIU_PAD_IP;
	net->up = INVALID_SOCKET;
	for (i = 0; i < ARRAY_SIZE(net->down); i++)
		net->down[i] = INVALID_SOCKET;
	if (bridge) {
		net->up = ziiu_local_socket(0);
		for (i = 0; i < ARRAY_SIZE(net->down); i++)
			net->down[i] = ziiu_local_socket(ziiu_down_ports[i]);
		wpa_printf(MSG_INFO, "ziiu-net: bridge on 127.0.0.1 (GamePad UDP <-> local ports, "
			   "TSF to %u)", ZIIU_TSF_PORT);
	}
	return net;
}

void ziiu_net_deinit(struct ziiu_net *net)
{
	size_t i;

	if (!net)
		return;
	if (net->up != INVALID_SOCKET)
		closesocket(net->up);
	for (i = 0; i < ARRAY_SIZE(net->down); i++)
		if (net->down[i] != INVALID_SOCKET)
			closesocket(net->down[i]);
	for (i = 0; i < ARRAY_SIZE(net->q); i++)
		os_free(net->q[i].ent);
	os_free(net);
}

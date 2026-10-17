/*
 * Minimal IPv4 side of the console, for the one GamePad on the link:
 * DHCP server, ARP replies for the console address, and a log of the UDP
 * traffic the GamePad sends (the libdrc services come later).
 */
#ifndef ZIIU_NET_H
#define ZIIU_NET_H

/* Sends one LLC/SNAP payload (ethertype + body) to dst over the encrypted link.
 * Returns -2 when the driver's TX ring is full (the packet stays queued). */
typedef int (*ziiu_net_tx_fn)(void *ctx, const u8 *dst, u16 ethertype,
			      const u8 *body, size_t len);

struct ziiu_net;

/* bridge: also relay UDP to/from 127.0.0.1 for tools/ziiu-drc. */
struct ziiu_net * ziiu_net_init(const u8 *own_mac, const u8 *peer_mac,
				ziiu_net_tx_fn tx, void *ctx, int bridge);
void ziiu_net_set_link(struct ziiu_net *net, int up);
void ziiu_net_poll(struct ziiu_net *net);
void ziiu_net_publish_tsf(struct ziiu_net *net, u64 tsf);
void ziiu_net_deinit(struct ziiu_net *net);
void ziiu_net_rx(struct ziiu_net *net, const u8 *src, const u8 *dst, u16 ethertype,
		 const u8 *body, size_t len);
void ziiu_net_report(struct ziiu_net *net);

#endif /* ZIIU_NET_H */

/*
 * hostapd driver backend for the ZiiUScreen MT7925 KMDF driver (Windows).
 *
 * ziiuctl brings the chip up as an AP (firmware, BSS, beacon offload) and starts
 * hostapd with an inherited driver handle. This backend only moves frames:
 * RX_READ records -> hostapd events, hostapd frames -> TX_FRAME. hostapd itself
 * answers probes, authentication and association and runs the (TENDONIN) WPA2
 * 4-way handshake.
 *
 * Environment (set by ziiuctl ap-hostapd):
 *   ZIIU_HANDLE   inherited driver handle value
 *   ZIIU_PEER     the saved GamePad MAC; every other sender is ignored
 *   ZIIU_LOG_DIR  where the generated beacon is written for offline comparison
 *
 * Encryption is done here in software (CCMP, IEEE 802.11 12.5.3), not in the chip:
 * the chip has no keys and sends/receives protected frames as opaque bytes, the
 * same path mt76 uses for ciphers without hardware support. Decrypted data goes to
 * ziiu_net.c (DHCP/ARP/UDP for the GamePad).
 */

#include "includes.h"
#include <windows.h>
#include <winioctl.h>
#include <mmsystem.h>

#include "common.h"
#include "eloop.h"
#include "driver.h"
#include "common/ieee802_11_defs.h"
#include "crypto/aes.h"
#include "crypto/aes_wrap.h"
#include "ziiu_wifi_ioctl.h"
#include "ziiu_net.h"

#define ZIIU_STA_WLAN_IDX 1       /* firmware station entry programmed in ziiu_station() */
#define ZIIU_POLL_US 1000
#define ZIIU_RX_BUF (256 * 1024)
#define ZIIU_UNKNOWN_REPORT_MS 2000

struct ziiu_drv {
	void *ctx;
	HANDLE h;
	u8 bssid[ETH_ALEN];
	u8 peer[ETH_ALEN];
	u16 seq;
	u8 mcu_seq;
	u8 *rx;
	char log_dir[MAX_PATH];
	DWORD last_unknown, last_mic_log, last_tsf;
	unsigned long rx_records, rx_frames, rx_ignored, tx_frames, tx_failed, tx_busy;
	/* Software CCMP. Key material is never logged. */
	u8 tk[16], gtk[16];
	int tk_set, gtk_set, gtk_idx;
	u64 tx_pn, gtk_pn;
	u64 rx_pn[17];            /* per TID, [16] = non-QoS */
	unsigned long rx_decrypted, rx_mic_fail, rx_replay, rx_cleartext_dropped, rx_null, rx_amsdu;
	struct ziiu_net *net;
	int dry;                  /* ZIIU_DRY: no radio, ziiu_send captures instead */
	u8 *dry_out;
	size_t dry_out_len;
	/* TX experiment: once the link is up, data frames rotate through the TX flag
	 * combinations (10 s each) so one run compares them. */
	int tx_mode, tx_mode_reports, tx_rotate;
	ZIIU_TX_STATS tx_last;
	u64 tx_call_ticks, tx_call_max, tx_calls;  /* QPC time inside IOCTL_ZIIU_TX_FRAME */
	u64 tsf_val;
	ULONG data_rate;          /* ZIIU_TX_F_RATE_SET(i) for encrypted data, ZIIU_DATA_RATE */
	unsigned long tsf_reads, tsf_fail, tsf_zero;
};

/* Order: known-good first; the AC queue last, in case it stalls the tokens. */
static const ULONG ziiu_tx_modes[] = {
	0, ZIIU_TX_F_NO_STATUS, ZIIU_TX_F_AC_QUEUE, ZIIU_TX_F_AC_QUEUE | ZIIU_TX_F_NO_STATUS
};

struct ziiu_tx_done {
	struct ziiu_drv *drv;
	ZIIU_TX_STATS before;
	size_t len;
	u8 frame[];
};

static int ziiu_ioctl(struct ziiu_drv *drv, DWORD code, const void *in, DWORD in_len,
		      void *out, DWORD out_len, DWORD *got)
{
	DWORD n = 0;

	if (!DeviceIoControl(drv->h, code, (void *) in, in_len, out, out_len, &n, NULL)) {
		DWORD err = GetLastError();

		if (err == ERROR_BUSY)        /* TX ring full: caller retries later */
			return -2;
		wpa_printf(MSG_ERROR, "ziiu: ioctl 0x%lx failed: %lu", (unsigned long) code, err);
		return -1;
	}
	if (got)
		*got = n;
	return 0;
}

static int ziiu_tx_stats(struct ziiu_drv *drv, ZIIU_TX_STATS *stats)
{
	DWORD got = 0;

	os_memset(stats, 0, sizeof(*stats));
	return ziiu_ioctl(drv, IOCTL_ZIIU_TX_STATS, NULL, 0, stats, sizeof(*stats), &got) ||
		got != sizeof(*stats) ? -1 : 0;
}

/* Firmware station record for the GamePad (wlan index 1). Byte-for-byte the
 * message tools/gamepad-link/connect.py Radio.station() sends: STA_REC basic,
 * HT/VHT off, PHY, state, single-link MLD mapping, header translation off. */
static int ziiu_station(struct ziiu_drv *drv, const u8 *addr)
{
	u8 msg[48 + 8 + 128], ev[16384];
	u8 *p = msg + 48;
	size_t len;
	ZIIU_MCU_CMD *cmd;
	u8 buf[sizeof(ZIIU_MCU_CMD) + sizeof(msg)];
	DWORD got = 0;

#define TLV(tag, body_len) do { WPA_PUT_LE16(p, tag); WPA_PUT_LE16(p + 2, (body_len) + 4); p += 4; } while (0)
	os_memset(msg, 0, sizeof(msg));
	/* payload header: bss 0, wlan idx lo 1, tlv count, append, muar, idx hi */
	p[0] = 0; p[1] = ZIIU_STA_WLAN_IDX; WPA_PUT_LE16(p + 2, 6); p[4] = 1;
	p += 8;
	TLV(0, 16);                               /* STA_REC_BASIC */
	WPA_PUT_LE32(p, 0x10001); p[4] = 2; p[5] = 0; WPA_PUT_LE16(p + 6, 1);
	os_memcpy(p + 8, addr, ETH_ALEN); WPA_PUT_LE16(p + 14, 3); p += 16;
	TLV(0x15, 8);                             /* STA_REC_PHY */
	WPA_PUT_LE16(p, 0x15); p[2] = 8; p += 8;
	TLV(1, 12);                               /* STA_REC_RA */
	WPA_PUT_LE16(p, 0xff0); p += 12;
	TLV(7, 12);                               /* STA_REC_STATE */
	p[0] = 1; p += 12;
	TLV(0x20, 24);                            /* STA_REC_MLD (single link) */
	os_memcpy(p, addr, ETH_ALEN); WPA_PUT_LE16(p + 6, 1); WPA_PUT_LE16(p + 10, 1);
	p[12] = 1; WPA_PUT_LE16(p + 16, 1); p += 24;
	TLV(0x2b, 4);                             /* STA_REC_HDR_TRANS */
	p[0] = 1; p[2] = 1; p += 4;
#undef TLV
	len = p - msg;

	drv->mcu_seq = drv->mcu_seq % 15 + 1;
	WPA_PUT_LE32(msg, (u32) len | (2u << 23) | (0x20u << 25));
	WPA_PUT_LE32(msg + 4, 1u << 14);
	WPA_PUT_LE16(msg + 32, (u16) (len - 32));
	WPA_PUT_LE16(msg + 34, 3);
	msg[37] = 0xa0;
	msg[39] = drv->mcu_seq;
	msg[43] = 7;

	cmd = (ZIIU_MCU_CMD *) buf;
	cmd->Flags = 1;
	cmd->Seq = drv->mcu_seq;
	cmd->TimeoutMs = 3000;
	cmd->Length = (ULONG) len;
	os_memcpy(buf + sizeof(*cmd), msg, len);
	if (ziiu_ioctl(drv, IOCTL_ZIIU_MCU_CMD, buf, (DWORD) (sizeof(*cmd) + len),
		       ev, sizeof(ev), &got) < 0)
		return -1;
	if (got < 52 || WPA_GET_LE32(ev + 48) != 0) {
		wpa_printf(MSG_ERROR, "ziiu: firmware refused station setup");
		return -1;
	}
	wpa_printf(MSG_INFO, "ziiu: station " MACSTR " programmed (wlan %d)",
		   MAC2STR(addr), ZIIU_STA_WLAN_IDX);
	return 0;
}


/* --- CCMP ------------------------------------------------------------------
 * Adapted from hostapd's wlantest/ccmp.c (BSD, Jouni Malinen) for the frames
 * this link carries: 3-address data or QoS data, no fragments, no MFP. */

static void ziiu_ccmp_aad_nonce(const u8 *hdr, const u8 *ccmp, u8 *aad, size_t *aad_len,
				u8 *nonce)
{
	u16 fc = WPA_GET_LE16(hdr);
	int qos = WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_DATA && (WLAN_FC_GET_STYPE(fc) & 0x08);
	u8 *pos;

	nonce[0] = qos ? (hdr[24] & 0x0f) : 0;
	if (WLAN_FC_GET_TYPE(fc) == WLAN_FC_TYPE_DATA) {
		fc &= ~0x0070;    /* subtype bits */
		if (qos)
			fc &= ~WLAN_FC_ORDER;
	}
	fc &= ~(WLAN_FC_RETRY | WLAN_FC_PWRMGT | WLAN_FC_MOREDATA);
	fc |= WLAN_FC_ISWEP;
	WPA_PUT_LE16(aad, fc);
	os_memcpy(aad + 2, hdr + 4, 3 * ETH_ALEN);
	WPA_PUT_LE16(aad + 20, WPA_GET_LE16(hdr + 22) & 0x000f);  /* fragment number only */
	pos = aad + 22;
	if (qos) {
		*pos++ = hdr[24] & 0x0f;  /* TID; EOSP, ack policy, A-MSDU bit masked */
		*pos++ = 0;
	}
	*aad_len = pos - aad;
	os_memcpy(nonce + 1, hdr + 10, ETH_ALEN);
	nonce[7] = ccmp[7];
	nonce[8] = ccmp[6];
	nonce[9] = ccmp[5];
	nonce[10] = ccmp[4];
	nonce[11] = ccmp[1];
	nonce[12] = ccmp[0];
}

static u64 ziiu_ccmp_pn(const u8 *ccmp)
{
	return (u64) ccmp[0] | ((u64) ccmp[1] << 8) | ((u64) ccmp[4] << 16) |
		((u64) ccmp[5] << 24) | ((u64) ccmp[6] << 32) | ((u64) ccmp[7] << 40);
}

/* Returns a new buffer: header (protected bit set) + CCMP header + data + MIC. */
static u8 * ziiu_ccmp_encrypt(const u8 *tk, const u8 *frame, size_t len, size_t hdrlen,
			      u64 pn, int keyid, size_t *out_len)
{
	u8 aad[30], nonce[13], *out, *c;
	size_t aad_len, plen = len - hdrlen;

	out = os_malloc(len + 16 + AES_BLOCK_SIZE);
	if (!out)
		return NULL;
	os_memcpy(out, frame, hdrlen);
	WPA_PUT_LE16(out, WPA_GET_LE16(out) | WLAN_FC_ISWEP);
	c = out + hdrlen;
	c[0] = (u8) pn;
	c[1] = (u8) (pn >> 8);
	c[2] = 0;
	c[3] = 0x20 | (keyid << 6);   /* ExtIV */
	c[4] = (u8) (pn >> 16);
	c[5] = (u8) (pn >> 24);
	c[6] = (u8) (pn >> 32);
	c[7] = (u8) (pn >> 40);
	ziiu_ccmp_aad_nonce(out, c, aad, &aad_len, nonce);
	if (aes_ccm_ae(tk, 16, nonce, 8, frame + hdrlen, plen, aad, aad_len,
		       c + 8, c + 8 + plen) < 0) {
		os_free(out);
		return NULL;
	}
	*out_len = hdrlen + 8 + plen + 8;
	return out;
}

/* plain must hold len bytes. */
static int ziiu_ccmp_decrypt(const u8 *tk, const u8 *frame, size_t len, size_t hdrlen,
			     u8 *plain, size_t *plain_len)
{
	u8 aad[30], nonce[13];
	size_t aad_len, mlen;

	if (len < hdrlen + 16 || !(frame[hdrlen + 3] & 0x20))
		return -1;
	mlen = len - hdrlen - 16;
	ziiu_ccmp_aad_nonce(frame, frame + hdrlen, aad, &aad_len, nonce);
	if (aes_ccm_ad(tk, 16, nonce, 8, frame + hdrlen + 8, mlen, aad, aad_len,
		       frame + hdrlen + 8 + mlen, plain) < 0)
		return -1;
	*plain_len = mlen;
	return 0;
}

/* IEEE Std 802.11-2012 M.6.4 CCMP test vector (expected MPDU checked against the
 * vector's own FCS 1d 99 f0 66). Run at every start: no radio use with broken crypto. */
static int ziiu_ccmp_selftest(void)
{
	static const u8 tk[16] = { 0xc9, 0x7c, 0x1f, 0x67, 0xce, 0x37, 0x11, 0x85,
				   0x51, 0x4a, 0x8a, 0x19, 0xf2, 0xbd, 0xd5, 0x2f };
	static const u8 frame[44] = {
		0x08, 0x48, 0xc3, 0x2c, 0x0f, 0xd2, 0xe1, 0x28, 0xa5, 0x7c, 0x50, 0x30,
		0xf1, 0x84, 0x44, 0x08, 0xab, 0xae, 0xa5, 0xb8, 0xfc, 0xba, 0x80, 0x33,
		0xf8, 0xba, 0x1a, 0x55, 0xd0, 0x2f, 0x85, 0xae, 0x96, 0x7b, 0xb6, 0x2f,
		0xb6, 0xcd, 0xa8, 0xeb, 0x7e, 0x78, 0xa0, 0x50 };
	static const u8 expect[36] = {
		0x0c, 0xe7, 0x00, 0x20, 0x76, 0x97, 0x03, 0xb5,
		0xf3, 0xd0, 0xa2, 0xfe, 0x9a, 0x3d, 0xbf, 0x23, 0x42, 0xa6, 0x43, 0xe4,
		0x32, 0x46, 0xe8, 0x0c, 0x3c, 0x04, 0xd0, 0x19,
		0x78, 0x45, 0xce, 0x0b, 0x16, 0xf9, 0x76, 0x23 };
	u8 plain[64];
	u8 *enc;
	size_t enc_len = 0, plain_len = 0;
	int ok;

	enc = ziiu_ccmp_encrypt(tk, frame, sizeof(frame), 24, 0xB5039776E70CULL, 0, &enc_len);
	ok = enc && enc_len == 60 && os_memcmp(enc, frame, 24) == 0 &&
		os_memcmp(enc + 24, expect, sizeof(expect)) == 0 &&
		ziiu_ccmp_decrypt(tk, enc, enc_len, 24, plain, &plain_len) == 0 &&
		plain_len == 20 && os_memcmp(plain, frame + 24, 20) == 0;
	if (ok) {
		enc[30] ^= 1;     /* a corrupted frame must fail the MIC */
		ok = ziiu_ccmp_decrypt(tk, enc, enc_len, 24, plain, &plain_len) < 0;
	}
	os_free(enc);
	wpa_printf(ok ? MSG_INFO : MSG_ERROR, "ziiu: CCMP self-test (802.11 M.6.4) %s",
		   ok ? "passed" : "FAILED");
	return ok ? 0 : -1;
}

static void ziiu_tx_done(void *eloop_ctx, void *user_ctx)
{
	struct ziiu_tx_done *done = eloop_ctx;
	struct ziiu_drv *drv = done->drv;
	const struct ieee80211_hdr *hdr = (const struct ieee80211_hdr *) done->frame;
	ZIIU_TX_STATS after;
	union wpa_event_data event;
	u16 fc = le_to_host16(hdr->frame_control);
	int ack = 1;

	/* One frame in flight at a time in practice. If firmware has not reported
	 * yet, assume delivery so hostapd's state machine is not stalled by a
	 * missing report; the log keeps the difference visible. */
	if (ziiu_tx_stats(drv, &after) == 0 && after.Reports != done->before.Reports)
		ack = after.Acked != done->before.Acked;
	wpa_printf(MSG_DEBUG, "ziiu: tx status stype=%u ack=%d (reports %lu->%lu acked %lu->%lu status 0x%lx)",
		   WLAN_FC_GET_STYPE(fc), ack,
		   (unsigned long) done->before.Reports, (unsigned long) after.Reports,
		   (unsigned long) done->before.Acked, (unsigned long) after.Acked,
		   (unsigned long) after.LastStatus);

	os_memset(&event, 0, sizeof(event));
	event.tx_status.type = WLAN_FC_GET_TYPE(fc);
	event.tx_status.stype = WLAN_FC_GET_STYPE(fc);
	event.tx_status.dst = hdr->addr1;
	event.tx_status.data = done->frame;
	event.tx_status.data_len = done->len;
	event.tx_status.ack = ack;
	wpa_supplicant_event(drv->ctx, EVENT_TX_STATUS, &event);
	os_free(done);
}

/* Send one frame (already encrypted if protected). Our sequence number. */
static int ziiu_send(struct ziiu_drv *drv, const u8 *frame, size_t len, int report)
{
	u8 *buf;
	ZIIU_TX_FRAME *tx;
	struct ziiu_tx_done *done = NULL;
	int ret;
	u16 seq_before;

	if (len < 24 || len > ZIIU_TX_FRAME_MAX)
		return -1;
	if (drv->dry) {
		os_free(drv->dry_out);
		drv->dry_out = os_malloc(len);
		if (!drv->dry_out)
			return -1;
		os_memcpy(drv->dry_out, frame, len);
		drv->dry_out_len = len;
		return 0;
	}
	buf = os_zalloc(sizeof(*tx) + len);
	if (!buf)
		return -1;
	tx = (ZIIU_TX_FRAME *) buf;
	tx->Length = (ULONG) len;
	tx->WlanIndex = ZIIU_STA_WLAN_IDX;
	if (drv->tk_set && (frame[0] & 0x0c) == 0x08 && !report)
		tx->Flags = ziiu_tx_modes[drv->tx_mode] | drv->data_rate;
	os_memcpy(buf + sizeof(*tx), frame, len);
	drv->seq = (drv->seq + 1) & 0xfff;
	WPA_PUT_LE16(buf + sizeof(*tx) + 22, drv->seq << 4);
	seq_before = (drv->seq - 1) & 0xfff;

	if (report) {
		done = os_zalloc(sizeof(*done) + len);
		if (done) {
			done->drv = drv;
			done->len = len;
			os_memcpy(done->frame, buf + sizeof(*tx), len);
			ziiu_tx_stats(drv, &done->before);
		}
	}
	{
		LARGE_INTEGER t0, t1;

		QueryPerformanceCounter(&t0);
		ret = ziiu_ioctl(drv, IOCTL_ZIIU_TX_FRAME, buf, (DWORD) (sizeof(*tx) + len), NULL, 0, NULL);
		QueryPerformanceCounter(&t1);
		drv->tx_calls++;
		drv->tx_call_ticks += t1.QuadPart - t0.QuadPart;
		if ((u64) (t1.QuadPart - t0.QuadPart) > drv->tx_call_max)
			drv->tx_call_max = t1.QuadPart - t0.QuadPart;
	}
	os_free(buf);
	if (ret < 0) {
		if (ret == -2) {
			drv->tx_busy++;
			drv->seq = seq_before;
		} else {
			drv->tx_failed++;
		}
		os_free(done);
		return ret;
	}
	drv->tx_frames++;
	if (done)
		eloop_register_timeout(0, 30000, ziiu_tx_done, done, NULL);
	return 0;
}

/* One LLC/SNAP data frame from the AP to the GamePad, CCMP-encrypted with the
 * pairwise key when encrypt is set. Plain (non-QoS) data, like the EAPOL frames
 * the GamePad already accepted. */
static int ziiu_send_data(struct ziiu_drv *drv, const u8 *dst, u16 ethertype,
			  const u8 *body, size_t body_len, int encrypt)
{
	static const u8 llc[6] = { 0xaa, 0xaa, 0x03, 0, 0, 0 };
	u8 *frame, *enc = NULL;
	size_t len = 24 + 8 + body_len, enc_len = 0;
	int ret;

	if (os_memcmp(dst, drv->peer, ETH_ALEN) != 0 || (encrypt && !drv->tk_set))
		return -1;
	frame = os_zalloc(len);
	if (!frame)
		return -1;
	WPA_PUT_LE16(frame, IEEE80211_FC(WLAN_FC_TYPE_DATA, WLAN_FC_STYPE_DATA) | WLAN_FC_FROMDS);
	os_memcpy(frame + 4, dst, ETH_ALEN);
	os_memcpy(frame + 10, drv->bssid, ETH_ALEN);
	os_memcpy(frame + 16, drv->bssid, ETH_ALEN);
	os_memcpy(frame + 24, llc, sizeof(llc));
	WPA_PUT_BE16(frame + 30, ethertype);
	os_memcpy(frame + 32, body, body_len);
	if (encrypt) {
		enc = ziiu_ccmp_encrypt(drv->tk, frame, len, 24, drv->tx_pn + 1, 0, &enc_len);
		ret = enc ? ziiu_send(drv, enc, enc_len, 0) : -1;
		if (ret == 0)
			drv->tx_pn++;
		os_free(enc);
	} else {
		ret = ziiu_send(drv, frame, len, 0);
	}
	os_free(frame);
	return ret;
}

static int ziiu_net_tx(void *ctx, const u8 *dst, u16 ethertype, const u8 *body, size_t len)
{
	return ziiu_send_data(ctx, dst, ethertype, body, len, 1);
}

/* Block Ack: decline the GamePad's ADDBA request (status 37). It then uses normal
 * acknowledged frames; the chip has no BA session to set up. */
static void ziiu_addba_decline(struct ziiu_drv *drv, const u8 *req, size_t len)
{
	u8 frame[24 + 9];
	u16 params = WPA_GET_LE16(req + 27);

	os_memset(frame, 0, sizeof(frame));
	WPA_PUT_LE16(frame, IEEE80211_FC(WLAN_FC_TYPE_MGMT, WLAN_FC_STYPE_ACTION));
	os_memcpy(frame + 4, drv->peer, ETH_ALEN);
	os_memcpy(frame + 10, drv->bssid, ETH_ALEN);
	os_memcpy(frame + 16, drv->bssid, ETH_ALEN);
	frame[24] = 3;                            /* Block Ack */
	frame[25] = 1;                            /* ADDBA response */
	frame[26] = req[26];                      /* dialog token */
	WPA_PUT_LE16(frame + 27, WLAN_STATUS_REQUEST_DECLINED);
	os_memcpy(frame + 29, req + 27, 4);       /* BA parameters, timeout */
	wpa_printf(MSG_INFO, "ziiu: ADDBA request tid=%u buf=%u declined",
		   (params >> 2) & 15, params >> 6);
	ziiu_send(drv, frame, sizeof(frame), 0);
}

/* RXD (mt7925_mac_fill_rx) -> 802.11 frame, as ziiuctl HandleFrame parses it. */
static const u8 * ziiu_rx_frame(const u8 *b, size_t len, size_t *flen, int *rssi)
{
	u32 rxd0, rxd1, rxd2;
	size_t off = 32, end;

	if (len < 32)
		return NULL;
	rxd0 = WPA_GET_LE32(b);
	rxd1 = WPA_GET_LE32(b + 4);
	rxd2 = WPA_GET_LE32(b + 8);
	end = rxd0 & 0xffff;
	if (((rxd0 >> 27) & 0x1f) != 2 || end > len || end < 32 || (rxd2 & (1u << 7)))
		return NULL;
	*rssi = 0;
	if (rxd1 & (1u << 19)) off += 16;
	if (rxd1 & (1u << 16)) off += 16;
	if (rxd1 & (1u << 17)) off += 16;
	if (rxd1 & (1u << 18)) {
		if (off + 16 <= end)
			*rssi = ((int) (WPA_GET_LE32(b + off + 12) & 0xff) - 220) / 2;
		off += 16;
		if (rxd1 & (1u << 20)) off += 96;
	}
	off += 2 * ((rxd2 >> 13) & 7);
	if (off + 24 > end)
		return NULL;
	*flen = end - off;
	return b + off;
}

static void ziiu_report_unknown(struct ziiu_drv *drv, const u8 *f, u16 fc)
{
	union wpa_event_data event;

	/* Data from a station hostapd does not know (e.g. a resumed cached session):
	 * let hostapd decide on a class-3 deauthentication, rate-limited. */
	if (GetTickCount() - drv->last_unknown < ZIIU_UNKNOWN_REPORT_MS)
		return;
	drv->last_unknown = GetTickCount();
	wpa_printf(MSG_DEBUG, "ziiu: rx data from peer (protected=%d) without keys, reporting",
		   !!(fc & WLAN_FC_ISWEP));
	os_memset(&event, 0, sizeof(event));
	event.rx_from_unknown.bssid = f + 4;
	event.rx_from_unknown.addr = f + 10;
	wpa_supplicant_event(drv->ctx, EVENT_RX_FROM_UNKNOWN, &event);
}

/* GamePad -> AP data (ToDS). Protected frames are decrypted here with the
 * pairwise key; after keys exist, cleartext data other than EAPOL is dropped. */
static void ziiu_handle_data(struct ziiu_drv *drv, const u8 *f, size_t len)
{
	static const u8 llc[6] = { 0xaa, 0xaa, 0x03, 0, 0, 0 };
	u16 fc = WPA_GET_LE16(f), stype = WLAN_FC_GET_STYPE(fc), ethertype;
	int qos = (stype & 0x08) != 0;
	size_t hdr = 24 + (qos ? 2 : 0), blen;
	const u8 *body;
	u8 *plain = NULL;
	int tid = qos ? (f[24] & 15) : 16;

	if (stype & 0x04) {               /* (QoS) Null: power-save signalling, no body */
		drv->rx_null++;
		return;
	}
	if (len < hdr)
		return;
	if (fc & WLAN_FC_ISWEP) {
		u64 pn;

		if (!drv->tk_set) {
			ziiu_report_unknown(drv, f, fc);
			return;
		}
		if (len < hdr + 16)
			return;
		pn = ziiu_ccmp_pn(f + hdr);
		plain = os_malloc(len);
		if (!plain || ziiu_ccmp_decrypt(drv->tk, f, len, hdr, plain, &blen) < 0) {
			drv->rx_mic_fail++;
			if (GetTickCount() - drv->last_mic_log >= 1000) {
				drv->last_mic_log = GetTickCount();
				wpa_printf(MSG_INFO, "ziiu: CCMP decrypt failed (len=%u keyid=%u pn=%llu)",
					   (unsigned) len, f[hdr + 3] >> 6, (unsigned long long) pn);
			}
			os_free(plain);
			return;
		}
		if (pn <= drv->rx_pn[tid]) {
			drv->rx_replay++;
			os_free(plain);
			return;
		}
		drv->rx_pn[tid] = pn;
		drv->rx_decrypted++;
		body = plain;
	} else {
		body = f + hdr;
		blen = len - hdr;
	}
	if (qos && (f[24] & 0x80)) {      /* A-MSDU: not expected from the GamePad */
		if (drv->rx_amsdu++ == 0)
			wpa_printf(MSG_INFO, "ziiu: A-MSDU from GamePad ignored");
		goto out;
	}
	if (blen < 8 || os_memcmp(body, llc, sizeof(llc)) != 0)
		goto out;
	ethertype = WPA_GET_BE16(body + 6);
	if (ethertype == ETH_P_EAPOL) {
		wpa_printf(MSG_DEBUG, "ziiu: rx EAPOL len=%u protected=%d",
			   (unsigned) (blen - 8), !!(fc & WLAN_FC_ISWEP));
		drv_event_eapol_rx(drv->ctx, f + 10, body + 8, blen - 8);
	} else if (!(fc & WLAN_FC_ISWEP)) {
		if (drv->tk_set)
			drv->rx_cleartext_dropped++;
		else
			ziiu_report_unknown(drv, f, fc);
	} else {
		ziiu_net_rx(drv->net, f + 10, f + 16, ethertype, body + 8, blen - 8);
	}
out:
	os_free(plain);
}

static void ziiu_handle_frame(struct ziiu_drv *drv, const u8 *f, size_t len, int rssi)
{
	u16 fc = WPA_GET_LE16(f);
	u16 type = WLAN_FC_GET_TYPE(fc), stype = WLAN_FC_GET_STYPE(fc);
	union wpa_event_data event;

	drv->rx_frames++;
	/* GamePad-only radio: nothing from any other sender reaches hostapd. */
	if (os_memcmp(f + 10, drv->peer, ETH_ALEN) != 0) {
		drv->rx_ignored++;
		return;
	}
	if (type == WLAN_FC_TYPE_MGMT) {
		wpa_printf(MSG_DEBUG, "ziiu: rx mgmt stype=%u len=%u rssi=%d",
			   stype, (unsigned) len, rssi);
		if (stype == WLAN_FC_STYPE_ACTION && len >= 25 && f[24] == 3) {
			if (f[25] == 0 && len >= 33)
				ziiu_addba_decline(drv, f, len);
			else
				wpa_printf(MSG_INFO, "ziiu: Block Ack action %u ignored", f[25]);
			return;
		}
		os_memset(&event, 0, sizeof(event));
		event.rx_mgmt.frame = f;
		event.rx_mgmt.frame_len = len;
		event.rx_mgmt.ssi_signal = rssi;
		wpa_supplicant_event(drv->ctx, EVENT_RX_MGMT, &event);
		return;
	}
	if (type == WLAN_FC_TYPE_DATA && (fc & (WLAN_FC_TODS | WLAN_FC_FROMDS)) == WLAN_FC_TODS)
		ziiu_handle_data(drv, f, len);
}

static int ziiu_read_reg(struct ziiu_drv *drv, u32 addr, u32 *val)
{
	ZIIU_POLL_REG req = { addr, 0, 0, 0 };
	ZIIU_POLL_RESULT res = { 0, 0 };
	DWORD got = 0;

	if (ziiu_ioctl(drv, IOCTL_ZIIU_POLL_REG, &req, sizeof(req), &res, sizeof(res), &got) < 0 ||
	    got != sizeof(res))
		return -1;
	*val = res.Value;
	return 0;
}

/* TSF of our BSS (omac 0): latch with LPON_TCR(0, 0) software-read mode, read UTTR0/1.
 * The MT7925 has the connac3 LPON layout of mt7996 (__mt7996_get_tsf: TCR at
 * 0xa8 + 0x40 * n, UTTR at 0x360/0x364), not mt792x_get_tsf's UTTR at 0x080/0x084,
 * which reads 0 on this chip (verified with ziiuctl tsf-probe, 2026-10-15). */
static int ziiu_read_tsf(struct ziiu_drv *drv, u64 *tsf)
{
	ZIIU_WRITE_REG w;
	u32 tcr, lo, hi;

	if (ziiu_read_reg(drv, 0x820eb0a8, &tcr) < 0)
		return -1;
	w.Addr = 0x820eb0a8;
	w.Value = (tcr & ~3u) | 3;
	if (ziiu_ioctl(drv, IOCTL_ZIIU_WRITE_REG, &w, sizeof(w), NULL, 0, NULL) < 0 ||
	    ziiu_read_reg(drv, 0x820eb360, &lo) < 0 || ziiu_read_reg(drv, 0x820eb364, &hi) < 0)
		return -1;
	*tsf = ((u64) hi << 32) | lo;
	return 0;
}

static void ziiu_poll(void *eloop_ctx, void *user_ctx)
{
	struct ziiu_drv *drv = eloop_ctx;
	ZIIU_RX_READ req = { 0 };
	DWORD got = 0, at;
	int rounds;

	for (rounds = 0; rounds < 8; rounds++) {
		if (ziiu_ioctl(drv, IOCTL_ZIIU_RX_READ, &req, sizeof(req), drv->rx, ZIIU_RX_BUF, &got) < 0 ||
		    got == 0)
			break;
		for (at = 0; at + 4 <= got;) {
			u32 n = WPA_GET_LE32(drv->rx + at);
			const u8 *f;
			size_t flen = 0;
			int rssi = 0;

			at += 4;
			if (n > got - at)
				break;
			drv->rx_records++;
			f = ziiu_rx_frame(drv->rx + at, n, &flen, &rssi);
			if (f)
				ziiu_handle_frame(drv, f, flen, rssi);
			at += (n + 3) & ~3u;
		}
	}
	ziiu_net_set_link(drv->net, drv->tk_set);
	ziiu_net_poll(drv->net);
	if (GetTickCount() - drv->last_tsf >= 100) {
		u64 tsf;

		drv->last_tsf = GetTickCount();
		if (ziiu_read_tsf(drv, &tsf) == 0) {
			drv->tsf_reads++;
			drv->tsf_zero += tsf == 0;
			drv->tsf_val = tsf;
			ziiu_net_publish_tsf(drv->net, tsf);
		} else {
			drv->tsf_fail++;
		}
	}
	/* ziiuctl ends hostapd at its time limit; keep the redirected log complete. */
	fflush(stdout);
	eloop_register_timeout(0, ZIIU_POLL_US, ziiu_poll, drv, NULL);
}

static void ziiu_report(void *eloop_ctx, void *user_ctx)
{
	struct ziiu_drv *drv = eloop_ctx;
	ZIIU_TX_STATS tx;
	/* MT_MIB_SDR36 band 0 TX airtime: advances while beacons go out. */
	ZIIU_POLL_REG reg = { 0x820ed054, 0, 0, 0 };
	ZIIU_POLL_RESULT airtime = { 0, 0 };

	ziiu_tx_stats(drv, &tx);
	ziiu_ioctl(drv, IOCTL_ZIIU_POLL_REG, &reg, sizeof(reg), &airtime, sizeof(airtime), NULL);
	wpa_printf(MSG_INFO, "ziiu: rx records=%lu frames=%lu ignored=%lu tx sent=%lu failed=%lu busy=%lu "
		   "fw queued=%lu released=%lu reports=%lu acked=%lu tx_airtime=%lu",
		   drv->rx_records, drv->rx_frames, drv->rx_ignored, drv->tx_frames, drv->tx_failed,
		   drv->tx_busy,
		   (unsigned long) tx.Queued, (unsigned long) tx.Released,
		   (unsigned long) tx.Reports, (unsigned long) tx.Acked, (unsigned long) airtime.Value);
	if (tx.Reports && tx.Released)
		wpa_printf(MSG_INFO, "ziiu: txs attempts avg=%.2f max=%lu retried=%lu delay avg=%lu max=%lu "
			   "ps=%lu queue_timeout=%lu ack_timeout=%lu last=0x%08lx; submit->free avg=%lu us max=%lu us",
			   (double) tx.TxCountSum / tx.Reports, (unsigned long) tx.TxCountMax,
			   (unsigned long) tx.Retried, (unsigned long) (tx.DelaySum / tx.Reports),
			   (unsigned long) tx.DelayMax, (unsigned long) tx.PsFlag,
			   (unsigned long) tx.QueueTimeout, (unsigned long) tx.AckTimeout,
			   (unsigned long) tx.LastStatus, (unsigned long) (tx.FreeUsSum / tx.Released),
			   (unsigned long) tx.FreeUsMax);
	if (drv->tk_set || drv->rx_mic_fail)
		wpa_printf(MSG_INFO, "ziiu: ccmp decrypted=%lu mic_fail=%lu replay=%lu cleartext_dropped=%lu "
			   "null=%lu amsdu=%lu tx_pn=%llu",
			   drv->rx_decrypted, drv->rx_mic_fail, drv->rx_replay, drv->rx_cleartext_dropped,
			   drv->rx_null, drv->rx_amsdu, (unsigned long long) drv->tx_pn);
	if (drv->tx_calls) {
		LARGE_INTEGER f;

		QueryPerformanceFrequency(&f);
		wpa_printf(MSG_INFO, "ziiu: TX ioctl calls=%llu avg=%llu us max=%llu us",
			   (unsigned long long) drv->tx_calls,
			   (unsigned long long) (drv->tx_call_ticks * 1000000 / f.QuadPart / drv->tx_calls),
			   (unsigned long long) (drv->tx_call_max * 1000000 / f.QuadPart));
		drv->tx_calls = drv->tx_call_ticks = drv->tx_call_max = 0;
	}
	wpa_printf(MSG_INFO, "ziiu: tsf=%llu reads=%lu zero=%lu failed=%lu",
		   (unsigned long long) drv->tsf_val, drv->tsf_reads, drv->tsf_zero, drv->tsf_fail);
	if (drv->tk_set) {
		ULONG rel = tx.Released - drv->tx_last.Released;

		wpa_printf(MSG_INFO, "ziiu: tx mode %d (%s queue, %s): last 5 s sent=%lu submit->free avg=%lu us "
			   "airtime=%lu us",
			   drv->tx_mode, ziiu_tx_modes[drv->tx_mode] & ZIIU_TX_F_AC_QUEUE ? "AC" : "ALTX0",
			   ziiu_tx_modes[drv->tx_mode] & ZIIU_TX_F_NO_STATUS ? "no TXS" : "TXS",
			   (unsigned long) rel,
			   rel ? (unsigned long) ((tx.FreeUsSum - drv->tx_last.FreeUsSum) / rel) : 0ul,
			   (unsigned long) airtime.Value);
		if (drv->tx_rotate && ++drv->tx_mode_reports == 2 &&
		    drv->tx_mode + 1 < (int) ARRAY_SIZE(ziiu_tx_modes)) {
			drv->tx_mode++;
			drv->tx_mode_reports = 0;
		}
	}
	drv->tx_last = tx;
	ziiu_net_report(drv->net);
	eloop_register_timeout(5, 0, ziiu_report, drv, NULL);
}

/* --- Dry-run loopback ---------------------------------------------------------
 * ZIIU_DRY=1 only, no radio: a fake GamePad (QoS data, ToDS, CCMP with the M.6.4
 * test key) sends DHCP DISCOVER, REQUEST and an ARP request through the real RX
 * path; the replies ziiu_send captured are decrypted and checked. */

static u16 ziiu_dry_csum(const u8 *p, size_t len, u32 sum)
{
	for (; len > 1; p += 2, len -= 2)
		sum += WPA_GET_BE16(p);
	if (len)
		sum += (u32) p[0] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return (u16) sum;
}

/* Feed one LLC payload from the fake GamePad; return the decrypted reply body
 * (after LLC) in out, or -1 if nothing valid came back. */
static int ziiu_dry_exchange(struct ziiu_drv *drv, u16 ethertype, const u8 *body, size_t len,
			     u64 pn, u8 *out, size_t *out_len, u16 *out_type)
{
	u8 frame[512], plain[512], *enc;
	size_t enc_len = 0, plen = 0;

	os_memset(frame, 0, 34);
	WPA_PUT_LE16(frame, IEEE80211_FC(WLAN_FC_TYPE_DATA, WLAN_FC_STYPE_QOS_DATA) | WLAN_FC_TODS);
	os_memcpy(frame + 4, drv->bssid, ETH_ALEN);
	os_memcpy(frame + 10, drv->peer, ETH_ALEN);
	os_memset(frame + 16, 0xff, ETH_ALEN);
	WPA_PUT_LE16(frame + 22, 0x0120);
	frame[24] = 6;                            /* TID 6 */
	os_memcpy(frame + 26, "\xaa\xaa\x03\x00\x00\x00", 6);
	WPA_PUT_BE16(frame + 32, ethertype);
	os_memcpy(frame + 34, body, len);
	enc = ziiu_ccmp_encrypt(drv->tk, frame, 34 + len, 26, pn, 0, &enc_len);
	if (!enc)
		return -1;
	os_free(drv->dry_out);
	drv->dry_out = NULL;
	ziiu_handle_frame(drv, enc, enc_len, -40);
	os_free(enc);
	if (!drv->dry_out || drv->dry_out_len < 24 + 16 + 8 ||
	    WPA_GET_LE16(drv->dry_out) != (IEEE80211_FC(WLAN_FC_TYPE_DATA, WLAN_FC_STYPE_DATA) |
					   WLAN_FC_FROMDS | WLAN_FC_ISWEP) ||
	    os_memcmp(drv->dry_out + 4, drv->peer, ETH_ALEN) != 0 ||
	    ziiu_ccmp_decrypt(drv->tk, drv->dry_out, drv->dry_out_len, 24, plain, &plen) < 0 ||
	    plen < 8 || plen - 8 > *out_len)
		return -1;
	*out_type = WPA_GET_BE16(plain + 6);
	*out_len = plen - 8;
	os_memcpy(out, plain + 8, plen - 8);
	return 0;
}

static u8 * ziiu_dry_dhcp(u8 *ip, int type, u32 req_ip, u32 server, const u8 *mac, size_t *len)
{
	u8 *b = ip + 28, *o = b + 240;

	os_memset(ip, 0, 28 + 300);
	b[0] = 1; b[1] = 1; b[2] = 6;
	WPA_PUT_BE32(b + 4, 0x1234abcd);
	WPA_PUT_BE16(b + 10, 0x8000);             /* broadcast flag */
	os_memcpy(b + 28, mac, ETH_ALEN);
	WPA_PUT_BE32(b + 236, 0x63825363);
	*o++ = 53; *o++ = 1; *o++ = (u8) type;
	if (req_ip) { *o++ = 50; *o++ = 4; WPA_PUT_BE32(o, req_ip); o += 4; }
	if (server) { *o++ = 54; *o++ = 4; WPA_PUT_BE32(o, server); o += 4; }
	*o++ = 55; *o++ = 2; *o++ = 1; *o++ = 3;
	*o++ = 255;
	*len = 28 + 300;
	ip[0] = 0x45;
	WPA_PUT_BE16(ip + 2, (u16) *len);
	ip[8] = 64; ip[9] = 17;
	WPA_PUT_BE32(ip + 16, 0xffffffff);
	WPA_PUT_BE16(ip + 10, (u16) ~ziiu_dry_csum(ip, 20, 0));
	WPA_PUT_BE16(ip + 20, 68);
	WPA_PUT_BE16(ip + 22, 67);
	WPA_PUT_BE16(ip + 24, 8 + 300);
	return ip;
}

/* Checks a DHCP reply: valid IP + UDP checksums, 67->68, yiaddr and message type. */
static int ziiu_dry_check_dhcp(const u8 *p, size_t len, int type, u32 yiaddr)
{
	u8 pseudo[12];
	size_t ulen;

	if (len < 28 + 244 || p[0] != 0x45 || ziiu_dry_csum(p, 20, 0) != 0xffff || p[9] != 17)
		return -1;
	ulen = WPA_GET_BE16(p + 24);
	os_memcpy(pseudo, p + 12, 8);
	pseudo[8] = 0; pseudo[9] = 17;
	WPA_PUT_BE16(pseudo + 10, (u16) ulen);
	if (ulen + 20 != len || ziiu_dry_csum(p + 20, ulen, ziiu_dry_csum(pseudo, 12, 0)) != 0xffff ||
	    WPA_GET_BE16(p + 20) != 67 || WPA_GET_BE16(p + 22) != 68 || p[28] != 2 ||
	    WPA_GET_BE32(p + 28 + 4) != 0x1234abcd || WPA_GET_BE32(p + 28 + 16) != yiaddr ||
	    p[28 + 240] != 53 || p[28 + 242] != type)
		return -1;
	return 0;
}

static int ziiu_dry_loopback(struct ziiu_drv *drv)
{
	static const u8 tk[16] = { 0xc9, 0x7c, 0x1f, 0x67, 0xce, 0x37, 0x11, 0x85,
				   0x51, 0x4a, 0x8a, 0x19, 0xf2, 0xbd, 0xd5, 0x2f };
	u8 in[400], out[512], arp[28];
	size_t in_len, out_len;
	u16 type;
	int ok = 0;
	const u32 console = 0xc0a8010a, pad = 0xc0a8010b;

	os_memcpy(drv->tk, tk, 16);
	drv->tk_set = 1;
	drv->net = ziiu_net_init(drv->bssid, drv->peer, ziiu_net_tx, drv, 0);
	if (!drv->net)
		goto done;

	ziiu_dry_dhcp(in, 1, 0, 0, drv->peer, &in_len);
	out_len = sizeof(out);
	if (ziiu_dry_exchange(drv, 0x0800, in, in_len, 1, out, &out_len, &type) < 0 ||
	    type != 0x0800 || ziiu_dry_check_dhcp(out, out_len, 2, pad) < 0)
		goto done;
	out_len = sizeof(out);
	if (ziiu_dry_exchange(drv, 0x0800, in, in_len, 1, out, &out_len, &type) == 0 ||
	    drv->rx_replay != 1)
		goto done;                        /* replayed PN must be dropped */
	ziiu_dry_dhcp(in, 3, pad, console, drv->peer, &in_len);
	out_len = sizeof(out);
	if (ziiu_dry_exchange(drv, 0x0800, in, in_len, 2, out, &out_len, &type) < 0 ||
	    type != 0x0800 || ziiu_dry_check_dhcp(out, out_len, 5, pad) < 0)
		goto done;
	os_memset(arp, 0, sizeof(arp));
	WPA_PUT_BE16(arp, 1); WPA_PUT_BE16(arp + 2, 0x0800); arp[4] = 6; arp[5] = 4;
	WPA_PUT_BE16(arp + 6, 1);
	os_memcpy(arp + 8, drv->peer, ETH_ALEN);
	WPA_PUT_BE32(arp + 14, pad);
	WPA_PUT_BE32(arp + 24, console);
	out_len = sizeof(out);
	if (ziiu_dry_exchange(drv, 0x0806, arp, sizeof(arp), 3, out, &out_len, &type) < 0 ||
	    type != 0x0806 || out_len != 28 || WPA_GET_BE16(out + 6) != 2 ||
	    os_memcmp(out + 8, drv->bssid, ETH_ALEN) != 0 || WPA_GET_BE32(out + 14) != console ||
	    os_memcmp(out + 18, drv->peer, ETH_ALEN) != 0 || WPA_GET_BE32(out + 24) != pad)
		goto done;
	ok = drv->rx_decrypted == 3 && drv->rx_mic_fail == 0;
done:
	wpa_printf(ok ? MSG_INFO : MSG_ERROR,
		   "ziiu: dry loopback (CCMP QoS RX -> DHCP OFFER/ACK, replay drop, ARP -> CCMP TX) %s",
		   ok ? "passed" : "FAILED");
	ziiu_net_deinit(drv->net);
	drv->net = NULL;
	drv->tk_set = 0;
	os_memset(drv->tk, 0, sizeof(drv->tk));
	os_memset(drv->rx_pn, 0, sizeof(drv->rx_pn));
	drv->rx_decrypted = drv->rx_replay = 0;
	return ok ? 0 : -1;
}

static void ziiu_dry_done(void *eloop_ctx, void *user_ctx)
{
	eloop_terminate();
}

static void * ziiu_hapd_init(struct hostapd_data *hapd, struct wpa_init_params *params)
{
	struct ziiu_drv *drv;
	const char *handle = getenv("ZIIU_HANDLE"), *peer = getenv("ZIIU_PEER");
	const char *dir = getenv("ZIIU_LOG_DIR");
	ZIIU_INFO info;
	DWORD got = 0;

	const char *dry = getenv("ZIIU_DRY");

	if (ziiu_ccmp_selftest() < 0)
		return NULL;
	if (dry && dry[0] == '1' && params->bssid) {
		/* Offline check: no radio. Only builds the beacon (set_ap) for comparison. */
		drv = os_zalloc(sizeof(*drv));
		if (!drv)
			return NULL;
		drv->ctx = hapd;
		drv->dry = 1;
		os_memcpy(drv->bssid, params->bssid, ETH_ALEN);
		if (!peer || hwaddr_aton(peer, drv->peer) < 0)
			os_memcpy(drv->peer, "\x02\x00\x00\x00\x00\x01", ETH_ALEN);
		if (dir)
			os_strlcpy(drv->log_dir, dir, sizeof(drv->log_dir));
		if (ziiu_dry_loopback(drv) < 0) {
			os_free(drv->dry_out);
			os_free(drv);
			return NULL;
		}
		os_memcpy(params->own_addr, drv->bssid, ETH_ALEN);
		wpa_printf(MSG_INFO, "ziiu: dry run, no radio access; exiting in 1 s");
		eloop_register_timeout(1, 0, ziiu_dry_done, NULL, NULL);
		return drv;
	}
	if (!handle || !peer || !params->bssid) {
		wpa_printf(MSG_ERROR, "ziiu: needs ZIIU_HANDLE, ZIIU_PEER and bssid= (run via ziiuctl ap-hostapd)");
		return NULL;
	}
	drv = os_zalloc(sizeof(*drv));
	if (!drv)
		return NULL;
	drv->ctx = hapd;
	drv->h = (HANDLE) (uintptr_t) _strtoui64(handle, NULL, 10);
	os_memcpy(drv->bssid, params->bssid, ETH_ALEN);
	if (hwaddr_aton(peer, drv->peer) < 0) {
		wpa_printf(MSG_ERROR, "ziiu: bad ZIIU_PEER");
		goto fail;
	}
	if (dir)
		os_strlcpy(drv->log_dir, dir, sizeof(drv->log_dir));
	if (ziiu_ioctl(drv, IOCTL_ZIIU_GET_INFO, NULL, 0, &info, sizeof(info), &got) < 0 ||
	    got != sizeof(info) || info.InterfaceVersion != ZIIU_WIFI_INTERFACE_VERSION || !info.DmaActive) {
		wpa_printf(MSG_ERROR, "ziiu: driver interface %d with running DMA required",
			   ZIIU_WIFI_INTERFACE_VERSION);
		goto fail;
	}
	drv->rx = os_malloc(ZIIU_RX_BUF);
	drv->net = ziiu_net_init(drv->bssid, drv->peer, ziiu_net_tx, drv, 1);
	if (!drv->rx || !drv->net || ziiu_station(drv, drv->peer) < 0)
		goto fail;
	os_memcpy(params->own_addr, drv->bssid, ETH_ALEN);
	{
		/* Encrypted data rate (OFDM, Mbps). 6 Mbps made keyframes and audio miss
		 * their display deadline; management and EAPOL stay at 6 Mbps. */
		static const int mbps[8] = { 6, 9, 12, 18, 24, 36, 48, 54 };
		const char *r = getenv("ZIIU_DATA_RATE");
		int want = r ? atoi(r) : 24, i;

		for (i = 0; i < 8 && mbps[i] != want; i++)
			;
		if (i == 8)
			i = 4;
		drv->data_rate = ZIIU_TX_F_RATE_SET(i);
		drv->tx_rotate = getenv("ZIIU_TX_ROTATE") && getenv("ZIIU_TX_ROTATE")[0] == '1';
		wpa_printf(MSG_INFO, "ziiu: data rate %d Mbps OFDM%s", mbps[i],
			   drv->tx_rotate ? ", rotating TX modes" : "");
	}
	timeBeginPeriod(1);
	eloop_register_timeout(0, 0, ziiu_poll, drv, NULL);
	eloop_register_timeout(5, 0, ziiu_report, drv, NULL);
	wpa_printf(MSG_INFO, "ziiu: AP " MACSTR ", accepting frames only from " MACSTR,
		   MAC2STR(drv->bssid), MAC2STR(drv->peer));
	return drv;
fail:
	ziiu_net_deinit(drv->net);
	os_free(drv->rx);
	os_free(drv);
	return NULL;
}

static void ziiu_hapd_deinit(void *priv)
{
	struct ziiu_drv *drv = priv;

	eloop_cancel_timeout(ziiu_dry_done, NULL, NULL);
	eloop_cancel_timeout(ziiu_poll, drv, NULL);
	eloop_cancel_timeout(ziiu_report, drv, NULL);
	eloop_cancel_timeout(ziiu_tx_done, ELOOP_ALL_CTX, ELOOP_ALL_CTX);
	if (drv->rx)
		timeEndPeriod(1);
	ziiu_net_deinit(drv->net);
	os_memset(drv->tk, 0, sizeof(drv->tk));
	os_memset(drv->gtk, 0, sizeof(drv->gtk));
	os_free(drv->dry_out);
	os_free(drv->rx);
	os_free(drv);
}

static int ziiu_send_mlme(void *priv, const u8 *data, size_t data_len, int noack,
			  unsigned int freq, const u16 *csa_offs, size_t csa_offs_len)
{
	struct ziiu_drv *drv = priv;

	if (data_len < 24 || os_memcmp(data + 4, drv->peer, ETH_ALEN) != 0) {
		wpa_printf(MSG_DEBUG, "ziiu: not sending mgmt frame to a non-GamePad address");
		return -1;
	}
	wpa_printf(MSG_DEBUG, "ziiu: tx mgmt stype=%u len=%u",
		   WLAN_FC_GET_STYPE(WPA_GET_LE16(data)), (unsigned) data_len);
	return ziiu_send(drv, data, data_len, !noack);
}

static int ziiu_send_eapol(void *priv, const u8 *addr, const u8 *data, size_t data_len,
			   int encrypt, const u8 *own_addr, u32 flags)
{
	struct ziiu_drv *drv = priv;

	/* The 4-way handshake goes out in cleartext; later EAPOL (group key updates)
	 * is protected once the pairwise key is in place. */
	encrypt = encrypt && drv->tk_set;
	wpa_printf(MSG_DEBUG, "ziiu: tx EAPOL len=%u encrypt=%d", (unsigned) data_len, encrypt);
	return ziiu_send_data(drv, addr, ETH_P_EAPOL, data, data_len, encrypt);
}

static int ziiu_send_mgmt_reason(struct ziiu_drv *drv, u16 stype, const u8 *addr, int reason)
{
	u8 frame[26];

	if (os_memcmp(addr, drv->peer, ETH_ALEN) != 0)
		return 0;
	os_memset(frame, 0, sizeof(frame));
	WPA_PUT_LE16(frame, IEEE80211_FC(WLAN_FC_TYPE_MGMT, stype));
	os_memcpy(frame + 4, addr, ETH_ALEN);
	os_memcpy(frame + 10, drv->bssid, ETH_ALEN);
	os_memcpy(frame + 16, drv->bssid, ETH_ALEN);
	WPA_PUT_LE16(frame + 24, (u16) reason);
	wpa_printf(MSG_DEBUG, "ziiu: tx %s reason=%d",
		   stype == WLAN_FC_STYPE_DEAUTH ? "deauth" : "disassoc", reason);
	return ziiu_send(drv, frame, sizeof(frame), 0);
}

static int ziiu_sta_deauth(void *priv, const u8 *own_addr, const u8 *addr, int reason)
{
	return ziiu_send_mgmt_reason(priv, WLAN_FC_STYPE_DEAUTH, addr, reason);
}

static int ziiu_sta_disassoc(void *priv, const u8 *own_addr, const u8 *addr, int reason)
{
	return ziiu_send_mgmt_reason(priv, WLAN_FC_STYPE_DISASSOC, addr, reason);
}

static int ziiu_set_key(const char *ifname, void *priv, enum wpa_alg alg, const u8 *addr,
			int key_idx, int set_tx, const u8 *seq, size_t seq_len,
			const u8 *key, size_t key_len)
{
	struct ziiu_drv *drv = priv;
	int group = !addr || is_broadcast_ether_addr(addr);

	/* Never log key material: only what kind of key it is. */
	wpa_printf(MSG_INFO, "ziiu: set_key alg=%d idx=%d set_tx=%d key_len=%u %s",
		   alg, key_idx, set_tx, (unsigned) key_len, group ? "group" : "pairwise");
	if (!group && os_memcmp(addr, drv->peer, ETH_ALEN) != 0)
		return -1;
	if (alg == WPA_ALG_NONE) {
		if (group) {
			drv->gtk_set = 0;
			os_memset(drv->gtk, 0, sizeof(drv->gtk));
		} else {
			drv->tk_set = 0;
			os_memset(drv->tk, 0, sizeof(drv->tk));
		}
		return 0;
	}
	if (alg != WPA_ALG_CCMP || key_len != 16) {
		wpa_printf(MSG_ERROR, "ziiu: only CCMP-128 is supported");
		return -1;
	}
	if (group) {
		/* Kept for completeness; everything we send is unicast to the GamePad. */
		os_memcpy(drv->gtk, key, 16);
		drv->gtk_idx = key_idx;
		drv->gtk_pn = 0;
		drv->gtk_set = 1;
	} else {
		os_memcpy(drv->tk, key, 16);
		drv->tx_pn = 0;
		os_memset(drv->rx_pn, 0, sizeof(drv->rx_pn));
		drv->tk_set = 1;
	}
	return 0;
}

/* Our transmit PN for the group key: goes into the GTK KDE as the RSC. */
static int ziiu_get_seqnum(const char *ifname, void *priv, const u8 *addr, int idx, u8 *seq)
{
	struct ziiu_drv *drv = priv;
	u64 pn = drv->gtk_set && idx == drv->gtk_idx ? drv->gtk_pn : 0;
	int i;

	for (i = 0; i < 6; i++)
		seq[i] = (u8) (pn >> (8 * i));
	seq[6] = seq[7] = 0;
	return 0;
}

static int ziiu_get_capa(void *priv, struct wpa_driver_capa *capa)
{
	os_memset(capa, 0, sizeof(*capa));
	capa->key_mgmt = WPA_DRIVER_CAPA_KEY_MGMT_WPA2_PSK;
	capa->enc = WPA_DRIVER_CAPA_ENC_CCMP;
	capa->auth = WPA_DRIVER_AUTH_OPEN;
	/* The console's WMM element sets U-APSD (QoS info 0x82). */
	capa->flags = WPA_DRIVER_FLAGS_AP_UAPSD;
	return 0;
}

static struct hostapd_hw_modes * ziiu_get_hw_feature_data(void *priv, u16 *num_modes, u16 *flags)
{
	static const u8 chans[] = { 36, 40, 44, 48, 149, 153, 157, 161, 165 };
	static const int rates[] = { 60, 90, 120, 180, 240, 360, 480, 540 };
	struct hostapd_hw_modes *mode;
	size_t i;

	mode = os_zalloc(sizeof(*mode));
	if (!mode)
		return NULL;
	mode->mode = HOSTAPD_MODE_IEEE80211A;
	mode->num_channels = ARRAY_SIZE(chans);
	mode->channels = os_calloc(mode->num_channels, sizeof(*mode->channels));
	mode->num_rates = ARRAY_SIZE(rates);
	mode->rates = os_calloc(mode->num_rates, sizeof(int));
	if (!mode->channels || !mode->rates) {
		os_free(mode->channels);
		os_free(mode->rates);
		os_free(mode);
		return NULL;
	}
	for (i = 0; i < ARRAY_SIZE(chans); i++) {
		mode->channels[i].chan = chans[i];
		mode->channels[i].freq = 5000 + 5 * chans[i];
		mode->channels[i].max_tx_power = 3;   /* GamePad-only radio: low power */
	}
	os_memcpy(mode->rates, rates, sizeof(rates));
	/* As the console advertises: HT cap info 0x001c (greenfield + SMPS disabled,
	 * the [GF] in hostapd.conf), A-MPDU params 0x19, MCS 0-15. */
	mode->ht_capab = 0x001c;
	mode->a_mpdu_params = 0x19;
	mode->mcs_set[0] = 0xff;
	mode->mcs_set[1] = 0xff;
	*num_modes = 1;
	*flags = 0;
	return mode;
}

/* ziiuctl keeps its verified console-identical beacon on air. Save hostapd's
 * version so the two (and hostapd's probe responses) can be compared offline. */
static int ziiu_set_ap(void *priv, struct wpa_driver_ap_params *params)
{
	struct ziiu_drv *drv = priv;
	char path[MAX_PATH + 32];
	FILE *f;

	if (!drv->log_dir[0])
		return 0;
	os_snprintf(path, sizeof(path), "%s\\hostapd-beacon.bin", drv->log_dir);
	f = fopen(path, "wb");
	if (f) {
		fwrite(params->head, 1, params->head_len, f);
		fwrite(params->tail, 1, params->tail_len, f);
		fclose(f);
		wpa_printf(MSG_INFO, "ziiu: hostapd beacon (%u bytes) saved for comparison; on-air beacon unchanged",
			   (unsigned) (params->head_len + params->tail_len));
	}
	return 0;
}

static int ziiu_sta_add(void *priv, struct hostapd_sta_add_params *params)
{
	wpa_printf(MSG_INFO, "ziiu: sta_add " MACSTR " aid=%u (firmware entry already programmed)",
		   MAC2STR(params->addr), params->aid);
	return 0;
}

static int ziiu_sta_remove(void *priv, const u8 *addr)
{
	return 0;
}

static int ziiu_sta_set_flags(void *priv, const u8 *addr, unsigned int total_flags,
			      unsigned int flags_or, unsigned int flags_and)
{
	return 0;
}

const struct wpa_driver_ops wpa_driver_ziiu_ops = {
	.name = "ziiu",
	.desc = "ZiiUScreen MT7925 KMDF driver (Windows)",
	.hapd_init = ziiu_hapd_init,
	.hapd_deinit = ziiu_hapd_deinit,
	.send_mlme = ziiu_send_mlme,
	.hapd_send_eapol = ziiu_send_eapol,
	.sta_deauth = ziiu_sta_deauth,
	.sta_disassoc = ziiu_sta_disassoc,
	.set_key = ziiu_set_key,
	.get_seqnum = ziiu_get_seqnum,
	.get_capa = ziiu_get_capa,
	.get_hw_feature_data = ziiu_get_hw_feature_data,
	.set_ap = ziiu_set_ap,
	.sta_add = ziiu_sta_add,
	.sta_remove = ziiu_sta_remove,
	.sta_set_flags = ziiu_sta_set_flags,
};

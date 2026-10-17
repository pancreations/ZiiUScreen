// Host-independent helpers for the MT7925 PCIe TX wire format.
// Layout reference: mt76 mt7925/mac.c, mt76_connac3_mac.h, mt76_connac.h.
#pragma once
#ifdef _KERNEL_MODE
typedef UCHAR uint8_t;
typedef ULONG uint32_t;
#else
#include <stdint.h>
#include <stddef.h>
#endif

static uint32_t ZiiuTxLe32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void ZiiuTxPut32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// Locate a complete TXS record in unified MCU TX_DONE raw TLVs.
static size_t ZiiuTxEventStatusOffset(const uint8_t* p, size_t length)
{
    size_t at, found = 0;
    if (length < 48 || (ZiiuTxLe32(p) >> 27) != 7 ||
        p[36] != 0x2d || !(p[38] & 4)) return 0;
    for (at = 48; at < length;) {
        uint32_t tag, size;
        if (length - at < 4) return 0;
        tag = p[at] | ((uint32_t)p[at + 1] << 8);
        size = p[at + 2] | ((uint32_t)p[at + 3] << 8);
        if (size < 4 || size > length - at) return 0;
        if (tag == 1 && size >= 56) found = at + 8;
        at += size;
    }
    return found;
}

// Return a mask of released tokens only for a COMPLETE validated v4/v5 TX-free
// packet. Never release memory from a truncated, future-version or wrong packet.
static uint32_t ZiiuTxFreedMask(const uint8_t* p, size_t length)
{
    uint32_t first, version, total, count = 0, mask = 0;
    size_t at;
    if (length < 8) return 0;
    first = ZiiuTxLe32(p);
    version = (ZiiuTxLe32(p + 4) >> 16) & 15;
    if ((first >> 27) != 6 || (first & 0xffff) != length ||
        (version != 4 && version != 5)) return 0;
    total = (first >> 16) & 1023;
    for (at = 8; count < total && at + 4 <= length; at += 4) {
        uint32_t info = ZiiuTxLe32(p + at), i;
        if (info & 0xc0000000) continue;
        for (i = 0; i < 2; i++) {
            uint32_t token = (info >> (15 * i)) & 0x7fff;
            if (token == 0x7fff) continue;
            if (++count > total) return 0;
            if (token < 16) mask |= (uint32_t)1 << token;
        }
    }
    return count == total ? mask : 0;
}

// TXD (32) + hardware TXP (32). Packet payload stays pinned until TX-free,
// not merely until the descriptor's DMA index advances. PROTECT_FRAME is never set:
// protected frames arrive already CCMP-encrypted by user mode (mt76's software
// crypto path: no hw key, so the TXD carries no PROTECT_FRAME either).
// Queue index for flags & 1 (data only): mt76_connac_lmac_mapping of the TID's AC,
// TID 1,2 BK -> 0; 0,3 BE -> 1; 4,5 VI -> 2; 6,7 VO -> 3. Otherwise ALTX0 (0x10).
static void ZiiuTxBuildHeader(uint8_t* out, const uint8_t* frame, uint32_t length,
                             uint32_t wcid, uint32_t token, uint32_t payloadPa, uint32_t flags)
{
    static const uint8_t tidQueue[8] = { 1, 0, 0, 1, 2, 2, 3, 3 };
    uint32_t i, fc = (uint32_t)frame[0] | ((uint32_t)frame[1] << 8);
    uint32_t multicast = frame[4] & 1;
    uint32_t qos = (fc & 0xfc) == 0x88;   // QoS data: 26-byte header, TID from QoS control
    uint32_t queue = 0x10, rate = 18;   // rate table entry: MT792x basic table, OFDM 6M
    if ((flags & 1) && (fc & 0x0c) == 0x08) queue = qos ? tidQueue[frame[24] & 7] : 1;
    if ((flags & 0x80) && (fc & 0x0c) == 0x08) rate = 18 + ((flags >> 4) & 7);  // OFDM 6..54M
    for (i = 0; i < 64; i++) out[i] = 0;
    ZiiuTxPut32(out, (length + 32) | (queue << 25)); // CT
    ZiiuTxPut32(out + 4, wcid | (2u << 14) | ((qos ? 13u : 12u) << 16) |
                         (qos ? (uint32_t)(frame[24] & 7) << 21 : 0) | (1u << 31));
    ZiiuTxPut32(out + 8, ((fc >> 2) & 3) << 4 | ((fc >> 4) & 15));
    ZiiuTxPut32(out + 12, (15u << 11) | (1u << 28) | (1u << 31) |
                         ((uint32_t)(frame[22] >> 4 | frame[23] << 4) << 16) |
                         (multicast ? 17u : 0)); // fixed sequence, BA disabled
    if (!(flags & 2)) ZiiuTxPut32(out + 20, (token + 3) | (1u << 10)); // request host TX status
    ZiiuTxPut32(out + 24, (rate << 16) | (1u << 4) | 12u); // fixed rate table entry
    out[32] = (uint8_t)token; out[33] = (uint8_t)((token >> 8) | 0x80);
    ZiiuTxPut32(out + 40, payloadPa);
    out[44] = (uint8_t)length; out[45] = (uint8_t)((length >> 8) | 0x80);
}

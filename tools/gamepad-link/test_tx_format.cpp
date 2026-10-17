#include <array>
#include <cstdio>
#include <cstdlib>
#include "../../driver/include/ziiu_tx_format.h"
#define ZIIU_TX_F_AC_QUEUE 0x1u   // ziiu_wifi_ioctl.h (needs windows.h)
#define ZIIU_TX_F_NO_STATUS 0x2u
#define ZIIU_TX_F_ALL 0xf3u
#define ZIIU_TX_F_RATE_SET(i) (0x80u | (((i) & 7u) << 4))
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "check failed line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
int main() {
    std::array<uint8_t, 64> tx{};
    std::array<uint8_t, 30> frame{};
    frame[0] = 0xb0; frame[4] = 2; frame[22] = 0x30; frame[23] = 0x12;
    ZiiuTxBuildHeader(tx.data(), frame.data(), 30, 19, 7, 0x12345040, ZIIU_TX_F_ALL);  // mgmt: flags only drop TXS
    CHECK(ZiiuTxLe32(tx.data()) == (62u | 0x20000000u));
    CHECK(ZiiuTxLe32(tx.data() + 4) == 0x800c8013);
    CHECK(ZiiuTxLe32(tx.data() + 8) == 11);
    CHECK(ZiiuTxLe32(tx.data() + 12) == 0x91237800);
    CHECK(ZiiuTxLe32(tx.data() + 20) == 0);
    ZiiuTxBuildHeader(tx.data(), frame.data(), 30, 19, 7, 0x12345040, 0);
    CHECK(ZiiuTxLe32(tx.data()) == (62u | 0x20000000u));
    CHECK(ZiiuTxLe32(tx.data() + 20) == 0x40a);
    CHECK(ZiiuTxLe32(tx.data() + 24) == 0x0012001c);
    CHECK(ZiiuTxLe32(tx.data() + 32) == 0x8007);
    CHECK(ZiiuTxLe32(tx.data() + 40) == 0x12345040);
    CHECK(ZiiuTxLe32(tx.data() + 44) == 0x801e);
    // QoS data (protected, user-mode CCMP): 26-byte header -> HDR_INFO 13, TID 5.
    std::array<uint8_t, 60> qos{};
    qos[0] = 0x88; qos[1] = 0x42; qos[22] = 0x10; qos[24] = 5;
    ZiiuTxBuildHeader(tx.data(), qos.data(), 60, 1, 2, 0x1000, 0);
    CHECK(ZiiuTxLe32(tx.data() + 4) == (0x800d8001u | (5u << 21)));
    CHECK(ZiiuTxLe32(tx.data() + 8) == 0x28);
    CHECK((ZiiuTxLe32(tx.data() + 12) & 2) == 0);   // never PROTECT_FRAME
    CHECK(ZiiuTxLe32(tx.data()) >> 25 == 0x10);      // ALTX0 by default
    ZiiuTxBuildHeader(tx.data(), qos.data(), 60, 1, 2, 0x1000, ZIIU_TX_F_AC_QUEUE);
    CHECK(ZiiuTxLe32(tx.data()) == (92u | (2u << 25)));  // TID 5 -> VI (AC02)
    CHECK(ZiiuTxLe32(tx.data() + 20) == 0x405);
    qos[0] = 0x08; qos[24] = 0;                          // non-QoS data -> BE (AC01)
    ZiiuTxBuildHeader(tx.data(), qos.data(), 60, 1, 2, 0x1000, ZIIU_TX_F_AC_QUEUE | ZIIU_TX_F_NO_STATUS);
    CHECK(ZiiuTxLe32(tx.data()) == (92u | (1u << 25)));
    CHECK(ZiiuTxLe32(tx.data() + 20) == 0);
    CHECK(ZiiuTxLe32(tx.data() + 24) == 0x0012001c);                 // data, no rate flag: 6M
    ZiiuTxBuildHeader(tx.data(), qos.data(), 60, 1, 2, 0x1000, ZIIU_TX_F_RATE_SET(4));
    CHECK(ZiiuTxLe32(tx.data() + 24) == 0x0016001c);                 // 24M: table entry 22
    ZiiuTxBuildHeader(tx.data(), frame.data(), 30, 19, 7, 0x12345040, ZIIU_TX_F_RATE_SET(7));
    CHECK(ZiiuTxLe32(tx.data() + 24) == 0x0012001c);                 // management stays 6M
    std::array<uint8_t, 20> notice{};
    ZiiuTxPut32(notice.data(), (6u << 27) | (2u << 16) | 20u);
    ZiiuTxPut32(notice.data() + 4, 4u << 16);
    ZiiuTxPut32(notice.data() + 8, 0x80013000); // WCID pair
    ZiiuTxPut32(notice.data() + 12, 0x40000000); // status, not token
    ZiiuTxPut32(notice.data() + 16, 7u | (15u << 15));
    CHECK(ZiiuTxFreedMask(notice.data(), notice.size()) == ((1u << 7) | (1u << 15)));
    for (size_t n = 0; n < notice.size(); n++) CHECK(ZiiuTxFreedMask(notice.data(), n) == 0);
    notice[6] = 5; CHECK(ZiiuTxFreedMask(notice.data(), notice.size()) == ((1u << 7) | (1u << 15)));
    for (size_t n = 0; n < notice.size(); n++) CHECK(ZiiuTxFreedMask(notice.data(), n) == 0);
    notice[6] = 6; CHECK(ZiiuTxFreedMask(notice.data(), notice.size()) == 0);
    notice[6] = 4; notice[2] = 3; CHECK(ZiiuTxFreedMask(notice.data(), notice.size()) == 0);
    notice[2] = 1; CHECK(ZiiuTxFreedMask(notice.data(), notice.size()) == 0);
    ZiiuTxPut32(notice.data() + 16, 7u | (0x7fffu << 15));
    CHECK(ZiiuTxFreedMask(notice.data(), notice.size()) == (1u << 7));
    std::array<uint8_t, 104> event{};
    ZiiuTxPut32(event.data(), 7u << 27);
    event[36] = 0x2d; event[38] = 4;
    event[48] = 1; event[50] = 56;
    CHECK(ZiiuTxEventStatusOffset(event.data(), event.size()) == 56);
    for (size_t n = 0; n < event.size(); ++n)
        CHECK(ZiiuTxEventStatusOffset(event.data(), n) == 0);
    event[50] = 0; CHECK(ZiiuTxEventStatusOffset(event.data(), event.size()) == 0);
    event[50] = 57; CHECK(ZiiuTxEventStatusOffset(event.data(), event.size()) == 0);
    event[50] = 56; event[36] = 0;
    CHECK(ZiiuTxEventStatusOffset(event.data(), event.size()) == 0);
    std::puts("TX descriptor and completion parser checks passed");
}

// Interface between ziiu_wifi.sys and user-mode tools/service.
// Shared by kernel and user mode: keep it plain C, fixed-size types only.
//
// The driver stays protocol-agnostic where it can: user mode builds complete MCU
// messages (TXD + MCU header + payload) and parses events. The driver owns the DMA
// rings and never lets user mode choose DMA addresses.
#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif
#include <initguid.h>

// {CACAFDFC-594F-441E-8847-2A178A86A2A9}
DEFINE_GUID(GUID_DEVINTERFACE_ZIIU_WIFI,
    0xcacafdfc, 0x594f, 0x441e, 0x88, 0x47, 0x2a, 0x17, 0x8a, 0x86, 0xa2, 0xa9);

#define ZIIU_WIFI_INTERFACE_VERSION 10

#define ZIIU_IOCTL(fn) CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800 + (fn), METHOD_BUFFERED, FILE_ANY_ACCESS)

// Driver state. Never touches hardware.
#define IOCTL_ZIIU_GET_INFO     ZIIU_IOCTL(0)
// Ownership handshake + read chip ID/revision (first hardware access).
#define IOCTL_ZIIU_READ_CHIP_ID ZIIU_IOCTL(1)
// Clear the crash latch after investigating a crash during a hardware operation.
#define IOCTL_ZIIU_CLEAR_LATCH  ZIIU_IOCTL(2)
// Reset the Wi-Fi subsystem (firmware back to ROM) and bring up the DMA rings.
#define IOCTL_ZIIU_DMA_START    ZIIU_IOCTL(3)
// Stop DMA and reset the Wi-Fi subsystem. Also done automatically when the device
// leaves D0 (driver switch, sleep, shutdown) while DMA is running.
#define IOCTL_ZIIU_DMA_STOP     ZIIU_IOCTL(4)
// Send one MCU message on the command ring; optionally wait for the matching event.
// In: ZIIU_MCU_CMD + message bytes. Out: raw event (RXD + payload), may be empty.
#define IOCTL_ZIIU_MCU_CMD      ZIIU_IOCTL(5)
// Send firmware bytes on the firmware-download ring (FW_SCATTER, no header).
// In: up to ZIIU_FW_SCATTER_MAX bytes, sent in ZIIU_FW_CHUNK pieces.
#define IOCTL_ZIIU_FW_SCATTER   ZIIU_IOCTL(6)
// Read a chip register, optionally polling until (value & Mask) == Value.
// In: ZIIU_POLL_REG. Out: ZIIU_POLL_RESULT.
#define IOCTL_ZIIU_POLL_REG     ZIIU_IOCTL(7)
// Read received frames from the data RX ring. In: ZIIU_RX_READ. Out: a sequence of
// records { ULONG Length; UCHAR Data[Length]; pad to 4 } (raw RXD + 802.11 frame).
// Lightweight: runs under the hardware lock and honours the latch, but doesn't write
// it (it touches only our ring memory and the ring's CPU index register).
#define IOCTL_ZIIU_RX_READ      ZIIU_IOCTL(8)
// Write one chip register in the Wi-Fi MAC block (0x82000000-0x83ffffff: WTBL, rate
// tables, RMAC/TMAC/MDP). Nothing else: DMA, PCIe and power registers are not
// reachable, so user mode can't redirect DMA. In: ZIIU_WRITE_REG.
#define IOCTL_ZIIU_WRITE_REG    ZIIU_IOCTL(9)
// Queue one raw 802.11 management/data frame. Driver builds TXD/TXP and owns
// all physical addresses. Completion is DMA ownership release, NOT peer ACK.
#define IOCTL_ZIIU_TX_FRAME     ZIIU_IOCTL(10)
#define IOCTL_ZIIU_TX_STATS     ZIIU_IOCTL(11) // memory-only snapshot, no chip access
typedef struct _ZIIU_TX_STATS {
    ULONG Queued;
    ULONG Released;
    ULONG BusyMask;
    ULONG Reports;
    ULONG Acked;
    ULONG LastStatus;
    ULONG LastPacketId;
    // 7+: where the time goes. From TX status reports (mt76 connac3 TXS): attempts
    // per frame (TXS5 TX_COUNT), firmware TX delay (TXS2, raw units) and the error
    // bits; and host-measured submit -> TX-free time per token.
    ULONG TxCountSum;
    ULONG TxCountMax;
    ULONG Retried;          // reports with more than one attempt
    ULONG DelaySum;
    ULONG DelayMax;
    ULONG PsFlag;           // TXS0 PS_FLAG: firmware thought the station was asleep
    ULONG QueueTimeout;     // TXS0 QUEUE_TIMEOUT
    ULONG AckTimeout;       // TXS0 ACK_TIMEOUT
    ULONG FreeUsSum;        // submit -> TX-free, microseconds
    ULONG FreeUsMax;
} ZIIU_TX_STATS;
// Optional additive diagnostics (0.5.2+); no packet contents or hardware access.
#define IOCTL_ZIIU_RX_STATS ZIIU_IOCTL(12)
typedef struct _ZIIU_RX_STATS {
    ULONG PacketTypes[32];
    ULONG EventIds[256];
    ULONG LastFreeHeader;
    ULONG LastFreeVersion;
} ZIIU_RX_STATS;
#define ZIIU_TX_FRAME_MAX       2304u
// Data frames only: the station's WMM queue for the frame's TID (non-QoS: best
// effort), as mt76 queues station data, instead of the ALTX0 queue.
#define ZIIU_TX_F_AC_QUEUE      0x1u
// No per-frame TX status report (TXS); the token is still freed by TX-free.
#define ZIIU_TX_F_NO_STATUS     0x2u
// Data frames only (10+): fixed OFDM rate from the basic rate table ziiuctl programs
// (mt76_rates order): 0 = 6, 1 = 9, 2 = 12, 3 = 18, 4 = 24, 5 = 36, 6 = 48, 7 = 54 Mbps.
// Without ZIIU_TX_F_RATE every frame goes at 6 Mbps.
#define ZIIU_TX_F_RATE          0x80u
#define ZIIU_TX_F_RATE_IDX(f)   (((f) >> 4) & 7u)
#define ZIIU_TX_F_RATE_SET(i)   (ZIIU_TX_F_RATE | (((i) & 7u) << 4))
#define ZIIU_TX_F_ALL           0xf3u
typedef struct _ZIIU_TX_FRAME {
    ULONG Length;
    ULONG WlanIndex;          // initialized firmware station table index, 0..4095
    ULONG Flags;              // ZIIU_TX_F_*, others must be zero (8+). Never encrypts.
    ULONG Reserved2;          // must be zero
    // UCHAR Frame[Length]; no FCS. Management (cleartext) or data/QoS data, which may
    // be protected: user mode CCMP-encrypts it first (v6+). No 4-address/fragments.
} ZIIU_TX_FRAME;

// Hardware operations, recorded in the crash latch while they run.
typedef enum _ZIIU_HW_OP {
    ZiiuHwOpNone       = 0,
    ZiiuHwOpReadChipId = 1,
    ZiiuHwOpDmaStart   = 2,
    ZiiuHwOpDmaStop    = 3,
    ZiiuHwOpMcuCmd     = 4,
    ZiiuHwOpFwScatter  = 5,
    ZiiuHwOpPollReg    = 6,
    ZiiuHwOpRxRead     = 7,   // never latched (see IOCTL_ZIIU_RX_READ)
    ZiiuHwOpWriteReg   = 8,
    ZiiuHwOpTxFrame    = 9,
    // Latch value for a whole radio session (9+): set and flushed once at the first
    // TX frame, cleared only when DMA stops. Operations in between skip their own
    // latch writes (two registry flushes each, ~13 ms, capped TX at ~77 frames/s).
    ZiiuHwOpRadioSession = 10,
} ZIIU_HW_OP;

#define ZIIU_WRITE_REG_MIN  0x82000000u
#define ZIIU_WRITE_REG_MAX  0x83ffffffu

typedef struct _ZIIU_WRITE_REG {
    ULONG Addr;               // ZIIU_WRITE_REG_MIN..ZIIU_WRITE_REG_MAX, 4-byte aligned
    ULONG Value;
} ZIIU_WRITE_REG;

#define ZIIU_MCU_MAX_MSG     16384u   // largest MCU message (header included)
#define ZIIU_MCU_MAX_EVENT   16384u   // largest event returned
#define ZIIU_FW_CHUNK        4096u    // FW_SCATTER piece size (mt76 uses 4096 on PCIe)
#define ZIIU_FW_SCATTER_MAX  65536u
#define ZIIU_POLL_MAX_MS     5000u
#define ZIIU_MCU_MAX_WAIT_MS 10000u

typedef struct _ZIIU_INFO {
    ULONG InterfaceVersion;   // ZIIU_WIFI_INTERFACE_VERSION
    ULONG BarMapped;          // 1 if BAR0 is mapped
    ULONG BarLength;          // bytes
    ULONG LatchedOp;          // ZIIU_HW_OP that was running when the PC last crashed, 0 if none
    ULONG DmaActive;          // 1 while the DMA rings are running
    ULONG PciCommand;         // PCI command register as last seen by DMA_START (0 = not read yet)
    ULONG EventsSkipped;      // MCU events dropped because nobody waited for them
    ULONG RxFrames;           // frames delivered by RX_READ
    ULONG RxDropped;          // frames dropped (too big for the caller's buffer)
} ZIIU_INFO;

typedef struct _ZIIU_CHIP_ID {
    ULONG Status;             // NTSTATUS of the handshake
    ULONG ChipId;             // MT_HW_CHIPID, expected 0x7925
    ULONG HwRev;              // MT_HW_REV
} ZIIU_CHIP_ID;

#define ZIIU_MCU_WAIT_RESPONSE 0x1u

typedef struct _ZIIU_MCU_CMD {
    ULONG Flags;              // ZIIU_MCU_WAIT_RESPONSE
    ULONG Seq;                // sequence number in the message (1-15); the event must echo it
    ULONG TimeoutMs;          // wait for the event, max ZIIU_MCU_MAX_WAIT_MS
    ULONG Length;             // message bytes that follow this header
} ZIIU_MCU_CMD;

typedef struct _ZIIU_POLL_REG {
    ULONG Addr;               // chip address (or BAR offset if < 0x200000)
    ULONG Mask;
    ULONG Value;
    ULONG TimeoutMs;          // 0 = single read; max ZIIU_POLL_MAX_MS
} ZIIU_POLL_REG;

#define ZIIU_RX_MAX_WAIT_MS 1000u

typedef struct _ZIIU_RX_READ {
    ULONG WaitMs;             // wait up to this long for the first frame, max ZIIU_RX_MAX_WAIT_MS
} ZIIU_RX_READ;

typedef struct _ZIIU_POLL_RESULT {
    ULONG Matched;            // 1 if (value & Mask) == Value
    ULONG Value;              // last value read
} ZIIU_POLL_RESULT;

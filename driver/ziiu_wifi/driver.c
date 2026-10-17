// ziiu_wifi.sys - KMDF driver for the MediaTek MT7925, used as the GamePad radio.
//
// Boot safety (docs/wireless.md, "Boot safety"):
// - DriverEntry, device add and PnP start never touch the chip. They only map BAR0.
// - Chip access happens only on IOCTLs from an admin tool, with one exception: if a
//   tool started DMA, leaving D0 (driver switch, sleep, shutdown) stops it first, so
//   the chip never writes into freed memory. At boot DMA is never running.
// - Before each hardware operation a crash latch is written and flushed to the
//   registry. If the PC crashes mid-operation, the latch survives the reboot and the
//   driver refuses further hardware operations until someone clears it. So a bug can
//   cause at most one crash, never a loop.
//
// DMA: no interrupts. Commands are sent and their events polled synchronously from
// the (sequential, passive-level) IOCTL queue. User mode never supplies DMA addresses.

#include <ntddk.h>
#include <wdf.h>
#include <initguid.h>
#include <wdmguid.h>
#include "..\include\ziiu_wifi_ioctl.h"
#include "mt7925_regs.h"
#include "..\include\ziiu_tx_format.h"

#define ZIIU_TAG "ziiu_wifi: "
#define ZiiuPrint(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, ZIIU_TAG fmt "\n", __VA_ARGS__)

#define LATCH_VALUE_NAME L"HwOpInProgress"

// Ring sizes. Smaller than mt76's: commands are synchronous, so only one TX
// descriptor is ever in flight per ring.
#define ZIIU_TX_DATA_NDESC  16
#define ZIIU_TX_MCU_NDESC   32
#define ZIIU_TX_FWDL_NDESC  32
#define ZIIU_RX_MCU_NDESC   64
#define ZIIU_RX_DATA_NDESC  64
#define ZIIU_DUMMY_NDESC    16
#define ZIIU_RX_SLOT_SIZE   2048u  // MT_RX_BUF_SIZE

// Descriptor memory layout (one common buffer, each ring 4 KB aligned).
#define ZIIU_DESC_OFF_TX_DATA 0x0000u
#define ZIIU_DESC_OFF_TX_MCU  0x1000u
#define ZIIU_DESC_OFF_TX_FWDL 0x2000u
#define ZIIU_DESC_OFF_RX_MCU  0x3000u
#define ZIIU_DESC_OFF_RX_DATA 0x4000u
#define ZIIU_DESC_OFF_DUMMY   0x5000u
#define ZIIU_DESC_MEM_SIZE    0x6000u
#define ZIIU_TX_FRAME_SLOT    (64u + ZIIU_TX_FRAME_MAX)
#define ZIIU_TX_FRAME_OFFSET (ZIIU_MCU_MAX_MSG + ZIIU_FW_CHUNK)
#define ZIIU_TX_MEM_SIZE      (ZIIU_TX_FRAME_OFFSET + ZIIU_TX_DATA_NDESC * ZIIU_TX_FRAME_SLOT)
#define ZIIU_RX_MEM_SIZE      ((ZIIU_RX_MCU_NDESC + ZIIU_RX_DATA_NDESC) * ZIIU_RX_SLOT_SIZE)

typedef struct _ZIIU_DESC {  // struct mt76_desc
    ULONG Buf0;
    ULONG Ctrl;
    ULONG Buf1;
    ULONG Info;
} ZIIU_DESC;

typedef struct _ZIIU_RING {
    ULONG               Regs;      // BAR offset of {desc_base, ring_size, cpu_idx, dma_idx}
    ULONG               Ndesc;
    volatile ZIIU_DESC* Desc;
    ULONG               DescPa;
    PUCHAR              Buf;       // TX: one bounce buffer; RX: Ndesc slots
    ULONG               BufPa;
    ULONG               SlotSize;  // TX: bounce buffer size; RX: bytes per slot
    ULONG               Head;
    ULONG               Tail;
} ZIIU_RING;

typedef struct _DEVICE_CONTEXT {
    PUCHAR          Bar;          // BAR0 mapping, NULL if unusable
    SIZE_T          BarLength;
    WDFWAITLOCK     HwLock;       // serializes all chip access
    ULONG           LatchedOp;    // ZIIU_HW_OP left over from a crash, 0 if none
    BOOLEAN         SessionLatched;  // latch holds ZiiuHwOpRadioSession (see ZiiuRunHwOp)

    BUS_INTERFACE_STANDARD BusIf; // PCI config access (bus master check)
    BOOLEAN         HaveBusIf;
    ULONG           PciCommand;

    WDFDMAENABLER   DmaEnabler;
    WDFCOMMONBUFFER DescMem;      // allocated on first DMA start, freed with the device
    WDFCOMMONBUFFER TxMem;
    WDFCOMMONBUFFER RxMem;
    ULONG           DummyPa;      // empty ring for queues we don't use
    ZIIU_RING       TxData;
    ZIIU_RING       TxMcu;
    ZIIU_RING       TxFwdl;
    ZIIU_RING       RxMcu;
    ZIIU_RING       RxData;
    BOOLEAN         DmaActive;
    ULONG           EventsSkipped;
    ULONG           RxFrames;
    ULONG           RxDropped;
    BOOLEAN         TxFrameBusy[ZIIU_TX_DATA_NDESC];
    ULONGLONG       TxSubmitTime[ZIIU_TX_DATA_NDESC];  // KeQueryInterruptTime, 100 ns
    ZIIU_TX_STATS    TxStats;
    ZIIU_RX_STATS    RxStats;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

// Arguments of one hardware operation (IOCTL buffers).
typedef struct _ZIIU_OP_ARGS {
    PVOID     In;
    size_t    InLength;
    PVOID     Out;
    size_t    OutLength;
    ULONG_PTR Written;
} ZIIU_OP_ARGS;

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD ZiiuEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE ZiiuEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE ZiiuEvtReleaseHardware;
EVT_WDF_DEVICE_D0_EXIT ZiiuEvtD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL ZiiuEvtIoDeviceControl;

// ---------------------------------------------------------------------------
// Crash latch
// ---------------------------------------------------------------------------

static NTSTATUS ZiiuReadLatch(_Out_ PULONG Op)
{
    WDFKEY key;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name, LATCH_VALUE_NAME);

    *Op = ZiiuHwOpNone;
    status = WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ,
                                                WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WdfRegistryQueryULong(key, &name, Op);
    WdfRegistryClose(key);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
        *Op = ZiiuHwOpNone;
        status = STATUS_SUCCESS;
    }
    return status;
}

// Write the latch and flush it to disk so it survives a crash right after.
static NTSTATUS ZiiuWriteLatch(_In_ ULONG Op)
{
    WDFKEY key;
    NTSTATUS status;
    DECLARE_CONST_UNICODE_STRING(name, LATCH_VALUE_NAME);

    status = WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ | KEY_SET_VALUE,
                                                WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WdfRegistryAssignULong(key, &name, Op);
    if (NT_SUCCESS(status)) {
        status = ZwFlushKey(WdfRegistryWdmGetHandle(key));
    }
    WdfRegistryClose(key);
    return status;
}

// ---------------------------------------------------------------------------
// Register access. Every access is bounds-checked against the mapped BAR.
// ---------------------------------------------------------------------------

#define ZIIU_REG_DEAD 0xffffffffu  // what reads return if the device fell off the bus

static BOOLEAN ZiiuRegInRange(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Offset)
{
    return Ctx->Bar != NULL && (Offset & 3) == 0 && Offset <= Ctx->BarLength - sizeof(ULONG);
}

static ULONG ZiiuRr(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Offset)
{
    if (!ZiiuRegInRange(Ctx, Offset)) {
        return ZIIU_REG_DEAD;
    }
    return READ_REGISTER_ULONG((volatile ULONG*)(Ctx->Bar + Offset));
}

static VOID ZiiuWr(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Offset, _In_ ULONG Value)
{
    if (ZiiuRegInRange(Ctx, Offset)) {
        WRITE_REGISTER_ULONG((volatile ULONG*)(Ctx->Bar + Offset), Value);
    }
}

static VOID ZiiuSet(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Offset, _In_ ULONG Bits)
{
    ZiiuWr(Ctx, Offset, ZiiuRr(Ctx, Offset) | Bits);
}

static VOID ZiiuClear(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Offset, _In_ ULONG Bits)
{
    ZiiuWr(Ctx, Offset, ZiiuRr(Ctx, Offset) & ~Bits);
}

// mt7925 default_fixed_map (mt7925/pci.c): chip address window -> BAR offset.
typedef struct _ZIIU_REG_MAP {
    ULONG Phys;
    ULONG Bar;
    ULONG Size;
} ZIIU_REG_MAP;

static const ZIIU_REG_MAP g_FixedMap[] = {
    { 0x830c0000, 0x000000, 0x0001000 }, { 0x54000000, 0x002000, 0x0001000 },
    { 0x55000000, 0x003000, 0x0001000 }, { 0x56000000, 0x004000, 0x0001000 },
    { 0x57000000, 0x005000, 0x0001000 }, { 0x58000000, 0x006000, 0x0001000 },
    { 0x59000000, 0x007000, 0x0001000 }, { 0x820c0000, 0x008000, 0x0004000 },
    { 0x820c8000, 0x00c000, 0x0002000 }, { 0x820cc000, 0x00e000, 0x0002000 },
    { 0x74030000, 0x010000, 0x0001000 }, { 0x820e0000, 0x020000, 0x0000400 },
    { 0x820e1000, 0x020400, 0x0000200 }, { 0x820e2000, 0x020800, 0x0000400 },
    { 0x820e3000, 0x020c00, 0x0000400 }, { 0x820e4000, 0x021000, 0x0000400 },
    { 0x820e5000, 0x021400, 0x0000800 }, { 0x820ce000, 0x021c00, 0x0000200 },
    { 0x820e7000, 0x021e00, 0x0000200 }, { 0x820cf000, 0x022000, 0x0001000 },
    { 0x820e9000, 0x023400, 0x0000200 }, { 0x820ea000, 0x024000, 0x0000200 },
    { 0x820eb000, 0x024200, 0x0000400 }, { 0x820ec000, 0x024600, 0x0000200 },
    { 0x820ed000, 0x024800, 0x0000800 }, { 0x820ca000, 0x026000, 0x0002000 },
    { 0x820d0000, 0x030000, 0x0010000 }, { 0x40000000, 0x070000, 0x0010000 },
    { 0x00400000, 0x080000, 0x0010000 }, { 0x00410000, 0x090000, 0x0010000 },
    { 0x820f0000, 0x0a0000, 0x0000400 }, { 0x820f1000, 0x0a0600, 0x0000200 },
    { 0x820f2000, 0x0a0800, 0x0000400 }, { 0x820f3000, 0x0a0c00, 0x0000400 },
    { 0x820f4000, 0x0a1000, 0x0000400 }, { 0x820f5000, 0x0a1400, 0x0000800 },
    { 0x820f7000, 0x0a1e00, 0x0000200 }, { 0x820f9000, 0x0a3400, 0x0000200 },
    { 0x820fa000, 0x0a4000, 0x0000200 }, { 0x820fb000, 0x0a4200, 0x0000400 },
    { 0x820fc000, 0x0a4600, 0x0000200 }, { 0x820fd000, 0x0a4800, 0x0000800 },
    { 0x820c4000, 0x0a8000, 0x0004000 }, { 0x820b0000, 0x0ae000, 0x0001000 },
    { 0x80020000, 0x0b0000, 0x0010000 }, { 0x81020000, 0x0c0000, 0x0010000 },
    { 0x7c020000, 0x0d0000, 0x0010000 }, { 0x7c060000, 0x0e0000, 0x0010000 },
    { 0x7c000000, 0x0f0000, 0x0010000 }, { 0x70020000, 0x1f0000, 0x0010000 },
    { 0x7c500000, 0x060000, 0x0200000 },
};

static BOOLEAN ZiiuFixedMap(_In_ ULONG Addr, _Out_ PULONG Bar)
{
    ULONG i;

    *Bar = 0;
    if (Addr < 0x200000) {
        *Bar = Addr;
        return TRUE;
    }
    for (i = 0; i < ARRAYSIZE(g_FixedMap); i++) {
        if (Addr >= g_FixedMap[i].Phys && Addr - g_FixedMap[i].Phys < g_FixedMap[i].Size) {
            *Bar = g_FixedMap[i].Bar + (Addr - g_FixedMap[i].Phys);
            return TRUE;
        }
    }
    return FALSE;
}

// Ranges mt7925 reaches through the L1 remap window.
static BOOLEAN ZiiuIsL1(_In_ ULONG Addr)
{
    return (Addr >= 0x18000000 && Addr < 0x18c00000) ||
           (Addr >= 0x70000000 && Addr < 0x78000000) ||
           (Addr >= 0x7c000000 && Addr < 0x7c400000);
}

// Access a chip address through the L1 remap window, then restore the window
// (mt7925_reg_map_l1 + mt7925_reg_remap_restore).
static ULONG ZiiuL1Access(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Addr, _In_ BOOLEAN Write, _In_ ULONG Value)
{
    ULONG backup = ZiiuRr(Ctx, MT_HIF_REMAP_L1);

    if (backup == ZIIU_REG_DEAD) {
        return ZIIU_REG_DEAD;
    }
    ZiiuWr(Ctx, MT_HIF_REMAP_L1, (backup & ~MT_HIF_REMAP_L1_MASK) | (Addr & MT_HIF_REMAP_L1_MASK));
    (VOID)ZiiuRr(Ctx, MT_HIF_REMAP_L1);  // read back to push the write
    if (Write) {
        ZiiuWr(Ctx, MT_HIF_REMAP_BASE_L1 + (Addr & 0xffffu), Value);
    } else {
        Value = ZiiuRr(Ctx, MT_HIF_REMAP_BASE_L1 + (Addr & 0xffffu));
    }
    ZiiuWr(Ctx, MT_HIF_REMAP_L1, backup);
    return Value;
}

static BOOLEAN ZiiuChipAddrOk(_In_ ULONG Addr)
{
    ULONG bar;
    return ZiiuFixedMap(Addr, &bar) || ZiiuIsL1(Addr);
}

static ULONG ZiiuChipRr(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Addr)
{
    ULONG bar;
    if (ZiiuFixedMap(Addr, &bar)) {
        return ZiiuRr(Ctx, bar);
    }
    if (ZiiuIsL1(Addr)) {
        return ZiiuL1Access(Ctx, Addr, FALSE, 0);
    }
    return ZIIU_REG_DEAD;
}

static VOID ZiiuChipWr(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Addr, _In_ ULONG Value)
{
    ULONG bar;
    if (ZiiuFixedMap(Addr, &bar)) {
        ZiiuWr(Ctx, bar, Value);
    } else if (ZiiuIsL1(Addr)) {
        (VOID)ZiiuL1Access(Ctx, Addr, TRUE, Value);
    }
}

static VOID ZiiuChipSet(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Addr, _In_ ULONG Bits)
{
    ZiiuChipWr(Ctx, Addr, ZiiuChipRr(Ctx, Addr) | Bits);
}

static VOID ZiiuSleepMs(_In_ ULONG Ms)
{
    LARGE_INTEGER interval;
    interval.QuadPart = -10000LL * Ms;
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

// Poll a chip register until (value & Mask) == Value, about TimeoutMs (mt76_poll_msec).
static NTSTATUS ZiiuPoll(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Addr, _In_ ULONG Mask, _In_ ULONG Value,
                         _In_ ULONG TimeoutMs, _Out_opt_ PULONG Last)
{
    ULONG ms = 0;
    ULONG reg;

    for (;;) {
        reg = ZiiuChipRr(Ctx, Addr);
        if (Last != NULL) {
            *Last = reg;
        }
        if (reg == ZIIU_REG_DEAD) {  // checked first: all-ones would "match" most masks
            return STATUS_DEVICE_NOT_CONNECTED;
        }
        if ((reg & Mask) == Value) {
            return STATUS_SUCCESS;
        }
        if (ms >= TimeoutMs) {
            return STATUS_IO_TIMEOUT;
        }
        ZiiuSleepMs(1);
        ms++;
    }
}

// mt792xe_mcu_fw_pmctrl: hand power ownership to the firmware/ROM.
static NTSTATUS ZiiuFwOwn(_In_ PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status = STATUS_IO_TIMEOUT;
    int i;
    for (i = 0; i < MT792X_OWN_RETRY_COUNT; i++) {
        ZiiuChipWr(Ctx, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_SET_OWN);
        status = ZiiuPoll(Ctx, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_OWN_SYNC, PCIE_LPCR_HOST_OWN_SYNC,
                          MT792X_OWN_POLL_MS, NULL);
        if (status != STATUS_IO_TIMEOUT) {
            break;
        }
    }
    return status;
}

// __mt792xe_mcu_drv_pmctrl: take power ownership back for the host driver.
static NTSTATUS ZiiuDrvOwn(_In_ PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status = STATUS_IO_TIMEOUT;
    int i;
    for (i = 0; i < MT792X_OWN_RETRY_COUNT; i++) {
        ZiiuChipWr(Ctx, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_CLR_OWN);
        ZiiuSleepMs(3);  // mt76 waits 2-3 ms when ASPM is on; harmless otherwise
        status = ZiiuPoll(Ctx, MT_CONN_ON_LPCTL, PCIE_LPCR_HOST_OWN_SYNC, 0, MT792X_OWN_POLL_MS, NULL);
        if (status != STATUS_IO_TIMEOUT) {
            break;
        }
    }
    return status;
}

// mt792x_wfsys_reset_default (connac3 address). Resets the Wi-Fi subsystem only:
// firmware goes back to ROM. Bluetooth is a separate subsystem and is not touched.
static NTSTATUS ZiiuWfsysReset(_In_ PDEVICE_CONTEXT Ctx)
{
    ZiiuChipWr(Ctx, MT_WFSYS_SW_RST_B, ZiiuChipRr(Ctx, MT_WFSYS_SW_RST_B) & ~WFSYS_SW_RST_B);
    ZiiuSleepMs(50);
    ZiiuChipSet(Ctx, MT_WFSYS_SW_RST_B, WFSYS_SW_RST_B);
    return ZiiuPoll(Ctx, MT_WFSYS_SW_RST_B, WFSYS_SW_INIT_DONE, WFSYS_SW_INIT_DONE, 500, NULL);
}

// ---------------------------------------------------------------------------
// Hardware operations (IOCTL only)
// ---------------------------------------------------------------------------

// Same order as mt7925_pci_probe: fw own, drv own, then read CHIPID/REV.
static NTSTATUS ZiiuReadChipId(_In_ PDEVICE_CONTEXT Ctx, _Out_ ZIIU_CHIP_ID* Out)
{
    NTSTATUS status;

    RtlZeroMemory(Out, sizeof(*Out));
    status = ZiiuFwOwn(Ctx);
    if (NT_SUCCESS(status)) {
        status = ZiiuDrvOwn(Ctx);
    }
    if (NT_SUCCESS(status)) {
        Out->ChipId = ZiiuChipRr(Ctx, MT_HW_CHIPID);
        Out->HwRev = ZiiuChipRr(Ctx, MT_HW_REV);
        if (Out->ChipId == ZIIU_REG_DEAD) {
            status = STATUS_DEVICE_NOT_CONNECTED;
        }
    }
    Out->Status = (ULONG)status;
    ZiiuPrint("read chip id: status 0x%08x chip 0x%08x rev 0x%08x", status, Out->ChipId, Out->HwRev);
    return STATUS_SUCCESS;  // result is in Out->Status
}

// --- DMA memory ------------------------------------------------------------

static NTSTATUS ZiiuAllocCommon(_In_ PDEVICE_CONTEXT Ctx, _In_ size_t Size, _Out_ WDFCOMMONBUFFER* Cb,
                                _Out_ PUCHAR* Va, _Out_ PULONG Pa)
{
    PHYSICAL_ADDRESS la;
    NTSTATUS status;

    *Va = NULL;
    *Pa = 0;
    status = WdfCommonBufferCreate(Ctx->DmaEnabler, Size, WDF_NO_OBJECT_ATTRIBUTES, Cb);
    if (!NT_SUCCESS(status)) {
        *Cb = NULL;
        return status;
    }
    la = WdfCommonBufferGetAlignedLogicalAddress(*Cb);
    // The chip uses 32-bit DMA addresses (mt7925: dma_set_mask(32)); rings must be 4 KB aligned.
    if (la.HighPart != 0 || (la.LowPart & 0xfff) != 0 || (ULONGLONG)la.LowPart + Size > 0x100000000ull) {
        WdfObjectDelete(*Cb);
        *Cb = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *Va = (PUCHAR)WdfCommonBufferGetAlignedVirtualAddress(*Cb);
    *Pa = la.LowPart;
    RtlZeroMemory(*Va, Size);
    return STATUS_SUCCESS;
}

static VOID ZiiuRingInit(_Out_ ZIIU_RING* R, _In_ ULONG Regs, _In_ ULONG Ndesc, _In_ PUCHAR DescVa,
                         _In_ ULONG DescPa, _In_ ULONG DescOff, _In_opt_ PUCHAR Buf, _In_ ULONG BufPa,
                         _In_ ULONG SlotSize)
{
    R->Regs = Regs;
    R->Ndesc = Ndesc;
    R->Desc = (volatile ZIIU_DESC*)(DescVa + DescOff);
    R->DescPa = DescPa + DescOff;
    R->Buf = Buf;
    R->BufPa = BufPa;
    R->SlotSize = SlotSize;
    R->Head = 0;
    R->Tail = 0;
}

// Allocate rings and buffers once; they live until the device is removed.
static NTSTATUS ZiiuDmaAlloc(_In_ PDEVICE_CONTEXT Ctx)
{
    PUCHAR descVa = NULL, txVa = NULL, rxVa = NULL;
    ULONG descPa = 0, txPa = 0, rxPa = 0;
    NTSTATUS status;

    if (Ctx->DescMem != NULL) {
        return STATUS_SUCCESS;
    }
    if (Ctx->DmaEnabler == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    status = ZiiuAllocCommon(Ctx, ZIIU_DESC_MEM_SIZE, &Ctx->DescMem, &descVa, &descPa);
    if (NT_SUCCESS(status)) {
        status = ZiiuAllocCommon(Ctx, ZIIU_TX_MEM_SIZE, &Ctx->TxMem, &txVa, &txPa);
    }
    if (NT_SUCCESS(status)) {
        status = ZiiuAllocCommon(Ctx, ZIIU_RX_MEM_SIZE, &Ctx->RxMem, &rxVa, &rxPa);
    }
    if (!NT_SUCCESS(status)) {
        if (Ctx->TxMem != NULL) { WdfObjectDelete(Ctx->TxMem); Ctx->TxMem = NULL; }
        if (Ctx->DescMem != NULL) { WdfObjectDelete(Ctx->DescMem); Ctx->DescMem = NULL; }
        return status;
    }

    ZiiuRingInit(&Ctx->TxData, MT_RING_REGS(MT_TX_RING_BASE, MT7925_TXQ_BAND0), ZIIU_TX_DATA_NDESC,
                 descVa, descPa, ZIIU_DESC_OFF_TX_DATA, txVa + ZIIU_TX_FRAME_OFFSET,
                 txPa + ZIIU_TX_FRAME_OFFSET, ZIIU_TX_FRAME_SLOT);
    ZiiuRingInit(&Ctx->TxMcu, MT_RING_REGS(MT_TX_RING_BASE, MT7925_TXQ_MCU_WM), ZIIU_TX_MCU_NDESC,
                 descVa, descPa, ZIIU_DESC_OFF_TX_MCU, txVa, txPa, ZIIU_MCU_MAX_MSG);
    ZiiuRingInit(&Ctx->TxFwdl, MT_RING_REGS(MT_TX_RING_BASE, MT7925_TXQ_FWDL), ZIIU_TX_FWDL_NDESC,
                 descVa, descPa, ZIIU_DESC_OFF_TX_FWDL, txVa + ZIIU_MCU_MAX_MSG, txPa + ZIIU_MCU_MAX_MSG,
                 ZIIU_FW_CHUNK);
    ZiiuRingInit(&Ctx->RxMcu, MT_RING_REGS(MT_RX_RING_BASE, MT7925_RXQ_MCU_WM), ZIIU_RX_MCU_NDESC,
                 descVa, descPa, ZIIU_DESC_OFF_RX_MCU, rxVa, rxPa, ZIIU_RX_SLOT_SIZE);
    ZiiuRingInit(&Ctx->RxData, MT_RING_REGS(MT_RX_RING_BASE, MT7925_RXQ_BAND0), ZIIU_RX_DATA_NDESC,
                 descVa, descPa, ZIIU_DESC_OFF_RX_DATA, rxVa + ZIIU_RX_MCU_NDESC * ZIIU_RX_SLOT_SIZE,
                 rxPa + ZIIU_RX_MCU_NDESC * ZIIU_RX_SLOT_SIZE, ZIIU_RX_SLOT_SIZE);
    Ctx->DummyPa = descPa + ZIIU_DESC_OFF_DUMMY;
    {
        volatile ZIIU_DESC* dummy = (volatile ZIIU_DESC*)(descVa + ZIIU_DESC_OFF_DUMMY);
        ULONG i;
        for (i = 0; i < ZIIU_DUMMY_NDESC; i++) {
            dummy[i].Ctrl = MT_DMA_CTL_DMA_DONE;
        }
    }
    ZiiuPrint("DMA memory: desc 0x%08x tx 0x%08x rx 0x%08x", descPa, txPa, rxPa);
    return STATUS_SUCCESS;
}

// --- Rings -------------------------------------------------------------------

static VOID ZiiuRxArm(_Inout_ ZIIU_RING* R, _In_ ULONG Idx)
{
    volatile ZIIU_DESC* d = &R->Desc[Idx];
    d->Buf0 = R->BufPa + Idx * R->SlotSize;
    d->Buf1 = 0;
    d->Info = 0;
    d->Ctrl = (R->SlotSize << 16) & MT_DMA_CTL_SD_LEN0;  // clears DMA_DONE: owned by hardware
}

// mt76_dma_queue_reset + mt76_dma_sync_idx (+ mt76_dma_rx_fill for RX rings).
static VOID ZiiuRingProgram(_In_ PDEVICE_CONTEXT Ctx, _Inout_ ZIIU_RING* R, _In_ BOOLEAN Rx)
{
    ULONG i;

    for (i = 0; i < R->Ndesc; i++) {
        R->Desc[i].Buf0 = 0;
        R->Desc[i].Buf1 = 0;
        R->Desc[i].Info = 0;
        R->Desc[i].Ctrl = MT_DMA_CTL_DMA_DONE;
    }
    R->Head = 0;
    R->Tail = 0;
    if (Rx) {
        // Fill all but one slot (mt76 keeps one free so head != tail means "not empty").
        while ((R->Head + 1) % R->Ndesc != R->Tail) {
            ZiiuRxArm(R, R->Head);
            R->Head = (R->Head + 1) % R->Ndesc;
        }
    }
    KeMemoryBarrier();
    ZiiuWr(Ctx, R->Regs + RING_CPU_IDX, 0);
    ZiiuWr(Ctx, R->Regs + RING_DMA_IDX, 0);
    ZiiuWr(Ctx, R->Regs + RING_SIZE, R->Ndesc);
    ZiiuWr(Ctx, R->Regs + RING_DESC_BASE, R->DescPa);
    ZiiuWr(Ctx, R->Regs + RING_CPU_IDX, R->Head);
}

// Point a ring we don't use at an empty dummy ring (cpu_idx == dma_idx: nothing to do),
// so stale addresses from a previous driver can never be used.
static VOID ZiiuRingNeutralize(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG Regs)
{
    ZiiuWr(Ctx, Regs + RING_CPU_IDX, 0);
    ZiiuWr(Ctx, Regs + RING_DMA_IDX, 0);
    ZiiuWr(Ctx, Regs + RING_SIZE, ZIIU_DUMMY_NDESC);
    ZiiuWr(Ctx, Regs + RING_DESC_BASE, Ctx->DummyPa);
}

// Send one buffer on a TX ring and wait until the DMA engine has fetched it.
static NTSTATUS ZiiuTxSend(_In_ PDEVICE_CONTEXT Ctx, _Inout_ ZIIU_RING* R,
                           _In_reads_bytes_(Length) const VOID* Data, _In_ ULONG Length)
{
    volatile ZIIU_DESC* d;
    ULONG i, idx;

    if (Length == 0 || Length > R->SlotSize || R->Buf == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlCopyMemory(R->Buf, Data, Length);
    idx = R->Head;
    d = &R->Desc[idx];
    d->Buf0 = R->BufPa;
    d->Buf1 = 0;
    d->Info = 0;
    d->Ctrl = ((Length << 16) & MT_DMA_CTL_SD_LEN0) | MT_DMA_CTL_LAST_SEC0;
    R->Head = (idx + 1) % R->Ndesc;
    KeMemoryBarrier();
    ZiiuWr(Ctx, R->Regs + RING_CPU_IDX, R->Head);

    // Short spin first (typical case is a few microseconds), then 1 ms sleeps.
    for (i = 0; i < 200 + 200; i++) {
        ULONG dmaIdx = ZiiuRr(Ctx, R->Regs + RING_DMA_IDX);
        if (dmaIdx == ZIIU_REG_DEAD) {
            return STATUS_DEVICE_NOT_CONNECTED;
        }
        if (dmaIdx == R->Head) {
            R->Tail = R->Head;
            return STATUS_SUCCESS;
        }
        if (i < 200) {
            KeStallExecutionProcessor(5);
        } else {
            ZiiuSleepMs(1);
        }
    }
    ZiiuPrint("TX ring 0x%x stuck: cpu %u dma %u", R->Regs, R->Head, ZiiuRr(Ctx, R->Regs + RING_DMA_IDX));
    return STATUS_IO_TIMEOUT;
}

// If the hardware filled the next RX slot, return it (data stays valid until the next
// ZiiuRxRelease after this one).
static BOOLEAN ZiiuRxPeek(_In_ ZIIU_RING* R, _Out_ PUCHAR* Data, _Out_ PULONG Length, _Out_ PBOOLEAN Last)
{
    ULONG ctrl = R->Desc[R->Tail].Ctrl;

    *Data = NULL;
    *Length = 0;
    *Last = TRUE;
    if (!(ctrl & MT_DMA_CTL_DMA_DONE)) {
        return FALSE;
    }
    KeMemoryBarrier();  // read the payload only after seeing DMA_DONE
    *Data = R->Buf + R->Tail * R->SlotSize;
    *Length = min((ctrl & MT_DMA_CTL_SD_LEN0) >> 16, R->SlotSize);
    *Last = (ctrl & MT_DMA_CTL_LAST_SEC0) != 0;
    return TRUE;
}

// Consume the slot at Tail and give one buffer back to the hardware (mt76 refills at head).
static VOID ZiiuRxRelease(_In_ PDEVICE_CONTEXT Ctx, _Inout_ ZIIU_RING* R)
{
    R->Tail = (R->Tail + 1) % R->Ndesc;
    ZiiuRxArm(R, R->Head);
    R->Head = (R->Head + 1) % R->Ndesc;
    KeMemoryBarrier();
    ZiiuWr(Ctx, R->Regs + RING_CPU_IDX, R->Head);
}

static VOID ZiiuTxStatus(_In_ PDEVICE_CONTEXT Ctx, _In_reads_(48) const UCHAR* Data)
{
    ULONG status = ZiiuTxLe32(Data);
    ULONG wcid = (ZiiuTxLe32(Data + 8) >> 16) & 4095;
    ULONG pid = ZiiuTxLe32(Data + 12) >> 24;
    ULONG count = (ZiiuTxLe32(Data + 20) >> 25) & 31;   // TXS5 TX_COUNT
    ULONG delay = ZiiuTxLe32(Data + 8) & 0xffff;        // TXS2 TX_DELAY
    if (wcid != 1 || pid < 3 || pid >= 3 + ZIIU_TX_DATA_NDESC) return;
    Ctx->TxStats.Reports++;
    if (!(status & 0x70000)) Ctx->TxStats.Acked++;
    Ctx->TxStats.TxCountSum += count;
    if (count > Ctx->TxStats.TxCountMax) Ctx->TxStats.TxCountMax = count;
    if (count > 1) Ctx->TxStats.Retried++;
    Ctx->TxStats.DelaySum += delay;
    if (delay > Ctx->TxStats.DelayMax) Ctx->TxStats.DelayMax = delay;
    if (status & (1u << 21)) Ctx->TxStats.PsFlag++;
    if (status & (1u << 18)) Ctx->TxStats.QueueTimeout++;
    if (status & (1u << 16)) Ctx->TxStats.AckTimeout++;
    Ctx->TxStats.LastStatus = status;
    Ctx->TxStats.LastPacketId = pid;
}

static VOID ZiiuTxFreeNotice(_In_ PDEVICE_CONTEXT Ctx, _In_reads_bytes_(Length) const UCHAR* Data,
                            _In_ ULONG Length)
{
    ULONG mask = ZiiuTxFreedMask(Data, Length), i;
    size_t eventOffset = ZiiuTxEventStatusOffset(Data, Length);
    if (Length >= 4) {
        ULONG type = ZiiuTxLe32(Data) >> 27;
        Ctx->RxStats.PacketTypes[type]++;
        if (type == 7 && Length >= 44) Ctx->RxStats.EventIds[Data[36]]++;
        if (type == 6 && Length >= 8) {
            Ctx->RxStats.LastFreeHeader = ZiiuTxLe32(Data);
            Ctx->RxStats.LastFreeVersion = (ZiiuTxLe32(Data + 4) >> 16) & 15;
        }
    }
    if (eventOffset) ZiiuTxStatus(Ctx, Data + eventOffset);
    for (i = 0; i < ZIIU_TX_DATA_NDESC; i++) {
        if ((mask & (1u << i)) && Ctx->TxFrameBusy[i]) {
            ULONG us = (ULONG)min((KeQueryInterruptTime() - Ctx->TxSubmitTime[i]) / 10, MAXULONG);
            Ctx->TxFrameBusy[i] = FALSE;
            Ctx->TxStats.Released++;
            Ctx->TxStats.FreeUsSum += us;
            if (us > Ctx->TxStats.FreeUsMax) Ctx->TxStats.FreeUsMax = us;
        }
    }
    if (Length >= 64 && (ZiiuTxLe32(Data) >> 27) == 0 &&
        (ZiiuTxLe32(Data) & 0xffff) == Length) {
        for (i = 16; i + 48 <= Length; i += 48) {
            ZiiuTxStatus(Ctx, Data + i);
        }
    }
}

static ULONG ZiiuRxDrain(_In_ PDEVICE_CONTEXT Ctx, _Inout_ ZIIU_RING* R)
{
    PUCHAR data;
    ULONG len, n = 0;
    BOOLEAN last;

    while (n < R->Ndesc && ZiiuRxPeek(R, &data, &len, &last)) {
        if (last) ZiiuTxFreeNotice(Ctx, data, len);
        ZiiuRxRelease(Ctx, R);
        n++;
    }
    return n;
}

// --- DMA start/stop ----------------------------------------------------------

// mt792x_dma_disable(dev, force = true)
static NTSTATUS ZiiuDmaDisable(_In_ PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status;

    ZiiuClear(Ctx, MT_WFDMA0_GLO_CFG,
              GLO_CFG_TX_DMA_EN | GLO_CFG_RX_DMA_EN | GLO_CFG_DISP_BASE_PTR_CHAIN_EN |
              GLO_CFG_OMIT_TX_INFO | GLO_CFG_OMIT_RX_INFO | GLO_CFG_OMIT_RX_INFO_PFET2);
    status = ZiiuPoll(Ctx, MT_WFDMA0_GLO_CFG, GLO_CFG_TX_DMA_BUSY | GLO_CFG_RX_DMA_BUSY, 0, 100, NULL);

    ZiiuClear(Ctx, MT_WFDMA0_GLO_CFG_EXT0, GLO_CFG_EXT0_TX_DMASHDL_EN);
    ZiiuChipSet(Ctx, MT_DMASHDL_SW_CONTROL, MT_DMASHDL_DMASHDL_BYPASS);

    ZiiuClear(Ctx, MT_WFDMA0_RST, MT_WFDMA0_RST_DMASHDL_ALL_RST | MT_WFDMA0_RST_LOGIC_RST);
    ZiiuSet(Ctx, MT_WFDMA0_RST, MT_WFDMA0_RST_DMASHDL_ALL_RST | MT_WFDMA0_RST_LOGIC_RST);
    return status;
}

static const ULONG g_UnusedTxRings[] = { 1, 2, 3 };
static const ULONG g_UnusedRxRings[] = { 1, 3 };

// Stop DMA and return the Wi-Fi subsystem to its ROM state (mt7925e_unregister_device:
// drv own, dma cleanup, wfsys reset). Leaves no ring pointing at our memory.
static NTSTATUS ZiiuDmaStop(_In_ PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status;
    ULONG i;

    (VOID)ZiiuDrvOwn(Ctx);
    ZiiuWr(Ctx, MT_WFDMA0_HOST_INT_ENA, 0);
    status = ZiiuDmaDisable(Ctx);
    if (!NT_SUCCESS(status)) {
        ZiiuPrint("DMA still busy after disable (0x%08x)", status);
    }

    // Empty every ring we programmed and drop its base address.
    if (Ctx->DescMem != NULL) {
        const ZIIU_RING* rings[] = { &Ctx->TxData, &Ctx->TxMcu, &Ctx->TxFwdl, &Ctx->RxMcu, &Ctx->RxData };
        for (i = 0; i < ARRAYSIZE(rings); i++) {
            ZiiuWr(Ctx, rings[i]->Regs + RING_CPU_IDX, 0);
            ZiiuWr(Ctx, rings[i]->Regs + RING_DESC_BASE, 0);
        }
        for (i = 0; i < ARRAYSIZE(g_UnusedTxRings); i++) {
            ZiiuWr(Ctx, MT_RING_REGS(MT_TX_RING_BASE, g_UnusedTxRings[i]) + RING_CPU_IDX, 0);
            ZiiuWr(Ctx, MT_RING_REGS(MT_TX_RING_BASE, g_UnusedTxRings[i]) + RING_DESC_BASE, 0);
        }
        for (i = 0; i < ARRAYSIZE(g_UnusedRxRings); i++) {
            ZiiuWr(Ctx, MT_RING_REGS(MT_RX_RING_BASE, g_UnusedRxRings[i]) + RING_CPU_IDX, 0);
            ZiiuWr(Ctx, MT_RING_REGS(MT_RX_RING_BASE, g_UnusedRxRings[i]) + RING_DESC_BASE, 0);
        }
        ZiiuWr(Ctx, MT_WFDMA0_RST_DTX_PTR, ~0u);
        ZiiuWr(Ctx, MT_WFDMA0_RST_DRX_PTR, ~0u);
    }
    Ctx->DmaActive = FALSE;

    status = ZiiuWfsysReset(Ctx);
    ZiiuPrint("DMA stopped, wfsys reset 0x%08x", status);
    return status;
}

// mt792x_dma_enable for mt7925, without interrupts.
static VOID ZiiuDmaEnable(_In_ PDEVICE_CONTEXT Ctx)
{
    // mt7925_dma_prefetch_setup
    ZiiuWr(Ctx, MT_WFDMA0_RX_RING_EXT_CTRL(0), MT_WFDMA_PREFETCH(0x0000, 0x4));
    ZiiuWr(Ctx, MT_WFDMA0_RX_RING_EXT_CTRL(1), MT_WFDMA_PREFETCH(0x0040, 0x4));
    ZiiuWr(Ctx, MT_WFDMA0_RX_RING_EXT_CTRL(2), MT_WFDMA_PREFETCH(0x0080, 0x4));
    ZiiuWr(Ctx, MT_WFDMA0_RX_RING_EXT_CTRL(3), MT_WFDMA_PREFETCH(0x00c0, 0x4));
    ZiiuWr(Ctx, MT_WFDMA0_TX_RING_EXT_CTRL(0), MT_WFDMA_PREFETCH(0x0100, 0x10));
    ZiiuWr(Ctx, MT_WFDMA0_TX_RING_EXT_CTRL(1), MT_WFDMA_PREFETCH(0x0200, 0x10));
    ZiiuWr(Ctx, MT_WFDMA0_TX_RING_EXT_CTRL(2), MT_WFDMA_PREFETCH(0x0300, 0x10));
    ZiiuWr(Ctx, MT_WFDMA0_TX_RING_EXT_CTRL(3), MT_WFDMA_PREFETCH(0x0400, 0x10));
    ZiiuWr(Ctx, MT_WFDMA0_TX_RING_EXT_CTRL(15), MT_WFDMA_PREFETCH(0x0500, 0x4));
    ZiiuWr(Ctx, MT_WFDMA0_TX_RING_EXT_CTRL(16), MT_WFDMA_PREFETCH(0x0540, 0x4));

    // reset dma idx
    ZiiuWr(Ctx, MT_WFDMA0_RST_DTX_PTR, ~0u);
    ZiiuWr(Ctx, MT_WFDMA0_RST_DRX_PTR, ~0u);
    ZiiuWr(Ctx, MT_WFDMA0_PRI_DLY_INT_CFG0, 0);

    ZiiuSet(Ctx, MT_WFDMA0_GLO_CFG,
            GLO_CFG_TX_WB_DDONE | GLO_CFG_FIFO_LITTLE_ENDIAN | GLO_CFG_CLK_GAT_DIS |
            GLO_CFG_OMIT_TX_INFO | GLO_CFG_DMA_SIZE_3 | GLO_CFG_FIFO_DIS_CHECK |
            GLO_CFG_RX_WB_DDONE | GLO_CFG_DISP_BASE_PTR_CHAIN_EN | GLO_CFG_OMIT_RX_INFO_PFET2);
    ZiiuSet(Ctx, MT_WFDMA0_GLO_CFG, GLO_CFG_TX_DMA_EN | GLO_CFG_RX_DMA_EN);

    // mt7925_wfdma_setup
    ZiiuChipSet(Ctx, MT_UWFDMA0_GLO_CFG_EXT1, 1u << 28);
    ZiiuSet(Ctx, MT_WFDMA0_INT_RX_PRI, 0x0F00);
    ZiiuSet(Ctx, MT_WFDMA0_INT_TX_PRI, 0x7F00);

    ZiiuChipSet(Ctx, MT_WFDMA_DUMMY_CR, MT_WFDMA_NEED_REINIT);
    // Interrupts stay masked: we poll.
}

// PCI command register: the device must be allowed to master the bus (it fetches our
// rings), and must not raise legacy INTx interrupts: we have no ISR, and an asserted,
// unhandled level interrupt could storm. (Our INF doesn't opt into MSI, so Windows
// assigns a line interrupt.) Config space goes through the PCI bus driver.
static NTSTATUS ZiiuPciSetup(_In_ PDEVICE_CONTEXT Ctx)
{
    USHORT cmd = 0, want;

    if (!Ctx->HaveBusIf || Ctx->BusIf.GetBusData == NULL || Ctx->BusIf.SetBusData == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (Ctx->BusIf.GetBusData(Ctx->BusIf.Context, PCI_WHICHSPACE_CONFIG, &cmd,
                              FIELD_OFFSET(PCI_COMMON_HEADER, Command), sizeof(cmd)) != sizeof(cmd)) {
        return STATUS_DEVICE_NOT_READY;
    }
    want = cmd | PCI_ENABLE_BUS_MASTER | PCI_DISABLE_LEVEL_INTERRUPT;
    if (want != cmd) {
        (VOID)Ctx->BusIf.SetBusData(Ctx->BusIf.Context, PCI_WHICHSPACE_CONFIG, &want,
                                    FIELD_OFFSET(PCI_COMMON_HEADER, Command), sizeof(want));
        (VOID)Ctx->BusIf.GetBusData(Ctx->BusIf.Context, PCI_WHICHSPACE_CONFIG, &cmd,
                                    FIELD_OFFSET(PCI_COMMON_HEADER, Command), sizeof(cmd));
    }
    Ctx->PciCommand = cmd;
    ZiiuPrint("PCI command 0x%04x", cmd);
    if ((cmd & (PCI_ENABLE_BUS_MASTER | PCI_DISABLE_LEVEL_INTERRUPT)) !=
        (PCI_ENABLE_BUS_MASTER | PCI_DISABLE_LEVEL_INTERRUPT)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    return STATUS_SUCCESS;
}

// Device-level interrupt masks (we poll; nothing may interrupt).
static VOID ZiiuMaskInterrupts(_In_ PDEVICE_CONTEXT Ctx)
{
    ZiiuWr(Ctx, MT_WFDMA0_HOST_INT_ENA, 0);
    ZiiuWr(Ctx, MT7925_PCIE_MAC_INT_ENABLE, 0);
    ZiiuWr(Ctx, MT_MCU2HOST_SW_INT_ENA, 0);
}

// mt7925_pci_probe (from the ownership handshake on) + mt7925e_mcu_init up to the
// firmware download: reset the Wi-Fi subsystem, set up rings, enable DMA.
static NTSTATUS ZiiuDmaStart(_In_ PDEVICE_CONTEXT Ctx)
{
    NTSTATUS status;
    ULONG chip, i;

    status = ZiiuDmaAlloc(Ctx);  // before touching the chip
    if (!NT_SUCCESS(status)) {
        ZiiuPrint("DMA memory allocation failed 0x%08x", status);
        return status;
    }
    status = ZiiuPciSetup(Ctx);  // before touching the chip
    if (!NT_SUCCESS(status)) {
        ZiiuPrint("PCI setup failed 0x%08x, not starting DMA", status);
        return status;
    }
    if (Ctx->DmaActive) {
        (VOID)ZiiuDmaStop(Ctx);
    }

    status = ZiiuFwOwn(Ctx);
    if (NT_SUCCESS(status)) {
        status = ZiiuDrvOwn(Ctx);
    }
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ZiiuMaskInterrupts(Ctx);
    chip = ZiiuChipRr(Ctx, MT_HW_CHIPID);
    if (chip != 0x7925) {
        ZiiuPrint("unexpected chip id 0x%08x, not starting DMA", chip);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    ZiiuChipSet(Ctx, MT_HW_EMI_CTL, MT_HW_EMI_CTL_SLPPROT_EN);

    status = ZiiuWfsysReset(Ctx);
    if (!NT_SUCCESS(status)) {
        ZiiuPrint("wfsys reset failed 0x%08x", status);
        return status;
    }

    ZiiuMaskInterrupts(Ctx);  // again, in case the reset restored defaults

    status = ZiiuDmaDisable(Ctx);
    if (!NT_SUCCESS(status)) {
        ZiiuPrint("DMA busy before setup 0x%08x", status);
        return status;
    }

    RtlZeroMemory(Ctx->TxFrameBusy, sizeof(Ctx->TxFrameBusy));
    RtlZeroMemory(&Ctx->TxStats, sizeof(Ctx->TxStats));
    RtlZeroMemory(&Ctx->RxStats, sizeof(Ctx->RxStats));
    ZiiuRingProgram(Ctx, &Ctx->TxData, FALSE);
    ZiiuWr(Ctx, MT_WFDMA0_TX_RING_EXT_CTRL(0), 0x4);
    ZiiuRingProgram(Ctx, &Ctx->TxMcu, FALSE);
    ZiiuRingProgram(Ctx, &Ctx->TxFwdl, FALSE);
    ZiiuRingProgram(Ctx, &Ctx->RxMcu, TRUE);
    ZiiuRingProgram(Ctx, &Ctx->RxData, TRUE);
    for (i = 0; i < ARRAYSIZE(g_UnusedTxRings); i++) {
        ZiiuRingNeutralize(Ctx, MT_RING_REGS(MT_TX_RING_BASE, g_UnusedTxRings[i]));
    }
    for (i = 0; i < ARRAYSIZE(g_UnusedRxRings); i++) {
        ZiiuRingNeutralize(Ctx, MT_RING_REGS(MT_RX_RING_BASE, g_UnusedRxRings[i]));
    }

    Ctx->DmaActive = TRUE;  // from here on, stop before freeing anything
    ZiiuDmaEnable(Ctx);

    // mt7925e_mcu_init: ownership round trip, disable L0s.
    status = ZiiuFwOwn(Ctx);
    if (NT_SUCCESS(status)) {
        status = ZiiuDrvOwn(Ctx);
    }
    if (!NT_SUCCESS(status)) {
        (VOID)ZiiuDmaStop(Ctx);
        return status;
    }
    ZiiuSet(Ctx, MT7925_PCIE_MAC_PM, MT_PCIE_MAC_PM_L0S_DIS);

    ZiiuPrint("DMA started: GLO_CFG 0x%08x, MCU rx cpu %u dma %u",
              ZiiuRr(Ctx, MT_WFDMA0_GLO_CFG), ZiiuRr(Ctx, Ctx->RxMcu.Regs + RING_CPU_IDX),
              ZiiuRr(Ctx, Ctx->RxMcu.Regs + RING_DMA_IDX));
    return STATUS_SUCCESS;
}

// Queue an 802.11 frame on the data ring. Tokens and DMA addresses are entirely
// driver-owned. A full ring/pool fails promptly; user mode must drain RX notices.
static NTSTATUS ZiiuTxFrame(_In_ PDEVICE_CONTEXT Ctx, _Inout_ ZIIU_OP_ARGS* A)
{
    ZIIU_TX_FRAME req;
    ZIIU_RING* r = &Ctx->TxData;
    const UCHAR* frame;
    PUCHAR slot;
    ULONG token, dma, next, pa, fc;
    volatile ZIIU_DESC* desc;
    if (!Ctx->DmaActive) return STATUS_DEVICE_NOT_READY;
    if (A->InLength < sizeof(req)) return STATUS_BUFFER_TOO_SMALL;
    RtlCopyMemory(&req, A->In, sizeof(req));
    if (req.Length < 24 || req.Length > ZIIU_TX_FRAME_MAX ||
        req.Length != A->InLength - sizeof(req) || req.WlanIndex > 4095 ||
        (req.Flags & ~ZIIU_TX_F_ALL) || req.Reserved2) return STATUS_INVALID_PARAMETER;
    frame = (const UCHAR*)A->In + sizeof(req);
    fc = frame[0] | ((ULONG)frame[1] << 8);
    // v0 management (cleartext) or data/QoS data (cleartext or already CCMP-encrypted
    // by user mode). No fragments, +HTC/order, four-address headers or control frames.
    // Firmware never receives user TXD.
    if ((fc & 3) || (fc & 0x8400) || (fc & 0x0300) == 0x0300 || (frame[22] & 15))
        return STATUS_INVALID_PARAMETER;
    if ((fc & 0x0c) == 0) {
        if (fc & 0x4000) return STATUS_INVALID_PARAMETER;
    } else if ((fc & 0xfc) == 0x88) {
        if (req.Length < 26) return STATUS_INVALID_PARAMETER;
    } else if ((fc & 0xfc) != 0x08) {
        return STATUS_INVALID_PARAMETER;
    }
    dma = ZiiuRr(Ctx, r->Regs + RING_DMA_IDX);
    if (dma == ZIIU_REG_DEAD) return STATUS_DEVICE_NOT_CONNECTED;
    if (dma >= r->Ndesc) return STATUS_DEVICE_DATA_ERROR;
    next = (r->Head + 1) % r->Ndesc;
    if (next == dma) return STATUS_DEVICE_BUSY;
    for (token = 0; token < ZIIU_TX_DATA_NDESC; token++) {
        if (!Ctx->TxFrameBusy[token]) break;
    }
    if (token == ZIIU_TX_DATA_NDESC) return STATUS_DEVICE_BUSY;
    slot = r->Buf + token * ZIIU_TX_FRAME_SLOT;
    pa = r->BufPa + token * ZIIU_TX_FRAME_SLOT;
    ZiiuTxBuildHeader(slot, frame, req.Length, req.WlanIndex, token, pa + 64, req.Flags);
    RtlCopyMemory(slot + 64, frame, req.Length);
    Ctx->TxFrameBusy[token] = TRUE;
    Ctx->TxSubmitTime[token] = KeQueryInterruptTime();
    Ctx->TxStats.Queued++;
    desc = &r->Desc[r->Head];
    desc->Buf0 = pa; desc->Buf1 = 0; desc->Info = 0;
    desc->Ctrl = (64u << 16) | MT_DMA_CTL_LAST_SEC0;
    r->Head = next;
    KeMemoryBarrier();
    ZiiuWr(Ctx, r->Regs + RING_CPU_IDX, next);
    return STATUS_SUCCESS;
}

// --- MCU messages --------------------------------------------------------------

// Event header layout (struct mt7925_mcu_rxd): 32 bytes RXD, len, pkt_type_id, eid, seq, option.
#define ZIIU_EVT_SEQ_OFFSET     37
#define ZIIU_EVT_OPTION_OFFSET  38
#define ZIIU_EVT_MIN_LENGTH     44
#define ZIIU_EVT_UNSOLICITED    0x04  // MCU_UNI_CMD_UNSOLICITED_EVENT

// Wait for the next complete event on the MCU RX ring and copy it to Out.
// Returns STATUS_IO_TIMEOUT if none arrives by the deadline (in ms ticks).
static NTSTATUS ZiiuRxNextEvent(_In_ PDEVICE_CONTEXT Ctx, _Out_writes_bytes_to_(OutLength, *Total) PUCHAR Out,
                                _In_ ULONG OutLength, _Out_ PULONG Total, _Inout_ PULONG ElapsedMs,
                                _In_ ULONG TimeoutMs)
{
    ULONG total = 0;
    BOOLEAN started = FALSE;

    *Total = 0;
    for (;;) {
        PUCHAR data;
        ULONG len;
        BOOLEAN last;

        if (ZiiuRxPeek(&Ctx->RxMcu, &data, &len, &last)) {
            ULONG n = min(len, OutLength - total);
            RtlCopyMemory(Out + total, data, n);
            total += n;
            started = TRUE;
            ZiiuRxRelease(Ctx, &Ctx->RxMcu);
            if (last) {
                *Total = total;
                ZiiuTxFreeNotice(Ctx, Out, total);
                return STATUS_SUCCESS;
            }
            continue;  // more fragments of the same event
        }
        if (*ElapsedMs >= TimeoutMs) {
            return started ? STATUS_PARTIAL_COPY : STATUS_IO_TIMEOUT;
        }
        ZiiuSleepMs(1);
        (*ElapsedMs)++;
    }
}

static NTSTATUS ZiiuMcuCmd(_In_ PDEVICE_CONTEXT Ctx, _Inout_ ZIIU_OP_ARGS* A)
{
    ZIIU_MCU_CMD cmd;
    const UCHAR* msg;
    ULONG elapsed = 0;
    NTSTATUS status;

    if (!Ctx->DmaActive) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (A->InLength < sizeof(ZIIU_MCU_CMD)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(&cmd, A->In, sizeof(cmd));
    if (cmd.Length == 0 || cmd.Length > ZIIU_MCU_MAX_MSG || cmd.Length > A->InLength - sizeof(cmd) ||
        cmd.TimeoutMs > ZIIU_MCU_MAX_WAIT_MS) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((cmd.Flags & ZIIU_MCU_WAIT_RESPONSE) && (A->Out == NULL || A->OutLength < ZIIU_EVT_MIN_LENGTH)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    msg = (const UCHAR*)A->In + sizeof(cmd);

    // Throw away leftovers (e.g. a late reply to a command that timed out) so they
    // can't be mistaken for this command's reply.
    Ctx->EventsSkipped += ZiiuRxDrain(Ctx, &Ctx->RxMcu);

    // The message is copied into the DMA buffer before the (shared, METHOD_BUFFERED)
    // output buffer is written.
    status = ZiiuTxSend(Ctx, &Ctx->TxMcu, msg, cmd.Length);
    if (!NT_SUCCESS(status) || !(cmd.Flags & ZIIU_MCU_WAIT_RESPONSE)) {
        return status;
    }

    for (;;) {
        PUCHAR out = (PUCHAR)A->Out;
        ULONG outLen = (ULONG)min(A->OutLength, ZIIU_MCU_MAX_EVENT);
        ULONG total;

        status = ZiiuRxNextEvent(Ctx, out, outLen, &total, &elapsed, cmd.TimeoutMs);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        if (total >= ZIIU_EVT_MIN_LENGTH && !(out[ZIIU_EVT_OPTION_OFFSET] & ZIIU_EVT_UNSOLICITED) &&
            out[ZIIU_EVT_SEQ_OFFSET] == (UCHAR)cmd.Seq) {
            A->Written = total;
            return STATUS_SUCCESS;
        }
        Ctx->EventsSkipped++;
    }
}

static NTSTATUS ZiiuFwScatter(_In_ PDEVICE_CONTEXT Ctx, _Inout_ ZIIU_OP_ARGS* A)
{
    const UCHAR* data = (const UCHAR*)A->In;
    size_t left = A->InLength;
    NTSTATUS status = STATUS_SUCCESS;

    if (!Ctx->DmaActive) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (left == 0 || left > ZIIU_FW_SCATTER_MAX) {
        return STATUS_INVALID_PARAMETER;
    }
    while (left > 0 && NT_SUCCESS(status)) {
        ULONG n = (ULONG)min(left, ZIIU_FW_CHUNK);
        status = ZiiuTxSend(Ctx, &Ctx->TxFwdl, data, n);
        data += n;
        left -= n;
    }
    return status;
}

static NTSTATUS ZiiuPollReg(_In_ PDEVICE_CONTEXT Ctx, _Inout_ ZIIU_OP_ARGS* A)
{
    ZIIU_POLL_REG req;
    ZIIU_POLL_RESULT* res;
    NTSTATUS status;

    if (A->InLength < sizeof(req) || A->OutLength < sizeof(ZIIU_POLL_RESULT)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(&req, A->In, sizeof(req));
    if (!ZiiuChipAddrOk(req.Addr) || (req.Addr & 3) || req.TimeoutMs > ZIIU_POLL_MAX_MS) {
        return STATUS_INVALID_PARAMETER;
    }
    res = (ZIIU_POLL_RESULT*)A->Out;
    RtlZeroMemory(res, sizeof(*res));
    A->Written = sizeof(*res);
    if (req.Mask == 0 && req.TimeoutMs == 0) {  // plain read, any value
        res->Value = ZiiuChipRr(Ctx, req.Addr);
        res->Matched = 1;
        return STATUS_SUCCESS;
    }
    status = ZiiuPoll(Ctx, req.Addr, req.Mask, req.Value, req.TimeoutMs, &res->Value);
    res->Matched = NT_SUCCESS(status);
    return (status == STATUS_IO_TIMEOUT) ? STATUS_SUCCESS : status;  // timeout = Matched 0
}

// Write one register in the Wi-Fi MAC block only (see IOCTL_ZIIU_WRITE_REG).
static NTSTATUS ZiiuWriteReg(_In_ PDEVICE_CONTEXT Ctx, _Inout_ ZIIU_OP_ARGS* A)
{
    ZIIU_WRITE_REG req;
    ULONG bar;

    if (!Ctx->DmaActive) {  // firmware/MAC only configured while running
        return STATUS_DEVICE_NOT_READY;
    }
    if (A->InLength < sizeof(req)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(&req, A->In, sizeof(req));
    if (req.Addr < ZIIU_WRITE_REG_MIN || req.Addr > ZIIU_WRITE_REG_MAX || (req.Addr & 3) ||
        !ZiiuFixedMap(req.Addr, &bar)) {
        return STATUS_ACCESS_DENIED;
    }
    ZiiuWr(Ctx, bar, req.Value);
    return STATUS_SUCCESS;
}

// Copy complete frames from the data RX ring into the caller's buffer as
// { ULONG Length; Data; pad to 4 } records. Frames split over several descriptors
// (more than one RX slot) are joined.
static NTSTATUS ZiiuRxRead(_In_ PDEVICE_CONTEXT Ctx, _Inout_ ZIIU_OP_ARGS* A)
{
    ZIIU_RING* r = &Ctx->RxData;
    ZIIU_RX_READ req;
    PUCHAR out = (PUCHAR)A->Out;
    size_t cap = A->OutLength, used = 0;
    ULONG waited = 0;

    if (!Ctx->DmaActive) {
        return STATUS_DEVICE_NOT_READY;
    }
    if (A->InLength < sizeof(req) || out == NULL || cap < sizeof(ULONG) + r->SlotSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(&req, A->In, sizeof(req));  // In and Out share the buffer
    if (req.WaitMs > ZIIU_RX_MAX_WAIT_MS) {
        return STATUS_INVALID_PARAMETER;
    }

    while (cap - used >= sizeof(ULONG) + r->SlotSize) {
        PUCHAR data;
        ULONG len, total = 0, wait;
        BOOLEAN last, fits = TRUE;
        size_t start = used + sizeof(ULONG);

        // TX-free notices may arrive on the MCU queue. No MCU request is in
        // flight here (sequential hardware lock), so discard unsolicited events
        // after reclaiming their tokens instead of exhausting the TX pool.
        Ctx->EventsSkipped += ZiiuRxDrain(Ctx, &Ctx->RxMcu);

        if (!ZiiuRxPeek(r, &data, &len, &last)) {
            if (used > 0 || waited >= req.WaitMs) {
                break;
            }
            ZiiuSleepMs(1);
            waited++;
            continue;
        }
        for (;;) {
            if (fits && start + total + len <= cap) {
                RtlCopyMemory(out + start + total, data, len);
                total += len;
            } else {
                fits = FALSE;
            }
            ZiiuRxRelease(Ctx, r);
            if (last) {
                break;
            }
            for (wait = 0; !ZiiuRxPeek(r, &data, &len, &last); wait++) {
                if (wait >= 50) {  // the rest of the frame never came
                    fits = FALSE;
                    break;
                }
                ZiiuSleepMs(1);
            }
            if (wait >= 50) {
                break;
            }
        }
        if (fits) {
            ZiiuTxFreeNotice(Ctx, out + start, total);
            *(ULONG UNALIGNED*)(out + used) = total;
            used = start + ((total + 3) & ~3u);
            Ctx->RxFrames++;
        } else {
            Ctx->RxDropped++;
        }
    }
    A->Written = used;
    return STATUS_SUCCESS;
}

// Runs a hardware operation under the lock with the crash latch armed.
static NTSTATUS ZiiuRunHwOp(_In_ PDEVICE_CONTEXT Ctx, _In_ ZIIU_HW_OP Op, _Inout_ ZIIU_OP_ARGS* A)
{
    NTSTATUS status;

    if (Ctx->Bar == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    WdfWaitLockAcquire(Ctx->HwLock, NULL);
    if (Ctx->LatchedOp != ZiiuHwOpNone) {
        // A previous hardware operation crashed the PC. Don't try again until cleared.
        status = STATUS_DEVICE_HUNG;
    } else {
        // Radio session: the first TX frame sets the latch to ZiiuHwOpRadioSession and
        // leaves it set until DMA stops, so a crash anywhere in the session is still
        // latched. Inside a session the per-operation writes are skipped.
        if (Ctx->SessionLatched) {
            status = STATUS_SUCCESS;
        } else if (Op == ZiiuHwOpTxFrame && Ctx->DmaActive) {
            status = ZiiuWriteLatch(ZiiuHwOpRadioSession);
            if (NT_SUCCESS(status)) Ctx->SessionLatched = TRUE;
        } else {
            status = ZiiuWriteLatch(Op);
        }
        if (NT_SUCCESS(status)) {
            switch (Op) {
            case ZiiuHwOpReadChipId:
                if (A->OutLength < sizeof(ZIIU_CHIP_ID)) {
                    status = STATUS_BUFFER_TOO_SMALL;
                } else {
                    status = ZiiuReadChipId(Ctx, (ZIIU_CHIP_ID*)A->Out);
                    A->Written = sizeof(ZIIU_CHIP_ID);
                }
                break;
            case ZiiuHwOpDmaStart:
                status = ZiiuDmaStart(Ctx);
                break;
            case ZiiuHwOpDmaStop:
                status = Ctx->DmaActive ? ZiiuDmaStop(Ctx) : STATUS_SUCCESS;
                break;
            case ZiiuHwOpMcuCmd:
                status = ZiiuMcuCmd(Ctx, A);
                break;
            case ZiiuHwOpFwScatter:
                status = ZiiuFwScatter(Ctx, A);
                break;
            case ZiiuHwOpPollReg:
                status = ZiiuPollReg(Ctx, A);
                break;
            case ZiiuHwOpWriteReg:
                status = ZiiuWriteReg(Ctx, A);
                break;
            case ZiiuHwOpTxFrame:
                status = ZiiuTxFrame(Ctx, A);
                break;
            default:
                status = STATUS_INVALID_PARAMETER;
                break;
            }
            // Reaching this point means no crash; disarm (a session only once DMA is off).
            if (!Ctx->SessionLatched || !Ctx->DmaActive) {
                if (NT_SUCCESS(ZiiuWriteLatch(ZiiuHwOpNone))) Ctx->SessionLatched = FALSE;
            }
        }
    }
    WdfWaitLockRelease(Ctx->HwLock);
    return status;
}

// ---------------------------------------------------------------------------
// IOCTL dispatch
// ---------------------------------------------------------------------------

static ZIIU_HW_OP ZiiuIoctlToOp(_In_ ULONG IoControlCode)
{
    switch (IoControlCode) {
    case IOCTL_ZIIU_READ_CHIP_ID: return ZiiuHwOpReadChipId;
    case IOCTL_ZIIU_DMA_START:    return ZiiuHwOpDmaStart;
    case IOCTL_ZIIU_DMA_STOP:     return ZiiuHwOpDmaStop;
    case IOCTL_ZIIU_MCU_CMD:      return ZiiuHwOpMcuCmd;
    case IOCTL_ZIIU_FW_SCATTER:   return ZiiuHwOpFwScatter;
    case IOCTL_ZIIU_POLL_REG:     return ZiiuHwOpPollReg;
    case IOCTL_ZIIU_WRITE_REG:    return ZiiuHwOpWriteReg;
    case IOCTL_ZIIU_TX_FRAME:     return ZiiuHwOpTxFrame;
    default:                      return ZiiuHwOpNone;
    }
}

VOID ZiiuEvtIoDeviceControl(_In_ WDFQUEUE Queue, _In_ WDFREQUEST Request, _In_ size_t OutputBufferLength,
                            _In_ size_t InputBufferLength, _In_ ULONG IoControlCode)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(WdfIoQueueGetDevice(Queue));
    ZIIU_OP_ARGS args;
    ZIIU_HW_OP op;
    NTSTATUS status;
    ULONG_PTR info = 0;
    PVOID out;

    RtlZeroMemory(&args, sizeof(args));
    if (InputBufferLength > 0) {
        status = WdfRequestRetrieveInputBuffer(Request, 1, &args.In, &args.InLength);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }
    }
    if (OutputBufferLength > 0) {
        status = WdfRequestRetrieveOutputBuffer(Request, 1, &args.Out, &args.OutLength);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }
    }

    switch (IoControlCode) {
    case IOCTL_ZIIU_GET_INFO:
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ZIIU_INFO), &out, NULL);
        if (NT_SUCCESS(status)) {
            ZIIU_INFO* i = (ZIIU_INFO*)out;
            RtlZeroMemory(i, sizeof(*i));
            i->InterfaceVersion = ZIIU_WIFI_INTERFACE_VERSION;
            i->BarMapped = ctx->Bar != NULL;
            i->BarLength = (ULONG)ctx->BarLength;
            i->LatchedOp = ctx->LatchedOp;
            i->DmaActive = ctx->DmaActive;
            i->PciCommand = ctx->PciCommand;
            i->EventsSkipped = ctx->EventsSkipped;
            i->RxFrames = ctx->RxFrames;
            i->RxDropped = ctx->RxDropped;
            info = sizeof(ZIIU_INFO);
        }
        break;

    case IOCTL_ZIIU_RX_STATS:
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ZIIU_RX_STATS), &out, NULL);
        if (NT_SUCCESS(status)) {
            WdfWaitLockAcquire(ctx->HwLock, NULL);
            RtlCopyMemory(out, &ctx->RxStats, sizeof(ctx->RxStats));
            WdfWaitLockRelease(ctx->HwLock);
            info = sizeof(ZIIU_RX_STATS);
        }
        break;

    case IOCTL_ZIIU_TX_STATS:
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ZIIU_TX_STATS), &out, NULL);
        if (NT_SUCCESS(status)) {
            ULONG n;
            ZIIU_TX_STATS* stats = (ZIIU_TX_STATS*)out;
            WdfWaitLockAcquire(ctx->HwLock, NULL);
            *stats = ctx->TxStats;
            stats->BusyMask = 0;
            for (n = 0; n < ZIIU_TX_DATA_NDESC; n++) {
                if (ctx->TxFrameBusy[n]) stats->BusyMask |= 1u << n;
            }
            WdfWaitLockRelease(ctx->HwLock);
            info = sizeof(*stats);
        }
        break;

    case IOCTL_ZIIU_RX_READ:
        // Not latched: polled many times a second; touches only ring memory and the
        // ring's CPU index. Still refused after a crash, like everything else.
        WdfWaitLockAcquire(ctx->HwLock, NULL);
        if (ctx->Bar == NULL) {
            status = STATUS_DEVICE_NOT_READY;
        } else if (ctx->LatchedOp != ZiiuHwOpNone) {
            status = STATUS_DEVICE_HUNG;
        } else {
            status = ZiiuRxRead(ctx, &args);
            info = NT_SUCCESS(status) ? args.Written : 0;
        }
        WdfWaitLockRelease(ctx->HwLock);
        break;

    case IOCTL_ZIIU_CLEAR_LATCH:
        WdfWaitLockAcquire(ctx->HwLock, NULL);
        status = ZiiuWriteLatch(ZiiuHwOpNone);
        if (NT_SUCCESS(status)) {
            ctx->LatchedOp = ZiiuHwOpNone;
            ctx->SessionLatched = FALSE;  // the next TX frame re-arms it
        }
        WdfWaitLockRelease(ctx->HwLock);
        break;

    default:
        op = ZiiuIoctlToOp(IoControlCode);
        if (op == ZiiuHwOpNone) {
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }
        status = ZiiuRunHwOp(ctx, op, &args);
        if (NT_SUCCESS(status)) {
            info = args.Written;
        }
        break;
    }
    WdfRequestCompleteWithInformation(Request, status, info);
}

// ---------------------------------------------------------------------------
// PnP/power. Only maps/unmaps BAR0; touches the chip only to stop DMA that a tool
// started (see top of file).
// ---------------------------------------------------------------------------

// Length of a CmResourceTypeMemory/MemoryLarge descriptor (64-bit BARs use the large forms).
static ULONGLONG ZiiuMemResourceLength(_In_ PCM_PARTIAL_RESOURCE_DESCRIPTOR d)
{
    if (d->Type == CmResourceTypeMemoryLarge) {
        if (d->Flags & CM_RESOURCE_MEMORY_LARGE_40) return (ULONGLONG)d->u.Memory40.Length40 << 8;
        if (d->Flags & CM_RESOURCE_MEMORY_LARGE_48) return (ULONGLONG)d->u.Memory48.Length48 << 16;
        if (d->Flags & CM_RESOURCE_MEMORY_LARGE_64) return (ULONGLONG)d->u.Memory64.Length64 << 32;
        return 0;
    }
    return d->u.Memory.Length;
}

NTSTATUS ZiiuEvtPrepareHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesRaw,
                                _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    ULONG i, count = WdfCmResourceListGetCount(ResourcesTranslated);

    UNREFERENCED_PARAMETER(ResourcesRaw);

    // PCI config access through the bus driver; no chip register access.
    if (NT_SUCCESS(WdfFdoQueryForInterface(Device, &GUID_BUS_INTERFACE_STANDARD, (PINTERFACE)&ctx->BusIf,
                                           sizeof(BUS_INTERFACE_STANDARD), 1, NULL))) {
        ctx->HaveBusIf = TRUE;
    }

    for (i = 0; i < count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        PHYSICAL_ADDRESS start;
        ULONGLONG length;

        if (d == NULL || (d->Type != CmResourceTypeMemory && d->Type != CmResourceTypeMemoryLarge)) {
            continue;
        }
        start = d->u.Memory.Start;
        length = ZiiuMemResourceLength(d);
        if (length < MT7925_MIN_BAR_LENGTH || length > 0x10000000ull) {
            // Unexpected BAR: stay loaded but unusable rather than failing the device.
            ZiiuPrint("BAR0 length 0x%llx unexpected, hardware access disabled", length);
            return STATUS_SUCCESS;
        }
        ctx->Bar = (PUCHAR)MmMapIoSpaceEx(start, (SIZE_T)length, PAGE_READWRITE | PAGE_NOCACHE);
        if (ctx->Bar != NULL) {
            ctx->BarLength = (SIZE_T)length;
        }
        ZiiuPrint("BAR0 0x%llx len 0x%llx mapped=%d", start.QuadPart, length, ctx->Bar != NULL);
        return STATUS_SUCCESS;  // first memory BAR is BAR0
    }
    ZiiuPrint("no memory BAR, hardware access disabled");
    return STATUS_SUCCESS;
}

NTSTATUS ZiiuEvtReleaseHardware(_In_ WDFDEVICE Device, _In_ WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (ctx->Bar != NULL) {
        MmUnmapIoSpace(ctx->Bar, ctx->BarLength);
        ctx->Bar = NULL;
        ctx->BarLength = 0;
    }
    if (ctx->HaveBusIf) {
        ctx->BusIf.InterfaceDereference(ctx->BusIf.Context);
        ctx->HaveBusIf = FALSE;
    }
    return STATUS_SUCCESS;
}

// Leaving D0 (driver switch, removal, sleep, shutdown): if a tool left DMA running,
// stop it while our ring memory still exists. Never runs chip code otherwise.
NTSTATUS ZiiuEvtD0Exit(_In_ WDFDEVICE Device, _In_ WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    if (ctx->Bar == NULL) {
        return STATUS_SUCCESS;
    }
    WdfWaitLockAcquire(ctx->HwLock, NULL);
    if (ctx->DmaActive) {
        // Stopping DMA matters more than the latch here, so stop even if the latch
        // write fails.
        NTSTATUS latch = ZiiuWriteLatch(ZiiuHwOpDmaStop);
        (VOID)ZiiuDmaStop(ctx);
        if (NT_SUCCESS(latch) && NT_SUCCESS(ZiiuWriteLatch(ZiiuHwOpNone))) {
            ctx->SessionLatched = FALSE;
        }
    }
    WdfWaitLockRelease(ctx->HwLock);
    return STATUS_SUCCESS;
}

NTSTATUS ZiiuEvtDeviceAdd(_In_ WDFDRIVER Driver, _Inout_ PWDFDEVICE_INIT DeviceInit)
{
    // Access (SYSTEM + Administrators only) is set by the INF's Security value, not here:
    // WdfDeviceInitAssignSDDLString on this unnamed PnP FDO fails with
    // STATUS_INVALID_SECURITY_DESCR (seen on first install).
    WDF_PNPPOWER_EVENT_CALLBACKS pnp;
    WDF_OBJECT_ATTRIBUTES attrs;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_DMA_ENABLER_CONFIG dmaConfig;
    WDFDEVICE device;
    PDEVICE_CONTEXT ctx;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnp);
    pnp.EvtDevicePrepareHardware = ZiiuEvtPrepareHardware;
    pnp.EvtDeviceReleaseHardware = ZiiuEvtReleaseHardware;
    pnp.EvtDeviceD0Exit = ZiiuEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attrs, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attrs, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    ctx = GetDeviceContext(device);

    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ParentObject = device;
    status = WdfWaitLockCreate(&attrs, &ctx->HwLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (!NT_SUCCESS(ZiiuReadLatch(&ctx->LatchedOp))) {
        ctx->LatchedOp = ZiiuHwOpNone;
    }
    if (ctx->LatchedOp != ZiiuHwOpNone) {
        ZiiuPrint("crash latch set (op %lu): hardware operations disabled until cleared", ctx->LatchedOp);
    }

    // 32-bit DMA (mt7925 uses dma_set_mask(32)). Only creates the adapter object; ring
    // memory is allocated on the first DMA start. If this fails the driver still loads,
    // DMA operations just report "not ready".
    WDF_DMA_ENABLER_CONFIG_INIT(&dmaConfig, WdfDmaProfileScatterGather, ZIIU_FW_SCATTER_MAX);
    if (!NT_SUCCESS(WdfDmaEnablerCreate(device, &dmaConfig, WDF_NO_OBJECT_ATTRIBUTES, &ctx->DmaEnabler))) {
        ctx->DmaEnabler = NULL;
        ZiiuPrint("DMA enabler creation failed, DMA disabled");
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = ZiiuEvtIoDeviceControl;
    WDF_OBJECT_ATTRIBUTES_INIT(&attrs);
    attrs.ExecutionLevel = WdfExecutionLevelPassive;  // registry + waits need PASSIVE_LEVEL
    status = WdfIoQueueCreate(device, &queueConfig, &attrs, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return WdfDeviceCreateDeviceInterface(device, &GUID_DEVINTERFACE_ZIIU_WIFI, NULL);
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, ZiiuEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

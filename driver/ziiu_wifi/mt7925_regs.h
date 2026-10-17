// MT7925 register definitions.
//
// Values taken from the mt76 Linux driver (https://github.com/openwrt/mt76):
// mt792x_regs.h, mt7925/regs.h, mt7925/pci.c, mt792x_dma.c, dma.h. mt76 is
// BSD-3-Clause / BSD-3-Clause-Clear, Copyright (C) 2016 Felix Fietkau,
// (C) 2020-2023 MediaTek Inc. and contributors.
#pragma once

// ---------------------------------------------------------------------------
// Chip-bus addresses (translated to BAR offsets by ZiiuChipRr/ZiiuChipWr)
// ---------------------------------------------------------------------------
#define MT_CONN_ON_LPCTL            0x7c060010u  // host/firmware power ownership
#define   PCIE_LPCR_HOST_SET_OWN    (1u << 0)
#define   PCIE_LPCR_HOST_CLR_OWN    (1u << 1)
#define   PCIE_LPCR_HOST_OWN_SYNC   (1u << 2)
#define MT_CONN_ON_MISC             0x7c0600f0u  // firmware state
#define   MT_TOP_MISC2_FW_PWR_ON    (1u << 0)
#define   MT_TOP_MISC2_FW_N9_RDY    0x3u
#define MT_HW_CHIPID                0x70010200u
#define MT_HW_REV                   0x70010204u
#define MT_HW_EMI_CTL               0x18011100u
#define   MT_HW_EMI_CTL_SLPPROT_EN  (1u << 1)
#define MT_WFSYS_SW_RST_B           0x7c000140u  // Wi-Fi subsystem reset only (not BT)
#define   WFSYS_SW_RST_B            (1u << 0)
#define   WFSYS_SW_INIT_DONE        (1u << 4)
#define MT_DMASHDL_SW_CONTROL       0x7c026004u
#define   MT_DMASHDL_DMASHDL_BYPASS (1u << 28)
#define MT_UWFDMA0_GLO_CFG_EXT1     0x7c0242b4u
#define MT_WFDMA_DUMMY_CR           0x54000120u
#define   MT_WFDMA_NEED_REINIT      (1u << 1)

// ---------------------------------------------------------------------------
// BAR0 offsets (addresses below 0x200000 are BAR offsets already)
// ---------------------------------------------------------------------------
#define MT_HIF_REMAP_L1             0x155024u
#define   MT_HIF_REMAP_L1_MASK      0xffff0000u
#define MT_HIF_REMAP_BASE_L1        0x130000u

#define MT7925_PCIE_MAC_INT_ENABLE  0x10188u
#define MT7925_PCIE_MAC_PM          0x10194u
#define   MT_PCIE_MAC_PM_L0S_DIS    (1u << 8)

#define MT_WFDMA0(ofs)              (0xd4000u + (ofs))
#define MT_WFDMA0_RST               MT_WFDMA0(0x100)
#define   MT_WFDMA0_RST_LOGIC_RST   (1u << 4)
#define   MT_WFDMA0_RST_DMASHDL_ALL_RST (1u << 5)
#define MT_MCU2HOST_SW_INT_ENA      MT_WFDMA0(0x1f4)
#define MT_WFDMA0_HOST_INT_ENA      MT_WFDMA0(0x204)
#define MT_WFDMA0_GLO_CFG           MT_WFDMA0(0x208)
#define   GLO_CFG_TX_DMA_EN         (1u << 0)
#define   GLO_CFG_TX_DMA_BUSY       (1u << 1)
#define   GLO_CFG_RX_DMA_EN         (1u << 2)
#define   GLO_CFG_RX_DMA_BUSY       (1u << 3)
#define   GLO_CFG_DMA_SIZE_3        (3u << 4)
#define   GLO_CFG_TX_WB_DDONE       (1u << 6)
#define   GLO_CFG_FIFO_DIS_CHECK    (1u << 11)
#define   GLO_CFG_FIFO_LITTLE_ENDIAN (1u << 12)
#define   GLO_CFG_RX_WB_DDONE       (1u << 13)
#define   GLO_CFG_DISP_BASE_PTR_CHAIN_EN (1u << 15)
#define   GLO_CFG_OMIT_RX_INFO_PFET2 (1u << 21)
#define   GLO_CFG_OMIT_RX_INFO      (1u << 27)
#define   GLO_CFG_OMIT_TX_INFO      (1u << 28)
#define   GLO_CFG_CLK_GAT_DIS       (1u << 30)
#define MT_WFDMA0_RST_DTX_PTR       MT_WFDMA0(0x20c)
#define MT_WFDMA0_RST_DRX_PTR       MT_WFDMA0(0x280)
#define MT_WFDMA0_INT_RX_PRI        MT_WFDMA0(0x298)
#define MT_WFDMA0_INT_TX_PRI        MT_WFDMA0(0x29c)
#define MT_WFDMA0_GLO_CFG_EXT0      MT_WFDMA0(0x2b0)
#define   GLO_CFG_EXT0_TX_DMASHDL_EN (1u << 6)
#define MT_WFDMA0_PRI_DLY_INT_CFG0  MT_WFDMA0(0x2f0)
#define MT_WFDMA0_TX_RING_EXT_CTRL(n) MT_WFDMA0(0x600 + 4 * (n))
#define MT_WFDMA0_RX_RING_EXT_CTRL(n) MT_WFDMA0(0x680 + 4 * (n))
#define MT_WFDMA_PREFETCH(base, depth) (((base) << 16) | (depth))

// DMA rings: {desc_base, ring_size, cpu_idx, dma_idx} per ring, 0x10 apart
#define MT_TX_RING_BASE             MT_WFDMA0(0x300)
#define MT_RX_RING_BASE             MT_WFDMA0(0x500)
#define MT_RING_REGS(base, n)       ((base) + 0x10u * (n))
#define   RING_DESC_BASE            0x0u
#define   RING_SIZE                 0x4u
#define   RING_CPU_IDX              0x8u
#define   RING_DMA_IDX              0xcu

// mt7925 queue numbers (mt7925.h)
#define MT7925_TXQ_BAND0            0
#define MT7925_TXQ_MCU_WM           15
#define MT7925_TXQ_FWDL             16
#define MT7925_RXQ_MCU_WM           0
#define MT7925_RXQ_BAND0            2

// DMA descriptor control word (dma.h)
#define MT_DMA_CTL_SD_LEN0          0x3fff0000u  // bits 29:16
#define MT_DMA_CTL_LAST_SEC0        (1u << 30)
#define MT_DMA_CTL_DMA_DONE         (1u << 31)

#define MT792X_OWN_RETRY_COUNT      10
#define MT792X_OWN_POLL_MS          50

// Minimum BAR0 size we accept (mt7925 fixed map spans 0x000000-0x1fffff).
#define MT7925_MIN_BAR_LENGTH       0x200000u

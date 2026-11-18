/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * BCM43455: the chip's registers as the host reaches them over SDIO.
 *
 * Function 1 holds a few control registers of the SDIO interface and a
 * 32 KB window onto the chip's backplane, the bus every core of the chip
 * sits on.  No code here.
 */

#ifndef KERN_DRIVERS_BCM43455_HW_H
#define KERN_DRIVERS_BCM43455_HW_H

/* The SDIO functions: the common registers, the backplane, and the frames. */
#define BCM43455_FUNCTION_COMMON	0U
#define BCM43455_FUNCTION_BACKPLANE	1U
#define BCM43455_FUNCTION_FRAMES	2U

/*
 * The common register that aborts a function's transfer.
 *
 * Value referenced: the SD Association SDIO Simplified Specification (the CCCR)
 * and Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/bus_protocols/whd_sdio.h (line 49).
 */
#define BCM43455_CCCR_IO_ABORT		0x06U

/*
 * The vendor registers of function 0: card capability (CMD14) and card control.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.h revision 1.3 (lines
 * 20-25).
 */
#define BCM43455_CCCR_CARD_CAPABILITY	0xf0U
#define BCM43455_CARD_CAPABILITY_CMD14	0x02U
#define BCM43455_CARD_CAPABILITY_CMD14_EXT	0x04U
#define BCM43455_CCCR_CARD_CONTROL	0xf1U
#define BCM43455_CARD_CONTROL_WLAN_RESET	0x02U

/*
 * The common register that enables the card's interrupts: master, F1, F2.
 *
 * Values referenced: the SD Association SDIO Simplified Specification (the
 * CCCR) and Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/bus_protocols/whd_sdio.h (lines 47 and 146-148).
 */
#define BCM43455_CCCR_INTERRUPT_ENABLE	0x04U
#define BCM43455_CCCR_INTERRUPT_MASTER	0x01U
#define BCM43455_CCCR_INTERRUPT_F1	0x02U
#define BCM43455_CCCR_INTERRUPT_F2	0x04U

/*
 * Function 1's control registers.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.h revision 1.3 (lines
 * 31, 41-43 and 67-71) and, for frame control, Infineon wifi-host-driver
 * bd99da7b WHD/COMPONENT_WIFI6/src/bus_protocols/whd_sdio.h (line 106).
 */
#define BCM43455_F1_WATERMARK		0x10008U
#define BCM43455_F1_WINDOW_LOW		0x1000aU
#define BCM43455_F1_WINDOW_MID		0x1000bU
#define BCM43455_F1_WINDOW_HIGH		0x1000cU
#define BCM43455_F1_FRAME_CONTROL	0x1000dU
#define BCM43455_F1_CHIP_CLOCK		0x1000eU
#define BCM43455_F1_PULL_UP		0x1000fU
#define BCM43455_F1_WAKEUP_CONTROL	0x1001eU
#define BCM43455_F1_SLEEP_CONTROL	0x1001fU

/*
 * Wakeup control: wait for the fast clock on wakeup.  Sleep control: keep SDIO
 * on.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.h revision 1.3 (lines
 * 68-71).
 */
#define BCM43455_WAKEUP_HT_WAIT		0x02U
#define BCM43455_SLEEP_KEEP_SDIO_ON	0x01U

/*
 * The SDIO device core revision from which the keep-SDIO-on bit is set at
 * attach.
 *
 * Value referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.c revision 1.44 (lines
 * 311-317).
 */
#define BCM43455_SDIO_REVISION_KSO	12U

/*
 * The watermark the frame function's read FIFO uses.
 *
 * Value referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.c revision 1.44 (line
 * 450).
 */
#define BCM43455_F1_WATERMARK_VALUE	8U

/*
 * Frame control: terminate the frame being read.
 *
 * Value referenced: Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/bus_protocols/whd_sdio.h (line 228).
 */
#define BCM43455_FRAME_CONTROL_READ_TERMINATE	0x01U

/*
 * The chip clock register's requests and states.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.h revision 1.3 (lines
 * 44-52) and Infineon wifi-host-driver bd99da7b
 * WHD/COMPONENT_WIFI6/src/bus_protocols/whd_sdio.h (lines 209 on).
 */
#define BCM43455_CLOCK_FORCE_ALP	0x01U
#define BCM43455_CLOCK_FORCE_HT		0x02U
#define BCM43455_CLOCK_ALP_REQUEST	0x08U
#define BCM43455_CLOCK_HT_REQUEST	0x10U
#define BCM43455_CLOCK_HW_REQUEST_OFF	0x20U
#define BCM43455_CLOCK_ALP_AVAILABLE	0x40U
#define BCM43455_CLOCK_HT_AVAILABLE	0x80U

/*
 * The backplane window: the high address bits go to the three window registers,
 * the low fifteen into the function 1 address, with a flag that makes each
 * access four bytes wide.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.h revision 1.3 (lines
 * 74-76).
 */
#define BCM43455_WINDOW_SIZE		0x8000U
#define BCM43455_WINDOW_OFFSET_MASK	0x7fffU
#define BCM43455_WINDOW_ACCESS_32	0x8000U
#define BCM43455_WINDOW_LOW_MASK	0x80U

/*
 * The chip common core, the first core on every backplane, and its registers.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines 20-34).
 */
#define BCM43455_CHIPCOMMON_BASE	0x18000000U
#define BCM43455_CHIPCOMMON_ID		0x00U
#define BCM43455_CHIPCOMMON_CAPABILITY	0x04U
#define BCM43455_CHIPCOMMON_CAPABILITY_EXT	0xacU
#define BCM43455_CHIPCOMMON_EROM	0xfcU

/*
 * The chip common capability bits: a PMU, and a PMU in a core of its own.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines 29-34).
 */
#define BCM43455_CAPABILITY_PMU		0x10000000U
#define BCM43455_CAPABILITY_EXT_AOB	0x00000040U

/*
 * The chip common revision from which the PMU may be a core of its own.
 *
 * Value referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113 (lines 1027-1040).
 */
#define BCM43455_CHIPCOMMON_REVISION_AOB	35U

/*
 * The PMU's registers: its control, its capabilities, and the chip control
 * window.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines 42-51).
 */
#define BCM43455_PMU_CONTROL		0x600U
#define BCM43455_PMU_CONTROL_RESOURCE_RELOAD	(2U << 13)
#define BCM43455_PMU_CAPABILITY		0x604U
#define BCM43455_PMU_REVISION_MASK	0xffU
#define BCM43455_PMU_CHIP_CONTROL_ADDRESS	0x650U
#define BCM43455_PMU_CHIP_CONTROL_DATA	0x654U

/*
 * Save-restore on the 4345: chip control register 3, bit 2, with a PMU of
 * revision 17 or later.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113 (lines
 * 1384-1401).
 */
#define BCM43455_PMU_CHIP_CONTROL_SR	3U
#define BCM43455_SAVE_RESTORE_BIT		0x04U
#define BCM43455_PMU_REVISION_SR	17U

/*
 * The chip id register: the chip number, its revision, and the bus type.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines 20-28).
 */
#define BCM43455_CHIP_ID_MASK		0x0000ffffU
#define BCM43455_CHIP_REVISION_SHIFT	16U
#define BCM43455_CHIP_REVISION_MASK	0x0fU
#define BCM43455_CHIP_BUS_SHIFT		28U
#define BCM43455_CHIP_BUS_MASK		0x0fU
#define BCM43455_CHIP_BUS_AXI		1U

/*
 * The chip this driver drives: BCM4345, revision 6 or later, the 43455 C0.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmvar.h revision 1.31 (line 35)
 * and OpenBSD sys/dev/sdmmc/if_bwfm_sdio.c revision 1.44 (lines 377-381).
 */
#define BCM43455_CHIP_ID		0x4345U
#define BCM43455_CHIP_REVISION_MIN	6U

/*
 * The backplane address of the chip's RAM on the 43455.
 *
 * Value referenced: OpenBSD sys/dev/ic/bwfm.c revision 1.113 (lines 1563-1564).
 */
#define BCM43455_RAM_BASE		0x198000U

/*
 * The enumeration ROM's descriptor types.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines
 * 121-128).
 */
#define BCM43455_EROM_TYPE_MASK		0x0fU
#define BCM43455_EROM_TYPE_SHORT_MASK	0x07U
#define BCM43455_EROM_COMPONENT		0x01U
#define BCM43455_EROM_MASTER_PORT	0x03U
#define BCM43455_EROM_ADDRESS		0x05U
#define BCM43455_EROM_END		0x0fU
#define BCM43455_EROM_WIDE		0x08U

/*
 * A component descriptor's first word: its class and its core id.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines
 * 129-144).
 */
#define BCM43455_EROM_CLASS_SHIFT	4U
#define BCM43455_EROM_CLASS_MASK	0x0fU
#define BCM43455_EROM_PART_SHIFT	8U
#define BCM43455_EROM_PART_MASK		0x0fffU

/*
 * A component descriptor's second word: its revision and its port counts.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines
 * 129-144).
 */
#define BCM43455_EROM_REVISION_SHIFT	24U
#define BCM43455_EROM_REVISION_MASK	0xffU
#define BCM43455_EROM_SLAVE_WRAPS_SHIFT	19U
#define BCM43455_EROM_MASTER_WRAPS_SHIFT	14U
#define BCM43455_EROM_COUNT_MASK	0x1fU

/*
 * An address descriptor: base, port, kind and size encoding.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines
 * 149-164).
 */
#define BCM43455_EROM_BASE_MASK		0xfffff000U
#define BCM43455_EROM_PORT_SHIFT	8U
#define BCM43455_EROM_PORT_MASK		0x0fU
#define BCM43455_EROM_KIND_SHIFT	6U
#define BCM43455_EROM_KIND_MASK		0x03U
#define BCM43455_EROM_KIND_SLAVE	0U
#define BCM43455_EROM_KIND_SLAVE_WRAP	2U
#define BCM43455_EROM_KIND_MASTER_WRAP	3U
#define BCM43455_EROM_SIZE_SHIFT	4U
#define BCM43455_EROM_SIZE_MASK		0x03U
#define BCM43455_EROM_SIZE_4K		0U
#define BCM43455_EROM_SIZE_8K		1U
#define BCM43455_EROM_SIZE_DESCRIBED	3U

/* The most enumeration ROM words read before giving up on its end. */
#define BCM43455_EROM_WORDS_MAX		1024U

/*
 * The core ids this driver looks for.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines 68-75).
 */
#define BCM43455_CORE_CHIPCOMMON	0x800U
#define BCM43455_CORE_D11		0x812U
#define BCM43455_CORE_PMU		0x827U
#define BCM43455_CORE_SDIO_DEVICE	0x829U
#define BCM43455_CORE_ARM_CR4		0x83eU

/*
 * A core's wrapper registers: the I/O control and the reset control.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines 57-65).
 */
#define BCM43455_WRAP_IO_CONTROL	0x408U
#define BCM43455_WRAP_RESET_CONTROL	0x800U
#define BCM43455_IO_CONTROL_CLOCK	0x0001U
#define BCM43455_IO_CONTROL_FORCE_GATE	0x0002U
#define BCM43455_RESET_CONTROL_RESET	0x0001U

/*
 * The I/O control bits of the ARM CR4 (halt) and of D11, the radio's PHY.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines 81-83).
 */
#define BCM43455_CR4_IO_CPU_HALT	0x0020U
#define BCM43455_D11_IO_PHY_CLOCK	0x0004U
#define BCM43455_D11_IO_PHY_RESET	0x0008U

/*
 * The ARM CR4's registers that describe its RAM banks.
 *
 * Values referenced: OpenBSD sys/dev/ic/bwfmreg.h revision 1.26 (lines 86-95).
 */
#define BCM43455_CR4_CAPABILITY		0x04U
#define BCM43455_CR4_BANK_INDEX		0x40U
#define BCM43455_CR4_BANK_INFO		0x44U
#define BCM43455_CR4_BANKS_A_MASK	0x0fU
#define BCM43455_CR4_BANKS_B_SHIFT	4U
#define BCM43455_CR4_BANKS_B_MASK	0x0fU
#define BCM43455_CR4_BANK_SIZE_MASK	0x7fU
#define BCM43455_CR4_BANK_UNIT		8192U
#define BCM43455_CR4_BANK_1K		0x200U

/*
 * The SDIO device core's registers.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.h revision 1.3 (lines
 * 87-149).
 */
#define BCM43455_SDIO_CORE_INTERRUPTS	0x20U
#define BCM43455_SDIO_CORE_INTERRUPT_MASK	0x24U
#define BCM43455_SDIO_TO_CHIP_MAILBOX	0x40U
#define BCM43455_SDIO_TO_HOST_MAILBOX	0x44U
#define BCM43455_SDIO_TO_CHIP_DATA	0x48U
#define BCM43455_SDIO_TO_HOST_DATA	0x4cU

/*
 * The SDIO device core's interrupt status: the to-host mailbox's bits and chip
 * activity.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.h revision 1.3 (lines
 * 94-120).
 */
#define BCM43455_INT_FLOW_STATE		0x00000010U
#define BCM43455_INT_FLOW_CHANGE	0x00000020U
#define BCM43455_INT_FRAME		0x00000040U
#define BCM43455_INT_HOST_MAILBOX	0x00000080U
#define BCM43455_INT_MAILBOX_MASK	0x000000f0U
#define BCM43455_INT_CHIP_ACTIVE	0x20000000U

/*
 * The to-chip mailbox: a received frame refused, and the host's
 * acknowledgement.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.h revision 1.3 (lines
 * 87-149).
 */
#define BCM43455_TO_CHIP_NAK		0x00000001U
#define BCM43455_TO_CHIP_ACK		0x00000002U

/*
 * The to-chip mailbox data: the host's protocol version.
 *
 * Value referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.h revision 1.3 (lines
 * 79-80).
 */
#define BCM43455_TO_CHIP_VERSION_SHIFT	16U

/*
 * The to-host mailbox data: what the firmware tells the host.
 *
 * Values referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.h revision 1.3 (lines
 * 79-80 and 145-149).
 */
#define BCM43455_TO_HOST_NAK_HANDLED	0x00000001U
#define BCM43455_TO_HOST_DEVICE_READY	0x00000002U
#define BCM43455_TO_HOST_FLOW_CONTROL	0x00000004U
#define BCM43455_TO_HOST_FIRMWARE_READY	0x00000008U
#define BCM43455_TO_HOST_FIRMWARE_HALT	0x00000010U
#define BCM43455_TO_HOST_VERSION_SHIFT	16U
#define BCM43455_TO_HOST_VERSION_MASK	0xffU
#define BCM43455_TO_HOST_FLOW_SHIFT	24U

/*
 * The SDPCM protocol version host and firmware speak.
 *
 * Value referenced: OpenBSD sys/dev/sdmmc/if_bwfm_sdio.h revision 1.3 (lines
 * 79-80).
 */
#define BCM43455_SDPCM_PROTOCOL		4U

/*
 * The function 2 address frames are read from and written to: the chip
 * common core's window with the four-byte flag.  Function 2 is a FIFO, so
 * the address only has to stay in the window.
 */
#define BCM43455_FRAME_ADDRESS		((BCM43455_CHIPCOMMON_BASE & BCM43455_WINDOW_OFFSET_MASK) | BCM43455_WINDOW_ACCESS_32)

#endif

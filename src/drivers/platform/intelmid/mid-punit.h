/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The power unit of the Intel MID 64-bit SoCs (Merrifield and
 * Moorefield) as the graphics driver uses it.
 *
 * The power unit sits on the SoC's message bus, which the host reaches
 * through three registers in the PCI configuration space of the host
 * bridge (00:00.0): a command word naming the operation, the port and
 * the register; the data word; and the upper bits of a register number
 * too wide for the command word.  The graphics block has one control
 * register with four power islands in it, each a two-bit field whose
 * state the power unit reports in the top byte of the same register.
 *
 * This part only builds the words and reads the answers; the caller
 * does the configuration space accesses, under the one lock that covers
 * every read, change and write of the power unit in the system.  It
 * holds no lock and no memory, and it builds on the host too.
 */

#ifndef KERN_DRIVERS_PLATFORM_INTELMID_MID_PUNIT_H
#define KERN_DRIVERS_PLATFORM_INTELMID_MID_PUNIT_H

#include <stdint.h>

/*
 * The message bus registers in the host bridge's configuration space,
 * the operations, and the byte enables of a whole word.
 *
 * Values referenced:
 * Linux v6.12 arch/x86/include/asm/iosf_mbi.h (MBI_MCR_OFFSET 0xD0,
 * MBI_MDR_OFFSET 0xD4, MBI_MCRX_OFFSET 0xD8, MBI_ENABLE 0xF0,
 * MBI_REG_READ 0x10, MBI_REG_WRITE 0x11, MBI_MASK_HI 0xFFFFFF00);
 * Android kernel/x86_64 android-x86_64-fugu-3.10-oreo-r6 8ff24327ec
 * arch/x86/platform/intel-mid/intel_mid_pcihelpers.c
 * (intel_mid_msgbus_read32 and intel_mid_msgbus_write32: the operation
 * at bit 24, the port at bit 16, the low byte of the register at bit 8,
 * the whole-word enables, and the register's upper bits in the extended
 * register, written only when not zero).
 */
#define MID_MSGBUS_COMMAND		0xd0U
#define MID_MSGBUS_DATA			0xd4U
#define MID_MSGBUS_EXTENDED		0xd8U
#define MID_MSGBUS_READ			0x10U
#define MID_MSGBUS_WRITE		0x11U
#define MID_MSGBUS_WHOLE_WORD		0xf0U
#define MID_MSGBUS_OPERATION_SHIFT	24U
#define MID_MSGBUS_PORT_SHIFT		16U
#define MID_MSGBUS_REGISTER_SHIFT	8U
#define MID_MSGBUS_REGISTER_LOW		0x000000ffU
#define MID_MSGBUS_REGISTER_HIGH	0xffffff00U

/*
 * The power unit's port and its graphics registers.
 *
 * The graphics control register holds the four islands' requested
 * states in bits 7:0 and their present states in bits 31:24; the
 * frequency register takes the clock code.  An island's field is 0 when
 * it is on and 3 when it is off.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/intel/display/tng/drv/pmu_tng.h (PUNIT_PORT 0x04,
 * NC_PM_SSS 0x3f, GFX_SS_PM0 0x30, GFX_SS_PM1 0x31, GFX_SLC_SHIFT 0,
 * GFX_SDKCK_SHIFT 2, GFX_RSCD_SHIFT 4, GFX_SLC_LDO_SHIFT 6,
 * SSC_TO_SSS_SHIFT 24) and ospm/gfx_ospm_ann.h (PMU_SLC 1, PMU_SDKCK 2,
 * PMU_RSCD 4, PMU_LDO 8); Android kernel/x86_64
 * android-x86_64-fugu-3.10-oreo-r6 8ff24327ec
 * arch/x86/platform/intel-mid/intel_soc_pmu.h (D0I0_MASK 0, D0I3_MASK 3,
 * BITS_PER_LSS 2) and intel_soc_mrfld.h (SSS_SHIFT 24).
 */
#define MID_PUNIT_PORT			0x04U
#define MID_PUNIT_ISLAND_STATUS		0x3fU
#define MID_PUNIT_GFX_CONTROL		0x30U
#define MID_PUNIT_GFX_FREQUENCY		0x31U
#define MID_PUNIT_STATE_SHIFT		24U
#define MID_PUNIT_FIELD_BITS		2U
#define MID_PUNIT_FIELD_ON		0x0U
#define MID_PUNIT_FIELD_OFF		0x3U
#define MID_PUNIT_ISLAND_SLC		0U
#define MID_PUNIT_ISLAND_SIDEKICK	1U
#define MID_PUNIT_ISLAND_RASCAL_DUST	2U
#define MID_PUNIT_ISLAND_SLC_LDO	3U
#define MID_PUNIT_ISLANDS		4U

/*
 * The graphics wrapper registers set after the islands come up, as
 * offsets in the display and graphics function's first memory window.
 *
 * On the Moorefield (Anniedale) parts the wrapper's clip control takes
 * the bit that turns off the AES bypass, and the arbitration control is
 * given weight 3 for each of its six requesters; the wrapper's register
 * at 0x160008 is cleared.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/intel/display/tng/drv/ospm/gfx_ospm_ann.c
 * (ospm_sidekick_power_up: 0x160020 |= 0x80; ospm_pnp_settings:
 * 0x160008 = 0, 0x160028 |= 0x333333).
 */
#define MID_GFX_WRAPPER_CLIP_CONTROL	0x160020U
#define MID_GFX_WRAPPER_AES_BYPASS_OFF	0x00000080U
#define MID_GFX_WRAPPER_PNP_0008	0x160008U
#define MID_GFX_WRAPPER_ARBITRATION	0x160028U
#define MID_GFX_WRAPPER_WEIGHTS		0x00333333U

/* Builds the command word that reads a register of a port. */
uint32_t mid_punit_read_command(uint32_t port, uint32_t reg);

/* Builds the command word that writes a register of a port. */
uint32_t mid_punit_write_command(uint32_t port, uint32_t reg);

/* Returns the extended register's word for a register, zero when it needs none. */
uint32_t mid_punit_extended(uint32_t reg);

/* Returns the field of an island in the control bits, or zero for an unknown island. */
uint32_t mid_punit_island_field(unsigned island);

/*
 * Builds the new control word that turns islands on or off.
 *
 * Islands is a set of island bits (1 << island); the other islands'
 * fields keep their value in current.
 */
uint32_t mid_punit_island_request(uint32_t current, unsigned islands, int on);

/*
 * Tells whether the power unit has carried out a request.
 *
 * Status is the register read back; the islands' present states have
 * to equal the requested ones.
 */
int mid_punit_island_done(uint32_t status, uint32_t request, unsigned islands);

/* Reads an island's present state from a register read back. */
uint32_t mid_punit_island_state(uint32_t status, unsigned island);

/*
 * Returns the island turned on at a step of the power-up order, or
 * MID_PUNIT_ISLANDS past the last step.
 *
 * Each island depends on the one before it: the SLC's regulator, then
 * the SLC, then the sidekick, then the rascal and dust; power-down is
 * the reverse.
 */
unsigned mid_punit_power_up_step(unsigned step);

#endif

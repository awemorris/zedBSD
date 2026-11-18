/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The message bus words and the graphics power island fields of the
 * Intel MID power unit.
 */

#include "drivers/platform/intelmid/mid-punit.h"

static uint32_t build_command(uint32_t operation, uint32_t port, uint32_t reg);

/*
 * The order the graphics islands come up in.
 *
 * Each island is the dependency of the next; the table is constant and
 * read without a lock.
 *
 * Values referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-nougat-mr2 999603b9ab
 * drivers/staging/imgtec/intel/display/tng/drv/ospm/gfx_ospm_ann.c
 * (ospm_rscd_init depends on the sidekick, ospm_sidekick_init on the
 * SLC, ospm_slc_init on the SLC's regulator).
 */
static const unsigned power_up_order[MID_PUNIT_ISLANDS] = {
	MID_PUNIT_ISLAND_SLC_LDO,
	MID_PUNIT_ISLAND_SLC,
	MID_PUNIT_ISLAND_SIDEKICK,
	MID_PUNIT_ISLAND_RASCAL_DUST
};

/*
 * Builds the command word that reads a register of a port.
 */
uint32_t
mid_punit_read_command(
	uint32_t port,
	uint32_t reg)
{
	uint32_t command;

	/* A register read with all four byte enables. */
	command = build_command(MID_MSGBUS_READ, port, reg);

	/* The word for the command register. */
	return command;
}

/*
 * Builds the command word that writes a register of a port.
 */
uint32_t
mid_punit_write_command(
	uint32_t port,
	uint32_t reg)
{
	uint32_t command;

	/* A register write with all four byte enables. */
	command = build_command(MID_MSGBUS_WRITE, port, reg);

	/* The word for the command register. */
	return command;
}

/*
 * Returns the extended register's word for a register.
 *
 * Zero means the register fits the command word and the extended
 * register is left alone; the hardware clears it after every command.
 */
uint32_t
mid_punit_extended(
	uint32_t reg)
{
	/* The bits of the register number above the command word's byte. */
	return reg & MID_MSGBUS_REGISTER_HIGH;
}

/*
 * Returns the field of an island in the control bits.
 */
uint32_t
mid_punit_island_field(
	unsigned island)
{
	/* An island the graphics register does not have has no field. */
	if (island >= MID_PUNIT_ISLANDS)
		return 0;

	/* Two bits per island, from bit 0 up. */
	return MID_PUNIT_FIELD_OFF << (MID_PUNIT_FIELD_BITS * island);
}

/*
 * Builds the new control word that turns islands on or off.
 *
 * Procedure referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-oreo-r6 8ff24327ec
 * arch/x86/platform/intel-mid/intel_soc_mrfld.c (mrfld_nc_set_power_state:
 * read the register, clear an island's field to turn it on or set it to
 * turn it off, and write only when the word changed).
 */
uint32_t
mid_punit_island_request(
	uint32_t current,
	unsigned islands,
	int on)
{
	uint32_t request;
	uint32_t field;
	unsigned island;

	/* Starts from the register as read; only the named islands' fields change. */
	request = current;

	/* Sets or clears the field of every named island. */
	for (island = 0; island < MID_PUNIT_ISLANDS; island++) {
		if ((islands & (1U << island)) == 0)
			continue;

		/* On is a cleared field, off a full one. */
		field = mid_punit_island_field(island);
		if (on) {
			request &= ~field;
		} else {
			request |= field;
		}
	}

	/* The word to write to the control register. */
	return request;
}

/*
 * Tells whether the power unit has carried out a request.
 *
 * Procedure referenced:
 * Android kernel/x86_64 android-x86_64-fugu-3.10-oreo-r6 8ff24327ec
 * arch/x86/platform/intel-mid/intel_soc_mrfld.c
 * (wait_for_nc_pmcmd_complete: the state byte, shifted down, compared
 * with the request under the islands' fields).
 */
int
mid_punit_island_done(
	uint32_t status,
	uint32_t request,
	unsigned islands)
{
	uint32_t mask;
	uint32_t state;
	unsigned island;

	/* Collects the fields of the islands the request is about. */
	mask = 0;
	for (island = 0; island < MID_PUNIT_ISLANDS; island++) {
		if ((islands & (1U << island)) != 0)
			mask |= mid_punit_island_field(island);
	}

	/* The present states sit in the top byte of the register. */
	state = status >> MID_PUNIT_STATE_SHIFT;

	/* Every named island has to be in the state asked for. */
	if ((state & mask) != (request & mask))
		return 0;

	/* The request is carried out. */
	return 1;
}

/*
 * Reads an island's present state from a register read back.
 */
uint32_t
mid_punit_island_state(
	uint32_t status,
	unsigned island)
{
	/* An island the graphics register does not have reads as on. */
	if (island >= MID_PUNIT_ISLANDS)
		return MID_PUNIT_FIELD_ON;

	/* The island's two bits of the state byte. */
	return (status >> (MID_PUNIT_STATE_SHIFT + MID_PUNIT_FIELD_BITS * island)) & MID_PUNIT_FIELD_OFF;
}

/*
 * Returns the island turned on at a step of the power-up order.
 */
unsigned
mid_punit_power_up_step(
	unsigned step)
{
	/* Past the last step there is no island left. */
	if (step >= MID_PUNIT_ISLANDS)
		return MID_PUNIT_ISLANDS;

	/* The island of this step. */
	return power_up_order[step];
}

/* Builds a message bus command word. */
static uint32_t
build_command(
	uint32_t operation,
	uint32_t port,
	uint32_t reg)
{
	/* The operation, the port, the register's low byte and the byte enables. */
	return (operation << MID_MSGBUS_OPERATION_SHIFT) |
	    ((port & 0xffU) << MID_MSGBUS_PORT_SHIFT) |
	    ((reg & MID_MSGBUS_REGISTER_LOW) << MID_MSGBUS_REGISTER_SHIFT) |
	    MID_MSGBUS_WHOLE_WORD;
}

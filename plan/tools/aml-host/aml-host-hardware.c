/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The simulated ACPI hardware of the host harness (WS049): the PM1 event
 * and control registers, a GPE block, the SMI command port and an
 * Embedded Controller, at the ports QEMU's q35 uses.  Status registers
 * clear the bits written as one; the EC answers every command at once.
 *
 * The FACS Global Lock has a simulated firmware on its other side: at each
 * tick (a sleep of the interpreter) it asks for a lock the operating system
 * owns, or lets go of a lock it owns; it takes the lock when GBL_RLS says
 * the operating system let it go, and raises the Global Lock event when it
 * lets the lock go while the operating system waits.  It prints each step.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aml-host-hardware.h"

/*
 * The ports of the simulated hardware.
 */
#define PM1A_EVENT	0x0600U
#define PM1_EVENT_LEN	4U
#define PM1A_CONTROL	0x0604U
#define PM1_CONTROL_LEN	2U
#define GPE0_BLOCK	0x0620U
#define GPE0_LEN	16U
#define SMI_COMMAND	0x00b2U
#define ACPI_ENABLE	0xf0U
#define QEMU_ACPI_ENABLE	0x02U
#define SCI_INTERRUPT	9U
#define EC_DATA_DEFAULT		0x0062U
#define EC_COMMAND_DEFAULT	0x0066U

/*
 * The Global Lock bits, the GBL_RLS bit of PM1_CNT, and the Global Lock
 * event's bit of PM1_STS.
 */
#define LOCK_PENDING	0x1U
#define LOCK_OWNED	0x2U
#define PM1_GBL_RLS	0x04U

/* SLP_EN in the second byte of PM1_CNT (bit 13 of the register). */
#define PM1_SLP_EN	0x20U
#define PM1_GBL_STS	5U

/*
 * The EC status bits and commands.
 */
#define EC_OBF		0x01U
#define EC_SCI_EVT	0x20U
#define EC_READ		0x80U
#define EC_WRITE	0x81U
#define EC_QUERY	0x84U

/*
 * What the EC expects on its data port next.
 */
enum ec_phase {
	EC_IDLE = 0,
	EC_READ_ADDRESS,
	EC_WRITE_ADDRESS,
	EC_WRITE_DATA
};


/*
 * The registers of the simulated hardware.
 *
 * They are all zero at start, as the harness starts with no event, no
 * enable bit and ACPI mode off; only the interpreter's port accesses and
 * the hardware_raise_* calls change them, and the harness runs one thread.
 */
static uint8_t pm1_event[PM1_EVENT_LEN];
static uint8_t pm1_control[PM1_CONTROL_LEN];
static uint8_t gpe0[GPE0_LEN];

/*
 * The simulated EC: its 256 bytes, the byte waiting on the data port and
 * whether one waits (OBF), the address of a write in progress and the
 * phase of the command in progress, and the queries it has to report.
 *
 * ec_query_count is how many of ec_queries wait, oldest first; while it is
 * not zero the status reports SCI_EVT.  The harness runs one thread.
 */
static uint8_t ec_ram[256];
static uint8_t ec_output;
static uint8_t ec_output_full;
static uint8_t ec_address;
static enum ec_phase ec_phase;
static uint8_t ec_queries[64];
static unsigned ec_query_count;

/*
 * The ports the simulated EC answers on: QEMU's and most firmware's unless
 * hardware_ec_ports() moved them to where a real table's _CRS puts its EC.
 */
static uint32_t ec_data_port = EC_DATA_DEFAULT;
static uint32_t ec_command_port = EC_COMMAND_DEFAULT;

/*
 * The Global Lock's dword, whether the simulated firmware owns it, and
 * whether it waits for the operating system to let it go.
 *
 * firmware_owns is set only while the dword holds OWNED for firmware, and
 * firmware_waits only while the dword holds PENDING that firmware set; the
 * interpreter changes the dword by atomic exchanges, the firmware at ticks.
 */
static volatile uint32_t global_lock;
static bool firmware_owns;
static bool firmware_waits;

static bool port_modelled(uint32_t port);
static int access_byte(uint32_t port, bool write, uint8_t *value);
static void control_write(uint32_t port, uint8_t *value);
static void smi_command(bool write, uint8_t *value);
static int ec_data(bool write, uint8_t *value);
static int ec_command(bool write, uint8_t *value);
static void ec_query_answer(void);

/*
 * Reads or writes a port of the simulated hardware.
 *
 * It reports 1 when the port is modelled and 0 when it is plain memory.
 */
int
hardware_port(
	uint32_t port,
	unsigned width,
	bool write,
	uint32_t *value)
{
	uint8_t byte;
	unsigned index;
	unsigned bytes;
	bool modelled;

	/* A port is modelled when its first byte is; asking does not touch the register. */
	modelled = port_modelled(port);
	if (!modelled)
		return 0;

	/* A read starts from zero and gathers its bytes. */
	bytes = width / 8U;
	if (!write)
		*value = 0;

	/* Moves the bytes lowest first, each with its register's rule. */
	for (index = 0; index < bytes; index++) {
		/* Writes or reads one byte; a byte past the modelled registers reads as zero and drops a write. */
		byte = (uint8_t)(*value >> (index * 8U));
		(void)access_byte(port + index, write, &byte);

		/* Puts a read byte in its place. */
		if (!write)
			*value |= (uint32_t)byte << (index * 8U);
	}

	/* Succeeded: the hardware answered the access. */
	return 1;
}

/*
 * Raises a GPE: sets its status bit.
 */
void
hardware_raise_gpe(
	unsigned gpe)
{
	/* Sets the bit when the block has it. */
	if (gpe < GPE0_LEN / 2U * 8U)
		gpe0[gpe / 8U] |= (uint8_t)(1U << (gpe % 8U));
}

/*
 * Raises a fixed event: sets its bit of PM1_STS.
 */
void
hardware_raise_fixed(
	unsigned bit)
{
	/* Sets the bit in its byte of PM1_STS, when PM1_STS has it. */
	if (bit < 16U)
		pm1_event[bit / 8U] |= (uint8_t)(1U << (bit % 8U));
}

/*
 * Queues an EC query and raises SCI_EVT until it is read.
 */
void
hardware_ec_query(
	uint8_t query)
{
	/* A full queue drops the query. */
	if (ec_query_count >= sizeof(ec_queries))
		return;

	/* Queues the query; the count makes the status report SCI_EVT. */
	ec_queries[ec_query_count] = query;
	ec_query_count++;
}

/*
 * Sets one byte of the EC's address space.
 */
void
hardware_ec_ram(
	uint8_t address,
	uint8_t value)
{
	/* Presets the byte before any AML reads it. */
	ec_ram[address] = value;
}

/*
 * Moves the simulated EC to the data and command ports a table's _CRS gives.
 */
void
hardware_ec_ports(
	uint32_t data,
	uint32_t command)
{
	/* The EC answers on these from now on. */
	ec_data_port = data;
	ec_command_port = command;
}

/*
 * Reads one byte of the EC's address space.
 */
uint8_t
hardware_ec_ram_read(
	uint8_t address)
{
	/* Reports the byte as AML left it. */
	return ec_ram[address];
}

/*
 * Reports the Global Lock's dword, as the FACS would hold it.
 */
volatile uint32_t *
hardware_global_lock(void)
{
	/* The lock lives in the harness. */
	return &global_lock;
}

/*
 * Plays the simulated firmware's side of the Global Lock at a sleep.
 */
void
hardware_tick(void)
{
	uint32_t old;

	/* Firmware owning the lock lets it go, and raises the event when the system waits. */
	if (firmware_owns) {
		/* Clears the dword; firmware_owns goes with it. */
		old = __atomic_exchange_n(&global_lock, 0U, __ATOMIC_SEQ_CST);
		firmware_owns = false;

		/* A pending old value means the system waits: firmware raises GBL_STS for it. */
		if ((old & LOCK_PENDING) != 0) {
			hardware_raise_fixed(PM1_GBL_STS);
			printf("FIRMWARE released the Global Lock, GBL_STS\n");
		} else {
			printf("FIRMWARE released the Global Lock\n");
		}

		/* Nothing else happens in the same tick. */
		return;
	}

	/* Firmware asks once for a lock the operating system owns; firmware_waits says it asked. */
	if (!firmware_waits && (global_lock & LOCK_OWNED) != 0) {
		(void)__atomic_fetch_or(&global_lock, LOCK_PENDING, __ATOMIC_SEQ_CST);
		firmware_waits = true;
		printf("FIRMWARE waits for the Global Lock\n");
	}
}

/*
 * Writes a FADT that describes the simulated hardware and reports its length.
 */
size_t
hardware_default_fadt(
	uint8_t *fadt,
	size_t size)
{
	size_t length;

	/* Refuses room too small for a FADT of revision 6, which is 276 bytes. */
	length = 276U;
	if (size < length)
		return 0;

	/* Writes the header: the signature, the length and the revision. */
	memset(fadt, 0, length);
	memcpy(fadt, "FACP", 4);
	fadt[4] = (uint8_t)length;
	fadt[5] = (uint8_t)(length >> 8);
	fadt[8] = 6;

	/* Writes the interrupt, the SMI command and the value that enables ACPI mode. */
	fadt[46] = SCI_INTERRUPT;
	fadt[48] = SMI_COMMAND;
	fadt[52] = ACPI_ENABLE;

	/* Writes the PM1 event and control blocks and the GPE block. */
	fadt[56] = PM1A_EVENT & 0xffU;
	fadt[57] = PM1A_EVENT >> 8;
	fadt[64] = PM1A_CONTROL & 0xffU;
	fadt[65] = PM1A_CONTROL >> 8;
	fadt[80] = GPE0_BLOCK & 0xffU;
	fadt[81] = GPE0_BLOCK >> 8;
	fadt[88] = PM1_EVENT_LEN;
	fadt[89] = PM1_CONTROL_LEN;
	fadt[92] = GPE0_LEN;

	/* Succeeded: reports the length; the checksum does not matter to the event code. */
	return length;
}

/* Reports whether a port is one the simulated hardware models, without touching it. */
static bool
port_modelled(
	uint32_t port)
{
	/* The PM1 event block. */
	if (port >= PM1A_EVENT && port < PM1A_EVENT + PM1_EVENT_LEN)
		return true;

	/* The PM1 control block. */
	if (port >= PM1A_CONTROL && port < PM1A_CONTROL + PM1_CONTROL_LEN)
		return true;

	/* The GPE block. */
	if (port >= GPE0_BLOCK && port < GPE0_BLOCK + GPE0_LEN)
		return true;

	/* The SMI command port and the EC's two ports. */
	if (port == SMI_COMMAND)
		return true;
	if (port == ec_data_port)
		return true;
	if (port == ec_command_port)
		return true;

	/* Reports a port of plain memory. */
	return false;
}

/* Reads or writes one byte of the modelled ports; reports whether it is modelled. */
static int
access_byte(
	uint32_t port,
	bool write,
	uint8_t *value)
{
	uint8_t *reg;
	bool status;
	int modelled;

	/* Finds the register and whether it is a status register. */
	reg = NULL;
	status = false;
	if (port >= PM1A_EVENT && port < PM1A_EVENT + PM1_EVENT_LEN) {
		/* The first half of the PM1 event block is its status register. */
		reg = &pm1_event[port - PM1A_EVENT];
		if (port - PM1A_EVENT < PM1_EVENT_LEN / 2U)
			status = true;
	} else if (port >= PM1A_CONTROL && port < PM1A_CONTROL + PM1_CONTROL_LEN) {
		/* A write to PM1_CNT may carry GBL_RLS for the waiting firmware. */
		reg = &pm1_control[port - PM1A_CONTROL];
		if (write)
			control_write(port, value);
	} else if (port >= GPE0_BLOCK && port < GPE0_BLOCK + GPE0_LEN) {
		/* The first half of the GPE block is its status registers. */
		reg = &gpe0[port - GPE0_BLOCK];
		if (port - GPE0_BLOCK < GPE0_LEN / 2U)
			status = true;
	} else if (port == SMI_COMMAND) {
		/* The SMI command port switches into ACPI mode and holds nothing. */
		smi_command(write, value);
		return 1;
	} else if (port == ec_data_port) {
		/* The EC's data port. */
		modelled = ec_data(write, value);
		return modelled;
	} else if (port == ec_command_port) {
		/* The EC's command and status port. */
		modelled = ec_command(write, value);
		return modelled;
	} else {
		/* Any other port is plain memory. */
		return 0;
	}

	/* A status bit written as one clears; everything else stores. */
	if (!write) {
		*value = *reg;
	} else if (status) {
		*reg &= (uint8_t)~*value;
	} else {
		*reg = *value;
	}

	/* Succeeded: the register answered. */
	return 1;
}

/*
 * Plays a write to PM1_CNT on the firmware's side: GBL_RLS hands the lock
 * to the waiting firmware, and the bit reads as zero afterwards.
 */
static void
control_write(
	uint32_t port,
	uint8_t *value)
{
	/* The second byte of PM1_CNT has SLP_TYP (bits 2 to 4) and SLP_EN (bit 5): the platform would sleep now (BUG-197). */
	if (port == PM1A_CONTROL + 1U && (*value & PM1_SLP_EN) != 0) {
		printf("FIRMWARE SLP_EN SLP_TYP %u\n", (unsigned)((*value >> 2) & 7U));
		return;
	}

	/* Only the first byte of PM1_CNT has GBL_RLS. */
	if (port != PM1A_CONTROL)
		return;

	/* A write without GBL_RLS hands nothing over. */
	if ((*value & PM1_GBL_RLS) == 0)
		return;

	/* Takes GBL_RLS out of what is stored. */
	*value = (uint8_t)(*value & ~PM1_GBL_RLS);
	printf("FIRMWARE GBL_RLS\n");

	/* Firmware that waits takes the lock once the operating system let it go. */
	if (firmware_waits && (global_lock & LOCK_OWNED) == 0) {
		global_lock = LOCK_OWNED;
		firmware_waits = false;
		firmware_owns = true;
		printf("FIRMWARE took the Global Lock\n");
	}
}

/*
 * Plays the SMI command port: writing the enable value switches into ACPI
 * mode, so SCI_EN comes on.  The simulated FADT uses 0xf0, and QEMU's uses
 * 0x02.  A read finds zero.
 */
static void
smi_command(
	bool write,
	uint8_t *value)
{
	/* A read finds zero. */
	if (!write) {
		*value = 0;
		return;
	}

	/* Either enable value sets SCI_EN in PM1_CNT. */
	if (*value == ACPI_ENABLE) {
		pm1_control[0] |= 0x01U;
	} else if (*value == QEMU_ACPI_ENABLE) {
		pm1_control[0] |= 0x01U;
	}
}

/* Reads or writes the EC's data port. */
static int
ec_data(
	bool write,
	uint8_t *value)
{
	/* A read takes the byte waiting there, which empties the output buffer. */
	if (!write) {
		*value = ec_output;
		ec_output_full = 0;
	} else {
		/* A write carries the address or the byte of the command in progress. */
		switch (ec_phase) {
		case EC_READ_ADDRESS:
			/* The address of a read: the byte there waits on the data port. */
			ec_output = ec_ram[*value];
			ec_output_full = 1;
			ec_phase = EC_IDLE;
			break;
		case EC_WRITE_ADDRESS:
			/* The address of a write: the byte follows. */
			ec_address = *value;
			ec_phase = EC_WRITE_DATA;
			break;
		case EC_WRITE_DATA:
			/* The byte of a write goes to its address. */
			ec_ram[ec_address] = *value;
			ec_phase = EC_IDLE;
			break;
		default:
			/* A byte no command expects is dropped. */
			break;
		}
	}

	/* Succeeded: the data port answered. */
	return 1;
}

/* Reads the EC's status or writes a command. */
static int
ec_command(
	bool write,
	uint8_t *value)
{
	/* A read gives the status: output full, and an event while queries wait. */
	if (!write) {
		/* Starts with no bit set. */
		*value = 0;

		/* OBF says a byte waits on the data port. */
		if (ec_output_full)
			*value |= EC_OBF;

		/* SCI_EVT says a query waits. */
		if (ec_query_count != 0)
			*value |= EC_SCI_EVT;
	} else {
		/* A command starts its phases, or answers a query at once. */
		switch (*value) {
		case EC_READ:
			/* A read waits for its address. */
			ec_phase = EC_READ_ADDRESS;
			break;
		case EC_WRITE:
			/* A write waits for its address. */
			ec_phase = EC_WRITE_ADDRESS;
			break;
		case EC_QUERY:
			/* A query is answered at once. */
			ec_query_answer();
			break;
		default:
			/* Any other command is ignored. */
			break;
		}
	}

	/* Succeeded: the command port answered. */
	return 1;
}

/* Answers a query: the oldest waiting query, or zero, waits on the data port. */
static void
ec_query_answer(void)
{
	/* Takes the oldest query out of the queue, when one waits. */
	ec_output = 0;
	if (ec_query_count != 0) {
		ec_output = ec_queries[0];
		memmove(ec_queries, ec_queries + 1, ec_query_count - 1U);
		ec_query_count--;
	}

	/* The answer waits on the data port. */
	ec_output_full = 1;
}

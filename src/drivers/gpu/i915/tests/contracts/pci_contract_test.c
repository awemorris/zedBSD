/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The PCI configuration, enable and MSI contract, checked on the host.
 *
 * Runs pci.c against the mock PCI function.  It checks the behaviour of
 * the Linux calls pci.c stands for -- the resource-aware enable
 * (pci_enable_resources), the enable reference count (pci_enable_device /
 * pci_disable_device), and the MSI setup that frees what it took when it
 * fails part way (pci_enable_msi) -- not only the bits it writes.
 */

#include "contract.h"
#include "mock_pci.h"

#include "../../pci.h"
#include "../../trace.h"

#include <errno.h>
#include <stdint.h>

static void pci_check_config_access(struct i915_pci *pci, struct mock_pci *mock, struct i915_trace *trace);
static void pci_check_enable(struct i915_pci *pci, struct mock_pci *mock, struct i915_trace *trace);
static void pci_check_enable_count(struct i915_pci *pci, struct mock_pci *mock, struct i915_trace *trace);
static void pci_check_msi(struct i915_pci *pci, struct mock_pci *mock, struct i915_trace *trace);
static void pci_check_msi_failures(struct i915_pci *pci, struct mock_pci *mock, struct i915_trace *trace);
static void pci_check_power_without_pm(struct i915_pci *pci, struct mock_pci *mock, struct i915_trace *trace);

/*
 * Runs the PCI contract checks.
 */
int
main(void)
{
	static struct i915_trace trace;
	static struct i915_pci pci;
	static struct mock_pci mock;
	int status;

	contract_begin("PCI contract tests (mock config space, GPU-free)");

	/* Every group builds its own mock function; they share one trace. */
	drv_i915_trace_init(&trace);

	/* Runs each contract group in the order the old suite ran them. */
	pci_check_config_access(&pci, &mock, &trace);
	pci_check_enable(&pci, &mock, &trace);
	pci_check_enable_count(&pci, &mock, &trace);
	pci_check_msi(&pci, &mock, &trace);
	pci_check_msi_failures(&pci, &mock, &trace);
	pci_check_power_without_pm(&pci, &mock, &trace);

	/* Reports whether every check held. */
	status = contract_end();
	if (status != 0)
		return status;

	/* Succeeded: the PCI layer keeps its contract. */
	return 0;
}

/* Checks the width of configuration access, the capability walk and the BAR kinds. */
static void
pci_check_config_access(
	struct i915_pci *pci,
	struct mock_pci *mock,
	struct i915_trace *trace)
{
	uint64_t address;
	uint32_t dword;
	uint16_t word;
	uint8_t byte;
	unsigned capability;
	int kind;

	contract_section("cfg: typed width access assembles little-endian (pci_bus_read_config_*)");

	/* Binds the PCI access to the full mock function. */
	mock_pci_setup_full(mock);
	drv_i915_pci_init(pci, mock_pci_ops(), mock, trace);

	/* A word and a dword assemble low byte first. */
	word = drv_i915_pci_read16(pci, 0x00U);
	contract_check(word == 0x8086U, "read16 vendor");
	dword = drv_i915_pci_read32(pci, 0x00U);
	contract_check(dword == 0x46a88086U, "read32 = device<<16|vendor");

	/* A byte write leaves its neighbour alone. */
	drv_i915_pci_write8(pci, 0x40U, 0xAAU);
	byte = drv_i915_pci_read8(pci, 0x41U);
	contract_check(byte == 0x50U, "write8 spared the next byte");

	contract_section("cap: present caps found; absent -> 0 (pci_find_capability)");

	/* Rebuilds the space the byte write disturbed. */
	mock_pci_setup_full(mock);

	/* The walk finds MSI where it is and reports a missing MSI-X as zero. */
	capability = drv_i915_pci_find_capability(pci, I915_PCI_CAP_ID_MSI);
	contract_check(capability == 0x50U, "MSI at 0x50");
	capability = drv_i915_pci_find_capability(pci, I915_PCI_CAP_ID_MSIX);
	contract_check(capability == 0U, "MSI-X absent -> 0");

	contract_section("bar: BAR classification IO vs MEM (pci resource flags)");

	/* BAR0 decodes memory at its programmed address. */
	address = 0U;
	kind = drv_i915_pci_bar_kind(pci, 0U, &address);
	contract_check(kind == I915_PCI_RES_MEM, "BAR0 is MEM");
	contract_check(address == 0xdf000000U, "BAR0 address decoded");

	/* An unprogrammed BAR decodes nothing. */
	kind = drv_i915_pci_bar_kind(pci, 1U, &address);
	contract_check(kind == I915_PCI_RES_NONE, "BAR1 unused");
}

/* Checks that the enable turns on decode only for the resources present. */
static void
pci_check_enable(
	struct i915_pci *pci,
	struct mock_pci *mock,
	struct i915_trace *trace)
{
	uint16_t status_before;
	uint16_t command;
	uint16_t status;
	uint16_t power;
	int error;

	contract_section("enable: MEM-only device enables MEM decode, NOT IO (pci_enable_resources)");

	/* Enables the memory-only function; the status register must not move. */
	status_before = drv_i915_pci_read16(pci, I915_PCI_STATUS);
	error = drv_i915_pci_enable_device(pci);
	contract_check(error == 0, "enable ok");

	/* Only memory decode is on. */
	command = drv_i915_pci_read16(pci, I915_PCI_COMMAND);
	contract_check((command & I915_PCI_CMD_MEMORY) != 0U, "MEM enabled");
	contract_check((command & I915_PCI_CMD_IO) == 0U, "IO NOT enabled (no IO resource)");
	status = drv_i915_pci_read16(pci, I915_PCI_STATUS);
	contract_check(status == status_before, "STATUS untouched");

	/* The device was brought from D3hot to D0. */
	power = drv_i915_pci_read16(pci, 0x40U + I915_PCI_PM_CTRL);
	contract_check((power & I915_PCI_PM_STATE_MASK) == I915_PCI_D0, "driven to D0");

	contract_section("enable: device with an IO BAR also enables IO decode");

	/* Enables a function that has an IO BAR as well. */
	mock_pci_setup_io_and_mem(mock);
	drv_i915_pci_init(pci, mock_pci_ops(), mock, trace);
	error = drv_i915_pci_enable_device(pci);
	contract_check(error == 0, "enable ok");

	/* Both kinds of decode are on. */
	command = drv_i915_pci_read16(pci, I915_PCI_COMMAND);
	contract_check((command & I915_PCI_CMD_IO) != 0U, "IO enabled (IO BAR present)");
	contract_check((command & I915_PCI_CMD_MEMORY) != 0U, "MEM enabled");
}

/* Checks that N enables need N disables. */
static void
pci_check_enable_count(
	struct i915_pci *pci,
	struct mock_pci *mock,
	struct i915_trace *trace)
{
	uint16_t command;
	int enabled;
	int error;

	contract_section("refcount: enable x2 / disable x1 leaves the device enabled (pci_enable_device / pci_disable_device)");

	/* Two users enable the function. */
	mock_pci_setup_full(mock);
	drv_i915_pci_init(pci, mock_pci_ops(), mock, trace);
	error = drv_i915_pci_enable_device(pci);
	contract_check(error == 0, "enable #1");
	error = drv_i915_pci_enable_device(pci);
	contract_check(error == 0, "enable #2 (refcount)");
	enabled = drv_i915_pci_is_enabled(pci);
	contract_check(enabled != 0, "enabled");

	/* One disable leaves the other user's decode on. */
	drv_i915_pci_disable_device(pci);
	enabled = drv_i915_pci_is_enabled(pci);
	contract_check(enabled != 0, "still enabled after one disable (a user remains)");
	command = drv_i915_pci_read16(pci, I915_PCI_COMMAND);
	contract_check((command & I915_PCI_CMD_MEMORY) != 0U, "decode still on");

	/* The last disable turns decode off. */
	drv_i915_pci_disable_device(pci);
	enabled = drv_i915_pci_is_enabled(pci);
	contract_check(enabled == 0, "disabled after last disable");
	command = drv_i915_pci_read16(pci, I915_PCI_COMMAND);
	contract_check((command & I915_PCI_CMD_MEMORY) == 0U, "decode cleared");
}

/* Checks that MSI is a vector, a message built from it, and then the enable bit. */
static void
pci_check_msi(
	struct i915_pci *pci,
	struct mock_pci *mock,
	struct i915_trace *trace)
{
	uint32_t address;
	uint16_t flags;
	uint16_t data;
	int enabled;
	int error;

	contract_section("msi: setup allocates a vector, builds message from it, then enables (pci_enable_msi)");

	/* Sets MSI up on the full function. */
	mock_pci_setup_full(mock);
	drv_i915_pci_init(pci, mock_pci_ops(), mock, trace);
	error = drv_i915_pci_setup_msi(pci);
	contract_check(error == 0, "setup_msi ok");
	contract_check(mock->alloc_calls == 1, "one vector allocated");

	/* The capability is enabled and addressed to the local APIC window. */
	flags = drv_i915_pci_read16(pci, 0x50U + I915_PCI_MSI_FLAGS);
	contract_check((flags & I915_PCI_MSI_ENABLE) != 0U, "enable bit set");
	address = drv_i915_pci_read32(pci, 0x50U + I915_PCI_MSI_ADDR_LO);
	contract_check(address == 0xFEE00000U, "arch message address");

	/* The message data is the vector just allocated, not a fixed copy. */
	data = drv_i915_pci_read16(pci, 0x50U + I915_PCI_MSI_DATA_64);
	contract_check((data & 0xFFU) == 0x21U, "message data = allocated vector, not a fixed copy");
	enabled = drv_i915_pci_msi_enabled(pci);
	contract_check(enabled != 0, "msi marked enabled");

	/* The teardown frees that vector and clears the enable bit. */
	drv_i915_pci_teardown_msi(pci);
	contract_check(mock->free_calls == 1, "teardown freed one vector");
	contract_check(mock->last_free_vector == 0x21, "teardown freed the allocated vector");
	flags = drv_i915_pci_read16(pci, 0x50U + I915_PCI_MSI_FLAGS);
	contract_check((flags & I915_PCI_MSI_ENABLE) == 0U, "enable cleared");
}

/* Checks the three ways the MSI setup can fail. */
static void
pci_check_msi_failures(
	struct i915_pci *pci,
	struct mock_pci *mock,
	struct i915_trace *trace)
{
	uint16_t flags;
	int enabled;
	int error;

	contract_section("msi: absent MSI cap: ENODEV, no config write, fall back to INTx");

	/* A function without MSI is refused before anything is written or allocated. */
	mock_pci_setup_no_msi(mock);
	drv_i915_pci_init(pci, mock_pci_ops(), mock, trace);
	mock->write16_calls = 0;
	mock->write32_calls = 0;
	error = drv_i915_pci_setup_msi(pci);
	contract_check(error == ENODEV, "setup_msi -> ENODEV");
	contract_check(mock->write16_calls == 0, "no 16-bit config write");
	contract_check(mock->write32_calls == 0, "no 32-bit config write");
	contract_check(mock->alloc_calls == 0, "no vector allocated");

	contract_section("msi: vector allocation failure: not enabled, no leak");

	/* A failed allocation holds nothing and enables nothing. */
	mock_pci_setup_full(mock);
	drv_i915_pci_init(pci, mock_pci_ops(), mock, trace);
	mock->msi_alloc_fail = 1;
	error = drv_i915_pci_setup_msi(pci);
	contract_check(error == EIO, "setup_msi -> EIO (alloc failed)");
	enabled = drv_i915_pci_msi_enabled(pci);
	contract_check(enabled == 0, "MSI not enabled");
	flags = drv_i915_pci_read16(pci, 0x50U + I915_PCI_MSI_FLAGS);
	contract_check((flags & I915_PCI_MSI_ENABLE) == 0U, "enable bit not set");
	contract_check(mock->free_calls == 0, "nothing to free (alloc failed)");

	contract_section("msi: message-config failure after alloc releases the vector");

	/* A vector that cannot form a message is given back. */
	mock_pci_setup_full(mock);
	drv_i915_pci_init(pci, mock_pci_ops(), mock, trace);
	mock->msi_alloc_bad = 1;
	error = drv_i915_pci_setup_msi(pci);
	contract_check(error != 0, "setup_msi fails on message config");
	enabled = drv_i915_pci_msi_enabled(pci);
	contract_check(enabled == 0, "not enabled");
	contract_check(mock->alloc_calls == 1, "one vector was allocated");
	contract_check(mock->free_calls == 1, "the acquired vector was released");
}

/* Checks that a function without power management is left alone. */
static void
pci_check_power_without_pm(
	struct i915_pci *pci,
	struct mock_pci *mock,
	struct i915_trace *trace)
{
	int error;

	contract_section("power: set_power_state with no PM cap writes nothing");

	/* The power change succeeds without touching the space. */
	mock_pci_setup_no_pm(mock);
	drv_i915_pci_init(pci, mock_pci_ops(), mock, trace);
	mock->write16_calls = 0;
	error = drv_i915_pci_set_power_state(pci, I915_PCI_D0);
	contract_check(error == 0, "returns 0");
	contract_check(mock->write16_calls == 0, "no write when PM cap absent");
}

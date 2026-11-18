/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * A mock PCI function behind struct i915_pci_ops (see mock_pci.h).
 *
 * Every preset is an Alder Lake-P graphics function (8086:46a8) with a
 * memory BAR0; the presets differ in their capability list and BARs:
 *
 *   full         PM at 0x40, MSI at 0x50, PCI Express at 0x60
 *   no_msi       PM at 0x40, PCI Express at 0x60
 *   no_pm        MSI at 0x50, PCI Express at 0x60
 *   io_and_mem   as full, plus an IO BAR2
 */

#include "mock_pci.h"

#include <errno.h>

/* The first vector the allocator hands out. */
#define MOCK_PCI_FIRST_VECTOR	0x21

/* A vector whose low byte is zero, which cannot form an MSI message. */
#define MOCK_PCI_BAD_VECTOR	0x100

/* Where the mock places its capabilities and BARs. */
#define MOCK_PCI_PM_AT		0x40U
#define MOCK_PCI_MSI_AT		0x50U
#define MOCK_PCI_PCIE_AT	0x60U
#define MOCK_PCI_MEM_BAR0	0xdf000000U
#define MOCK_PCI_IO_BAR2	0x0000e000U

static uint8_t mock_pci_read8(void *context, unsigned offset);
static uint16_t mock_pci_read16(void *context, unsigned offset);
static uint32_t mock_pci_read32(void *context, unsigned offset);
static void mock_pci_write8(void *context, unsigned offset, uint8_t value);
static void mock_pci_write16(void *context, unsigned offset, uint16_t value);
static void mock_pci_write32(void *context, unsigned offset, uint32_t value);
static int mock_pci_alloc_msi_vector(void *context, int *vector);
static void mock_pci_free_msi_vector(void *context, int vector);
static void mock_pci_base(struct mock_pci *mock);
static void mock_pci_store32(struct mock_pci *mock, unsigned offset, uint32_t value);
static void mock_pci_mem_bar(struct mock_pci *mock, unsigned index, uint32_t address);
static void mock_pci_io_bar(struct mock_pci *mock, unsigned index, uint32_t address);
static void mock_pci_pm_capability(struct mock_pci *mock, unsigned at, uint8_t next);
static void mock_pci_msi_capability(struct mock_pci *mock, unsigned at, uint8_t next);
static void mock_pci_pcie_capability(struct mock_pci *mock, unsigned at);

/*
 * Returns the operations that reach a mock PCI function.
 *
 * The context those operations receive is a struct mock_pci.
 */
const struct i915_pci_ops *
mock_pci_ops(void)
{
	static const struct i915_pci_ops ops = {
		mock_pci_read8,
		mock_pci_read16,
		mock_pci_read32,
		mock_pci_write8,
		mock_pci_write16,
		mock_pci_write32,
		mock_pci_alloc_msi_vector,
		mock_pci_free_msi_vector
	};

	/* Succeeded: the operations reach the mock. */
	return &ops;
}

/*
 * Builds a function with power management, MSI and a memory BAR only.
 */
void
mock_pci_setup_full(
	struct mock_pci *mock)
{
	/* Starts from the bare header and chains PM, MSI and PCI Express. */
	mock_pci_base(mock);
	mock->config[I915_PCI_CAP_PTR] = MOCK_PCI_PM_AT;
	mock_pci_pm_capability(mock, MOCK_PCI_PM_AT, MOCK_PCI_MSI_AT);
	mock_pci_msi_capability(mock, MOCK_PCI_MSI_AT, MOCK_PCI_PCIE_AT);
	mock_pci_pcie_capability(mock, MOCK_PCI_PCIE_AT);

	/* Decodes memory only. */
	mock_pci_mem_bar(mock, 0U, MOCK_PCI_MEM_BAR0);
}

/*
 * Builds a function without an MSI capability.
 */
void
mock_pci_setup_no_msi(
	struct mock_pci *mock)
{
	/* Starts from the bare header and chains PM and PCI Express. */
	mock_pci_base(mock);
	mock->config[I915_PCI_CAP_PTR] = MOCK_PCI_PM_AT;
	mock_pci_pm_capability(mock, MOCK_PCI_PM_AT, MOCK_PCI_PCIE_AT);
	mock_pci_pcie_capability(mock, MOCK_PCI_PCIE_AT);

	/* Decodes memory only. */
	mock_pci_mem_bar(mock, 0U, MOCK_PCI_MEM_BAR0);
}

/*
 * Builds a function without a power-management capability.
 */
void
mock_pci_setup_no_pm(
	struct mock_pci *mock)
{
	/* Starts from the bare header and chains MSI and PCI Express. */
	mock_pci_base(mock);
	mock->config[I915_PCI_CAP_PTR] = MOCK_PCI_MSI_AT;
	mock_pci_msi_capability(mock, MOCK_PCI_MSI_AT, MOCK_PCI_PCIE_AT);
	mock_pci_pcie_capability(mock, MOCK_PCI_PCIE_AT);

	/* Decodes memory only. */
	mock_pci_mem_bar(mock, 0U, MOCK_PCI_MEM_BAR0);
}

/*
 * Builds a function with both a memory BAR and an IO BAR.
 */
void
mock_pci_setup_io_and_mem(
	struct mock_pci *mock)
{
	/* Starts from the full capability list. */
	mock_pci_setup_full(mock);

	/* Adds an IO BAR, which asks for IO decode too. */
	mock_pci_io_bar(mock, 2U, MOCK_PCI_IO_BAR2);
}

/* Reads one configuration byte; outside the space reads all-ones. */
static uint8_t
mock_pci_read8(
	void *context,
	unsigned offset)
{
	struct mock_pci *mock;

	/* An offset past the space reads what an absent register would. */
	mock = context;
	if (offset >= MOCK_PCI_CONFIG_BYTES)
		return 0xffU;

	/* Succeeded: reports the byte. */
	return mock->config[offset];
}

/* Reads one little-endian configuration word; outside the space reads all-ones. */
static uint16_t
mock_pci_read16(
	void *context,
	unsigned offset)
{
	struct mock_pci *mock;
	uint16_t value;

	/* A word that runs past the space reads what an absent register would. */
	mock = context;
	if (offset + 1U >= MOCK_PCI_CONFIG_BYTES)
		return 0xffffU;

	/* Assembles the word, low byte first. */
	value = (uint16_t)mock->config[offset];
	value |= (uint16_t)((uint16_t)mock->config[offset + 1U] << 8);

	/* Succeeded: reports the word. */
	return value;
}

/* Reads one little-endian configuration dword; outside the space reads all-ones. */
static uint32_t
mock_pci_read32(
	void *context,
	unsigned offset)
{
	struct mock_pci *mock;
	uint32_t value;

	/* A dword that runs past the space reads what an absent register would. */
	mock = context;
	if (offset + 3U >= MOCK_PCI_CONFIG_BYTES)
		return 0xffffffffU;

	/* Assembles the dword, low byte first. */
	value = (uint32_t)mock->config[offset];
	value |= (uint32_t)mock->config[offset + 1U] << 8;
	value |= (uint32_t)mock->config[offset + 2U] << 16;
	value |= (uint32_t)mock->config[offset + 3U] << 24;

	/* Succeeded: reports the dword. */
	return value;
}

/* Writes one configuration byte and counts it. */
static void
mock_pci_write8(
	void *context,
	unsigned offset,
	uint8_t value)
{
	struct mock_pci *mock;

	/* Counts the write even when it falls outside the space. */
	mock = context;
	mock->write8_calls++;
	if (offset >= MOCK_PCI_CONFIG_BYTES)
		return;

	mock->config[offset] = value;
}

/* Writes one little-endian configuration word and counts it. */
static void
mock_pci_write16(
	void *context,
	unsigned offset,
	uint16_t value)
{
	struct mock_pci *mock;

	/* Counts the write even when it runs past the space. */
	mock = context;
	mock->write16_calls++;
	if (offset + 1U >= MOCK_PCI_CONFIG_BYTES)
		return;

	/* Stores the word, low byte first. */
	mock->config[offset] = (uint8_t)(value & 0xffU);
	mock->config[offset + 1U] = (uint8_t)((value >> 8) & 0xffU);
}

/* Writes one little-endian configuration dword and counts it. */
static void
mock_pci_write32(
	void *context,
	unsigned offset,
	uint32_t value)
{
	struct mock_pci *mock;

	/* Counts the write even when it runs past the space. */
	mock = context;
	mock->write32_calls++;
	if (offset + 3U >= MOCK_PCI_CONFIG_BYTES)
		return;

	mock_pci_store32(mock, offset, value);
}

/* Hands out the next vector, a bad one, or fails, as the test asked. */
static int
mock_pci_alloc_msi_vector(
	void *context,
	int *vector)
{
	struct mock_pci *mock;

	/* Counts the allocation whatever its outcome. */
	mock = context;
	mock->alloc_calls++;

	/* A failed allocation holds nothing. */
	if (mock->msi_alloc_fail != 0)
		return EIO;

	/* A vector with a zero low byte makes the message setup fail after the allocation. */
	if (mock->msi_alloc_bad != 0) {
		mock->last_alloc_vector = MOCK_PCI_BAD_VECTOR;
		*vector = MOCK_PCI_BAD_VECTOR;
		return 0;
	}

	/* Hands out the next vector in sequence. */
	mock->last_alloc_vector = mock->msi_vector_next;
	*vector = mock->msi_vector_next;
	mock->msi_vector_next++;

	/* Succeeded: the vector is held until it is freed. */
	return 0;
}

/* Records a vector being given back. */
static void
mock_pci_free_msi_vector(
	void *context,
	int vector)
{
	struct mock_pci *mock;

	/* Counts the release and remembers which vector it was. */
	mock = context;
	mock->free_calls++;
	mock->last_free_vector = vector;
}

/* Clears the space and the counters and writes the bare configuration header. */
static void
mock_pci_base(
	struct mock_pci *mock)
{
	unsigned offset;

	/* Clears the whole configuration space. */
	for (offset = 0U; offset < MOCK_PCI_CONFIG_BYTES; offset++)
		mock->config[offset] = 0U;

	/* Forgets every write. */
	mock->write8_calls = 0;
	mock->write16_calls = 0;
	mock->write32_calls = 0;

	/* Resets the vector allocator to hand out vectors from the first one. */
	mock->msi_alloc_fail = 0;
	mock->msi_alloc_bad = 0;
	mock->msi_vector_next = MOCK_PCI_FIRST_VECTOR;
	mock->alloc_calls = 0;
	mock->free_calls = 0;
	mock->last_alloc_vector = -1;
	mock->last_free_vector = -1;

	/* The vendor and device identify an Alder Lake-P graphics function. */
	mock->config[0x00] = 0x86U;
	mock->config[0x01] = 0x80U;
	mock->config[0x02] = 0xa8U;
	mock->config[0x03] = 0x46U;

	/* Decode starts off; the status says a capability list exists, with read-only bits set. */
	mock->config[I915_PCI_COMMAND] = 0x00U;
	mock->config[I915_PCI_COMMAND + 1U] = 0x00U;
	mock->config[I915_PCI_STATUS] = 0x90U;
	mock->config[I915_PCI_STATUS + 1U] = 0x02U;
}

/* Stores a little-endian dword into the configuration space. */
static void
mock_pci_store32(
	struct mock_pci *mock,
	unsigned offset,
	uint32_t value)
{
	/* Stores the dword, low byte first. */
	mock->config[offset] = (uint8_t)(value & 0xffU);
	mock->config[offset + 1U] = (uint8_t)((value >> 8) & 0xffU);
	mock->config[offset + 2U] = (uint8_t)((value >> 16) & 0xffU);
	mock->config[offset + 3U] = (uint8_t)((value >> 24) & 0xffU);
}

/* Writes a memory BAR: bit 0 clear selects memory space. */
static void
mock_pci_mem_bar(
	struct mock_pci *mock,
	unsigned index,
	uint32_t address)
{
	/* Keeps the address above the flag bits. */
	mock_pci_store32(mock, I915_PCI_BAR0 + index * 4U, address & ~0xFU);
}

/* Writes an IO BAR: bit 0 set selects IO space. */
static void
mock_pci_io_bar(
	struct mock_pci *mock,
	unsigned index,
	uint32_t address)
{
	/* Keeps the address above the flag bits and marks the BAR as IO. */
	mock_pci_store32(mock, I915_PCI_BAR0 + index * 4U, (address & ~0x3U) | 0x1U);
}

/* Writes a power-management capability whose device sits in D3hot. */
static void
mock_pci_pm_capability(
	struct mock_pci *mock,
	unsigned at,
	uint8_t next)
{
	/* Links the capability into the list. */
	mock->config[at] = I915_PCI_CAP_ID_PM;
	mock->config[at + 1U] = next;

	/* The control and status register reports D3hot. */
	mock->config[at + I915_PCI_PM_CTRL] = 0x03U;
	mock->config[at + I915_PCI_PM_CTRL + 1U] = 0x00U;
}

/* Writes a 64-bit capable MSI capability with MSI disabled. */
static void
mock_pci_msi_capability(
	struct mock_pci *mock,
	unsigned at,
	uint8_t next)
{
	/* Links the capability into the list. */
	mock->config[at] = I915_PCI_CAP_ID_MSI;
	mock->config[at + 1U] = next;

	/* The message control says 64-bit capable and not enabled. */
	mock->config[at + I915_PCI_MSI_FLAGS] = (uint8_t)I915_PCI_MSI_64BIT;
	mock->config[at + I915_PCI_MSI_FLAGS + 1U] = 0x00U;
}

/* Writes a PCI Express capability that ends the list. */
static void
mock_pci_pcie_capability(
	struct mock_pci *mock,
	unsigned at)
{
	/* Links the capability as the last one. */
	mock->config[at] = I915_PCI_CAP_ID_PCIE;
	mock->config[at + 1U] = 0x00U;
}

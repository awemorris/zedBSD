/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The payloads of the calls the GSP boot makes, and the GSP's register
 * program (the CPU sequencer) it sends back.
 *
 * Before the GSP starts, the driver queues the system information (where
 * the GPU's windows are and what PCI says about it) and the registry (the
 * resource manager's tunables).  During the start the GSP may send a list
 * of register operations for the driver to perform; this file decodes them,
 * and the caller performs them.  The payloads are laid out by byte offset
 * (the 570.144 layouts); nothing here touches the hardware.
 */

#include <stddef.h>
#include <stdint.h>

#include <uapi/errno.h>

#include "drivers/gpu/nvrtx/nvrtx-gsp.h"

/*
 * The system information's fields the driver fills; every other byte is zero.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/nvrm/gsp.h (GspSystemInfo)
 * and nvkm/subdev/gsp/rm/r570/gsp.c (r570_gsp_set_system_info).
 */
#define RECORDS_SYSINFO_BAR0_AT		0U
#define RECORDS_SYSINFO_BAR1_AT		8U
#define RECORDS_SYSINFO_BAR3_AT		16U
#define RECORDS_SYSINFO_BDF_AT		32U
#define RECORDS_SYSINFO_CONSOLE_AT	64U
#define RECORDS_SYSINFO_USER_VA_AT	72U
#define RECORDS_SYSINFO_MIRROR_AT	80U
#define RECORDS_SYSINFO_MIRROR_SIZE_AT	84U
#define RECORDS_SYSINFO_PCI_ID_AT	88U
#define RECORDS_SYSINFO_PCI_SUB_ID_AT	92U
#define RECORDS_SYSINFO_REVISION_AT	96U
#define RECORDS_SYSINFO_PRIMARY_AT	896U

/*
 * The copy of the PCI configuration space in the register window.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/pci/gp100.c (gp100_pci_func).
 */
#define RECORDS_CONFIG_MIRROR		0x088000U
#define RECORDS_CONFIG_MIRROR_SIZE	0x1000U

/*
 * The registry: its header, one 16-byte entry per tunable, then the names;
 * every tunable is a 32-bit number.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/gsp.h
 * (PACKED_REGISTRY_TABLE, PACKED_REGISTRY_ENTRY) and
 * nvkm/subdev/gsp/rm/r535/gsp.c (REGISTRY_TABLE_ENTRY_TYPE_DWORD).
 */
#define RECORDS_REGISTRY_HEADER		8U
#define RECORDS_REGISTRY_ENTRY		16U
#define RECORDS_REGISTRY_TYPE_NUMBER	1U

/*
 * The sequencer payload: the command count in words, and the commands after the
 * eight saved registers.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/nvrm/gsp.h
 * (rpc_run_cpu_sequencer_v17_00).
 */
#define RECORDS_SEQUENCER_COUNT_AT	4U
#define RECORDS_SEQUENCER_SAVE_AT	8U
#define RECORDS_SEQUENCER_COMMANDS_AT	40U
#define RECORDS_SEQUENCER_SAVE_SLOTS	8U

/*
 * One tunable the resource manager is started with.
 */
struct records_tunable {
	const char *name;
	uint32_t value;
};

/*
 * The tunables every boot passes: secondary bus reset allowed, the PCI
 * configuration kept over a reset, and no check of the device ID against the
 * firmware's own list of products.
 *
 * Values referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c
 * (r535_registry_entries).
 */
static const struct records_tunable records_tunables[] = {
	{ "RMSecBusResetEnable", 1U },
	{ "RMForcePcieConfigSave", 1U },
	{ "RMDevidCheckIgnore", 1U }
};

static size_t records_strlen(const char *text);

/*
 * Writes the system information the GSP's resource manager starts with.
 *
 * The record is NVRTX_SYSINFO_BYTES long; fields not written are zero (no
 * ACPI data, no virtual functions).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r570/gsp.c
 * (r570_gsp_set_system_info).
 */
void
nvrtx_sysinfo(
	uint8_t *record,
	const struct nvrtx_sysinfo_values *values)
{
	size_t at;

	/* Clears the record. */
	for (at = 0U; at < NVRTX_SYSINFO_BYTES; at++)
		record[at] = 0U;

	/* Gives the CPU addresses of the register, VRAM and instance-memory windows. */
	nvrtx_put64(record, RECORDS_SYSINFO_BAR0_AT, values->registers);
	nvrtx_put64(record, RECORDS_SYSINFO_BAR1_AT, values->vram);
	nvrtx_put64(record, RECORDS_SYSINFO_BAR3_AT, values->instance);

	/* Gives the PCI address and the highest user address. */
	nvrtx_put64(record, RECORDS_SYSINFO_BDF_AT, values->bus_device_function);
	nvrtx_put64(record, RECORDS_SYSINFO_USER_VA_AT, values->user_address_limit);

	/* Gives the size of the firmware's console at the VRAM's start, which the resource manager keeps (0: none). */
	nvrtx_put64(record, RECORDS_SYSINFO_CONSOLE_AT, values->console_size);

	/* Gives the configuration-space copy in the register window. */
	nvrtx_put32(record, RECORDS_SYSINFO_MIRROR_AT, RECORDS_CONFIG_MIRROR);
	nvrtx_put32(record, RECORDS_SYSINFO_MIRROR_SIZE_AT, RECORDS_CONFIG_MIRROR_SIZE);

	/* Gives the PCI identities: device over vendor, subsystem device over subsystem vendor. */
	nvrtx_put32(record, RECORDS_SYSINFO_PCI_ID_AT, ((uint32_t)values->device << 16) | values->vendor);
	nvrtx_put32(record, RECORDS_SYSINFO_PCI_SUB_ID_AT, ((uint32_t)values->subdevice << 16) | values->subvendor);
	nvrtx_put32(record, RECORDS_SYSINFO_REVISION_AT, values->revision);

	/* Says whether this GPU drives the primary display (the firmware's picture is on it). */
	nvrtx_put8(record, RECORDS_SYSINFO_PRIMARY_AT, (uint8_t)(values->primary != 0 ? 1U : 0U));
}

/*
 * Writes the registry of tunables.
 *
 * Returns 0 with the registry's length, or E2BIG when it does not fit.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c
 * (r535_gsp_rpc_set_registry).
 */
int
nvrtx_registry(
	uint8_t *registry,
	size_t capacity,
	size_t *length)
{
	size_t count;
	size_t names_at;
	size_t name_length;
	size_t index;
	size_t at;

	/* The names follow the header and the entries. */
	count = sizeof(records_tunables) / sizeof(records_tunables[0]);
	names_at = RECORDS_REGISTRY_HEADER + count * RECORDS_REGISTRY_ENTRY;
	if (names_at > capacity)
		return E2BIG;

	/* Clears the header and the entries. */
	for (at = 0U; at < names_at; at++)
		registry[at] = 0U;

	/* Writes each entry and appends its name with its terminating zero. */
	for (index = 0U; index < count; index++) {
		name_length = records_strlen(records_tunables[index].name) + 1U;
		if (names_at + name_length > capacity)
			return E2BIG;

		at = RECORDS_REGISTRY_HEADER + index * RECORDS_REGISTRY_ENTRY;
		nvrtx_put32(registry, at + 0U, (uint32_t)names_at);
		nvrtx_put8(registry, at + 4U, RECORDS_REGISTRY_TYPE_NUMBER);
		nvrtx_put32(registry, at + 8U, records_tunables[index].value);
		nvrtx_put32(registry, at + 12U, 4U);
		for (at = 0U; at < name_length; at++)
			registry[names_at + at] = (uint8_t)records_tunables[index].name[at];
		names_at += name_length;
	}

	/* Writes the header: the whole length and the entry count. */
	nvrtx_put32(registry, 0U, (uint32_t)names_at);
	nvrtx_put32(registry, 4U, (uint32_t)count);

	/* Succeeded: the registry's length. */
	*length = names_at;
	return 0;
}

/*
 * Writes the arguments of the unload call for a plain unload (not a suspend).
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c
 * (r535_gsp_rpc_unloading_guest_driver).
 */
void
nvrtx_unload_args(
	uint8_t *args)
{
	size_t at;

	/* Not in a power transition, not entering GC6, power level 0. */
	for (at = 0U; at < NVRTX_UNLOAD_ARGS_BYTES; at++)
		args[at] = 0U;
}

/*
 * Decodes the next command of a CPU sequencer payload.
 *
 * The cursor counts words of the command list.  Returns 0 with the command,
 * ENOENT when the list is done, or EINVAL for an unknown opcode or a command
 * running past the payload.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/gsp.c
 * (r535_gsp_msg_run_cpu_sequencer).
 */
int
nvrtx_sequencer_next(
	const uint8_t *payload,
	size_t length,
	uint32_t *cursor,
	struct nvrtx_seq_command *command)
{
	uint32_t count;
	uint32_t words;
	uint32_t index;
	size_t at;

	/* The list ends at the count the payload gives. */
	if (length < RECORDS_SEQUENCER_COMMANDS_AT)
		return EINVAL;
	count = nvrtx_get32(payload, RECORDS_SEQUENCER_COUNT_AT);
	if (*cursor >= count)
		return ENOENT;

	/* Reads the opcode and finds how many argument words follow it. */
	at = RECORDS_SEQUENCER_COMMANDS_AT + (size_t)*cursor * 4U;
	if (at + 4U > length)
		return EINVAL;
	command->opcode = nvrtx_get32(payload, at);

	/* Each opcode has a fixed number of argument words. */
	switch (command->opcode) {
	case NVRTX_SEQ_WRITE:
		words = 2U;
		break;
	case NVRTX_SEQ_MODIFY:
		words = 3U;
		break;
	case NVRTX_SEQ_POLL:
		words = 5U;
		break;
	case NVRTX_SEQ_DELAY:
		words = 1U;
		break;
	case NVRTX_SEQ_STORE:
		words = 2U;
		break;
	case NVRTX_SEQ_CORE_RESET:
	case NVRTX_SEQ_CORE_START:
	case NVRTX_SEQ_CORE_WAIT_HALT:
	case NVRTX_SEQ_CORE_RESUME:
		words = 0U;
		break;
	default:
		return EINVAL;
	}

	/* Reads the argument words. */
	if (at + 4U + (size_t)words * 4U > length)
		return EINVAL;
	for (index = 0U; index < 5U; index++) {
		command->argument[index] = 0U;
		if (index < words)
			command->argument[index] = nvrtx_get32(payload, at + 4U + (size_t)index * 4U);
	}

	/* Moves the cursor past the command. */
	*cursor += 1U + words;

	/* Succeeded: the command is decoded. */
	return 0;
}

/*
 * Stores a register value into one of the sequencer payload's eight save slots.
 */
int
nvrtx_sequencer_save(
	uint8_t *payload,
	size_t length,
	uint32_t slot,
	uint32_t value)
{
	/* Refuses a slot beyond the eight, or a payload too short to hold them. */
	if (slot >= RECORDS_SEQUENCER_SAVE_SLOTS || length < RECORDS_SEQUENCER_COMMANDS_AT)
		return EINVAL;

	/* Succeeded: the value is in its slot. */
	nvrtx_put32(payload, RECORDS_SEQUENCER_SAVE_AT + (size_t)slot * 4U, value);
	return 0;
}

/*
 * Writes the allocation call for one resource-manager object.
 *
 * The call is the 32-byte header (client, parent, object, class, status,
 * parameter length, flags, four reserved bytes) followed by the class's
 * parameters.  Returns the call's length, or 0 when it does not fit.
 *
 * Procedure referenced: Linux v6.19
 * drivers/gpu/drm/nouveau/nvkm/subdev/gsp/rm/r535/alloc.c
 * (r535_gsp_rpc_rm_alloc_get).
 */
size_t
nvrtx_alloc_call(
	uint8_t *call,
	size_t capacity,
	const struct nvrtx_object *object,
	const uint8_t *parameters,
	size_t parameters_length)
{
	size_t at;

	/* Refuses a call that does not fit. */
	if (NVRTX_ALLOC_HEADER_BYTES + parameters_length > capacity)
		return 0U;

	/* Writes the header; the status stays zero for the resource manager to fill. */
	for (at = 0U; at < NVRTX_ALLOC_HEADER_BYTES; at++)
		call[at] = 0U;
	nvrtx_put32(call, 0U, object->client);
	nvrtx_put32(call, 4U, object->parent);
	nvrtx_put32(call, 8U, object->handle);
	nvrtx_put32(call, 12U, object->class_number);
	nvrtx_put32(call, NVRTX_ALLOC_STATUS_AT, 0U);
	nvrtx_put32(call, 20U, (uint32_t)parameters_length);

	/* Copies the parameters after the header. */
	for (at = 0U; at < parameters_length; at++)
		call[NVRTX_ALLOC_HEADER_BYTES + at] = parameters[at];

	/* Succeeded: the call's length. */
	return NVRTX_ALLOC_HEADER_BYTES + parameters_length;
}

/* Measures a zero-terminated name. */
static size_t
records_strlen(
	const char *text)
{
	size_t length;

	/* Counts the characters before the zero. */
	length = 0U;
	while (text[length] != '\0')
		length++;

	/* Succeeded: the length. */
	return length;
}

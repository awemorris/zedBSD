/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Minimal IEEE 1275 client interface used by the SPARC V9 loaders.
 */

#include "ofw.h"

/* The value OpenFirmware's client interface returns, and the handle value, for a failure. */
#define OFW_FAILED ((ofw_cell_t)-1)

/* The most arguments, and the most results, ofw_call_method() passes. */
#define OFW_METHOD_CELLS 6U

/*
 * The firmware's client interface entry.
 *
 * ofw_init() sets it once from the pointer the firmware passed; zero means
 * no firmware, and every call then fails.
 */
static ofw_client_t ofw_client;

/*
 * The instance handles of the console, the MMU and the memory node from
 * /chosen.
 *
 * ofw_init() fills them; OFW_FAILED means /chosen did not name one.
 */
static ofw_cell_t ofw_stdout;
static ofw_cell_t ofw_mmu;
static ofw_cell_t ofw_memory;

static ofw_cell_t ofw_call(ofw_cell_t *arguments);
static ofw_cell_t ofw_chosen_handle(ofw_scell_t chosen, const char *name);
static ofw_scell_t ofw_one_node(const char *service, ofw_cell_t node);
static void *ofw_claim_virtual(void *address, unsigned long size);
static int ofw_map(void *virtual_address, unsigned long long physical, unsigned long size, ofw_cell_t mode);

/* Starts the client interface and finds the console, the MMU and the memory node. */
void
ofw_init(
	ofw_client_t client)
{
	ofw_scell_t chosen;

	/* Starts with the entry and no handles. */
	ofw_client = client;
	ofw_stdout = OFW_FAILED;
	ofw_mmu = OFW_FAILED;
	ofw_memory = OFW_FAILED;

	/* A firmware without /chosen names no handles. */
	chosen = ofw_finddevice("/chosen");
	if (chosen < 0)
		return;

	/* Reads the three handles /chosen names. */
	ofw_stdout = ofw_chosen_handle(chosen, "stdout");
	ofw_mmu = ofw_chosen_handle(chosen, "mmu");
	ofw_memory = ofw_chosen_handle(chosen, "memory");
}

/* Finds a device tree node by its path. */
ofw_scell_t
ofw_finddevice(
	const char *path)
{
	ofw_cell_t arguments[5];
	ofw_cell_t status;

	/* Asks the firmware: one argument, one result. */
	arguments[0] = (ofw_cell_t)"finddevice";
	arguments[1] = 1;
	arguments[2] = 1;
	arguments[3] = (ofw_cell_t)path;
	status = ofw_call(arguments);
	if (status == OFW_FAILED)
		return -1;

	/* Reports the node, which the firmware gives as -1 when there is none. */
	return (ofw_scell_t)arguments[4];
}

/* Finds a node's first child. */
ofw_scell_t
ofw_child(
	ofw_cell_t node)
{
	ofw_scell_t child;

	/* Asks the firmware for the child. */
	child = ofw_one_node("child", node);

	/* Reports the child, or zero or -1 when there is none. */
	return child;
}

/* Finds a node's next sibling. */
ofw_scell_t
ofw_peer(
	ofw_cell_t node)
{
	ofw_scell_t peer;

	/* Asks the firmware for the sibling. */
	peer = ofw_one_node("peer", node);

	/* Reports the sibling, or zero or -1 when there is none. */
	return peer;
}

/* Finds a node's parent. */
ofw_scell_t
ofw_parent(
	ofw_cell_t node)
{
	ofw_scell_t parent;

	/* Asks the firmware for the parent. */
	parent = ofw_one_node("parent", node);

	/* Reports the parent, or zero or -1 when there is none. */
	return parent;
}

/* Reads a node's property into a buffer and reports its length. */
int
ofw_getprop(
	ofw_cell_t node,
	const char *name,
	void *buffer,
	unsigned long size)
{
	ofw_cell_t arguments[8];
	ofw_cell_t status;

	/* Asks the firmware: four arguments, one result. */
	arguments[0] = (ofw_cell_t)"getprop";
	arguments[1] = 4;
	arguments[2] = 1;
	arguments[3] = node;
	arguments[4] = (ofw_cell_t)name;
	arguments[5] = (ofw_cell_t)buffer;
	arguments[6] = size;
	status = ofw_call(arguments);
	if (status == OFW_FAILED)
		return -1;

	/* Reports the property's length, or -1 when the node has none of the name. */
	return (int)(ofw_scell_t)arguments[7];
}

/* Finds the package node of an instance handle. */
ofw_scell_t
ofw_instance_to_package(
	ofw_cell_t handle)
{
	ofw_scell_t package;

	/* Asks the firmware for the package. */
	package = ofw_one_node("instance-to-package", handle);

	/* Reports the package, or -1 when the handle names none. */
	return package;
}

/*
 * Calls a method of an open instance.
 *
 * The arguments and the results are top-of-stack first, as OpenBIOS gives
 * them.  Returns 0, or -1 when the call or the method fails.
 */
int
ofw_call_method(
	const char *method,
	ofw_cell_t handle,
	unsigned int argument_count,
	const ofw_cell_t *arguments,
	unsigned int result_count,
	ofw_cell_t *results)
{
	ofw_cell_t call[OFW_METHOD_CELLS * 2U + 6U];
	ofw_cell_t status;
	unsigned int index;

	/* Refuses more cells than the call array holds. */
	if (argument_count > OFW_METHOD_CELLS || result_count > OFW_METHOD_CELLS)
		return -1;

	/* Names the service, the method, the instance and the arguments. */
	call[0] = (ofw_cell_t)"call-method";
	call[1] = argument_count + 2U;
	call[2] = result_count + 1U;
	call[3] = (ofw_cell_t)method;
	call[4] = handle;

	/* Copies the arguments after the instance. */
	for (index = 0; index < argument_count; index++)
		call[5U + index] = arguments[index];

	/* Calls the method; the first result is its catch result. */
	status = ofw_call(call);
	if (status != 0)
		return -1;
	if (call[5U + argument_count] != 0)
		return -1;

	/* Copies the method's results. */
	for (index = 0; index < result_count; index++)
		results[index] = call[6U + argument_count + index];

	/* Succeeded: the method ran. */
	return 0;
}

/*
 * Finds the physical address behind a virtual one through the MMU's
 * "translate" method: ( virt -- false | phys.lo phys.hi mode true ).
 *
 * Returns 0, or -1 when there is no MMU or the address is not mapped.
 */
int
ofw_translate(
	unsigned long virtual_address,
	unsigned long long *physical)
{
	ofw_cell_t argument;
	ofw_cell_t results[4];
	int status;

	/* Refuses a firmware without an MMU handle. */
	if (ofw_mmu == OFW_FAILED)
		return -1;

	/* Asks the MMU; results are the flag, the mode and the two address cells. */
	argument = virtual_address;
	status = ofw_call_method("translate", ofw_mmu, 1, &argument, 4, results);
	if (status != 0)
		return -1;
	if (results[0] == 0)
		return -1;

	/* Combines the two address cells. */
	*physical = ((unsigned long long)(unsigned int)results[2] << 32) | (unsigned int)results[3];

	/* Succeeded: the address is mapped. */
	return 0;
}

/* Opens a device by its path. */
ofw_scell_t
ofw_open(
	const char *path)
{
	ofw_cell_t arguments[5];
	ofw_cell_t status;

	/* Asks the firmware: one argument, one result. */
	arguments[0] = (ofw_cell_t)"open";
	arguments[1] = 1;
	arguments[2] = 1;
	arguments[3] = (ofw_cell_t)path;
	status = ofw_call(arguments);
	if (status == OFW_FAILED)
		return -1;

	/* Reports the instance handle, which the firmware gives as zero on failure. */
	return (ofw_scell_t)arguments[4];
}

/* Closes a device. */
int
ofw_close(
	ofw_cell_t handle)
{
	ofw_cell_t arguments[4];
	ofw_cell_t status;

	/* Asks the firmware: one argument, no result. */
	arguments[0] = (ofw_cell_t)"close";
	arguments[1] = 1;
	arguments[2] = 0;
	arguments[3] = handle;
	status = ofw_call(arguments);
	if (status == OFW_FAILED)
		return -1;

	/* Succeeded: the device is closed. */
	return 0;
}

/* Reads from a device at its current position. */
long
ofw_read(
	ofw_cell_t handle,
	void *buffer,
	unsigned long size)
{
	ofw_cell_t arguments[7];
	ofw_cell_t status;

	/* Asks the firmware: three arguments, one result. */
	arguments[0] = (ofw_cell_t)"read";
	arguments[1] = 3;
	arguments[2] = 1;
	arguments[3] = handle;
	arguments[4] = (ofw_cell_t)buffer;
	arguments[5] = size;
	status = ofw_call(arguments);
	if (status == OFW_FAILED)
		return -1;

	/* Reports how many bytes were read. */
	return (long)(ofw_scell_t)arguments[6];
}

/* Moves a device's position to a byte offset. */
int
ofw_seek(
	ofw_cell_t handle,
	unsigned long long offset)
{
	ofw_cell_t arguments[7];
	ofw_cell_t status;

	/* Asks the firmware: the handle and the offset's high and low halves, one result. */
	arguments[0] = (ofw_cell_t)"seek";
	arguments[1] = 3;
	arguments[2] = 1;
	arguments[3] = handle;
	arguments[4] = (ofw_cell_t)(offset >> 32);
	arguments[5] = (ofw_cell_t)(unsigned int)offset;
	status = ofw_call(arguments);
	if (status == OFW_FAILED)
		return -1;

	/* Reports the firmware's result, negative on failure. */
	return (int)(ofw_scell_t)arguments[6];
}

/*
 * Claims virtual memory at an address with physical memory behind it.
 *
 * Returns the address, or (void *)-1 when any of the three steps fails.
 */
void *
ofw_claim(
	void *address,
	unsigned long size,
	unsigned long align)
{
	ofw_cell_t physical_arguments[10];
	ofw_cell_t status;
	unsigned long long physical;
	void *claimed;
	int mapped;

	/* Refuses a claim without the MMU and memory handles, or of nothing. */
	if (ofw_mmu == OFW_FAILED ||
	    ofw_memory == OFW_FAILED ||
	    size == 0)
		return (void *)-1;

	/* Claims the virtual range at the address. */
	claimed = ofw_claim_virtual(address, size);
	if (claimed == (void *)-1)
		return (void *)-1;

	/* Claims physical memory anywhere at the alignment. */
	physical_arguments[0] = (ofw_cell_t)"call-method";
	physical_arguments[1] = 4;
	physical_arguments[2] = 3;
	physical_arguments[3] = (ofw_cell_t)"claim";
	physical_arguments[4] = ofw_memory;
	physical_arguments[5] = align;
	physical_arguments[6] = size;
	status = ofw_call(physical_arguments);
	if (status != 0 || physical_arguments[7] != 0)
		return (void *)-1;

	/* Maps the virtual range onto the physical memory the firmware gave. */
	physical = ((unsigned long long)(unsigned int)physical_arguments[8] << 32) | (unsigned int)physical_arguments[9];
	mapped = ofw_map(claimed, physical, size, OFW_FAILED);
	if (!mapped)
		return (void *)-1;

	/* Succeeded: the address is backed by memory. */
	return claimed;
}

/*
 * Claims virtual memory at an address with a given physical range behind it.
 *
 * The range is mapped as one locked, cacheable, writable, privileged 4 MiB
 * page.  Returns the address, or (void *)-1 when any step fails.
 */
void *
ofw_claim_fixed(
	void *address,
	unsigned long long physical,
	unsigned long size)
{
	ofw_cell_t physical_arguments[12];
	ofw_cell_t status;
	unsigned long long result;
	void *claimed;
	int mapped;

	/* Refuses a claim without the MMU and memory handles, or of nothing. */
	if (ofw_mmu == OFW_FAILED ||
	    ofw_memory == OFW_FAILED ||
	    size == 0)
		return (void *)-1;

	/* Claims the virtual range at the address. */
	claimed = ofw_claim_virtual(address, size);
	if (claimed == (void *)-1)
		return (void *)-1;

	/* OpenBIOS call-method arguments and results are top-of-stack first. */
	physical_arguments[0] = (ofw_cell_t)"call-method";
	physical_arguments[1] = 6;
	physical_arguments[2] = 3;
	physical_arguments[3] = (ofw_cell_t)"claim";
	physical_arguments[4] = ofw_memory;
	physical_arguments[5] = 0;
	physical_arguments[6] = size;
	physical_arguments[7] = (ofw_cell_t)(physical >> 32);
	physical_arguments[8] = (ofw_cell_t)(unsigned int)physical;
	status = ofw_call(physical_arguments);
	if (status != 0 || physical_arguments[9] != 0)
		return (void *)-1;

	/* Refuses a firmware that gave other physical memory than asked. */
	result = ((unsigned long long)(unsigned int)physical_arguments[10] << 32) | (unsigned int)physical_arguments[11];
	if (result != physical)
		return (void *)-1;

	/* CP|CV|writable|privileged|locked: one locked 4 MiB sun4u TTE. */
	mapped = ofw_map(claimed, physical, size, 0x76);
	if (!mapped)
		return (void *)-1;

	/* Succeeded: the address is backed by the physical range. */
	return claimed;
}

/* Writes to a device. */
long
ofw_write(
	ofw_cell_t handle,
	const void *buffer,
	unsigned long size)
{
	ofw_cell_t arguments[7];
	ofw_cell_t status;

	/* Asks the firmware: three arguments, one result. */
	arguments[0] = (ofw_cell_t)"write";
	arguments[1] = 3;
	arguments[2] = 1;
	arguments[3] = handle;
	arguments[4] = (ofw_cell_t)buffer;
	arguments[5] = size;
	status = ofw_call(arguments);
	if (status == OFW_FAILED)
		return -1;

	/* Reports how many bytes were written. */
	return (long)(ofw_scell_t)arguments[6];
}

/* Writes a string to the firmware console. */
void
ofw_puts(
	const char *text)
{
	unsigned long length;

	/* A firmware without a console shows nothing. */
	if (ofw_stdout == OFW_FAILED)
		return;

	/* Measures the string. */
	length = 0;
	while (text[length] != '\0')
		length++;

	/* Writes it; a failed write has nowhere to be reported. */
	(void)ofw_write(ofw_stdout, text, length);
}

/* Reports the firmware console's instance handle. */
ofw_cell_t
ofw_stdout_handle(
	void)
{
	/* Reports the handle, or OFW_FAILED when there is no console. */
	return ofw_stdout;
}

/* Reads the boot path from /chosen into a buffer and terminates it. */
int
ofw_bootpath(
	char *buffer,
	unsigned long size)
{
	ofw_scell_t chosen;
	int length;

	/* Refuses a buffer too small for a character and the terminator. */
	if (size < 2)
		return -1;

	/* Refuses a firmware without /chosen. */
	chosen = ofw_finddevice("/chosen");
	if (chosen < 0)
		return -1;

	/* Refuses a missing or empty boot path, and one that fills the buffer. */
	length = ofw_getprop((ofw_cell_t)chosen, "bootpath", buffer, size - 1);
	if (length <= 0 || (unsigned long)length >= size)
		return -1;

	/* Terminates the path. */
	buffer[length] = '\0';

	/* Succeeded: reports the path's length. */
	return length;
}

/* Returns to the firmware's prompt. */
void
ofw_exit(
	void)
{
	ofw_cell_t arguments[3];

	/* Asks the firmware to exit: no argument, no result. */
	arguments[0] = (ofw_cell_t)"exit";
	arguments[1] = 0;
	arguments[2] = 0;
	(void)ofw_call(arguments);

	/* A firmware that comes back is never left. */
	for (;;)
		__asm__ volatile("nop");
}

/* Calls the firmware's client interface. */
static ofw_cell_t
ofw_call(
	ofw_cell_t *arguments)
{
	ofw_cell_t status;

	/* There is no firmware to call. */
	if (ofw_client == (ofw_client_t)0)
		return OFW_FAILED;

	/* Calls the firmware, which fills the results in the argument array. */
	status = ofw_client(arguments);

	/* Reports the firmware's status, OFW_FAILED on failure. */
	return status;
}

/* Reads one instance handle property of /chosen, or OFW_FAILED when it is not there. */
static ofw_cell_t
ofw_chosen_handle(
	ofw_scell_t chosen,
	const char *name)
{
	unsigned int handle;
	int length;

	/* Reads the 32-bit handle; anything but a whole one is no handle. */
	handle = 0;
	length = ofw_getprop((ofw_cell_t)chosen, name, &handle, sizeof(handle));
	if (length != (int)sizeof(handle))
		return OFW_FAILED;

	/* Succeeded: reports the handle. */
	return handle;
}

/* Asks the firmware for a node related to a node (child, peer or parent). */
static ofw_scell_t
ofw_one_node(
	const char *service,
	ofw_cell_t node)
{
	ofw_cell_t arguments[5];
	ofw_cell_t status;

	/* Asks the firmware: one argument, one result. */
	arguments[0] = (ofw_cell_t)service;
	arguments[1] = 1;
	arguments[2] = 1;
	arguments[3] = node;
	status = ofw_call(arguments);
	if (status == OFW_FAILED)
		return -1;

	/* Reports the related node. */
	return (ofw_scell_t)arguments[4];
}

/* Claims a virtual range at an address from the MMU, or reports (void *)-1. */
static void *
ofw_claim_virtual(
	void *address,
	unsigned long size)
{
	ofw_cell_t virtual_arguments[10];
	ofw_cell_t status;
	void *claimed;

	/* Asks the MMU for the range at the address. */
	virtual_arguments[0] = (ofw_cell_t)"call-method";
	virtual_arguments[1] = 5;
	virtual_arguments[2] = 2;
	virtual_arguments[3] = (ofw_cell_t)"claim";
	virtual_arguments[4] = ofw_mmu;
	virtual_arguments[5] = 0;
	virtual_arguments[6] = size;
	virtual_arguments[7] = (ofw_cell_t)address;
	status = ofw_call(virtual_arguments);
	if (status != 0 || virtual_arguments[8] != 0)
		return (void *)-1;

	/* Refuses a range the MMU put elsewhere. */
	claimed = (void *)virtual_arguments[9];
	if (claimed != address)
		return (void *)-1;

	/* Succeeded: the range at the address is claimed. */
	return claimed;
}

/* Maps a claimed virtual range onto physical memory with a mode. */
static int
ofw_map(
	void *virtual_address,
	unsigned long long physical,
	unsigned long size,
	ofw_cell_t mode)
{
	ofw_cell_t map_arguments[10];
	ofw_cell_t status;

	/* Asks the MMU to map the range. */
	map_arguments[0] = (ofw_cell_t)"call-method";
	map_arguments[1] = 7;
	map_arguments[2] = 0;
	map_arguments[3] = (ofw_cell_t)"map";
	map_arguments[4] = ofw_mmu;
	map_arguments[5] = mode;
	map_arguments[6] = size;
	map_arguments[7] = (ofw_cell_t)virtual_address;
	map_arguments[8] = (ofw_cell_t)(physical >> 32);
	map_arguments[9] = (ofw_cell_t)(unsigned int)physical;
	status = ofw_call(map_arguments);
	if (status != 0)
		return 0;

	/* Succeeded: the range is mapped. */
	return 1;
}

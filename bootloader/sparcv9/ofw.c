/*
 * Minimal IEEE 1275 client interface used by the SPARC V9 loaders.
 * Copyright (C) 2026 Awe Morris
 * SPDX-License-Identifier: Zlib
 */

#include "ofw.h"

static ofw_client_t ofw_client;
static ofw_cell_t ofw_stdout;
static ofw_cell_t ofw_mmu;
static ofw_cell_t ofw_memory;

static ofw_cell_t
ofw_call(ofw_cell_t *arguments)
{
	if (ofw_client == (ofw_client_t)0)
		return (ofw_cell_t)-1;
	return ofw_client(arguments);
}

void
ofw_init(ofw_client_t client)
{
	ofw_scell_t chosen;
	unsigned int handle;

	ofw_client = client;
	ofw_stdout = (ofw_cell_t)-1;
	ofw_mmu = (ofw_cell_t)-1;
	ofw_memory = (ofw_cell_t)-1;
	chosen = ofw_finddevice("/chosen");
	if (chosen < 0)
		return;
	handle = 0;
	if (ofw_getprop((ofw_cell_t)chosen, "stdout", &handle,
	    sizeof(handle)) == (int)sizeof(handle))
		ofw_stdout = handle;
	handle = 0;
	if (ofw_getprop((ofw_cell_t)chosen, "mmu", &handle,
	    sizeof(handle)) == (int)sizeof(handle))
		ofw_mmu = handle;
	handle = 0;
	if (ofw_getprop((ofw_cell_t)chosen, "memory", &handle,
	    sizeof(handle)) == (int)sizeof(handle))
		ofw_memory = handle;
}

ofw_scell_t
ofw_finddevice(const char *path)
{
	ofw_cell_t arguments[5];

	arguments[0] = (ofw_cell_t)"finddevice";
	arguments[1] = 1;
	arguments[2] = 1;
	arguments[3] = (ofw_cell_t)path;
	if (ofw_call(arguments) == (ofw_cell_t)-1)
		return -1;
	return (ofw_scell_t)arguments[4];
}

static ofw_scell_t
ofw_one_node(const char *service, ofw_cell_t node)
{
	ofw_cell_t arguments[5];

	arguments[0] = (ofw_cell_t)service;
	arguments[1] = 1;
	arguments[2] = 1;
	arguments[3] = node;
	if (ofw_call(arguments) == (ofw_cell_t)-1)
		return -1;
	return (ofw_scell_t)arguments[4];
}

ofw_scell_t
ofw_child(ofw_cell_t node)
{
	return ofw_one_node("child", node);
}

ofw_scell_t
ofw_peer(ofw_cell_t node)
{
	return ofw_one_node("peer", node);
}

ofw_scell_t
ofw_parent(ofw_cell_t node)
{
	return ofw_one_node("parent", node);
}

int
ofw_getprop(ofw_cell_t node, const char *name, void *buffer,
	    unsigned long size)
{
	ofw_cell_t arguments[8];

	arguments[0] = (ofw_cell_t)"getprop";
	arguments[1] = 4;
	arguments[2] = 1;
	arguments[3] = node;
	arguments[4] = (ofw_cell_t)name;
	arguments[5] = (ofw_cell_t)buffer;
	arguments[6] = size;
	if (ofw_call(arguments) == (ofw_cell_t)-1)
		return -1;
	return (int)(ofw_scell_t)arguments[7];
}

ofw_scell_t
ofw_open(const char *path)
{
	ofw_cell_t arguments[5];

	arguments[0] = (ofw_cell_t)"open";
	arguments[1] = 1;
	arguments[2] = 1;
	arguments[3] = (ofw_cell_t)path;
	if (ofw_call(arguments) == (ofw_cell_t)-1)
		return -1;
	return (ofw_scell_t)arguments[4];
}

int
ofw_close(ofw_cell_t handle)
{
	ofw_cell_t arguments[4];

	arguments[0] = (ofw_cell_t)"close";
	arguments[1] = 1;
	arguments[2] = 0;
	arguments[3] = handle;
	return ofw_call(arguments) == (ofw_cell_t)-1 ? -1 : 0;
}

long
ofw_read(ofw_cell_t handle, void *buffer, unsigned long size)
{
	ofw_cell_t arguments[7];

	arguments[0] = (ofw_cell_t)"read";
	arguments[1] = 3;
	arguments[2] = 1;
	arguments[3] = handle;
	arguments[4] = (ofw_cell_t)buffer;
	arguments[5] = size;
	if (ofw_call(arguments) == (ofw_cell_t)-1)
		return -1;
	return (long)(ofw_scell_t)arguments[6];
}

int
ofw_seek(ofw_cell_t handle, unsigned long long offset)
{
	ofw_cell_t arguments[7];

	arguments[0] = (ofw_cell_t)"seek";
	arguments[1] = 3;
	arguments[2] = 1;
	arguments[3] = handle;
	arguments[4] = (ofw_cell_t)(offset >> 32);
	arguments[5] = (ofw_cell_t)(unsigned int)offset;
	if (ofw_call(arguments) == (ofw_cell_t)-1)
		return -1;
	return (int)(ofw_scell_t)arguments[6];
}

void *
ofw_claim(void *address, unsigned long size, unsigned long align)
{
	ofw_cell_t virtual_arguments[10];
	ofw_cell_t physical_arguments[10];
	ofw_cell_t map_arguments[10];
	unsigned long long physical;
	void *claimed;

	if (ofw_mmu == (ofw_cell_t)-1 ||
	    ofw_memory == (ofw_cell_t)-1 || size == 0)
		return (void *)-1;
	virtual_arguments[0] = (ofw_cell_t)"call-method";
	virtual_arguments[1] = 5;
	virtual_arguments[2] = 2;
	virtual_arguments[3] = (ofw_cell_t)"claim";
	virtual_arguments[4] = ofw_mmu;
	virtual_arguments[5] = 0;
	virtual_arguments[6] = size;
	virtual_arguments[7] = (ofw_cell_t)address;
	if (ofw_call(virtual_arguments) != 0 || virtual_arguments[8] != 0)
		return (void *)-1;
	claimed = (void *)virtual_arguments[9];
	if (claimed != address)
		return (void *)-1;

	physical_arguments[0] = (ofw_cell_t)"call-method";
	physical_arguments[1] = 4;
	physical_arguments[2] = 3;
	physical_arguments[3] = (ofw_cell_t)"claim";
	physical_arguments[4] = ofw_memory;
	physical_arguments[5] = align;
	physical_arguments[6] = size;
	if (ofw_call(physical_arguments) != 0 ||
	    physical_arguments[7] != 0)
		return (void *)-1;
	physical = ((unsigned long long)(unsigned int)physical_arguments[8] <<
	    32) | (unsigned int)physical_arguments[9];

	map_arguments[0] = (ofw_cell_t)"call-method";
	map_arguments[1] = 7;
	map_arguments[2] = 0;
	map_arguments[3] = (ofw_cell_t)"map";
	map_arguments[4] = ofw_mmu;
	map_arguments[5] = (ofw_cell_t)-1;
	map_arguments[6] = size;
	map_arguments[7] = (ofw_cell_t)claimed;
	map_arguments[8] = (ofw_cell_t)(physical >> 32);
	map_arguments[9] = (ofw_cell_t)(unsigned int)physical;
	if (ofw_call(map_arguments) != 0)
		return (void *)-1;
	return claimed;
}

void *
ofw_claim_fixed(void *address, unsigned long long physical,
	    unsigned long size)
{
	ofw_cell_t virtual_arguments[10];
	ofw_cell_t physical_arguments[12];
	ofw_cell_t map_arguments[10];
	unsigned long long result;
	void *claimed;

	if (ofw_mmu == (ofw_cell_t)-1 ||
	    ofw_memory == (ofw_cell_t)-1 || size == 0)
		return (void *)-1;
	virtual_arguments[0] = (ofw_cell_t)"call-method";
	virtual_arguments[1] = 5;
	virtual_arguments[2] = 2;
	virtual_arguments[3] = (ofw_cell_t)"claim";
	virtual_arguments[4] = ofw_mmu;
	virtual_arguments[5] = 0;
	virtual_arguments[6] = size;
	virtual_arguments[7] = (ofw_cell_t)address;
	if (ofw_call(virtual_arguments) != 0 || virtual_arguments[8] != 0)
		return (void *)-1;
	claimed = (void *)virtual_arguments[9];
	if (claimed != address)
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
	if (ofw_call(physical_arguments) != 0 || physical_arguments[9] != 0)
		return (void *)-1;
	result = ((unsigned long long)(unsigned int)physical_arguments[10] <<
	    32) | (unsigned int)physical_arguments[11];
	if (result != physical)
		return (void *)-1;

	map_arguments[0] = (ofw_cell_t)"call-method";
	map_arguments[1] = 7;
	map_arguments[2] = 0;
	map_arguments[3] = (ofw_cell_t)"map";
	map_arguments[4] = ofw_mmu;
	/* CP|CV|writable|privileged|locked: one locked 4 MiB sun4u TTE. */
	map_arguments[5] = 0x76;
	map_arguments[6] = size;
	map_arguments[7] = (ofw_cell_t)claimed;
	map_arguments[8] = (ofw_cell_t)(physical >> 32);
	map_arguments[9] = (ofw_cell_t)(unsigned int)physical;
	if (ofw_call(map_arguments) != 0)
		return (void *)-1;
	return claimed;
}

long
ofw_write(ofw_cell_t handle, const void *buffer, unsigned long size)
{
	ofw_cell_t arguments[7];

	arguments[0] = (ofw_cell_t)"write";
	arguments[1] = 3;
	arguments[2] = 1;
	arguments[3] = handle;
	arguments[4] = (ofw_cell_t)buffer;
	arguments[5] = size;
	if (ofw_call(arguments) == (ofw_cell_t)-1)
		return -1;
	return (long)(ofw_scell_t)arguments[6];
}

void
ofw_puts(const char *text)
{
	unsigned long length;

	if (ofw_stdout == (ofw_cell_t)-1)
		return;
	length = 0;
	while (text[length] != '\0')
		length++;
	(void)ofw_write(ofw_stdout, text, length);
}

ofw_cell_t
ofw_stdout_handle(void)
{
	return ofw_stdout;
}

int
ofw_bootpath(char *buffer, unsigned long size)
{
	ofw_scell_t chosen;
	int length;

	if (size < 2)
		return -1;
	chosen = ofw_finddevice("/chosen");
	if (chosen < 0)
		return -1;
	length = ofw_getprop((ofw_cell_t)chosen, "bootpath", buffer, size - 1);
	if (length <= 0 || (unsigned long)length >= size)
		return -1;
	buffer[length] = '\0';
	return length;
}

void
ofw_exit(void)
{
	ofw_cell_t arguments[3];

	arguments[0] = (ofw_cell_t)"exit";
	arguments[1] = 0;
	arguments[2] = 0;
	(void)ofw_call(arguments);
	for (;;)
		__asm__ volatile("nop");
}

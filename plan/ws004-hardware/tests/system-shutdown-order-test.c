/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/system-device.h"

#include <assert.h>
#include <stdio.h>
#include <errno.h>

static unsigned sequence;
static unsigned net_sequence;
static unsigned usb_sequence;
static unsigned pci_sequence;
static unsigned net_calls;
static unsigned usb_calls;
static unsigned pci_calls;

static int begin_error, sync_error;
static unsigned begins, syncs, aborts, commits;
int writeback_shutdown_begin(void) { begins++; return begin_error; }
int mount_sync_all(void) { syncs++; if (!sync_error) ++sequence; return sync_error; }
void writeback_shutdown_finish(int committed)
{
 if (committed) { commits++; ++sequence; }
 else aborts++;
}

void
net_shutdown_for_boot(void)
{
	net_calls++;
	net_sequence = ++sequence;
}

void
drv_usb_shutdown(void)
{
	usb_calls++;
	usb_sequence = ++sequence;
}

void
drv_pci_shutdown(void)
{
	pci_calls++;
	pci_sequence = ++sequence;
}

void
sched_yield(void)
{
	assert(!"unexpected concurrent shutdown preparation wait");
}

int
main(void)
{
	begin_error = EIO;
	assert(system_shutdown_prepare() == EIO);
	assert(begins == 1 && syncs == 0 && aborts == 0 && commits == 0);
	assert(net_calls == 0 && usb_calls == 0 && pci_calls == 0);
	begin_error = 0; sync_error = EIO;
	assert(system_shutdown_prepare() == EIO);
	assert(begins == 2 && syncs == 1 && aborts == 1 && commits == 0);
	assert(net_calls == 0 && usb_calls == 0 && pci_calls == 0);
	sync_error = 0;
	assert(system_shutdown_prepare() == 0);
	assert(begins == 3 && syncs == 2 && aborts == 1 && commits == 1);
	assert(net_sequence == 3);
	assert(usb_sequence == 4);
	assert(pci_sequence == 5);
	assert(net_calls == 1 && usb_calls == 1 && pci_calls == 1);
	/* The common boundary is process-wide and idempotent. */
	assert(system_shutdown_prepare() == 0);
	assert(net_calls == 1 && usb_calls == 1 && pci_calls == 1);
	puts("system shutdown ordering: PASS");
	return 0;
}

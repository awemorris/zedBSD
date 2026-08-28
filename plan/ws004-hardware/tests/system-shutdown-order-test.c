/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "kern/system-device.h"

#include <assert.h>
#include <stdio.h>

static unsigned sequence;
static unsigned net_sequence;
static unsigned usb_sequence;
static unsigned pci_sequence;
static unsigned net_calls;
static unsigned usb_calls;
static unsigned pci_calls;

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
	system_shutdown_prepare();
	assert(net_sequence == 1);
	assert(usb_sequence == 2);
	assert(pci_sequence == 3);
	assert(net_calls == 1 && usb_calls == 1 && pci_calls == 1);
	/* The common boundary is process-wide and idempotent. */
	system_shutdown_prepare();
	assert(net_calls == 1 && usb_calls == 1 && pci_calls == 1);
	puts("system shutdown ordering: PASS");
	return 0;
}

/* SWAP-T007/T008: production /dev/system swap-boundary fixture. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <kern/swap-control.h>
#include <kern/system-swap-device.h>
#include <kern/uaccess.h>

#include <zedbsd/system.h>

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define BAD_ADDRESS UINTPTR_MAX

static unsigned add_calls;
static unsigned remove_calls;
static unsigned get_calls;
static int add_result;
static int remove_result;
static int get_result;
static int fail_copyout;
static uint32_t get_state = SWAP_SOURCE_STATE_ACTIVE;
static char last_selector[ZEDBSD_SYSTEM_SWAP_SOURCE_MAX];

int
copyin(uintptr_t source, void *destination, size_t size)
{
	if (source == BAD_ADDRESS)
		return EFAULT;
	memcpy(destination, (const void *)source, size);
	return 0;
}

int
copyout(const void *source, uintptr_t destination, size_t size)
{
	if (destination == BAD_ADDRESS || fail_copyout)
		return EFAULT;
	memcpy((void *)destination, source, size);
	return 0;
}

int
kern_swap_control_add(const char *selector)
{
	add_calls++;
	strncpy(last_selector, selector, sizeof(last_selector));
	last_selector[sizeof(last_selector) - 1U] = '\0';
	return add_result;
}

int
kern_swap_control_remove(const char *selector)
{
	remove_calls++;
	strncpy(last_selector, selector, sizeof(last_selector));
	last_selector[sizeof(last_selector) - 1U] = '\0';
	return remove_result;
}

int
kern_swap_control_get(unsigned source_id,
		      struct kern_swap_control_source_info *result)
{
	unsigned index;

	get_calls++;
	if (get_result != 0)
		return get_result;
	memset(result, 0, sizeof(*result));
	result->source_id = source_id;
	result->state = get_state;
	result->header_version = 2;
	result->total_pages = 8191;
	result->used_pages = 17;
	for (index = 0; index < sizeof(result->uuid); index++)
		result->uuid[index] = (uint8_t)(index + 1U);
	memcpy(result->label, "runtime-swap", sizeof("runtime-swap"));
	memcpy(result->source, "boot0:swapfile", sizeof("boot0:swapfile"));
	return 0;
}

static void
control_init(struct system_swap_control *control, const char *source)
{
	memset(control, 0, sizeof(*control));
	control->version = ZEDBSD_SYSTEM_SWAP_VERSION;
	control->struct_size = sizeof(*control);
	strncpy(control->source, source, sizeof(control->source));
}

static void
query_init(struct system_swap_source_info *query, unsigned source_id)
{
	memset(query, 0, sizeof(*query));
	query->version = ZEDBSD_SYSTEM_SWAP_VERSION;
	query->struct_size = sizeof(*query);
	query->source_id = source_id;
}

static void
test_control_validation(void)
{
	struct system_swap_control control;
	unsigned calls;

	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_SWAP_ADD, BAD_ADDRESS,
	    1) == EFAULT);
	control_init(&control, "boot0:swapfile");
	control.version++;
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_SWAP_ADD,
	    (uintptr_t)&control, 1) == EINVAL);
	control_init(&control, "boot0:swapfile");
	control.struct_size--;
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_SWAP_ADD,
	    (uintptr_t)&control, 1) == EINVAL);
	control_init(&control, "boot0:swapfile");
	control.flags = 1;
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_SWAP_ADD,
	    (uintptr_t)&control, 1) == EINVAL);
	control_init(&control, "boot0:swapfile");
	control.reserved0 = 1;
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_SWAP_ADD,
	    (uintptr_t)&control, 1) == EINVAL);
	control_init(&control, "boot0:swapfile");
	control.reserved[7] = 1;
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_SWAP_ADD,
	    (uintptr_t)&control, 1) == EINVAL);
	control_init(&control, "");
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_SWAP_ADD,
	    (uintptr_t)&control, 1) == EINVAL);
	control_init(&control, "x");
	memset(control.source, 'x', sizeof(control.source));
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_SWAP_ADD,
	    (uintptr_t)&control, 1) == EINVAL);
	assert(add_calls == 0);

	control_init(&control, "boot0:swapfile");
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_SWAP_ADD,
	    (uintptr_t)&control, 0) == EPERM);
	assert(add_calls == 0);
	add_result = EEXIST;
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_SWAP_ADD,
	    (uintptr_t)&control, 1) == EEXIST);
	assert(add_calls == 1);
	assert(strcmp(last_selector, "boot0:swapfile") == 0);
	add_result = 0;
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_SWAP_ADD,
	    (uintptr_t)&control, 1) == 0);
	assert(add_calls == 2);

	control_init(&control, "/dev/sda2");
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_SWAP_REMOVE,
	    (uintptr_t)&control, 0) == EPERM);
	assert(remove_calls == 0);
	remove_result = EINTR;
	calls = remove_calls;
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_SWAP_REMOVE,
	    (uintptr_t)&control, 1) == EINTR);
	assert(remove_calls == calls + 1U);
	assert(strcmp(last_selector, "/dev/sda2") == 0);
	assert(system_swap_device_ioctl(0, (uintptr_t)&control, 1) ==
	    EOPNOTSUPP);
}

static void
test_query_validation(void)
{
	struct system_swap_source_info query;
	struct system_swap_source_info before;
	unsigned index;

	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_GET_SWAP_SOURCE,
	    BAD_ADDRESS, 0) == EFAULT);
	query_init(&query, 0);
	query.version++;
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_GET_SWAP_SOURCE,
	    (uintptr_t)&query, 0) == EINVAL);
	query_init(&query, 0);
	query.struct_size--;
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_GET_SWAP_SOURCE,
	    (uintptr_t)&query, 0) == EINVAL);
	query_init(&query, 0);
	query.flags = 1;
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_GET_SWAP_SOURCE,
	    (uintptr_t)&query, 0) == EINVAL);
	query_init(&query, ZEDBSD_SYSTEM_SWAP_SOURCE_COUNT);
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_GET_SWAP_SOURCE,
	    (uintptr_t)&query, 0) == EINVAL);
	query_init(&query, 0);
	query.reserved[0] = 1;
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_GET_SWAP_SOURCE,
	    (uintptr_t)&query, 0) == EINVAL);
	assert(get_calls == 0);

	query_init(&query, 3);
	memset(query.label, 0x5a, sizeof(query.label));
	memset(query.source, 0x5a, sizeof(query.source));
	get_result = EINTR;
	before = query;
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_GET_SWAP_SOURCE,
	    (uintptr_t)&query, 0) == EINTR);
	assert(memcmp(&query, &before, sizeof(query)) == 0);
	get_result = 0;
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_GET_SWAP_SOURCE,
	    (uintptr_t)&query, 0) == 0);
	assert(query.version == ZEDBSD_SYSTEM_SWAP_VERSION);
	assert(query.struct_size == sizeof(query));
	assert(query.flags == 0 && query.source_id == 3);
	assert(query.state == ZEDBSD_SYSTEM_SWAP_STATE_ACTIVE);
	assert(query.header_version == 2 && query.total_pages == 8191 &&
	    query.used_pages == 17);
	for (index = 0; index < sizeof(query.uuid); index++)
		assert(query.uuid[index] == index + 1U);
	assert(strcmp(query.label, "runtime-swap") == 0);
	assert(strcmp(query.source, "boot0:swapfile") == 0);
	for (index = 0; index < sizeof(query.reserved) /
	    sizeof(query.reserved[0]); index++)
		assert(query.reserved[index] == 0);

	get_state = SWAP_SOURCE_STATE_PREPARED;
	query_init(&query, 0);
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_GET_SWAP_SOURCE,
	    (uintptr_t)&query, 0) == 0);
	assert(query.state == ZEDBSD_SYSTEM_SWAP_STATE_INACTIVE);
	get_state = SWAP_SOURCE_STATE_DRAINING;
	query_init(&query, 0);
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_GET_SWAP_SOURCE,
	    (uintptr_t)&query, 0) == 0);
	assert(query.state == ZEDBSD_SYSTEM_SWAP_STATE_DRAINING);
	get_state = SWAP_SOURCE_STATE_REMOVING;
	query_init(&query, 0);
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_GET_SWAP_SOURCE,
	    (uintptr_t)&query, 0) == 0);
	assert(query.state == ZEDBSD_SYSTEM_SWAP_STATE_DRAINING);
	get_state = UINT32_MAX;
	query_init(&query, 0);
	before = query;
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_GET_SWAP_SOURCE,
	    (uintptr_t)&query, 0) == EIO);
	assert(memcmp(&query, &before, sizeof(query)) == 0);

	get_state = SWAP_SOURCE_STATE_ACTIVE;
	query_init(&query, 0);
	fail_copyout = 1;
	assert(system_swap_device_ioctl(ZEDBSD_SYSTEM_GET_SWAP_SOURCE,
	    (uintptr_t)&query, 0) == EFAULT);
	fail_copyout = 0;
}

int
main(void)
{
	test_control_validation();
	test_query_validation();
	puts("SWAP-T007/T008: /dev/system swap validation: PASS");
	return 0;
}

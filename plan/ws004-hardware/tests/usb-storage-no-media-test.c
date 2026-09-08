/* USB-storage empty-removable production-path fixture. SPDX-License-Identifier: Zlib */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int tid_t; /* Host libc has no zedBSD kernel thread identifier. */
#include "../../../src/drivers/usb/usb-storage.c"

static unsigned checks;

#define CHECK(expression) do { \
	checks++; \
	if (!(expression)) { \
		fprintf(stderr, "check %u failed at %s:%d: %s\n", checks, \
		    __FILE__, __LINE__, #expression); \
		exit(1); \
	} \
} while (0)

struct drv_usb_device {
	unsigned unused;
};

struct drv_usb_endpoint {
	uint8_t direction;
};

struct drv_usb_interface {
	struct drv_usb_device *device;
	struct drv_usb_endpoint *bulk_in;
	struct drv_usb_endpoint *bulk_out;
	void *driver_data;
};

struct drv_usb_urb {
	struct drv_usb_device *device;
	struct drv_usb_endpoint *endpoint;
	struct drv_usb_control_request control;
	void *buffer;
	size_t length;
	size_t actual;
};

static struct drv_usb_device fixture_device;
static int (*transfer_override)(struct drv_usb_urb *);
static int bio_error;
static size_t bio_bytes;
static unsigned device_disconnected;
enum drv_usb_device_state
drv_usb_device_state(const struct drv_usb_device *device)
{
	(void)device;
	return device_disconnected ? DRV_USB_STATE_NOT_ATTACHED :
	    DRV_USB_STATE_CONFIGURED;
}

int
drv_usb_urb_reserve_sync(struct drv_usb_urb *urb, size_t capacity)
{
	(void)urb;
	(void)capacity;
	return 0;
}
#ifndef USB_STORAGE_CUSTOM_HOST
unsigned drv_usb_device_hcd_capabilities(const struct drv_usb_device *device)
{ (void)device; return 0; }
int drv_usb_urb_reserve_transfer(struct drv_usb_urb *urb, size_t capacity)
{ (void)urb; (void)capacity; return EOPNOTSUPP; }
#endif
static struct drv_usb_endpoint fixture_bulk_in = { DRV_USB_DIR_IN };
static struct drv_usb_endpoint fixture_bulk_out = { DRV_USB_DIR_OUT };
static unsigned live_allocations;
static unsigned live_urbs;
static unsigned disk_calls;
static unsigned ready_commands;
static unsigned sense_commands;
static uint8_t active_opcode;
static uint8_t active_tag[4];
static int inquiry_removable;
static uint8_t sense_response;
static char diagnostic[4096];
static size_t diagnostic_length;

static void
fixture_reset(struct drv_usb_interface *interface, int removable,
	uint8_t response)
{
	memset(interface, 0, sizeof(*interface));
	interface->device = &fixture_device;
	interface->bulk_in = &fixture_bulk_in;
	interface->bulk_out = &fixture_bulk_out;
	live_allocations = 0;
	live_urbs = 0;
	disk_calls = 0;
	ready_commands = 0;
	sense_commands = 0;
	active_opcode = 0xffU;
	memset(active_tag, 0, sizeof(active_tag));
	inquiry_removable = removable;
	sense_response = response;
	diagnostic_length = 0;
	diagnostic[0] = '\0';
}

static unsigned
diagnostic_occurrences(const char *needle)
{
	const char *at = diagnostic;
	unsigned count = 0;

	while ((at = strstr(at, needle)) != NULL) {
		count++;
		at += strlen(needle);
	}
	return count;
}

void *
hal_malloc(size_t size)
{
	void *pointer = malloc(size);

	if (pointer != NULL)
		live_allocations++;
	return pointer;
}

void
hal_free(void *pointer)
{
	if (pointer == NULL)
		return;
	CHECK(live_allocations != 0);
	live_allocations--;
	free(pointer);
}

int
hal_printf(const char *format, ...)
{
	va_list arguments;
	int written;

	if (diagnostic_length >= sizeof(diagnostic))
		return 0;
	va_start(arguments, format);
	written = vsnprintf(diagnostic + diagnostic_length,
	    sizeof(diagnostic) - diagnostic_length, format, arguments);
	va_end(arguments);
	if (written > 0) {
		size_t amount = (size_t)written;

		if (amount >= sizeof(diagnostic) - diagnostic_length)
			diagnostic_length = sizeof(diagnostic) - 1U;
		else
			diagnostic_length += amount;
	}
	return written;
}

uint64_t
sched_ticks(void)
{
	static uint64_t ticks;

	return ++ticks;
}

void
sched_yield(void)
{
}

/* Controlled scheduler boundary; native tests execute the real worker loop. */
static struct thread control_thread;
static unsigned control_live;
static int control_create_error, control_join_error;
static unsigned control_hold_stop, control_run_loop;
int kthread_create(void (*entry)(void *),void *argument,int priority,struct thread **out)
{
	(void)priority;CHECK(!control_live);
	if(control_create_error)return control_create_error;
	memset(&control_thread,0,sizeof(control_thread));
	control_thread.kernel_entry=entry;control_thread.kernel_arg=argument;
	control_thread.task=(hal_task_t)&control_thread;control_thread.state=THREAD_NEW;
	control_live=1;*out=&control_thread;return 0;
}
void thread_start(struct thread *thread)
{ CHECK(thread==&control_thread && control_live);thread->state=THREAD_SLEEPING; }
void kernel_notify_task(hal_task_t task)
{
	struct usb_storage *storage=control_thread.kernel_arg;
	CHECK(task==(hal_task_t)&control_thread && control_live);
	if(storage->control_stopping && !control_hold_stop)control_thread.state=THREAD_ZOMBIE;
}
void kernel_wait_task(void)
{ struct usb_storage *s=control_thread.kernel_arg;CHECK(control_run_loop);s->control_ready=1; }
void sched_sleep(uint64_t deadline)
{ struct usb_storage *s=control_thread.kernel_arg;(void)deadline;CHECK(control_run_loop && !s->lock.locked && !s->control_lock.locked);s->control_stopping=1; }
int thread_wait(struct thread *thread,int *status)
{
	(void)status;CHECK(thread==&control_thread && control_live);
	if(control_join_error)return control_join_error;
	CHECK(thread->state==THREAD_ZOMBIE);thread->state=THREAD_DEAD;control_live=0;return 0;
}

int
mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name)
{
	(void)rank;
	(void)name;
	memset(mutex, 0, sizeof(*mutex));
	return 0;
}

void
mutex_lock(struct mutex *mutex)
{
	CHECK(mutex->locked == 0);
	mutex->locked = 1;
}

void
mutex_unlock(struct mutex *mutex)
{
	CHECK(mutex->locked == 1);
	mutex->locked = 0;
}

struct drv_usb_device *
drv_usb_interface_device(const struct drv_usb_interface *interface)
{
	return interface->device;
}

unsigned
drv_usb_interface_number(const struct drv_usb_interface *interface)
{
	(void)interface;
	return 0;
}

struct drv_usb_endpoint *
drv_usb_interface_find_endpoint(struct drv_usb_interface *interface,
	enum drv_usb_transfer_type type, uint8_t direction,
	struct drv_usb_endpoint *after)
{
	if (type != DRV_USB_TRANSFER_BULK || after != NULL)
		return NULL;
	return direction == DRV_USB_DIR_IN ? interface->bulk_in :
	    direction == DRV_USB_DIR_OUT ? interface->bulk_out : NULL;
}

void *
drv_usb_interface_driver_data(const struct drv_usb_interface *interface)
{
	return interface->driver_data;
}

int
drv_usb_interface_set_driver_data(struct drv_usb_interface *interface,
	void *data)
{
	interface->driver_data = data;
	return 0;
}

struct drv_usb_urb *
drv_usb_urb_alloc(struct drv_usb_device *device,
	struct drv_usb_endpoint *endpoint, unsigned iso_count)
{
	struct drv_usb_urb *urb;

	(void)iso_count;
	urb = calloc(1, sizeof(*urb));
	if (urb != NULL) {
		urb->device = device;
		urb->endpoint = endpoint;
		live_urbs++;
	}
	return urb;
}

void
drv_usb_urb_free(struct drv_usb_urb *urb)
{
	if (urb == NULL)
		return;
	CHECK(live_urbs != 0);
	live_urbs--;
	free(urb);
}

int
drv_usb_urb_setup(struct drv_usb_urb *urb, void *buffer, size_t length,
	unsigned flags, unsigned timeout, drv_usb_urb_callback_t callback,
	void *argument)
{
	(void)flags;
	(void)timeout;
	(void)callback;
	(void)argument;
	urb->buffer = buffer;
	urb->length = length;
	urb->actual = 0;
	return 0;
}

int
drv_usb_urb_setup_control_flags(struct drv_usb_urb *urb,
	const struct drv_usb_control_request *request, void *buffer,
	size_t length, unsigned flags, unsigned timeout,
	drv_usb_urb_callback_t callback, void *argument)
{
	urb->control = *request;
	return drv_usb_urb_setup(urb, buffer, length, flags, timeout, callback,
	    argument);
}

static void
fixture_complete_csw(struct drv_usb_urb *urb)
{
	uint8_t *csw = urb->buffer;

	CHECK(urb->length == 13U);
	memset(csw, 0, urb->length);
	csw[0] = 0x55U;
	csw[1] = 0x53U;
	csw[2] = 0x42U;
	csw[3] = 0x53U;
	memcpy(csw + 4, active_tag, sizeof(active_tag));
	csw[12] = active_opcode == SCSI_TEST_UNIT_READY ? 1U : 0U;
	urb->actual = urb->length;
}

int
drv_usb_urb_submit(struct drv_usb_urb *urb)
{
	uint8_t *bytes = urb->buffer;

	if (transfer_override != NULL)
		return transfer_override(urb);

	if (urb->endpoint == NULL) {
		CHECK(urb->control.request == USB_MASS_STORAGE_GET_MAX_LUN);
		CHECK(urb->length == 1U);
		bytes[0] = 0;
		urb->actual = 1U;
		return 0;
	}
	if (urb->endpoint == &fixture_bulk_out) {
		CHECK(urb->length == sizeof(struct bot_cbw));
		CHECK(bytes[0] == 0x55U && bytes[1] == 0x53U &&
		    bytes[2] == 0x42U && bytes[3] == 0x43U);
		memcpy(active_tag, bytes + 4, sizeof(active_tag));
		active_opcode = bytes[15];
		if (active_opcode == SCSI_TEST_UNIT_READY)
			ready_commands++;
		else if (active_opcode == SCSI_REQUEST_SENSE)
			sense_commands++;
		urb->actual = urb->length;
		return 0;
	}
	CHECK(urb->endpoint == &fixture_bulk_in);
	if (urb->length == sizeof(struct bot_csw)) {
		fixture_complete_csw(urb);
		return 0;
	}
	memset(bytes, 0, urb->length);
	if (active_opcode == SCSI_INQUIRY) {
		CHECK(urb->length == 36U);
		bytes[0] = 0;
		bytes[1] = inquiry_removable != 0 ? 0x80U : 0;
	} else {
		CHECK(active_opcode == SCSI_REQUEST_SENSE);
		CHECK(urb->length == 18U);
		bytes[0] = sense_response;
		bytes[2] = 0x02U;
		bytes[7] = 10U;
		bytes[12] = 0x3aU;
		bytes[13] = 0x00U;
	}
	urb->actual = urb->length;
	return 0;
}

int
drv_usb_urb_wait_reusable(struct drv_usb_urb *urb)
{
	(void)urb;
	return 0;
}

size_t
drv_usb_urb_actual_length(const struct drv_usb_urb *urb)
{
	return urb->actual;
}

int
drv_usb_endpoint_clear_halt(struct drv_usb_endpoint *endpoint)
{
	(void)endpoint;
	return 0;
}

int
drv_usb_driver_register(struct drv_usb_driver *driver)
{
	(void)driver;
	return 0;
}

#ifndef USB_STORAGE_CUSTOM_HOST
struct disk *
disk_alloc(void)
{
	disk_calls++;
	return NULL;
}

int
disk_alloc_sd_name(struct disk *disk)
{
	(void)disk;
	disk_calls++;
	return EIO;
}

int
disk_create(struct disk *disk)
{
	(void)disk;
	disk_calls++;
	return EIO;
}

int
disk_gone_if_idle(struct disk *disk)
{
	(void)disk;
	disk_calls++;
	return EIO;
}


int
disk_destroy(struct disk *disk)
{
	(void)disk;
	disk_calls++;
	return EIO;
}

#endif
static unsigned persistence_invalidations;
static struct disk *persistence_disk;
void disk_persistence_invalidate(struct disk *disk)
{ persistence_invalidations++; persistence_disk=disk; }
void disk_persistence_forget(struct disk *disk)
{ persistence_invalidations++; persistence_disk=disk; }
void disk_media_revoke(struct disk *disk)
{ CHECK(disk!=NULL);disk->d_media_revoked=1; }
int disk_media_status(const struct disk *disk)
{ return disk==NULL || disk->d_media_revoked ? ENXIO : 0; }
#ifndef USB_STORAGE_CUSTOM_HOST
int disk_media_retire(struct disk *disk) { (void)disk;return EBUSY; }
#endif
int partition_retire_media(struct disk *disk) { return disk_media_retire(disk); }
static unsigned partition_reload_calls;
static int partition_reload_error;
int partition_reload(struct disk *disk)
{
	struct usb_storage *storage = disk->d_data;
	CHECK(storage->lock.locked == 0);
	partition_reload_calls++;
	return partition_reload_error;
}
void
bio_complete(struct bio *bio, int error, size_t transferred)
{
	(void)bio;
	bio_error = error;
	bio_bytes = transferred;
}

static void
test_current_no_medium_binds_idle(void)
{
	struct drv_usb_interface interface;
	int error;

	fixture_reset(&interface, 1, 0x70U);
	error = storage_attach(&interface, &storage_ids[0]);
	CHECK(error == 0);
	CHECK(interface.driver_data != NULL);
	CHECK(live_allocations == 1U);
	CHECK(live_urbs == 3U);
	CHECK(disk_calls == 0U);
	CHECK(ready_commands == 1U);
	CHECK(sense_commands == 1U);
	CHECK(diagnostic_occurrences("BOT check-condition") == 0U);
	CHECK(diagnostic_occurrences("has no medium; reader attached without a disk")
	    == 1U);
	CHECK(diagnostic_occurrences("attach-failed") == 0U);

	error = storage_detach(&interface, 0);
	CHECK(error == 0);
	CHECK(live_allocations == 0U);
	CHECK(live_urbs == 0U);
	CHECK(disk_calls == 0U);
}

static void
test_deferred_no_medium_is_not_current_state(void)
{
	struct drv_usb_interface interface;
	int error;

	fixture_reset(&interface, 1, 0x71U);
	error = storage_attach(&interface, &storage_ids[0]);
	CHECK(error == EIO);
	CHECK(interface.driver_data == NULL);
	CHECK(live_allocations == 0U);
	CHECK(live_urbs == 0U);
	CHECK(disk_calls == 0U);
	CHECK(ready_commands == 3U);
	CHECK(sense_commands == 3U);
	CHECK(diagnostic_occurrences("has no medium") == 0U);
	CHECK(diagnostic_occurrences("sense=02/3a/00") == 1U);
}

static void
test_fixed_disk_no_medium_is_not_an_idle_reader(void)
{
	struct drv_usb_interface interface;
	int error;

	fixture_reset(&interface, 0, 0x70U);
	error = storage_attach(&interface, &storage_ids[0]);
	CHECK(error == EIO);
	CHECK(interface.driver_data == NULL);
	CHECK(live_allocations == 0U);
	CHECK(live_urbs == 0U);
	CHECK(disk_calls == 0U);
	CHECK(ready_commands == 3U);
	CHECK(sense_commands == 3U);
	CHECK(diagnostic_occurrences("has no medium") == 0U);
	CHECK(diagnostic_occurrences("sense=02/3a/00") == 1U);
}

int
main(void)
{
	test_current_no_medium_binds_idle();
	test_deferred_no_medium_is_not_current_state();
	test_fixed_disk_no_medium_is_not_an_idle_reader();
	printf("USB storage no-medium production path: PASS (%u checks)\n",
	    checks);
	return 0;
}

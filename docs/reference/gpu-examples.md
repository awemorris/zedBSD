# GPU interface examples

Status: experimental reference for GPU ABI version 1.

This file holds four complete C programs that call the requests of `/dev/gpuN` directly. They show the calling conventions of [the device interface](gpu.md): the `version` and `size` fields, the `-1` and `errno` convention, a capability check before a request, the handling of the errors that a program has to expect, and the order in which objects are made and released. A real program does not write command streams or manage leases by hand; it reaches the GPU through Vulkan in libvulkan. Each program is a fenced C block whose first line is `/* example: NAME */`. Each one is plain C11 that builds without warnings (`-Wall -Wextra`) for every target that has a GPU driver, and each asserts with `_Static_assert` the size of every structure it uses; those sizes are the `Bytes` values of the structure tables in the other documents. The programs include the headers as `<uapi/gpu.h>` and its siblings, and they must be built with a compiler that targets zedBSD ([the device interface](gpu.md) explains why).

## Opening a node and reading its capabilities

The program opens a node read-write, which is the open mode that lets it make any request, and asks `GPU_GET_INFO` for the driver name, the limits and the capability bits ([capabilities](gpu-capabilities.md)). It prints each bit by name and ignores the bits it does not know. Then it asks `GPU_DEVICE_QUERY` who the node is ([memory](gpu-memory.md)); that request needs no capability bit. Last it makes a display request, `GPU_DISPLAY_QUERY` with `GPU_DISPLAY_COUNT_ONLY`, but only after it has seen `GPU_CAP_DISPLAY` in the bits: a request whose bit is clear would fail with `EOPNOTSUPP`.

Every argument is zeroed first, so that the output fields and the reserved fields are zero on input, and then `version` is set to `GPU_ABI_VERSION` and `size` to the size of the structure. A request that fails returns `-1` and leaves the reason in `errno`.

```c
/* example: info */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <uapi/gpu.h>
#include <uapi/gpu-allocation.h>
#include <uapi/gpu-display.h>
#include <uapi/gpu-fence.h>
#include <uapi/gpu-job.h>
#include <uapi/gpu-scanout.h>

_Static_assert(sizeof(struct gpu_info) == 56, "struct gpu_info");
_Static_assert(sizeof(struct gpu_device_info) == 40, "struct gpu_device_info");
_Static_assert(sizeof(struct gpu_display_info) == 152, "struct gpu_display_info");

struct capability {
	uint32_t bit;
	const char *name;
};

static const struct capability capabilities[] = {
	{ GPU_CAP_RESOURCE, "GPU_CAP_RESOURCE" },
	{ GPU_CAP_CAPSET, "GPU_CAP_CAPSET" },
	{ GPU_CAP_BLOB, "GPU_CAP_BLOB" },
	{ GPU_CAP_TRANSFER, "GPU_CAP_TRANSFER" },
	{ GPU_CAP_COMMAND, "GPU_CAP_COMMAND" },
	{ GPU_CAP_PRESENT, "GPU_CAP_PRESENT" },
	{ GPU_CAP_MAPPING, "GPU_CAP_MAPPING" },
	{ GPU_CAP_DISPLAY, "GPU_CAP_DISPLAY" },
	{ GPU_CAP_SHARE, "GPU_CAP_SHARE" },
	{ GPU_CAP_NOTIFICATION, "GPU_CAP_NOTIFICATION" },
	{ GPU_CAP_ALLOCATION_SHARE, "GPU_CAP_ALLOCATION_SHARE" },
	{ GPU_CAP_FENCE, "GPU_CAP_FENCE" },
	{ GPU_CAP_DISPLAY_EVENTS, "GPU_CAP_DISPLAY_EVENTS" },
	{ GPU_CAP_JOB, "GPU_CAP_JOB" },
	{ GPU_CAP_JOB_CAPACITY, "GPU_CAP_JOB_CAPACITY" },
	{ GPU_CAP_DISPLAY_CONTROL, "GPU_CAP_DISPLAY_CONTROL" },
	{ GPU_CAP_HOST_BLOB, "GPU_CAP_HOST_BLOB" },
};

int
main(int argc, char **argv)
{
	const char *node = "/dev/gpu0";
	struct gpu_info info;
	struct gpu_device_info device;
	struct gpu_display_info display;
	uint32_t known = 0U;
	size_t index;
	int fd;

	if (argc > 1)
		node = argv[1];

	/* A read-write open can make every request that needs either mode. */
	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", node, strerror(errno));
		return 1;
	}

	/* Every request starts with the ABI version and the size of its structure. */
	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	if (ioctl(fd, GPU_GET_INFO, &info) != 0) {
		/* A failed request returns -1 and leaves the reason in errno. */
		fprintf(stderr, "GPU_GET_INFO: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	printf("driver %s, ABI version %u\n", info.driver_name, (unsigned)info.version);
	if (info.max_resources == UINT32_MAX)
		printf("resources per open: no count limit\n");
	else
		printf("resources per open: %u\n", (unsigned)info.max_resources);
	printf("bytes per resource: %ju\n", (uintmax_t)info.max_resource_bytes);

	for (index = 0; index < sizeof(capabilities) / sizeof(capabilities[0]); index++) {
		known |= capabilities[index].bit;
		if ((info.capabilities & capabilities[index].bit) != 0U)
			printf("capability %s\n", capabilities[index].name);
	}

	/* Bits this program does not know are left alone. */
	if ((info.capabilities & ~known) != 0U)
		printf("other capability bits 0x%x\n", (unsigned)(info.capabilities & ~known));

	/* The identity request needs no capability bit; every output field is zero on input. */
	memset(&device, 0, sizeof(device));
	device.version = GPU_ABI_VERSION;
	device.size = sizeof(device);
	if (ioctl(fd, GPU_DEVICE_QUERY, &device) != 0) {
		fprintf(stderr, "GPU_DEVICE_QUERY: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	printf("device %ju, companion %ju, roles%s%s\n", (uintmax_t)device.device_id,
	    (uintmax_t)device.companion_id,
	    (device.roles & GPU_DEVICE_RENDER) != 0U ? " render" : "",
	    (device.roles & GPU_DEVICE_DISPLAY) != 0U ? " display" : "");

	/* A display request is made only on a node that offers the display bit. */
	if ((info.capabilities & GPU_CAP_DISPLAY) != 0U) {
		memset(&display, 0, sizeof(display));
		display.version = GPU_ABI_VERSION;
		display.size = sizeof(display);
		display.index = GPU_DISPLAY_COUNT_ONLY;
		if (ioctl(fd, GPU_DISPLAY_QUERY, &display) != 0)
			fprintf(stderr, "GPU_DISPLAY_QUERY: %s\n", strerror(errno));
		else
			printf("outputs: %u\n", (unsigned)display.count);
	} else {
		printf("no display outputs\n");
	}

	close(fd);
	return 0;
}
```

## Allocating, writing and reading back storage

A storage resource is plain memory that belongs to the open. `GPU_RESOURCE_CREATE` returns a handle, `GPU_RESOURCE_WRITE` copies bytes from the program into the resource, `GPU_RESOURCE_READ` copies them back, and `GPU_RESOURCE_DESTROY` releases the handle. One transfer moves at most `GPU_COPY_MAX` bytes and carries the program's buffer as a 64-bit address. The first part of the program runs only when the node offers `GPU_CAP_RESOURCE` and `GPU_CAP_TRANSFER` ([memory](gpu-memory.md)).

The second part runs only when the node offers `GPU_CAP_BLOB` and `GPU_CAP_MAPPING`. It makes a blob with `GPU_BLOB_MAPPABLE`, asks `GPU_RESOURCE_MAP` for the token of the mapping, and calls `mmap` on the same descriptor with that token as the file offset. The request returns a token and not an address, and the mapping must be `MAP_SHARED`. The mapping is removed with `munmap` before the blob is destroyed, because `GPU_RESOURCE_DESTROY` fails with `EBUSY` while a mapping exists. The program compares the size of its area with `max_resource_bytes` from `GPU_GET_INFO` before it allocates.

```c
/* example: storage */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <uapi/gpu.h>

_Static_assert(sizeof(struct gpu_info) == 56, "struct gpu_info");
_Static_assert(sizeof(struct gpu_resource_create) == 32, "struct gpu_resource_create");
_Static_assert(sizeof(struct gpu_resource_destroy) == 16, "struct gpu_resource_destroy");
_Static_assert(sizeof(struct gpu_transfer) == 40, "struct gpu_transfer");
_Static_assert(sizeof(struct gpu_blob_create) == 40, "struct gpu_blob_create");
_Static_assert(sizeof(struct gpu_resource_map) == 32, "struct gpu_resource_map");

#define AREA_BYTES 4096U
_Static_assert(AREA_BYTES <= GPU_COPY_MAX, "one transfer moves the whole area");

/* Calls one request and reports the errno of a failure. */
static int
request(int fd, unsigned long number, void *argument, const char *name)
{
	if (ioctl(fd, number, argument) != 0) {
		fprintf(stderr, "%s: %s\n", name, strerror(errno));
		return -1;
	}
	return 0;
}

/* Releases a handle; the node refuses a handle that is mapped (EBUSY). */
static void
release(int fd, uint64_t handle)
{
	struct gpu_resource_destroy destroy;

	memset(&destroy, 0, sizeof(destroy));
	destroy.version = GPU_ABI_VERSION;
	destroy.size = sizeof(destroy);
	destroy.handle = handle;
	request(fd, GPU_RESOURCE_DESTROY, &destroy, "GPU_RESOURCE_DESTROY");
}

/* Writes a pattern into a storage resource, reads it back and compares. */
static int
copy_round_trip(int fd)
{
	uint8_t out[AREA_BYTES];
	uint8_t in[AREA_BYTES];
	struct gpu_resource_create create;
	struct gpu_transfer transfer;
	unsigned index;
	int same;

	for (index = 0; index < AREA_BYTES; index++)
		out[index] = (uint8_t)(index * 7U + 1U);
	memset(in, 0, sizeof(in));

	memset(&create, 0, sizeof(create));
	create.version = GPU_ABI_VERSION;
	create.size = sizeof(create);
	create.bytes = AREA_BYTES;
	create.usage = GPU_RESOURCE_USAGE_STORAGE;
	if (request(fd, GPU_RESOURCE_CREATE, &create, "GPU_RESOURCE_CREATE") != 0)
		return -1;

	/* The address is an ordinary user pointer carried in 64 bits. */
	memset(&transfer, 0, sizeof(transfer));
	transfer.version = GPU_ABI_VERSION;
	transfer.size = sizeof(transfer);
	transfer.handle = create.handle;
	transfer.offset = 0;
	transfer.address = (uint64_t)(uintptr_t)out;
	transfer.bytes = AREA_BYTES;
	if (request(fd, GPU_RESOURCE_WRITE, &transfer, "GPU_RESOURCE_WRITE") != 0) {
		release(fd, create.handle);
		return -1;
	}

	transfer.address = (uint64_t)(uintptr_t)in;
	if (request(fd, GPU_RESOURCE_READ, &transfer, "GPU_RESOURCE_READ") != 0) {
		release(fd, create.handle);
		return -1;
	}

	same = memcmp(out, in, AREA_BYTES);
	printf("storage read back %s\n", same == 0 ? "matches" : "differs");
	release(fd, create.handle);
	return same == 0 ? 0 : -1;
}

/* Makes a mappable blob, maps it with mmap and uses it as memory. */
static int
mapped_blob(int fd)
{
	struct gpu_blob_create blob;
	struct gpu_resource_map map;
	uint8_t *area;
	unsigned index;
	int same = 0;

	memset(&blob, 0, sizeof(blob));
	blob.version = GPU_ABI_VERSION;
	blob.size = sizeof(blob);
	blob.bytes = AREA_BYTES;
	blob.flags = GPU_BLOB_MAPPABLE;
	if (request(fd, GPU_BLOB_CREATE, &blob, "GPU_BLOB_CREATE") != 0)
		return -1;

	/* The request returns a token, not an address; mmap turns the token into memory. */
	memset(&map, 0, sizeof(map));
	map.version = GPU_ABI_VERSION;
	map.size = sizeof(map);
	map.handle = blob.handle;
	if (request(fd, GPU_RESOURCE_MAP, &map, "GPU_RESOURCE_MAP") != 0) {
		release(fd, blob.handle);
		return -1;
	}

	area = mmap(NULL, (size_t)map.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)map.offset);
	if (area == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		release(fd, blob.handle);
		return -1;
	}

	for (index = 0; index < AREA_BYTES; index++)
		area[index] = (uint8_t)(index * 3U + 5U);
	for (index = 0; index < AREA_BYTES; index++)
		same += area[index] == (uint8_t)(index * 3U + 5U);
	printf("mapped blob holds %d of %u bytes written (resource id %u)\n", same, AREA_BYTES,
	    (unsigned)blob.resource_id);

	/* The mapping must be removed first: a mapped handle cannot be destroyed. */
	munmap(area, (size_t)map.bytes);
	release(fd, blob.handle);
	return same == (int)AREA_BYTES ? 0 : -1;
}

int
main(void)
{
	struct gpu_info info;
	int fd;
	int status = 0;

	fd = open("/dev/gpu0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open: %s\n", strerror(errno));
		return 1;
	}

	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	if (request(fd, GPU_GET_INFO, &info, "GPU_GET_INFO") != 0) {
		close(fd);
		return 1;
	}

	/* Each part runs only when the node offers the bits it needs and the area fits its limit. */
	if (info.max_resource_bytes < AREA_BYTES) {
		fprintf(stderr, "the node limits a resource to %ju bytes\n", (uintmax_t)info.max_resource_bytes);
		close(fd);
		return 1;
	}

	if ((info.capabilities & (GPU_CAP_RESOURCE | GPU_CAP_TRANSFER)) == (GPU_CAP_RESOURCE | GPU_CAP_TRANSFER))
		status |= copy_round_trip(fd);
	else
		printf("no storage resources with transfers on this node\n");

	if ((info.capabilities & (GPU_CAP_BLOB | GPU_CAP_MAPPING)) == (GPU_CAP_BLOB | GPU_CAP_MAPPING))
		status |= mapped_blob(fd);
	else
		printf("no mappable blobs on this node\n");

	close(fd);
	return status == 0 ? 0 : 1;
}
```

## Submitting and waiting with a timeout

`GPU_COMMAND_SUBMIT` hands a command stream to the node and returns a `sequence` that names a completion record of the open. `GPU_COMMAND_WAIT` observes the record. Success of the submission means only that the node accepted the stream; the record becomes terminal when the node has finished with it, and its `status` is zero or a positive `errno` value that is the transport outcome, not a Vulkan result ([submission](gpu-submission.md)). The program waits with `GPU_WAIT_CONSUME`, so that the record is retired once its status was copied out and the open does not run out of the `GPU_SUBMIT_MAX` records it may hold.

The program treats the outcomes of the wait as follows.

- A finite `timeout_ns` that expires returns `ETIMEDOUT`. The record is unchanged, and the program waits again; it gives up after five seconds.
- A signal returns `EINTR` when the handler was installed without `SA_RESTART`. The record is unchanged, so the program waits again.
- A request that fails with `EIO` or `ENODEV`, or a record whose `status` is not zero, can mean that the open or the whole device has failed. After a non-zero `status` the program asks `GPU_GET_INFO`, which fails on a failed open; if it fails, the program closes the descriptor and opens the node again, as [submission](gpu-submission.md) describes. Nothing of the old open is revived, so the program starts the whole sequence again on the new open.

The stream is not one that libvulkan would send for a Vulkan call. It is the framing that libvulkan puts around every command: it names the blob that receives the replies (`GPU_OP_SET_REPLY_STREAM`), moves the reply cursor to the last 20 bytes of the blob (`GPU_OP_SEEK_REPLY_STREAM`) and asks for the version query that ends each submission (`GPU_OP_ENUMERATE_INSTANCE_VERSION` with `GPU_OP_FLAG_REPLY`). A real stream comes from libvulkan, which also reads the replies where the Vulkan results are; this program does not read them. Whether a node decodes the stream itself or passes it to a host is in [the command stream](gpu-command-stream.md) and [drivers](gpu-drivers.md).

```c
/* example: submit-wait */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <uapi/gpu.h>
#include <uapi/gpu-op.h>

_Static_assert(sizeof(struct gpu_info) == 56, "struct gpu_info");
_Static_assert(sizeof(struct gpu_blob_create) == 40, "struct gpu_blob_create");
_Static_assert(sizeof(struct gpu_command_submit) == 40, "struct gpu_command_submit");
_Static_assert(sizeof(struct gpu_command_wait) == 32, "struct gpu_command_wait");

#define NODE "/dev/gpu0"
#define REPLY_BYTES 4096U
#define TRAILER_BYTES 20U
#define WAIT_NS UINT64_C(1000000000)
#define WAIT_TRIES 5

/* The bytes of a stream, in the little-endian form of the command protocol. */
struct wire {
	uint8_t bytes[128];
	size_t length;
};

static void
wire_u32(struct wire *wire, uint32_t value)
{
	unsigned shift;

	for (shift = 0; shift < 32U; shift += 8U)
		wire->bytes[wire->length++] = (uint8_t)(value >> shift);
}

static void
wire_u64(struct wire *wire, uint64_t value)
{
	wire_u32(wire, (uint32_t)value);
	wire_u32(wire, (uint32_t)(value >> 32));
}

/*
 * A stream is made by libvulkan in a real program. This one is the framing
 * libvulkan puts around a command (68 bytes): it names the blob that
 * receives the replies, moves the reply cursor to the last 20 bytes of the
 * blob and asks for the version query that ends every submission.
 */
static void
build_stream(struct wire *wire, uint32_t reply_resource)
{
	wire->length = 0;

	wire_u32(wire, GPU_OP_SET_REPLY_STREAM);
	wire_u32(wire, 0);
	wire_u64(wire, 1);
	wire_u32(wire, reply_resource);
	wire_u64(wire, 0);
	wire_u64(wire, REPLY_BYTES);

	wire_u32(wire, GPU_OP_SEEK_REPLY_STREAM);
	wire_u32(wire, 0);
	wire_u64(wire, REPLY_BYTES - TRAILER_BYTES);

	wire_u32(wire, GPU_OP_ENUMERATE_INSTANCE_VERSION);
	wire_u32(wire, GPU_OP_FLAG_REPLY);
	wire_u64(wire, 1);
}

/* Notes in *lost that an errno means the device or this open has failed. */
static int
failed(int error, int *lost)
{
	if (error == EIO || error == ENODEV)
		*lost = 1;
	return error;
}

/*
 * Waits for a completion record for at most WAIT_TRIES seconds. Returns 0
 * with the record's status, or an errno value: ETIMEDOUT when the time ran
 * out (the record is unchanged and the program may wait again or leave).
 */
static int
await_record(int fd, uint64_t sequence, uint32_t *status)
{
	struct gpu_command_wait wait;
	int tries = 0;

	while (tries < WAIT_TRIES) {
		memset(&wait, 0, sizeof(wait));
		wait.version = GPU_ABI_VERSION;
		wait.size = sizeof(wait);
		wait.sequence = sequence;
		wait.timeout_ns = WAIT_NS;
		wait.flags = GPU_WAIT_CONSUME;
		if (ioctl(fd, GPU_COMMAND_WAIT, &wait) == 0) {
			*status = wait.status;
			return 0;
		}

		/* A signal ends the wait; the record is unchanged, so wait again. */
		if (errno == EINTR)
			continue;

		/* The time ran out before the record was terminal. */
		if (errno == ETIMEDOUT) {
			tries++;
			continue;
		}

		return errno;
	}

	return ETIMEDOUT;
}

/* True while the open still answers requests. */
static int
still_usable(int fd)
{
	struct gpu_info info;

	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	return ioctl(fd, GPU_GET_INFO, &info) == 0;
}

/* Submits one stream and waits for it. Returns 0 or an errno value; *lost says the open must be replaced. */
static int
submit_and_wait(int fd, int *lost)
{
	struct gpu_info info;
	struct gpu_blob_create blob;
	struct gpu_command_submit submit;
	struct wire wire;
	uint32_t status;
	int error;

	*lost = 0;

	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	if (ioctl(fd, GPU_GET_INFO, &info) != 0)
		return failed(errno, lost);

	/* Submission with a completion record needs GPU_CAP_NOTIFICATION; the reply blob needs GPU_CAP_BLOB. */
	if ((info.capabilities & (GPU_CAP_NOTIFICATION | GPU_CAP_BLOB)) != (GPU_CAP_NOTIFICATION | GPU_CAP_BLOB))
		return EOPNOTSUPP;
	if (info.max_resource_bytes < REPLY_BYTES)
		return EINVAL;

	/* The blob that receives the stream's replies; the stream names it by its resource id. */
	memset(&blob, 0, sizeof(blob));
	blob.version = GPU_ABI_VERSION;
	blob.size = sizeof(blob);
	blob.bytes = REPLY_BYTES;
	blob.flags = GPU_BLOB_MAPPABLE;
	if (ioctl(fd, GPU_BLOB_CREATE, &blob) != 0)
		return failed(errno, lost);

	build_stream(&wire, blob.resource_id);

	/* The kernel copies the bytes during the call, so the buffer is free afterwards. */
	memset(&submit, 0, sizeof(submit));
	submit.version = GPU_ABI_VERSION;
	submit.size = sizeof(submit);
	submit.address = (uint64_t)(uintptr_t)wire.bytes;
	submit.bytes = (uint32_t)wire.length;
	if (ioctl(fd, GPU_COMMAND_SUBMIT, &submit) != 0) {
		/* EAGAIN: all GPU_SUBMIT_MAX records of this open are in use. */
		return failed(errno, lost);
	}

	/* Acceptance is not completion: the record is observed with the wait request. */
	error = await_record(fd, submit.sequence, &status);
	if (error != 0)
		return failed(error, lost);

	/* A non-zero status is the transport outcome of the record, not a Vulkan result. */
	if (status != 0U) {
		fprintf(stderr, "the stream ended with status %s\n", strerror((int)status));
		if (!still_usable(fd))
			*lost = 1;
		return (int)status;
	}

	printf("sequence %ju completed\n", (uintmax_t)submit.sequence);
	return 0;
}

int
main(void)
{
	int fd;
	int lost;
	int error;

	fd = open(NODE, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open: %s\n", strerror(errno));
		return 1;
	}

	error = submit_and_wait(fd, &lost);
	if (lost) {
		/* Nothing of a failed open is revived: close it and start again on a new one. */
		close(fd);
		fd = open(NODE, O_RDWR | O_CLOEXEC);
		if (fd < 0) {
			fprintf(stderr, "reopen: %s\n", strerror(errno));
			return 1;
		}
		error = submit_and_wait(fd, &lost);
	}

	close(fd);
	if (error != 0) {
		fprintf(stderr, "submit and wait: %s\n", strerror(error));
		return 1;
	}
	return 0;
}
```

## Presenting to a display

A display is used through a lease. The program counts the outputs with `GPU_DISPLAY_QUERY` and `GPU_DISPLAY_COUNT_ONLY`, queries them by index, and picks the first one that is connected, takes whole-frame FIFO presents, is not `GPU_DISPLAY_LIMITED` and accepts BGRA frames of its current size. `GPU_DISPLAY_CLAIM` takes the lease on plane 0 of that output with the `display_id` and `generation` of the query. The frame is a storage resource filled with one colour, written in pieces of at most `GPU_COPY_MAX` bytes. `GPU_DISPLAY_PRESENT` shows the whole frame and returns its `sequence`. `GPU_DISPLAY_WAIT` waits until the frame is adopted. On the ghci and bcm2711 drivers one call waits at most one second; on the i915 driver it can wait for the flip of its last present to latch, whatever `timeout_ns` says; the other drivers answer at once, because their presents return only once the frame is taken. The lease is released at the end whatever happened ([display](gpu-display.md)). The first successful present of a boot also ends the text console's use of the screen.

The errors it expects are these.

- `ESTALE`: the output's `generation` is no longer current, because its topology changed after the query. The program queries again, which `show_frame` does on every attempt, and retries up to three times.
- `EBUSY`: another open holds the plane, or the lease is not this open's any more. The program reports it and does not retry.
- `ETIMEDOUT` and `EINTR` from the wait: the frame is not adopted yet. The program waits again, three times at most.

The storage resource is destroyed as soon as the present has returned, because ordinary storage is no longer borrowed then.

```c
/* example: display */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <uapi/gpu.h>
#include <uapi/gpu-display.h>

_Static_assert(sizeof(struct gpu_info) == 56, "struct gpu_info");
_Static_assert(sizeof(struct gpu_resource_create) == 32, "struct gpu_resource_create");
_Static_assert(sizeof(struct gpu_resource_destroy) == 16, "struct gpu_resource_destroy");
_Static_assert(sizeof(struct gpu_transfer) == 40, "struct gpu_transfer");
_Static_assert(sizeof(struct gpu_display_info) == 152, "struct gpu_display_info");
_Static_assert(sizeof(struct gpu_display_claim) == 32, "struct gpu_display_claim");
_Static_assert(sizeof(struct gpu_display_release) == 16, "struct gpu_display_release");
_Static_assert(sizeof(struct gpu_display_present) == 80, "struct gpu_display_present");
_Static_assert(sizeof(struct gpu_display_wait) == 56, "struct gpu_display_wait");

#define NODE "/dev/gpu0"
#define ATTEMPTS 3
#define WAIT_NS UINT64_C(1000000000)
#define PIXEL 0xff2050d0U

/* The output the program uses, as one query described it. */
struct output {
	uint32_t display_id;
	uint64_t generation;
	uint32_t width;
	uint32_t height;
	uint32_t refresh_millihz;
	uint64_t frame_bytes;
};

/* Chooses the first output that is lit, takes BGRA frames and accepts a whole frame of its current mode. */
static int
choose_output(int fd, struct output *chosen)
{
	struct gpu_display_info info;
	uint32_t count;
	uint32_t index;

	/* A count-only query fills in nothing but count. */
	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	info.index = GPU_DISPLAY_COUNT_ONLY;
	if (ioctl(fd, GPU_DISPLAY_QUERY, &info) != 0)
		return errno;
	count = info.count;

	for (index = 0; index < count; index++) {
		memset(&info, 0, sizeof(info));
		info.version = GPU_ABI_VERSION;
		info.size = sizeof(info);
		info.index = index;
		if (ioctl(fd, GPU_DISPLAY_QUERY, &info) != 0)
			return errno;

		if ((info.flags & GPU_DISPLAY_CONNECTED) == 0U ||
		    (info.flags & GPU_DISPLAY_FIFO) == 0U ||
		    (info.flags & GPU_DISPLAY_LIMITED) != 0U ||
		    (info.formats & GPU_DISPLAY_FORMAT_BGRA8888) == 0U)
			continue;
		if (info.current_width == 0U || info.current_height == 0U || info.refresh_millihz == 0U)
			continue;

		chosen->display_id = info.display_id;
		chosen->generation = info.generation;
		chosen->width = info.current_width;
		chosen->height = info.current_height;
		chosen->refresh_millihz = info.refresh_millihz;
		chosen->frame_bytes = (uint64_t)info.current_width * 4U * info.current_height;
		if (chosen->frame_bytes > info.max_frame_bytes)
			continue;
		return 0;
	}

	return ENODEV;
}

/* Makes a storage resource of the frame's size and fills it with one colour, GPU_COPY_MAX bytes at a time. */
static int
make_frame(int fd, const struct output *output, uint64_t *handle)
{
	static uint32_t chunk[GPU_COPY_MAX / 4U];
	struct gpu_resource_create create;
	struct gpu_transfer transfer;
	uint64_t offset;
	size_t index;
	uint64_t step;

	for (index = 0; index < sizeof(chunk) / sizeof(chunk[0]); index++)
		chunk[index] = PIXEL;

	memset(&create, 0, sizeof(create));
	create.version = GPU_ABI_VERSION;
	create.size = sizeof(create);
	create.bytes = output->frame_bytes;
	create.usage = GPU_RESOURCE_USAGE_STORAGE;
	if (ioctl(fd, GPU_RESOURCE_CREATE, &create) != 0)
		return errno;

	for (offset = 0; offset < output->frame_bytes; offset += step) {
		step = output->frame_bytes - offset;
		if (step > sizeof(chunk))
			step = sizeof(chunk);

		memset(&transfer, 0, sizeof(transfer));
		transfer.version = GPU_ABI_VERSION;
		transfer.size = sizeof(transfer);
		transfer.handle = create.handle;
		transfer.offset = offset;
		transfer.address = (uint64_t)(uintptr_t)chunk;
		transfer.bytes = (uint32_t)step;
		if (ioctl(fd, GPU_RESOURCE_WRITE, &transfer) != 0) {
			int error = errno;
			struct gpu_resource_destroy destroy = { GPU_ABI_VERSION, sizeof(destroy), create.handle };

			ioctl(fd, GPU_RESOURCE_DESTROY, &destroy);
			return error;
		}
	}

	*handle = create.handle;
	return 0;
}

/* Claims an output, presents one frame, waits until it is adopted and releases the lease. */
static int
show_frame(int fd)
{
	struct output output;
	struct gpu_display_claim claim;
	struct gpu_display_present present;
	struct gpu_display_wait wait;
	struct gpu_display_release release;
	struct gpu_resource_destroy destroy;
	uint64_t handle;
	int tries;
	int error;

	/* Query again on every attempt: a generation is valid only until the output's topology changes. */
	error = choose_output(fd, &output);
	if (error != 0)
		return error;

	memset(&claim, 0, sizeof(claim));
	claim.version = GPU_ABI_VERSION;
	claim.size = sizeof(claim);
	claim.display_id = output.display_id;
	claim.plane_index = 0;
	claim.generation = output.generation;
	if (ioctl(fd, GPU_DISPLAY_CLAIM, &claim) != 0)
		return errno;

	error = make_frame(fd, &output, &handle);
	if (error != 0)
		goto done;

	/* A whole-frame FIFO present; the sequence comes back in the same structure. */
	memset(&present, 0, sizeof(present));
	present.version = GPU_ABI_VERSION;
	present.size = sizeof(present);
	present.lease = claim.lease;
	present.handle = handle;
	present.offset = 0;
	present.frame = 1;
	present.width = output.width;
	present.height = output.height;
	present.stride = output.width * 4U;
	present.format = GPU_PIXEL_BGRA8888;
	present.refresh_millihz = output.refresh_millihz;
	present.flags = GPU_DISPLAY_PRESENT_FIFO;
	present.generation = output.generation;
	if (ioctl(fd, GPU_DISPLAY_PRESENT, &present) != 0) {
		error = errno;
	} else {
		/* Ordinary storage is copied before the call returns, so it can go at once. */
		memset(&destroy, 0, sizeof(destroy));
		destroy.version = GPU_ABI_VERSION;
		destroy.size = sizeof(destroy);
		destroy.handle = handle;
		ioctl(fd, GPU_RESOURCE_DESTROY, &destroy);

		/* On ghci and bcm2711 one wait lasts at most a second; a timeout leaves the frame pending. */
		for (tries = 0; tries < ATTEMPTS; tries++) {
			memset(&wait, 0, sizeof(wait));
			wait.version = GPU_ABI_VERSION;
			wait.size = sizeof(wait);
			wait.lease = claim.lease;
			wait.sequence = present.sequence;
			wait.timeout_ns = WAIT_NS;
			if (ioctl(fd, GPU_DISPLAY_WAIT, &wait) == 0) {
				printf("frame %ju adopted\n", (uintmax_t)wait.completed_sequence);
				error = 0;
				break;
			}
			error = errno;
			if (error != EINTR && error != ETIMEDOUT)
				break;
		}
		handle = 0;
	}

done:
	if (handle != 0U) {
		memset(&destroy, 0, sizeof(destroy));
		destroy.version = GPU_ABI_VERSION;
		destroy.size = sizeof(destroy);
		destroy.handle = handle;
		ioctl(fd, GPU_RESOURCE_DESTROY, &destroy);
	}

	memset(&release, 0, sizeof(release));
	release.version = GPU_ABI_VERSION;
	release.size = sizeof(release);
	release.lease = claim.lease;
	ioctl(fd, GPU_DISPLAY_RELEASE, &release);
	return error;
}

int
main(void)
{
	struct gpu_info info;
	int attempt;
	int error = 0;
	int fd;

	fd = open(NODE, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open: %s\n", strerror(errno));
		return 1;
	}

	memset(&info, 0, sizeof(info));
	info.version = GPU_ABI_VERSION;
	info.size = sizeof(info);
	if (ioctl(fd, GPU_GET_INFO, &info) != 0) {
		fprintf(stderr, "GPU_GET_INFO: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	/* Display requests need GPU_CAP_DISPLAY, and the frame is made with storage and transfers. */
	if ((info.capabilities & (GPU_CAP_DISPLAY | GPU_CAP_RESOURCE | GPU_CAP_TRANSFER)) !=
	    (GPU_CAP_DISPLAY | GPU_CAP_RESOURCE | GPU_CAP_TRANSFER)) {
		fprintf(stderr, "the node cannot show a stored frame\n");
		close(fd);
		return 1;
	}

	for (attempt = 0; attempt < ATTEMPTS; attempt++) {
		error = show_frame(fd);

		/* ESTALE: the topology changed after the query. Query again and retry. */
		if (error != ESTALE)
			break;
	}

	close(fd);
	if (error == EBUSY)
		fprintf(stderr, "another open leases the output or this lease ended\n");
	else if (error != 0)
		fprintf(stderr, "display: %s\n", strerror(error));
	return error == 0 ? 0 : 1;
}
```

## Where the tests are

These programs under `userland/tests/` call the requests directly, with the ioctl structures of this ABI.

- [`gpu-admission`](../../userland/tests/gpu-admission/main.c): several threads share one open and issue resource, transfer, information and display requests at the same time.
- [`gpu-recovery`](../../userland/tests/gpu-recovery/main.c): a submission whose renderer does not answer ends as a transport timeout, the device fails, and a new open recovers it.
- [`gpu-i915`](../../userland/tests/gpu-i915/main.c): native command streams for the i915 driver with `GPU_COMMAND_SUBMIT` and `GPU_COMMAND_WAIT`, and one supervised job.
- [`venus-frame`](../../userland/tests/venus-frame/venus-frame.c): a command stream sent with `GPU_COMMAND` to a virtual machine's renderer, and a frame shown with `GPU_PRESENT`; [`client.c`](../../userland/tests/venus-frame/client.c) holds the requests.
- [`amdrdnactl`](../../userland/tests/amdrdnactl/gpu-check.c): blobs, storage, transfers, mappings, and export and import between two opens.
- [`display-inventory`](../../userland/tests/display-inventory/main.c): the outputs of a node and the display events.
- [`display-scanout`](../../userland/tests/display-scanout/main.c): the node identity, a lease, a shared frame presented without a copy, and the cursor.
- [`display-control`](../../userland/tests/display-control/main.c): refresh boundaries and display power on a leased output.
- [`gpu-bars`](../../userland/tests/gpu-bars/main.c): a lease and two presents of a colour-bar frame, in RGBA and in BGRA.
- [`ghci-display`](../../userland/tests/ghci-display/main.c): leases and presents in every mode of a node.
- [`hvs-load`](../../userland/tests/hvs-load/main.c): commits of overlay planes with `GPU_DISPLAY_COMMIT`.
- [`acquire-fence`](../../userland/tests/acquire-fence/main.c): kernel fences made, bound and signaled by a program that drives a window.

These programs reach the GPU only through Vulkan, so they are tests of the Vulkan level and not examples of the requests.

- [`gpu-selftest`](../../userland/tests/gpu-selftest/main.c): the stages of a Vulkan driver, one after the other.
- [`gpu-fence`](../../userland/tests/gpu-fence/main.c): sharing a Vulkan fence between processes.

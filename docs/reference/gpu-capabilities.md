# GPU capabilities and capability sets

Status: experimental reference for GPU ABI version 1.

A GPU node tells a program what it supports in two ways. `GPU_GET_INFO`
returns the node's capability bits, limits and driver name. `GPU_GET_CAPSET`
returns a backend-defined capability set (capset): a small byte payload that
the Vulkan driver (libvulkan) reads to learn the command stream's protocol
version and the backend's features. The capability bits say which requests a
node accepts; the capset says how the renderer behind the command stream
behaves. The conventions shared by every request (version and size fields,
open modes, error reporting) are in [gpu.md](gpu.md).

## Capability bits

Each bit enables the requests named in its row; the table lists the requests and the error a request returns when the bit is clear. A program reads the bits with `GPU_GET_INFO`.

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_CAP_RESOURCE` | 1 | The node allocates ordinary storage resources: `GPU_RESOURCE_CREATE` works. Without it `GPU_RESOURCE_CREATE` fails with `EOPNOTSUPP`. A node with this bit also supports `GPU_RESOURCE_DESTROY`; a node with only `GPU_CAP_BLOB` supports it too. |
| `GPU_CAP_CAPSET` | 2 | The node answers `GPU_GET_CAPSET`. Without it `GPU_GET_CAPSET` fails with `EOPNOTSUPP`. |
| `GPU_CAP_BLOB` | 4 | The node creates blobs: `GPU_BLOB_CREATE` and `GPU_BLOB_CREATE_PLACED` work (a placed blob with a physical-placement requirement additionally needs a driver that can arrange the backing, otherwise `ENOTSUP`). Without it both fail with `EOPNOTSUPP`. `GPU_CAP_SHARE` and `GPU_CAP_ALLOCATION_SHARE` are only set together with this bit. |
| `GPU_CAP_TRANSFER` | 8 | The node copies bytes into and out of a resource: `GPU_RESOURCE_READ` and `GPU_RESOURCE_WRITE` work. Without it both fail with `EOPNOTSUPP`. |
| `GPU_CAP_COMMAND` | 16 | The node accepts a command stream through `GPU_COMMAND`. Without it `GPU_COMMAND` fails with `EOPNOTSUPP`. Libvulkan uses a node only if it also has `GPU_CAP_CAPSET`, `GPU_CAP_BLOB`, `GPU_CAP_TRANSFER` and `GPU_CAP_MAPPING`. |
| `GPU_CAP_PRESENT` | 32 | The node shows an image on the screen through `GPU_PRESENT`. Without it `GPU_PRESENT` fails with `EOPNOTSUPP`. It is a separate bit from `GPU_CAP_DISPLAY`: `GPU_DISPLAY_PRESENT` and the other display requests do not need it. |
| `GPU_CAP_MAPPING` | 64 | The node gives CPU views of resources: `GPU_RESOURCE_MAP` works and its token can be passed to `mmap`. Without it `GPU_RESOURCE_MAP` fails with `EOPNOTSUPP`. A node with this bit also has `GPU_CAP_RESOURCE` or `GPU_CAP_BLOB`. |
| `GPU_CAP_DISPLAY` | 128 | The node has display outputs: `GPU_DISPLAY_QUERY`, `GPU_DISPLAY_MODE`, `GPU_DISPLAY_CLAIM`, `GPU_DISPLAY_RELEASE`, `GPU_DISPLAY_PRESENT`, `GPU_DISPLAY_WAIT`, the cursor requests, `GPU_DISPLAY_OVERLAY_INFO`, `GPU_DISPLAY_COMMIT` and `GPU_DISPLAY_CONSTRAINTS` are available, subject to the operations the driver provides (the kernel answers the cursor requests with `EOPNOTSUPP` when the driver has no cursor operations, and `GPU_DISPLAY_COMMIT` and `GPU_DISPLAY_OVERLAY_INFO` when it has no overlay operations). Without it these requests fail with `EOPNOTSUPP`. `GPU_DISPLAY_POWER` and `GPU_DISPLAY_REFRESH` additionally need `GPU_CAP_DISPLAY_CONTROL`. |
| `GPU_CAP_SHARE` | 256 | The node exports a resource to another open or device and imports one: `GPU_RESOURCE_EXPORT` and `GPU_RESOURCE_IMPORT` work, and a blob may be created with `GPU_BLOB_SHAREABLE`. Without it `GPU_RESOURCE_EXPORT` fails with `EOPNOTSUPP`; `GPU_RESOURCE_IMPORT` still works on a node whose display driver imports foreign images. A blob with a sharing flag needs this bit or `GPU_CAP_ALLOCATION_SHARE`, otherwise `GPU_BLOB_CREATE` fails with `EOPNOTSUPP`. |
| `GPU_CAP_NOTIFICATION` | 512 | The node queues commands with a completion record: `GPU_COMMAND_SUBMIT` and `GPU_COMMAND_WAIT` work, and an open reports `POLLIN` while a completion record is waiting to be consumed. Without it both requests fail with `EOPNOTSUPP`. `GPU_CAP_JOB` is only set together with this bit. |
| `GPU_CAP_ALLOCATION_SHARE` | 1024 | The node exports and imports an allocation without an image layout: `GPU_ALLOCATION_EXPORT` and `GPU_ALLOCATION_IMPORT` work, and a blob may be created with `GPU_BLOB_SHAREABLE` or `GPU_BLOB_CROSS_DEVICE`. Without it both allocation requests fail with `EOPNOTSUPP`. It is only set together with `GPU_CAP_BLOB`. |
| `GPU_CAP_FENCE` | 2048 | The node supports fence descriptors: `GPU_FENCE_CREATE`, `GPU_FENCE_QUERY`, `GPU_FENCE_WAIT`, `GPU_FENCE_RESET`, `GPU_FENCE_SIGNAL`, `GPU_FENCE_BIND`, `GPU_COMMAND_SUBMIT_SYNC` and `GPU_DISPLAY_PRESENT_SYNC` work, and `GPU_DISPLAY_COMMIT` accepts a fence descriptor. The two `_SYNC` requests also need the bit of the request they carry: `GPU_CAP_NOTIFICATION` for `GPU_COMMAND_SUBMIT_SYNC` and `GPU_CAP_DISPLAY` for `GPU_DISPLAY_PRESENT_SYNC`. Without it these requests fail with `EOPNOTSUPP`; `GPU_DISPLAY_COMMIT` fails that way only when a plane or the commit names a fence descriptor. |
| `GPU_CAP_DISPLAY_EVENTS` | 0x1000 | The node reports display topology changes: `GPU_DISPLAY_EVENTS` works, and a readable open reports `POLLPRI` while a topology change is unacknowledged. Without it `GPU_DISPLAY_EVENTS` fails with `EOPNOTSUPP` and `POLLPRI` is never reported for display events. |
| `GPU_CAP_JOB` | 0x2000 | The node supports supervised jobs: `GPU_JOB_RESERVE`, `GPU_JOB_COMMIT`, `GPU_JOB_CANCEL` and `GPU_JOB_POLICY` work. Without it they fail with `EOPNOTSUPP`. The bit does not by itself promise that a reservation succeeds (see rule 4 below). It is only set together with `GPU_CAP_NOTIFICATION`. |
| `GPU_CAP_JOB_CAPACITY` | 0x4000 | The node reports whether a timeline can take another reservation, and lets a program wait for room: `GPU_JOB_CAPACITY` works (its `available` field is 0 or 1, not a count). Without it `GPU_JOB_CAPACITY` fails with `EOPNOTSUPP`. It is only set together with `GPU_CAP_JOB`. |
| `GPU_CAP_DISPLAY_CONTROL` | 0x8000 | The node controls display power and observes refresh boundaries: `GPU_DISPLAY_POWER` and `GPU_DISPLAY_REFRESH` work. Without it both fail with `EOPNOTSUPP`. It is only set together with `GPU_CAP_DISPLAY`. |
| `GPU_CAP_HOST_BLOB` | 0x10000 | The node can back a blob created with `GPU_BLOB_HOST` in memory that the CPU reads through its cache. Without it the kernel clears `GPU_BLOB_HOST` before the driver sees the request, so asking for it is always safe; no request fails because the bit is clear. |

Rules:

1. The bits that `GPU_GET_INFO` returns are the node's registered
   capabilities. The kernel copies the node's capability mask into the reply
   after the driver has filled the rest of `struct gpu_info`; a value the driver
   writes into the `capabilities` field is discarded. Two calls on the same node
   therefore return the same bits.
2. The bits do not change during a node's life. Every driver sets its bits
   before it registers the node, including the bits that depend on the hardware
   or on a boot option (a renderer's command and job bits, a display's bits).
   The kernel checks the mask once, when the node is registered: it rejects a
   bit that has no defined meaning, and a bit whose operations are missing or
   inconsistent. No driver adds or removes a bit afterwards. A program that
   keeps a node open can cache the bits. A node that disappears (requests fail
   with `ENODEV`) and a node that later appears under the same name are
   different registrations and may have different bits; a program opens the node
   again and calls `GPU_GET_INFO` again. One physical GPU can publish several
   nodes, and each node has its own bits (see [gpu-drivers.md](gpu-drivers.md)).
3. A program may ask for `GPU_BLOB_HOST` on any node. A node without
   `GPU_CAP_HOST_BLOB` clears the flag before its driver sees the request, and
   the blob is made as any other blob. The check that `GPU_BLOB_HOST` is not
   combined with `GPU_BLOB_SHAREABLE` or `GPU_BLOB_CROSS_DEVICE` comes first and
   fails with `EINVAL` on every node.
4. A set bit does not promise that every request it guards succeeds on every
   node. A node may set `GPU_CAP_JOB` and `GPU_CAP_JOB_CAPACITY` while
   `GPU_JOB_RESERVE` and `GPU_JOB_CAPACITY` fail with `ENOTSUP` (the same value as `EOPNOTSUPP`): a Venus node
   (a virtual machine's GPU) always sets both bits, and its driver refuses both
   requests when the host's renderer does not complete native work strictly.
   The capset tells a program in advance: the host promises strict completion
   when `VENDOR_STRICT_QUEUE` is set in `vendor_flags` (see [Capability set 4](#capability-set-4)).
   A bit also does not cover a request's other requirements: a request can still
   fail because the driver has no operation for it (a display without a cursor
   plane fails the cursor requests with `EOPNOTSUPP`) or because its arguments are
   not supported.
5. Some bits imply others. The kernel refuses to register a node that sets
   `GPU_CAP_JOB` without `GPU_CAP_NOTIFICATION`, `GPU_CAP_JOB_CAPACITY` without
   `GPU_CAP_JOB`, `GPU_CAP_SHARE` or `GPU_CAP_ALLOCATION_SHARE` without
   `GPU_CAP_BLOB`, `GPU_CAP_MAPPING` without `GPU_CAP_RESOURCE` or
   `GPU_CAP_BLOB`, and `GPU_CAP_DISPLAY_EVENTS` or `GPU_CAP_DISPLAY_CONTROL`
   without display operations. A program can rely on these implications.
6. Libvulkan requires `GPU_CAP_CAPSET`, `GPU_CAP_BLOB`, `GPU_CAP_TRANSFER`,
   `GPU_CAP_COMMAND` and `GPU_CAP_MAPPING` on a node before it reads the capset.
   It additionally needs `GPU_CAP_JOB` and `GPU_CAP_JOB_CAPACITY` to create a
   native Vulkan device on the node, and uses `GPU_CAP_NOTIFICATION` for queued
   submission.

## Requests

### `GPU_GET_INFO`

Requires: open mode none; capability none.

Returns the node's identity, its limits and its capability bits in a
`struct gpu_info` ([Structures](#structures)). The caller sets `version` to
`GPU_ABI_VERSION` and `size` to the structure's size (56); the kernel reads no
other input field, so the other fields can hold any value. The kernel zeroes
its copy of the reply, lets the driver fill it, and then writes `version`,
`size` and `capabilities` itself: `capabilities` is the node's registered mask
described in [Capability bits](#capability-bits), never a value the driver put
in the field. The kernel also forces the last byte of `driver_name` to NUL, so a
driver name has at most 31 characters and the unused bytes of the field are
zero. The argument is
copied back to the caller only when the request succeeds.

`max_resources` is the number of resources one open file description may hold
at the same time. `max_resource_bytes` is the size limit of one resource in
bytes. The kernel checks both limits when it creates a resource record: for
`GPU_RESOURCE_CREATE`, `GPU_BLOB_CREATE`, `GPU_BLOB_CREATE_PLACED`,
`GPU_ALLOCATION_IMPORT` and `GPU_RESOURCE_IMPORT`. A request for 0 bytes or for
more than `max_resource_bytes` fails with `EINVAL`; a request that would exceed
`max_resources` fails with `ENOSPC`. A driver that places no limit on the count
reports `UINT32_MAX`; neither limit is reported as 0 by any current driver, and
the kernel does not treat 0 as "unlimited": a limit of 0 would refuse every
resource. The limits are properties of the node and do not depend on the
resources currently held, so `GPU_GET_INFO` can be called at any time. See
[gpu-memory.md](gpu-memory.md) for the requests that create resources.

The request runs under the open's admission like most requests: it waits
(interruptibly) while another admitted request on the same open file
description is running. See [gpu.md](gpu.md).

Errors:

- `EFAULT`: the kernel cannot read the argument or write the reply.
- `EINVAL`: `version` is not `GPU_ABI_VERSION` or `size` is not the size of `struct gpu_info`.
- `ENODEV`: the node has been withdrawn, or the open file description has no session.
- `EINTR`: a signal ended the wait for the open's admission.
- `EBUSY`: the calling thread already holds the open's admission (a call made from inside a driver callback; a program does not see it).
- Other values: the value is the driver's `get_info` result, which the kernel passes through unchanged, or the error that the device or this open recorded when it failed (see [gpu.md](gpu.md)).

Sources: [include/uapi/gpu.h](../../include/uapi/gpu.h); [src/drivers/gpu/gpu.c](../../src/drivers/gpu/gpu.c) `gpu_info_ioctl`, `gpu_ioctl_body`, `gpu_resource_reserve`; [userland/tests/gpu-admission/main.c](../../userland/tests/gpu-admission/main.c).

### `GPU_GET_CAPSET`

Requires: open mode none; capability `GPU_CAP_CAPSET`.

Returns a capability set: a payload of up to `GPU_CAPSET_MAX` bytes that the
driver chooses by `capset_id` and `capset_version`. The argument is a
`struct gpu_capset` ([Structures](#structures)). The caller sets `version` and
`size` (280), selects the capset with `capset_id` and `capset_version`, sets
`capacity` to the number of bytes of `data` it can accept (1 to
`GPU_CAPSET_MAX`), and leaves `bytes` at 0. The kernel zeroes the whole reply,
passes the selectors and the capacity to the driver, and lets the driver store
the payload in `data` and its length in `bytes`. The bytes of `data` after
`bytes` are zero. On success the kernel copies the reply back with the
request's `version`, `size`, `capset_id`, `capset_version` and `capacity`
unchanged. `data` in the request is not read. Nothing is written to the
argument when the request fails.

The kernel does not interpret `capset_id` or `capset_version`; the driver
decides which capsets it has. Every current driver has one capset, id 4
version 0, whose layout is described in [Capability set 4](#capability-set-4),
and a request for any other id or version fails: the native drivers return
`ENOTSUP` and the Venus driver returns `EOPNOTSUPP` (the same value). A
program asks for id 4 and version 0.

If `capacity` is smaller than the capset, the request fails and no payload is
returned. The drivers report `EINVAL` when the capset does not fit
(`EMSGSIZE` for a Venus node, where the size is the host's). Two drivers give
a shorter payload instead of failing when the larger form does not fit:
the nvrtx driver returns its 168-byte payload without the native word when
`capacity` is at least 168 and below 176, and the bcm2711 driver does the same
for its video form. Passing `GPU_CAPSET_MAX` as `capacity`, as libvulkan does,
always lets the full payload fit. The size of the payload, which differs
between drivers, is in `bytes`; a program uses `bytes`, not `capacity`, to
decide how much of `data` is valid.

The request runs under the open's admission (see [gpu.md](gpu.md)). A driver
may take time to answer: the Venus driver asks the host and waits for the
reply.

Errors:

- `EOPNOTSUPP`: the node does not have `GPU_CAP_CAPSET`.
- `EFAULT`: the kernel cannot read the argument or write the reply.
- `EINVAL`: `version` is not `GPU_ABI_VERSION`, `size` is not the size of `struct gpu_capset`, `capacity` is 0 or larger than `GPU_CAPSET_MAX`, or `bytes` is not 0 on input.
- `EIO`: the driver reported a payload longer than `capacity`.
- `ENODEV`: the node has been withdrawn, or the open file description has no session.
- `EINTR`: a signal ended the wait for the open's admission.
- `EBUSY`: the calling thread already holds the open's admission (a call made from inside a driver callback; a program does not see it).
- Other values: the driver's `get_capset` result passes through unchanged, or the error the device or this open recorded when it failed. Known values: `ENOTSUP` for a capset id or version the driver does not have (all native drivers; `EOPNOTSUPP` for the Venus driver) and for an i915 node without a Vulkan executor; `EINVAL` when `capacity` is below the payload size (native drivers and i915) and `EMSGSIZE` for the Venus driver; `ECANCELED` from the bcm2711 driver when the Vulkan namespace of the open is closing and `EIO` when it is stopping or its state is uncertain; for a Venus node, the errors of the exchange with the host (for example `EIO` for a reply that is not a complete capset reply, and the error that the host's refusal maps to).

Sources: [include/uapi/gpu.h](../../include/uapi/gpu.h); [src/drivers/gpu/gpu.c](../../src/drivers/gpu/gpu.c) `gpu_capset_ioctl`, `gpu_ioctl_body`; drivers' capset operations: [venus.c](../../src/drivers/gpu/venus/venus.c) `venus_get_capset`, [amdrdna/runtime.c](../../src/drivers/gpu/amdrdna/runtime.c) `runtime_capset`, [nvrtx/runtime.c](../../src/drivers/gpu/nvrtx/runtime.c) `runtime_capset`, [powervr/render-runtime.c](../../src/drivers/gpu/powervr/render-runtime.c) `runtime_capset`, [bcm2711/render-runtime.c](../../src/drivers/gpu/bcm2711/render-runtime.c) `runtime_capset`, [i915/command.c](../../src/drivers/gpu/i915/command.c) `i915_get_capset`, [mali/sched.c](../../src/drivers/gpu/mali/sched.c) `sched_capset`, [ghci/vk/vk-node.c](../../src/drivers/gpu/ghci/vk/vk-node.c) `node_capset`; [userland/tests/venus-frame/client.c](../../userland/tests/venus-frame/client.c).

## Capability set 4

Capability set 4, version 0, is the capset that every current driver returns
and that libvulkan reads. It describes the renderer behind the command stream
([gpu-command-stream.md](gpu-command-stream.md)). Its layout is not in a public
header: this document is the reference. The names in the tables below
(`wire_version`, `VENDOR_MAGIC` and so on) are this document's names: the
payload fields are named as libvulkan names the words it reads, and the names in
the `Capset constant` column are not macros of any header. All words are
little-endian 32-bit values. A program requests it with `capset_id` 4,
`capset_version` 0 and `capacity` `GPU_CAPSET_MAX`.

The payload has three parts. The base part is the first 156 bytes or more:
a renderer-defined record in which libvulkan reads three words, `wire_version`
at offset 0, `xml_version` at offset 4 and `timelines` at offset 152. The
vendor suffix is the 8 bytes at offsets 160 and 164, and the payload is 168
bytes long when it ends there. The native suffix is the 8 bytes at offsets 168
and 172, and the payload is 176 bytes long when it ends there. A payload of
any other length has no suffix that libvulkan reads.

| Offset | Bytes | Payload field | Meaning |
| ---: | ---: | --- | --- |
| 0 | 4 | `wire_version` | The version of the command stream's wire format. Libvulkan accepts only 1. Every native driver writes 1. |
| 4 | 4 | `xml_version` | The Vulkan registry version that the command stream was generated from, in the `VK_MAKE_VERSION` encoding (major in bits 22 and up, minor in bits 12 to 21, patch in bits 0 to 11). Libvulkan accepts 1.3.269 (0x0040310d) and 1.4.343 (0x00404157) and rejects any other value. Every native driver writes 1.3.269. |
| 8 | 144 | renderer data (offsets 8 to 151) | Not read by libvulkan. A virtual machine's host fills the range with its renderer's data, which the Venus driver copies unchanged; a native driver leaves it zero. |
| 152 | 4 | `timelines` | The renderer's support for completion timelines. Libvulkan rejects 0 and accepts any other value. Every native driver writes 1. |
| 156 | 4 | renderer data (offsets 156 to 159) | Not read by libvulkan, which requires `bytes` of at least 156. Filled as the range at offset 8 is. |
| 160 | 4 | `vendor_magic` | `VENDOR_MAGIC` when the vendor suffix is present. Libvulkan reads the word only when `bytes` is `VENDOR_BYTES` or `NATIVE_BYTES`, and ignores `vendor_flags` unless the magic matches. |
| 164 | 4 | `vendor_flags` | A set of `VENDOR_OPAQUE`, `VENDOR_STRICT_QUEUE`, `VENDOR_QUIESCE` and `VENDOR_HOST_SCANOUT` bits. Libvulkan acts on only the exact combinations listed under the constants below; any other value leaves it with the default behaviour of a renderer without the suffix. |
| 168 | 4 | `native_magic` | `NATIVE_MAGIC` when the native suffix is present. Libvulkan reads the word only when `bytes` is `NATIVE_BYTES`. |
| 172 | 4 | `native_flags` | A set of `NATIVE_VIDEO_DECODE_H264` and `NATIVE_SYNC_COMMAND` bits, read only when `native_magic` is `NATIVE_MAGIC`. Libvulkan ignores bits it does not know. |

Libvulkan accepts a node only when `bytes` is at least 156 and at most
`GPU_CAPSET_MAX`, `wire_version` is 1, `xml_version` is one of the two accepted
values and `timelines` is not 0; otherwise the node is incompatible. The
bytes of `data` after `bytes` are zero.

| Capset constant | Value | Meaning |
| --- | ---: | --- |
| `CAPSET_ID` | 4 | The `capset_id` that selects this capset; the `capset_version` is 0. |
| `NATIVE_BYTES` | 176 | The value of `bytes` of a payload that has both suffixes. |
| `NATIVE_MAGIC` | 0x5a4e4154 | The value of `native_magic`. Stored little-endian, its bytes read `T`, `A`, `N`, `Z`. |
| `NATIVE_SYNC_COMMAND` | 2 | The renderer runs a `GPU_COMMAND` at once on the calling thread and has written the command's replies when the call returns (`EAGAIN` means earlier work of the open is still queued and the command was not run). Libvulkan sends commands with `GPU_COMMAND` first when it is set and the node also has `GPU_CAP_NOTIFICATION`, and uses queued submission when the call is not run. |
| `NATIVE_VIDEO_DECODE_H264` | 1 | The renderer promises a video-decode queue family that decodes H.264. Without it libvulkan hides the video queue flags, the video format features and the video queries from the application. |
| `VENDOR_BYTES` | 168 | The value of `bytes` of a payload that has the vendor suffix and no native suffix. |
| `VENDOR_HOST_SCANOUT` | 8 | The renderer lets the display use its images directly. Libvulkan reads it only with `xml_version` 1.4.343: when `vendor_flags` is exactly 15 (this flag with `VENDOR_OPAQUE`, `VENDOR_STRICT_QUEUE` and `VENDOR_QUIESCE`) it imports renderer images into the display directly; with any other `vendor_flags` it does not import them. With `xml_version` 1.3.269 libvulkan does not look at this flag. On a Venus node the kernel refuses to export an allocation as a display image (`GPU_RESOURCE_EXPORT` with an image descriptor) for a blob created without `GPU_BLOB_CROSS_DEVICE` unless this flag is set. |
| `VENDOR_MAGIC` | 0x5a424453 | The value of `vendor_magic`. Stored little-endian, its bytes read `S`, `D`, `B`, `Z`. |
| `VENDOR_OPAQUE` | 1 | The renderer supports allocations that share by an opaque descriptor. Libvulkan selects the opaque-descriptor handle type for external memory when `vendor_flags` is exactly 1, 3, 7 or 15; with any other value it keeps the DMA-BUF handle type. |
| `VENDOR_QUIESCE` | 4 | The renderer can prove that a session's native work has retired, so that the kernel can stop one session without failing the others. Libvulkan reads it only in the combinations `VENDOR_OPAQUE` and `VENDOR_STRICT_QUEUE` with this flag (7), or those with `VENDOR_HOST_SCANOUT` as well (15). On a Venus node the kernel enables its stop handshake for a session only in those combinations. |
| `VENDOR_STRICT_QUEUE` | 2 | The renderer completes a native fence only when the work succeeded. Libvulkan reads it in the combinations 3, 7 and 15. On a Venus node the kernel enables `GPU_JOB_RESERVE` and `GPU_JOB_CAPACITY` only in those combinations; otherwise both fail with `ENOTSUP`. |

Libvulkan uses a node for native Vulkan devices only when `bytes` is `VENDOR_BYTES` or `NATIVE_BYTES`, `vendor_magic` is `VENDOR_MAGIC`, `vendor_flags` is 7 or 15 (strict queue and quiesce both set) and the node has both `GPU_CAP_JOB` and `GPU_CAP_JOB_CAPACITY`. For any other node it creates no native Vulkan instance, and an attempt to create a device on it fails.

The combinations are read as exact values. A Venus host that reports the
vendor suffix with another `vendor_flags` value (for example 2 or 6) gets no
strict-queue behaviour from either the kernel or libvulkan. A payload whose
`bytes` is neither `VENDOR_BYTES` nor `NATIVE_BYTES` is treated as a renderer
without a vendor suffix: libvulkan keeps the DMA-BUF handle type, the node is
not given a strict queue, and the node is not usable for native Vulkan devices.

Which drivers return which form (a payload is returned only when `capacity`
is at least its length unless the row says otherwise):

| Driver | bytes | vendor flags | native flags |
| --- | --- | --- | --- |
| venus (a virtual machine's GPU) | The size the host reports for capset 4, 1 to 256. The payload is copied from the host unchanged | The host's. The kernel looks at it only when `bytes` is `VENDOR_BYTES`, and recognises the values 3, 7 and 15 | The host's, when the payload is `NATIVE_BYTES` |
| amdrdna | `NATIVE_BYTES` | 7 | `NATIVE_SYNC_COMMAND` |
| nvrtx | `VENDOR_BYTES`; `NATIVE_BYTES` when `capacity` is at least 176 | 7 | `NATIVE_SYNC_COMMAND`, plus `NATIVE_VIDEO_DECODE_H264` when the device decodes video |
| powervr | `VENDOR_BYTES` | 7 | none (no native suffix) |
| bcm2711 | `VENDOR_BYTES`; `NATIVE_BYTES` when the driver is built with video support, `capacity` is at least 176 and the firmware's video link is up when the open asks with that `capacity`; once an open has been given the 176-byte form it keeps getting it for its life, whatever the link does | 7 | `NATIVE_VIDEO_DECODE_H264` only |
| i915 | `VENDOR_BYTES`; `NATIVE_BYTES` when the device decodes video (the payload length is fixed at attach and does not depend on `capacity`) | 7 | `NATIVE_VIDEO_DECODE_H264` only |
| mali | `VENDOR_BYTES` | 7 | none (no native suffix) |
| ghci | `NATIVE_BYTES` | 7 | 0 (the native suffix is present with no flag set) |

The vendor flags 7 of the native drivers are `VENDOR_OPAQUE`,
`VENDOR_STRICT_QUEUE` and `VENDOR_QUIESCE`. A node without a renderer (for
example a display-only node) does not set `GPU_CAP_CAPSET`.

A Venus host that reports a `NATIVE_BYTES` payload with a vendor suffix is
read differently by the two readers: libvulkan reads the vendor suffix from
both lengths, but the kernel looks at it only in a `VENDOR_BYTES` payload, so
the kernel does not enable `GPU_JOB_RESERVE` for such a host and the request
fails with `ENOTSUP` even though libvulkan sees strict-queue flags.

## Constants

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_CAPSET_MAX` | 256 | The size of `data` in `struct gpu_capset` and the largest `capacity` a request may give. A capset payload is never longer than this. |

## Structures

#### `struct gpu_info`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. The reply carries the same value. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of the structure (56). The reply carries the same value. |
| 8 | 4 | `uint32_t` | `capabilities` | out | The node's capability bits, a mask of the `GPU_CAP_` values in [Capability bits](#capability-bits). Set by the kernel from the node's registered mask. |
| 12 | 4 | `uint32_t` | `max_resources` | out | The most resources one open file description can hold at once. `UINT32_MAX` means the driver sets no count limit. Never 0 for a current driver. |
| 16 | 8 | `uint64_t` | `max_resource_bytes` | out | The largest size in bytes of one resource. Never 0 for a current driver. |
| 24 | 32 | `char[32]` | `driver_name` | out | The driver's name as a NUL-terminated string of at most 31 characters; the unused bytes are zero. |

#### `struct gpu_capset`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. The reply carries the same value. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of the structure (280). The reply carries the same value. |
| 8 | 4 | `uint32_t` | `capset_id` | in | Selects the capset. Capset 4 is the one every driver has. The reply repeats the requested value. |
| 12 | 4 | `uint32_t` | `capset_version` | in | Selects the version of the capset. Capset 4 has version 0. The reply repeats the requested value. |
| 16 | 4 | `uint32_t` | `capacity` | in | The number of bytes of `data` the caller accepts, 1 to `GPU_CAPSET_MAX`. The reply repeats the requested value. |
| 20 | 4 | `uint32_t` | `bytes` | out | The length of the payload in `data`, in bytes, never more than `capacity`. Must be 0 on input, otherwise the request fails with `EINVAL`. |
| 24 | 256 | `uint8_t[256]` | `data` | out | The payload. Bytes from `bytes` on are zero. Its input value is not read. |

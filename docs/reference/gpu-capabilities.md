# GPU capabilities and capability sets

Status: experimental reference for GPU ABI version 1.

<!-- spec: One paragraph: a node advertises what it supports as capability
bits returned by GPU_GET_INFO, and a backend-defined capability set
(capset) returned by GPU_GET_CAPSET that the Vulkan driver reads to learn
the command protocol's version and the backend's features. Link back to
gpu.md for the shared rules. -->

PENDING

## Capability bits

<!-- spec: Fill the Meaning column of the table for every GPU_CAP_* bit:
which requests it enables (name them), what a caller may rely on when it
is set, and what happens when it is clear (the requests fail with the
errno the source returns; see gpu_ioctl_body). Then, below the table, the
rules: (1) The bits GPU_GET_INFO returns are the node's registered
capabilities (gpu_info_ioctl copies device->ops->capabilities; a value
the backend writes into the info request is not what the caller gets).
(2) Whether the bits can change during a node's life: read whether any
driver adds bits after drv_gpu_register publishes the node (the runtime
binding in amdrdna and nvrtx; see gpu-drivers.md) and state the result
exactly; if bits can be added later, say when a program must query again.
(3) GPU_CAP_HOST_BLOB's rule from its header comment (asking for
GPU_BLOB_HOST is always safe). (4) A bit that is set does not by itself
promise that every request it guards succeeds on every node: name any
case the source shows (a node may set GPU_CAP_JOB while GPU_JOB_RESERVE
answers ENOTSUP when the host lacks a strict queue; the capset's
STRICT_QUEUE flag is what tells a program). -->

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_CAP_RESOURCE` | 1 | PENDING |
| `GPU_CAP_CAPSET` | 2 | PENDING |
| `GPU_CAP_BLOB` | 4 | PENDING |
| `GPU_CAP_TRANSFER` | 8 | PENDING |
| `GPU_CAP_COMMAND` | 16 | PENDING |
| `GPU_CAP_PRESENT` | 32 | PENDING |
| `GPU_CAP_MAPPING` | 64 | PENDING |
| `GPU_CAP_DISPLAY` | 128 | PENDING |
| `GPU_CAP_SHARE` | 256 | PENDING |
| `GPU_CAP_NOTIFICATION` | 512 | PENDING |
| `GPU_CAP_ALLOCATION_SHARE` | 1024 | PENDING |
| `GPU_CAP_FENCE` | 2048 | PENDING |
| `GPU_CAP_DISPLAY_EVENTS` | 0x1000 | PENDING |
| `GPU_CAP_JOB` | 0x2000 | PENDING |
| `GPU_CAP_JOB_CAPACITY` | 0x4000 | PENDING |
| `GPU_CAP_DISPLAY_CONTROL` | 0x8000 | PENDING |
| `GPU_CAP_HOST_BLOB` | 0x10000 | PENDING |

PENDING

## Requests

### `GPU_GET_INFO`

<!-- spec: Argument struct gpu_info (Structures). Read gpu_info_ioctl. Cover: which fields are outputs, driver_name (NUL
termination, length), max_resources and max_resource_bytes (what limits
they are and which requests enforce them; zero meaning), the source of
capabilities (see Capability bits). It needs no capability bit. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_GET_CAPSET`

<!-- spec: Argument struct gpu_capset (Structures). Read gpu_capset_ioctl and the drivers' get_capset operations (venus.c
venus_get_capset, amdrdna/runtime.c runtime_capset, nvrtx/runtime.c,
powervr/render-runtime.c, bcm2711/render-runtime.c, mali, ghci/vk/vk-node.c;
find them with grep get_capset). Cover: inputs capset_id, capset_version,
capacity (bounds: GPU_CAPSET_MAX), outputs bytes and data; that every
current driver answers only capset id 4 version 0 (say what other ids
return); what happens when capacity is smaller than the capset; unused
bytes of data. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

## Capability set 4

<!-- spec: The payload of capset id 4, version 0, which every current
driver returns and the Vulkan driver reads (userland/desktop/libvulkan/
context.c). This layout is not in a public header; the documentation is
its reference. Cover: (1) the payload table below: fill Meaning for each
word (wire_version: the command stream's wire format version;
xml_version: the Vulkan registry version encoded with VK_MAKE_VERSION;
timelines: number of completion timelines; vendor_magic/vendor_flags:
present when bytes is VENDOR_BYTES or NATIVE_BYTES; native_magic/
native_flags: present when bytes is NATIVE_BYTES). Add rows for bytes 8 to
151 as one range row (the Venus renderer's fields: what a virtual
machine's host fills, what native drivers leave zero; read venus.c and a
native driver's capset function). All words are little-endian u32.
(2) The constants table: fill Meaning (the two payload sizes, the two
magic values, each flag: OPAQUE, STRICT_QUEUE, QUIESCE, HOST_SCANOUT,
VIDEO_DECODE_H264, SYNC_COMMAND; read what libvulkan does with each flag
and which drivers set it). Say that the names in the Capset constant
column are this document's names for the values (there is no header
macro). (3) Which drivers return the vendor or the native form (read each
driver's capset function; a table | Driver | bytes | vendor flags |
native flags |). -->

| Offset | Bytes | Payload field | Meaning |
| ---: | ---: | --- | --- |
| 0 | 4 | `wire_version` | PENDING |
| 4 | 4 | `xml_version` | PENDING |
| 152 | 4 | `timelines` | PENDING |
| 160 | 4 | `vendor_magic` | PENDING |
| 164 | 4 | `vendor_flags` | PENDING |
| 168 | 4 | `native_magic` | PENDING |
| 172 | 4 | `native_flags` | PENDING |

| Capset constant | Value | Meaning |
| --- | ---: | --- |
| `CAPSET_ID` | 4 | PENDING |
| `NATIVE_BYTES` | 176 | PENDING |
| `NATIVE_MAGIC` | 0x5a4e4154 | PENDING |
| `NATIVE_SYNC_COMMAND` | 2 | PENDING |
| `NATIVE_VIDEO_DECODE_H264` | 1 | PENDING |
| `VENDOR_BYTES` | 168 | PENDING |
| `VENDOR_HOST_SCANOUT` | 8 | PENDING |
| `VENDOR_MAGIC` | 0x5a424453 | PENDING |
| `VENDOR_OPAQUE` | 1 | PENDING |
| `VENDOR_QUIESCE` | 4 | PENDING |
| `VENDOR_STRICT_QUEUE` | 2 | PENDING |

PENDING

## Constants

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_CAPSET_MAX` | 256 | PENDING |

## Structures

#### `struct gpu_info`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `uint32_t` | `capabilities` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `max_resources` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `max_resource_bytes` | PENDING | PENDING |
| 24 | 32 | `char[32]` | `driver_name` | PENDING | PENDING |

#### `struct gpu_capset`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `uint32_t` | `capset_id` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `capset_version` | PENDING | PENDING |
| 16 | 4 | `uint32_t` | `capacity` | PENDING | PENDING |
| 20 | 4 | `uint32_t` | `bytes` | PENDING | PENDING |
| 24 | 256 | `uint8_t[256]` | `data` | PENDING | PENDING |

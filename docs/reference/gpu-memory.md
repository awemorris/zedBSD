# GPU memory: resources, blobs, transfers and mappings

Status: experimental reference for GPU ABI version 1.

<!-- spec: One paragraph: the memory objects an open creates (storage
resources and blobs), how they are named (handles local to the open file
description, resource ids for the renderer, blob ids), and the requests
that create, copy, map and destroy them. Link back to gpu.md. -->

PENDING

## Object model and lifetime

<!-- spec: From src/drivers/gpu/gpu.c (gpu_handle_allocate,
gpu_resource_lookup, gpu_resource_publish, gpu_resource_pin and unpin,
gpu_destroy_ioctl, gpu_close, gpu_mapping_release): (1) A handle is a
64-bit value valid only in the open that made it (and its dup and
SCM_RIGHTS copies, which share the open file description); a stale or
foreign handle never names another object. (2) Who owns an object, when
it is freed: on GPU_RESOURCE_DESTROY, on close of the last reference,
and what keeps it alive past destroy (mappings, display borrowing, shared
exports; say which). (3) Storage resources versus blobs: what each is
for, which requests accept which (GPU_PRESENT takes storage, not a blob;
display presentation in BLOB mode takes a blob). (4) Limits: max_resources
and max_resource_bytes of GPU_GET_INFO, GPU_COPY_MAX. (5) Device identity
and pairing (GPU_DEVICE_QUERY) as the base for sharing (gpu-sharing.md). -->

PENDING

## Mapping into a process

<!-- spec: From gpu_map_ioctl and gpu_mmap: GPU_RESOURCE_MAP returns a
session-local offset, never a physical address; mmap(2) with that offset
and length maps the object. Cover: which objects can be mapped (blobs
created with GPU_BLOB_MAPPABLE; storage resources? read the code), the
required GPU_CAP_MAPPING, the protection checks against the open's
readable and writable permission, the length and offset rules, caching
(GPU_BLOB_HOST, GPU_CAP_HOST_BLOB), what happens to a mapping when the
object is destroyed or the open closes, and the errno mmap returns for
each failure. -->

PENDING

## Requests

### `GPU_RESOURCE_CREATE`

<!-- spec: Argument struct gpu_resource_create (Structures). Read gpu_create_ioctl. Cover bytes (limits, alignment, zero), usage
(GPU_RESOURCE_USAGE_STORAGE and what other values do), flags (which are
accepted), the returned handle. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_RESOURCE_DESTROY`

<!-- spec: Argument struct gpu_resource_destroy (Structures). Read gpu_destroy_ioctl. Cover what destroy does to an object that is
still mapped, borrowed by a display, bound to pending work or exported. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_BLOB_CREATE`

<!-- spec: Argument struct gpu_blob_create (Structures). Read gpu_blob_ioctl. Cover bytes, blob_id (what the backend uses it for;
zero), the flags GPU_BLOB_MAPPABLE, GPU_BLOB_SHAREABLE,
GPU_BLOB_CROSS_DEVICE, GPU_BLOB_HOST (and which capability each needs),
outputs handle and resource_id (resource_id is the renderer's name used
in the command stream; zero meaning). State that the request is 40 bytes
and that GPU_BLOB_CREATE_PLACED is a separate 64-byte request. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_BLOB_CREATE_PLACED`

<!-- spec: Argument struct gpu_blob_create_placed (Structures). Read gpu_blob_ioctl's placed path and include/uapi/gpu-scanout.h. Cover
the outer size rule (blob.size is 64), placement flags DMA32, CONTIGUOUS,
COHERENT, max_dma_address (inclusive, zero for no limit), alignment (zero
or one for none, else a power of two on the first DMA address), that the
conditions are requirements (ENOTSUP when a backend cannot establish
them), and that zero conditions use the ordinary allocator. Say which
drivers implement the placed allocator (grep the ops). -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_RESOURCE_READ`

<!-- spec: Argument struct gpu_transfer (Structures). Read gpu_transfer_ioctl. Cover handle, offset, address (a user address
encoded as a 64-bit value), bytes (bounds, GPU_COPY_MAX), the direction
(resource to user memory), and EFAULT for a bad user address. Say whether
the copy is ordered after pending GPU work (it is not a synchronization
point unless the source shows one). -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_RESOURCE_WRITE`

<!-- spec: Argument struct gpu_transfer (Structures). Same structure as GPU_RESOURCE_READ in the other direction; refer to it
for the shared rules and list only the differences. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_RESOURCE_MAP`

<!-- spec: Argument struct gpu_resource_map (Structures). Read gpu_map_ioctl. Inputs handle and zero output fields; outputs offset
and bytes; how the offset is used with mmap (see Mapping into a process). -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_DEVICE_QUERY`

<!-- spec: Argument struct gpu_device_info (Structures). Read gpu_device_query_ioctl. Cover device_id (kernel-assigned, nonzero,
stable for the device's life, same for every node of one device?),
companion_id (preferred rendering companion; zero is no preference and
not proof that sharing fails), roles (GPU_DEVICE_RENDER,
GPU_DEVICE_DISPLAY), flags, reserved. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

## Constants

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_RESOURCE_USAGE_STORAGE` | 1 | PENDING |
| `GPU_BLOB_MAPPABLE` | 1 | PENDING |
| `GPU_BLOB_SHAREABLE` | 2 | PENDING |
| `GPU_BLOB_CROSS_DEVICE` | 4 | PENDING |
| `GPU_BLOB_HOST` | 8 | PENDING |
| `GPU_COPY_MAX` | 0x10000 | PENDING |
| `GPU_PLACEMENT_DMA32` | 1 | PENDING |
| `GPU_PLACEMENT_CONTIGUOUS` | 2 | PENDING |
| `GPU_PLACEMENT_COHERENT` | 4 | PENDING |
| `GPU_DEVICE_RENDER` | 1 | PENDING |
| `GPU_DEVICE_DISPLAY` | 2 | PENDING |

## Structures

#### `struct gpu_resource_create`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `bytes` | PENDING | PENDING |
| 16 | 4 | `uint32_t` | `usage` | PENDING | PENDING |
| 20 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `handle` | PENDING | PENDING |

#### `struct gpu_resource_destroy`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `handle` | PENDING | PENDING |

#### `struct gpu_blob_create`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `bytes` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `blob_id` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `handle` | PENDING | PENDING |
| 32 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 36 | 4 | `uint32_t` | `resource_id` | PENDING | PENDING |

#### `struct gpu_blob_create_placed`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 40 | `struct gpu_blob_create` | `blob` | PENDING | PENDING |
| 40 | 24 | `struct gpu_placement` | `placement` | PENDING | PENDING |

#### `struct gpu_placement`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `reserved` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `max_dma_address` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `alignment` | PENDING | PENDING |

#### `struct gpu_transfer`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `handle` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `offset` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `address` | PENDING | PENDING |
| 32 | 4 | `uint32_t` | `bytes` | PENDING | PENDING |
| 36 | 4 | `uint32_t` | `reserved` | PENDING | PENDING |

#### `struct gpu_resource_map`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `handle` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `offset` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `bytes` | PENDING | PENDING |

#### `struct gpu_device_info`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `device_id` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `companion_id` | PENDING | PENDING |
| 24 | 4 | `uint32_t` | `roles` | PENDING | PENDING |
| 28 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 32 | 8 | `uint64_t` | `reserved` | PENDING | PENDING |

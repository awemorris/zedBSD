# GPU sharing between opens and devices

Status: experimental reference for GPU ABI version 1.

<!-- spec: One paragraph: an open exports an object as a file descriptor
that another open (in the same or another process, on the same or
another GPU node) imports. Two forms exist: the resource form with an
image descriptor (GPU_RESOURCE_EXPORT/IMPORT, gpu.h) and the allocation
form with application metadata (GPU_ALLOCATION_EXPORT/IMPORT,
gpu-allocation.h). Link back to gpu.md. -->

PENDING

## Sharing model

<!-- spec: From src/drivers/gpu/gpu.c (gpu_export_ioctl, gpu_import_ioctl,
gpu_export_install, gpu_allocation_export_ioctl,
gpu_allocation_import_ioctl, gpu_allocation_validate, gpu_image_validate,
gpu_scanout_import, gpu_shared_hold_device, gpu_shared_release,
drv_gpu_shared_backing): (1) The exported descriptor is a capability for
the whole allocation; it is independent of the exporting open (closing
the exporter does not revoke it) and the importer gets its own resource.
(2) Descriptor flags GPU_HANDLE_CLOEXEC and GPU_HANDLE_CLOFORK. (3) The fd
is installed only after the whole output is copied out (a failed copyout
installs nothing). (4) The image descriptor and allocation descriptor are
immutable once exported; the kernel assigns device_id; import returns the
stored description, not the caller's. (5) Cross-device import: what
GPU_BLOB_CROSS_DEVICE, GPU_SCANOUT_FOREIGN and the companion pairing
mean, and when import fails between devices. (6) Scanout import
(GPU_IMPORT_SCANOUT): a display node checks the image against its
constraints (GPU_DISPLAY_CONSTRAINTS in gpu-display.md); a native-only
import returns resource_id zero. (7) Capabilities: GPU_CAP_SHARE and
GPU_CAP_ALLOCATION_SHARE, and which side needs which. (8) The supplied fd
stays owned by the caller on import. -->

PENDING

## Requests

### `GPU_RESOURCE_EXPORT`

<!-- spec: Argument struct gpu_resource_export (Structures). Inputs handle, flags, fd = -1, image (which fields the caller fills and
that image.device_id is zero on input); outputs fd and the image with
device_id. Read gpu_export_ioctl and gpu_image_validate for every check
on the image fields (format, stride, offset, allocation_bytes, tiling
GPU_IMAGE_LINEAR, memory_type, usage). -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_RESOURCE_IMPORT`

<!-- spec: Argument struct gpu_resource_import (Structures). Inputs version, size, fd, flags (zero: renderer import; GPU_IMPORT_SCANOUT:
checked display import); outputs handle, resource_id, image. Read
gpu_import_ioctl and gpu_scanout_import. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_ALLOCATION_EXPORT`

<!-- spec: Argument struct gpu_allocation_export (Structures). Read gpu_allocation_export_ioctl and gpu_allocation_validate. Cover
schema and metadata (owned by the communicating libraries, opaque to the
kernel, unused bytes zero, bound GPU_ALLOCATION_METADATA_MAX), that
metadata never narrows the grant or proves scanout eligibility, and that
the kernel checks allocation_bytes against the resource. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_ALLOCATION_IMPORT`

<!-- spec: Argument struct gpu_allocation_import (Structures). Read gpu_allocation_import_ioctl. Only version, size and fd are inputs;
outputs handle, resource_id and the stored descriptor. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

## Constants

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_HANDLE_CLOEXEC` | 1 | PENDING |
| `GPU_HANDLE_CLOFORK` | 2 | PENDING |
| `GPU_IMAGE_LINEAR` | 1 | PENDING |
| `GPU_IMPORT_SCANOUT` | 1 | PENDING |
| `GPU_ALLOCATION_METADATA_MAX` | 128 | PENDING |

## Structures

#### `struct gpu_image_descriptor`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `uint32_t` | `width` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `height` | PENDING | PENDING |
| 16 | 4 | `uint32_t` | `format` | PENDING | PENDING |
| 20 | 4 | `uint32_t` | `stride` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `offset` | PENDING | PENDING |
| 32 | 8 | `uint64_t` | `allocation_bytes` | PENDING | PENDING |
| 40 | 4 | `uint32_t` | `memory_type` | PENDING | PENDING |
| 44 | 4 | `uint32_t` | `usage` | PENDING | PENDING |
| 48 | 4 | `uint32_t` | `tiling` | PENDING | PENDING |
| 52 | 4 | `uint32_t` | `reserved` | PENDING | PENDING |
| 56 | 8 | `uint64_t` | `device_id` | PENDING | PENDING |

#### `struct gpu_resource_export`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `handle` | PENDING | PENDING |
| 16 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 20 | 4 | `int32_t` | `fd` | PENDING | PENDING |
| 24 | 64 | `struct gpu_image_descriptor` | `image` | PENDING | PENDING |

#### `struct gpu_resource_import`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `int32_t` | `fd` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `handle` | PENDING | PENDING |
| 24 | 4 | `uint32_t` | `resource_id` | PENDING | PENDING |
| 28 | 4 | `uint32_t` | `reserved` | PENDING | PENDING |
| 32 | 64 | `struct gpu_image_descriptor` | `image` | PENDING | PENDING |

#### `struct gpu_allocation_descriptor`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `allocation_bytes` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `device_id` | PENDING | PENDING |
| 24 | 4 | `uint32_t` | `schema` | PENDING | PENDING |
| 28 | 4 | `uint32_t` | `metadata_bytes` | PENDING | PENDING |
| 32 | 128 | `uint8_t[128]` | `metadata` | PENDING | PENDING |

#### `struct gpu_allocation_export`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `handle` | PENDING | PENDING |
| 16 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 20 | 4 | `int32_t` | `fd` | PENDING | PENDING |
| 24 | 160 | `struct gpu_allocation_descriptor` | `allocation` | PENDING | PENDING |

#### `struct gpu_allocation_import`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `int32_t` | `fd` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `handle` | PENDING | PENDING |
| 24 | 4 | `uint32_t` | `resource_id` | PENDING | PENDING |
| 28 | 4 | `uint32_t` | `reserved` | PENDING | PENDING |
| 32 | 160 | `struct gpu_allocation_descriptor` | `allocation` | PENDING | PENDING |

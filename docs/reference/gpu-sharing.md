# GPU sharing between opens and devices

Status: experimental reference for GPU ABI version 1.

An open of a GPU node can hand an allocation to another open, in the same process or another one, on the same node or on another GPU node, by exporting it as a file descriptor that the other open imports. Two forms exist. The resource form carries an image description and is the one used to show an allocation on a display: `GPU_RESOURCE_EXPORT` and `GPU_RESOURCE_IMPORT`, with the `GPU_IMPORT_SCANOUT` flag for a display that scans out memory of another device. The allocation form carries a description that only the communicating libraries understand: `GPU_ALLOCATION_EXPORT` and `GPU_ALLOCATION_IMPORT`. The two forms do not mix: the kernel keeps one kind of description in each descriptor and refuses to import it with the other form's request. The rules shared by all requests (framing, permissions, concurrency) are in [the device interface](gpu.md); creating the blobs that are exported is in [memory](gpu-memory.md), and showing an imported allocation is in [display](gpu-display.md).

## Sharing model

##### What the descriptor is

An export creates a kernel object that holds one reference on the allocation's storage and one on the exporting device, and installs a descriptor for it in the calling process. The descriptor is a capability for the whole allocation. A holder can import all of it, and nothing in the description narrows that: the image fields of the resource form and the metadata of the allocation form are information for the importer, not a security boundary, and the metadata never proves that the memory can be scanned out.

The descriptor is independent of the open that exported it. Destroying the source resource, or closing the source open, does not revoke the descriptor and does not change what an importer receives; the storage lives until the descriptor and every imported resource are gone. An import creates a new resource in the importing open, with a new handle that is local to that open. The resource names the same storage as the source, not a copy, and it counts against the importing open's limits (`max_resources` and `max_resource_bytes` of `GPU_GET_INFO`, see [capabilities](gpu-capabilities.md)). Destroying it, or closing that open, ends only that import. The same descriptor can be imported any number of times, into one open or into several.

A live descriptor also keeps the exporting device from being torn down. While one exists, a failed device cannot recover in place on a new open (see Session failure and recovery in [submission](gpu-submission.md)). After the device is withdrawn, new exports and imports on it fail with `ENODEV`.

A descriptor of the allocation form can also be given to a kernel driver that reads the allocation's pages. The camera driver does this for the buffers it fills (see [camera](camera.md)); a driver that cannot describe the pages refuses the descriptor.

##### Descriptor flags

The `flags` of an export select the descriptor's inheritance. `GPU_HANDLE_CLOEXEC` closes the descriptor when the process runs a new program, and `GPU_HANDLE_CLOFORK` keeps it out of the child created by `fork`. With neither bit the descriptor is kept across both. Any other bit fails with `EINVAL`. Apart from these bits the descriptor is an ordinary entry of the process's descriptor table, and closing it releases that entry's reference on the capability.

##### Publication of the descriptor

An export does its work in a fixed order. The kernel validates the description, asks the driver for an export of the allocation, reserves the lowest free descriptor number, copies the complete output (including that number in `fd` and the kernel-assigned `device_id`) to the caller, and only then installs the descriptor. If the copy fails, the reservation is withdrawn, no descriptor exists and the driver's export is released again. A failed export therefore leaves nothing behind, and the `fd` field of the caller's structure names nothing. On input `fd` must be -1.

On import the descriptor that the caller passes in `fd` is only read. The kernel takes its own reference for the duration of the call, and the descriptor stays owned by the caller whether the import succeeds or fails. The caller may close it at once; the imported resource does not depend on it.

##### Immutable description

The description (the image descriptor or the allocation descriptor) is fixed when the descriptor is exported and never changes. The exporter supplies the description's fields and the kernel checks them against the resource; the exporter must leave `device_id` zero, and the kernel assigns the identity of the exporting device. An import never takes a description from its caller: the description field of the import structure must be all zero on input, otherwise the request fails with `EINVAL`, and on success the kernel returns the stored description.

Only a blob can be exported. A resource made by `GPU_RESOURCE_CREATE`, and a resource that an import for a display returned (see Scanout import), cannot.

##### Device identity and cross-device import

Every GPU node has a `device_id` that the kernel assigns when the node is registered, from the counter that also issues resource handles, so an identity is never reused while the system runs. A node reports its own with `GPU_DEVICE_QUERY` ([memory](gpu-memory.md)), and the descriptor carries the identity of the exporting node in `device_id`. Two nodes of one physical GPU, such as a render node and a display node, are separate nodes with separate identities.

An import on the same node as the export is an ordinary import: the driver makes an alias of the allocation for the importing open. An import on a different node fails with `EXDEV`, with one exception: `GPU_RESOURCE_IMPORT` with `GPU_IMPORT_SCANOUT` on a node that has a display, which asks that display to scan out the memory of the other node (see Scanout import). An allocation descriptor never crosses nodes. Which cross-device names matter to a program:

- `GPU_BLOB_CROSS_DEVICE` is a flag of blob creation. It must be set together with `GPU_BLOB_SHAREABLE`, and either sharing flag needs `GPU_CAP_SHARE` or `GPU_CAP_ALLOCATION_SHARE` on the node ([memory](gpu-memory.md)). The export and import requests do not test the flag. It tells the driver that the program means to use the blob with another device; what that changes is the driver's. Some drivers refuse an image export of a blob that was created without it.
- `GPU_SCANOUT_FOREIGN` is a route bit that `GPU_DISPLAY_CONSTRAINTS` reports for a display ([display](gpu-display.md)). It says that the display may take backing from another device. It is information for the program's choice of route; the import request does not read it, and it does not promise that an arbitrary allocation imports.
- The companion identity of `GPU_DEVICE_QUERY` (`companion_id`) is the preferred rendering companion that a driver may name for a node. The kernel does not consult it on import. Zero means no preference, and a non-zero value is not proof that sharing works; it only guides which node the program allocates on.

##### Scanout import

`GPU_RESOURCE_IMPORT` with `GPU_IMPORT_SCANOUT` is the checked import for a display. A program queries the constraints of the output with `GPU_DISPLAY_CONSTRAINTS` before it allocates, so that the allocation meets the alignments, placement and address limit the display reports. The import itself is the final check: the display driver examines this exact allocation (address reach, alignment, cache behaviour, format) and refuses it with its own error if it cannot scan it out. The program can then use the copy route (`GPU_SCANOUT_COPY`).

For an allocation from another node the kernel needs two things besides the display: the destination node must have `GPU_CAP_DISPLAY` and an import operation, and the descriptor must be of the resource form (the allocation form fails with `EINVAL`), and the source node must be able to describe the exported memory as physical pages. Not every driver can, and one that cannot answers `ENOTSUP`. The kernel rejects a page description that is incomplete or shorter than the image's `allocation_bytes` with `EIO` before the display driver sees it, and passes the display driver's refusal through. A successful import of this kind returns `resource_id` zero: the resource is a native display import. It is a handle for `GPU_DISPLAY_PRESENT` with `GPU_DISPLAY_PRESENT_BLOB` and for display planes, and it has no identity in a renderer's command stream. The kernel keeps the exported capability referenced for as long as that resource exists, so the memory stays alive after the program closes the descriptor, until the resource is destroyed or the open is closed.

An import with `GPU_IMPORT_SCANOUT` from the same node is an ordinary import (with a non-zero `resource_id`) in which the driver is asked for a resource that its own display can scan out. The driver may refuse with `ENOTSUP` for an allocation its display cannot scan; the program then copies.

##### Capabilities

`GPU_CAP_SHARE` ([capabilities](gpu-capabilities.md)) is the resource form: the exporting node needs it, and so does an importing node, except that a display node whose driver offers scanout import accepts `GPU_RESOURCE_IMPORT` without it. `GPU_CAP_ALLOCATION_SHARE` is the allocation form, and a node needs it for both `GPU_ALLOCATION_EXPORT` and `GPU_ALLOCATION_IMPORT`. A node can offer either, both or neither. A node without the capability that a request needs answers `EOPNOTSUPP` before it reads the argument, so a program tests the node's capability bits rather than try the request.

## Requests

### `GPU_RESOURCE_EXPORT`

Requires: open mode W; capability `GPU_CAP_SHARE`.

Exports the blob named by `handle` as a descriptor in the resource form and returns the descriptor in `fd`. The source resource stays valid and usable. The blob must belong to the calling open: a handle of another open, a stale or zero handle, a resource made by `GPU_RESOURCE_CREATE` and a resource returned by a display import all fail with `EINVAL`. Whether the driver also requires the blob to have been created with `GPU_BLOB_SHAREABLE` is the driver's decision.

The caller fills `version`, `size`, `handle` and `flags`, sets `fd` to -1, and fills `image` except `device_id`, which it leaves zero, and `reserved`, which it leaves zero. The image describes one linear picture inside the blob: `width` by `height` pixels in `format` (`GPU_PIXEL_BGRA8888` or `GPU_PIXEL_RGBA8888`), rows `stride` bytes apart, starting `offset` bytes into the blob. `allocation_bytes` must be the size of the blob exactly, which is the size of the whole grant. `tiling` must be `GPU_IMAGE_LINEAR`. `memory_type` is a Vulkan memory type index, below 32. `usage` is a non-zero mask of the eight Vulkan 1.0 image-usage bits (a value above 255 is refused). The kernel stores `memory_type` and `usage` and checks only their ranges; the driver may use them. `stride` must be at least four times `width` and a multiple of four, `offset` must fit in 32 bits, and `offset` plus `stride` times `height` must not pass the end of the blob, so every pixel the image describes lies inside the allocation. The kernel puts no upper bound on `width` and `height` apart from `width` times four fitting in 32 bits; a driver may refuse sizes it cannot scan out.

On success `fd` holds the new descriptor and `image` is returned with `device_id` set to the identity of the exporting node; the other fields are returned as given. A driver may export the same blob more than once. A driver that keeps one image per allocation refuses a later export whose image differs. On failure no descriptor exists, and the `fd` field names nothing.

The request runs under the open's admission and waits (interruptibly) for it.

Errors:

- `ENODEV`: the device was withdrawn, or the device or this open failed with `ENODEV`.
- `EIO`: the device or this open has failed and the recorded errno is returned (`EIO` and `ETIMEDOUT` are values the kernel itself records, and a driver can record others); or the driver reported success without an exported object.
- `EINTR`: a signal arrived while the request waited for the open's admission.
- `EACCES`: the open is not writable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_SHARE`; or `tiling` is not `GPU_IMAGE_LINEAR`, `memory_type` is 32 or more, or `format` is not one of the two pixel formats.
- `EFAULT`: the argument cannot be read, or the output cannot be written back (no descriptor is installed).
- `EINVAL`: `version` or `size` is not the structure's, or the same fields of `image` are wrong; `fd` is not -1; `flags` has a bit other than the two `GPU_HANDLE_*` bits; `handle` is zero or names no blob of this open; `image.device_id` or `image.reserved` is not zero; `image.allocation_bytes` differs from the blob's size; `usage` is zero or above 255; `width` or `height` is zero, or `width` is above 1073741823; `stride` is below four times `width` or not a multiple of four; `offset` is above 4294967295 or above the blob's size; `stride` times `height` is larger than the blob's size minus `offset`.
- `ENOMEM`: the kernel could not allocate its record of the export.
- `EOVERFLOW`: the device already has the largest possible number of live exports.
- `EMFILE`: the process has no free descriptor number below its limit.
- `ESRCH`: the calling thread has no process descriptor table.
- Other values: the driver's export operation returns its own errno unchanged, and so does the descriptor table when it cannot reserve or install the number. In-tree drivers return `EOPNOTSUPP` for a blob that was not created with `GPU_BLOB_SHAREABLE`, `EOPNOTSUPP` for an image smaller than 16 or larger than 4096 pixels on a side (one driver), `EINVAL` for a resource that is not a blob or for a second image that differs from the first, and `ENODEV` when the driver's transport has failed.

Sources: [gpu.h](../../include/uapi/gpu.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_export_ioctl`, `gpu_image_validate`, `gpu_export_install`, `gpu_shared_hold_device`, `gpu_shared_release`), [display-scanout](../../userland/tests/display-scanout/main.c), [gpu-check.c](../../userland/tests/amdrdnactl/gpu-check.c).

### `GPU_RESOURCE_IMPORT`

Requires: open mode W; capability none.

Turns the descriptor in `fd`, made by `GPU_RESOURCE_EXPORT`, into a new resource of the calling open, and returns its `handle`, its `resource_id` and the stored image description. The caller fills `version`, `size`, `fd` and `flags`; every other field, including the whole `image`, must be zero. The descriptor stays owned by the caller. Resource accounting applies: the new resource counts against the open's `max_resources` and the allocation's size against `max_resource_bytes`.

The request has no capability bit that every node needs. A node accepts it when it offers `GPU_CAP_SHARE`, or when its display driver has an import operation (a display-only node, whose driver cannot export). Which of the two paths runs depends on `flags`, and on whether the descriptor was exported by this node.

`flags` zero, descriptor from this node: the driver makes an alias of the allocation in the open. `handle` is the new resource's handle, `resource_id` is a non-zero identity that a command stream of this open can use for the resource, and `image` is the stored description with `device_id`. The alias is a blob resource of this open. A descriptor from another node fails with `EXDEV`. The descriptor must have been made by an export of this form; a descriptor of the allocation form, and a descriptor that is not a GPU capability at all (a closed number, a file, a handle of another subsystem), fail with `EINVAL`.

`flags` set to `GPU_IMPORT_SCANOUT`: the import is for a display. From this node it is an ordinary import with the display's preference (see Scanout import); from another node it is the checked native import that returns `resource_id` zero. Any other bit in `flags` fails with `EINVAL`.

On failure nothing is returned and the open gains no resource. If the output cannot be copied out, the new resource is destroyed again.

The request runs under the open's admission and waits (interruptibly) for it.

Errors:

- `ENODEV`: the device was withdrawn, or the device or this open failed with `ENODEV`.
- `EIO`: the device or this open has failed and the recorded errno is returned (`EIO` and `ETIMEDOUT` are values the kernel itself records, and a driver can record others); or the driver returned success without a resource or, for an import from this node, without a `resource_id`; or the source's page description for a display import is incomplete or smaller than the image.
- `EINTR`: a signal arrived while the request waited for the open's admission.
- `EACCES`: the open is not writable.
- `EOPNOTSUPP`: the node has neither `GPU_CAP_SHARE` nor a display import operation; or the import is a display import from another node and the destination has no display or no import operation, or the source driver cannot describe the memory as pages; or, from this node, the driver has no sharing operations.
- `EFAULT`: the argument cannot be read, or the output cannot be written back.
- `EINVAL`: `version` or `size` is not the structure's; `fd` is negative; `flags` has a bit other than `GPU_IMPORT_SCANOUT`; `handle`, `resource_id` or `reserved` is not zero; `image` is not all zero; `fd` is not an open descriptor, is not a GPU capability, or is a capability of the allocation form; the allocation is larger than the node's `max_resource_bytes`.
- `EXDEV`: the descriptor was exported by another node and `flags` does not contain `GPU_IMPORT_SCANOUT`.
- `ENOSPC`: the open already has `max_resources` resources.
- `ENOMEM`: the kernel could not allocate the resource.
- `EOVERFLOW`: the counter that issues handles is exhausted.
- Other values: the driver's operations return their own errno unchanged: the source driver's page description (`ENOTSUP` for an allocation without an image), the display driver's import (its refusal of an allocation it cannot scan), the driver's import of an alias (`ENODEV` when its transport has failed, `ENOTSUP` for `GPU_IMPORT_SCANOUT` on a driver whose display cannot scan the memory), and the error of the node's information query.

Sources: [gpu.h](../../include/uapi/gpu.h), [gpu-scanout.h](../../include/uapi/gpu-scanout.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_import_ioctl`, `gpu_scanout_import`, `gpu_resource_reserve`), [display-scanout](../../userland/tests/display-scanout/main.c), [gpu-check.c](../../userland/tests/amdrdnactl/gpu-check.c).

### `GPU_ALLOCATION_EXPORT`

Requires: open mode W; capability `GPU_CAP_ALLOCATION_SHARE`.

Exports the blob named by `handle` as a descriptor in the allocation form and returns the descriptor in `fd`. The form carries no image: it makes no claim about layout, tiling or pixel format, and it cannot be scanned out. What it carries is the size of the allocation and a description that the libraries on the two sides agree on. The source resource stays valid. The blob must belong to the calling open, and the other conditions on the source resource are those of `GPU_RESOURCE_EXPORT`.

The caller fills `version`, `size`, `handle` and `flags`, sets `fd` to -1, and fills `allocation`. `allocation.version` and `allocation.size` describe the descriptor itself. `allocation_bytes` must be the size of the blob exactly. `device_id` is left zero. `schema` is a non-zero number that identifies the format of `metadata`; the libraries on both sides define it and the kernel does not interpret it. `metadata_bytes` is the number of used bytes of `metadata`, at most `GPU_ALLOCATION_METADATA_MAX`, and the unused bytes must be zero so that the whole descriptor has one representation. The kernel stores the bytes unchanged. Metadata does not narrow the grant and does not make the allocation eligible for scanout.

On success `fd` holds the new descriptor and `allocation` is returned with `device_id` set to the identity of the exporting node. The request does not read an image, so a driver is asked for an export without one. Failure behaves as for `GPU_RESOURCE_EXPORT`: no descriptor exists, and the `fd` field names nothing.

The request runs under the open's admission and waits (interruptibly) for it.

Errors:

- `ENODEV`: the device was withdrawn, or the device or this open failed with `ENODEV`.
- `EIO`: the device or this open has failed and the recorded errno is returned (`EIO` and `ETIMEDOUT` are values the kernel itself records, and a driver can record others); or the driver reported success without an exported object.
- `EINTR`: a signal arrived while the request waited for the open's admission.
- `EACCES`: the open is not writable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_ALLOCATION_SHARE`.
- `EFAULT`: the argument cannot be read, or the output cannot be written back (no descriptor is installed).
- `EINVAL`: `version` or `size` is not the structure's, or the same fields of `allocation` are wrong; `fd` is not -1; `flags` has a bit other than the two `GPU_HANDLE_*` bits; `handle` is zero or names no blob of this open; `allocation.device_id` is not zero; `allocation_bytes` differs from the blob's size; `schema` is zero; `metadata_bytes` is above `GPU_ALLOCATION_METADATA_MAX`; a byte of `metadata` at or after `metadata_bytes` is not zero.
- `ENOMEM`: the kernel could not allocate its record of the export.
- `EOVERFLOW`: the device already has the largest possible number of live exports.
- `EMFILE`: the process has no free descriptor number below its limit.
- `ESRCH`: the calling thread has no process descriptor table.
- Other values: the driver's export operation returns its own errno unchanged, and so does the descriptor table when it cannot reserve or install the number. In-tree drivers return `EOPNOTSUPP` for a blob that was not created with `GPU_BLOB_SHAREABLE` and `EINVAL` for a resource that is not a blob.

Sources: [gpu-allocation.h](../../include/uapi/gpu-allocation.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_allocation_export_ioctl`, `gpu_allocation_validate`, `gpu_export_install`, `gpu_shared_hold_device`), no source-tree test.

### `GPU_ALLOCATION_IMPORT`

Requires: open mode W; capability `GPU_CAP_ALLOCATION_SHARE`.

Turns the descriptor in `fd`, made by `GPU_ALLOCATION_EXPORT`, into a new resource of the calling open. Only `version`, `size` and `fd` are inputs; `flags` must be zero and `handle`, `resource_id`, `reserved` and the whole `allocation` must be zero. On success `handle` is the new resource's handle, `resource_id` is a non-zero identity that a command stream of this open can use for the resource, and `allocation` is the description the exporter stored: the importer reads the sender's `schema` and `metadata` from it and decides whether it understands them. Metadata supplied by the importer would be refused, so the stored description is the only source.

The descriptor must be made by this node's export of the allocation form. A descriptor of the resource form fails with `EINVAL`, a descriptor from another node fails with `EXDEV` (the allocation form does not cross nodes), and a number that is not a GPU capability fails with `EINVAL`. The descriptor stays owned by the caller. The new resource counts against the open's `max_resources` and `max_resource_bytes`. On failure nothing is returned and the open gains no resource; if the output cannot be copied out, the new resource is destroyed again.

The request runs under the open's admission and waits (interruptibly) for it.

Errors:

- `ENODEV`: the device was withdrawn, or the device or this open failed with `ENODEV`.
- `EIO`: the device or this open has failed and the recorded errno is returned (`EIO` and `ETIMEDOUT` are values the kernel itself records, and a driver can record others); or the driver returned success without a resource or a `resource_id`.
- `EINTR`: a signal arrived while the request waited for the open's admission.
- `EACCES`: the open is not writable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_ALLOCATION_SHARE`.
- `EFAULT`: the argument cannot be read, or the output cannot be written back.
- `EINVAL`: `version` or `size` is not the structure's; `fd` is negative; `flags`, `handle`, `resource_id` or `reserved` is not zero; `allocation` is not all zero; `fd` is not an open descriptor, is not a GPU capability, or is a capability of the resource form; the allocation is larger than the node's `max_resource_bytes`.
- `EXDEV`: the descriptor was exported by another node.
- `ENOSPC`: the open already has `max_resources` resources.
- `ENOMEM`: the kernel could not allocate the resource.
- `EOVERFLOW`: the counter that issues handles is exhausted.
- Other values: the driver's import operation returns its own errno unchanged (`ENODEV` when its transport has failed, for example), and so does the node's information query.

Sources: [gpu-allocation.h](../../include/uapi/gpu-allocation.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_allocation_import_ioctl`, `gpu_resource_reserve`, `gpu_shared_operations`, `drv_gpu_shared_backing`), no source-tree test.

## Constants

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_HANDLE_CLOEXEC` | 1 | Export flag: the descriptor is closed when the process runs a new program. The fence requests use the same bit. |
| `GPU_HANDLE_CLOFORK` | 2 | Export flag: the descriptor is not copied into the child created by `fork`. The fence requests use the same bit. |
| `GPU_IMAGE_LINEAR` | 1 | Value of the `tiling` field of an image descriptor: the rows follow each other `stride` bytes apart with no tiling. It is the only value an export accepts. |
| `GPU_IMPORT_SCANOUT` | 1 | Import flag of `GPU_RESOURCE_IMPORT`: the import is for a display. From another node it is the checked native import that returns `resource_id` zero; from this node it asks the driver for a resource its display can scan out. |
| `GPU_ALLOCATION_METADATA_MAX` | 128 | Size in bytes of the `metadata` array of an allocation descriptor, and the largest value of `metadata_bytes`. |

## Structures

#### `struct gpu_image_descriptor`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in/out | Version of this record; `GPU_ABI_VERSION`. The exporter sets it, and an import returns it. All zero on input to an import. |
| 4 | 4 | `uint32_t` | `size` | in/out | Size of this record in bytes, 64. The exporter sets it, and an import returns it. |
| 8 | 4 | `uint32_t` | `width` | in/out | Width of the image in pixels; from 1 to 1073741823. Set by the exporter. |
| 12 | 4 | `uint32_t` | `height` | in/out | Height of the image in pixels; not zero. Set by the exporter. |
| 16 | 4 | `uint32_t` | `format` | in/out | Pixel format: `GPU_PIXEL_BGRA8888` or `GPU_PIXEL_RGBA8888`; a four-byte pixel. Set by the exporter. |
| 20 | 4 | `uint32_t` | `stride` | in/out | Distance in bytes between the starts of two rows; at least four times `width` and a multiple of four. Set by the exporter. |
| 24 | 8 | `uint64_t` | `offset` | in/out | Byte offset of the first pixel inside the allocation; at most 4294967295 and not beyond the end of the allocation. Set by the exporter. |
| 32 | 8 | `uint64_t` | `allocation_bytes` | in/out | Size of the whole allocation in bytes; equal to the size of the exported blob. `offset` plus `stride` times `height` must not pass it. Set by the exporter. |
| 40 | 4 | `uint32_t` | `memory_type` | in/out | Vulkan memory type index of the allocation; below 32. Stored by the kernel, which checks only the range. Set by the exporter. |
| 44 | 4 | `uint32_t` | `usage` | in/out | Mask of the eight Vulkan 1.0 image-usage bits the image is made for; not zero and at most 255. Stored by the kernel, which checks only the range. Set by the exporter. |
| 48 | 4 | `uint32_t` | `tiling` | in/out | Tiling of the image; `GPU_IMAGE_LINEAR`, the only accepted value. Set by the exporter. |
| 52 | 4 | `uint32_t` | `reserved` | zero | Must be zero. |
| 56 | 8 | `uint64_t` | `device_id` | out | Identity of the exporting node, assigned by the kernel and never zero on output. Must be zero on input to an export. |

#### `struct gpu_resource_export`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_resource_export)`, 88). |
| 8 | 8 | `uint64_t` | `handle` | in | Handle of a blob of this open to export; not zero. |
| 16 | 4 | `uint32_t` | `flags` | in | Inheritance of the new descriptor: zero or a combination of `GPU_HANDLE_CLOEXEC` and `GPU_HANDLE_CLOFORK`. |
| 20 | 4 | `int32_t` | `fd` | in/out | Must be -1 on input. On success the number of the new descriptor. |
| 24 | 64 | `struct gpu_image_descriptor` | `image` | in/out | The image the blob holds, filled by the caller; the kernel returns it with `device_id` set. |

#### `struct gpu_resource_import`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_resource_import)`, 96). |
| 8 | 4 | `int32_t` | `fd` | in | Descriptor made by `GPU_RESOURCE_EXPORT`; not negative. It stays owned by the caller. |
| 12 | 4 | `uint32_t` | `flags` | in | Zero for a renderer import, or `GPU_IMPORT_SCANOUT` for an import for a display. |
| 16 | 8 | `uint64_t` | `handle` | out | Handle of the new resource, local to this open and not zero. Must be zero on input. |
| 24 | 4 | `uint32_t` | `resource_id` | out | Identity of the resource for a command stream of this open; zero for a native display import from another node. Must be zero on input. |
| 28 | 4 | `uint32_t` | `reserved` | zero | Must be zero. |
| 32 | 64 | `struct gpu_image_descriptor` | `image` | out | The description stored by the exporter, with its `device_id`. All zero on input. |

#### `struct gpu_allocation_descriptor`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in/out | Version of this record; `GPU_ABI_VERSION`. Set by the exporter and returned by an import. All zero on input to an import. |
| 4 | 4 | `uint32_t` | `size` | in/out | Size of this record in bytes, 160. Set by the exporter and returned by an import. |
| 8 | 8 | `uint64_t` | `allocation_bytes` | in/out | Size of the whole allocation in bytes; equal to the size of the exported blob. Set by the exporter. |
| 16 | 8 | `uint64_t` | `device_id` | out | Identity of the exporting node, assigned by the kernel and never zero on output. Must be zero on input to an export. |
| 24 | 4 | `uint32_t` | `schema` | in/out | Number that identifies the format of `metadata`, chosen by the communicating libraries; not zero. The kernel does not interpret it. Set by the exporter. |
| 28 | 4 | `uint32_t` | `metadata_bytes` | in/out | Number of used bytes of `metadata`; at most `GPU_ALLOCATION_METADATA_MAX`. Set by the exporter. |
| 32 | 128 | `uint8_t[128]` | `metadata` | in/out | Application data for the importing library, stored unchanged and opaque to the kernel. Bytes at or after `metadata_bytes` must be zero. Set by the exporter. |

#### `struct gpu_allocation_export`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_allocation_export)`, 184). |
| 8 | 8 | `uint64_t` | `handle` | in | Handle of a blob of this open to export; not zero. |
| 16 | 4 | `uint32_t` | `flags` | in | Inheritance of the new descriptor: zero or a combination of `GPU_HANDLE_CLOEXEC` and `GPU_HANDLE_CLOFORK`. |
| 20 | 4 | `int32_t` | `fd` | in/out | Must be -1 on input. On success the number of the new descriptor. |
| 24 | 160 | `struct gpu_allocation_descriptor` | `allocation` | in/out | The description of the allocation, filled by the caller; the kernel returns it with `device_id` set. |

#### `struct gpu_allocation_import`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | ABI version; `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Size of the structure in bytes (`sizeof(struct gpu_allocation_import)`, 192). |
| 8 | 4 | `int32_t` | `fd` | in | Descriptor made by `GPU_ALLOCATION_EXPORT`; not negative. It stays owned by the caller. |
| 12 | 4 | `uint32_t` | `flags` | zero | Must be zero. |
| 16 | 8 | `uint64_t` | `handle` | out | Handle of the new resource, local to this open and not zero. Must be zero on input. |
| 24 | 4 | `uint32_t` | `resource_id` | out | Identity of the resource for a command stream of this open; not zero. Must be zero on input. |
| 28 | 4 | `uint32_t` | `reserved` | zero | Must be zero. |
| 32 | 160 | `struct gpu_allocation_descriptor` | `allocation` | out | The description stored by the exporter, with its `device_id`. All zero on input. |

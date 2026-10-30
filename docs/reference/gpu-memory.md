# GPU memory: resources, blobs, transfers and mappings

Status: experimental reference for GPU ABI version 1.

An open of a GPU node owns the memory objects it creates. There are two kinds. A storage resource is plain memory that the program fills and reads with copy requests; a blob is memory that the node's driver allocates for the renderer, and which can also be mapped into the process and exported. Both are named to the kernel by a 64-bit handle that is local to the open. A blob also carries a resource id, the name by which the renderer's command stream refers to it, and may carry a blob id, with which the program tells the driver what the blob stands for. `GPU_RESOURCE_CREATE` makes a storage resource, `GPU_BLOB_CREATE` and `GPU_BLOB_CREATE_PLACED` make blobs, `GPU_RESOURCE_READ` and `GPU_RESOURCE_WRITE` copy bytes between a resource and the caller's memory, `GPU_RESOURCE_MAP` and `mmap(2)` map a resource into the process, `GPU_RESOURCE_DESTROY` releases one, and `GPU_DEVICE_QUERY` identifies the node. The rules shared by every request (nodes, permissions, calling conventions, concurrency) are in [the device interface](gpu.md).

## Object model and lifetime

A handle is a non-zero 64-bit value. The kernel draws it from one counter that is shared with its other identities (completion sequences and device identities among them), so a handle is unique across all opens and nodes for the life of the system, is not consecutive, and is never reused: even a creation that later fails uses up its number. A handle is valid only in the open that created it, and in the descriptors that share that open file description (`dup`, inheritance across `fork`, and descriptors passed with `SCM_RIGHTS`). Every request that takes a handle looks it up in the table of the calling open only. A handle of zero, a handle that was destroyed, and a handle that belongs to another open all fail the same way, with `EINVAL`, and none of them reaches the driver. A program therefore cannot name another open's object by guessing a number. To use an object in another open it exports it and the other open imports it ([sharing](gpu-sharing.md)); the importer then holds a resource of its own with its own handle.

An open owns every resource in its table. The owner is the open file description, not the process or the thread that created the object. An object is freed in one of two ways. `GPU_RESOURCE_DESTROY` frees one object at once, provided no mapping of it exists. When the last reference to the open file description is closed, the kernel waits for the driver to finish with the open's submitted work and then destroys every object that is left. A mapping counts as a reference: a mapping made with `mmap(2)` keeps the open file description alive, so closing every descriptor does not release the open's objects while a mapping exists, and the objects go when the last mapping is removed. While a mapping exists `GPU_RESOURCE_DESTROY` of its object fails with `EBUSY`, so a mapping never outlives the memory it shows. A request that is in the middle of using a resource without holding the open's admission (the display requests that run outside it, listed in [the device interface](gpu.md)) pins it; `GPU_RESOURCE_DESTROY` then waits, interruptibly, until the pin ends. Destroy does not look at work that the program has already submitted: the kernel keeps no list of work bound to a resource and does not wait for it, and what happens to such work is up to the driver. A descriptor obtained from `GPU_RESOURCE_EXPORT` is a capability that is independent of the exporting open ([sharing](gpu-sharing.md)); destroying a resource or closing the exporting open does not revoke it.

The two kinds serve different requests. A storage resource is created with `GPU_RESOURCE_CREATE` and is reached through `GPU_RESOURCE_READ`, `GPU_RESOURCE_WRITE` and, where the node supports it, a mapping. It is the only kind that `GPU_PRESENT` accepts, and the only kind that the copying form of `GPU_DISPLAY_PRESENT` accepts: a blob is refused there with `EINVAL`. A blob is created with `GPU_BLOB_CREATE` or `GPU_BLOB_CREATE_PLACED`, is the memory that the command stream names by its resource id, and is the kind that `GPU_DISPLAY_PRESENT` takes when it is called in blob mode and that display plane images take ([display](gpu-display.md)). The copy requests and the mapping requests do not look at the kind; whether a particular resource can be read, written or mapped is decided by the driver, and the in-tree drivers refuse what their memory cannot offer (see `Other values` of each request). Imported allocations also appear in the table as blobs, and an import of a foreign scanout image as a third kind that is meant for the display requests: `GPU_PRESENT`, the copying form of `GPU_DISPLAY_PRESENT` and the export requests refuse it with `EINVAL`, and the copy and mapping requests leave it to the driver like any other resource ([sharing](gpu-sharing.md)).

Two limits bound what an open can own. `GPU_GET_INFO` reports `max_resources`, the number of objects one open can hold at the same time, and `max_resource_bytes`, the largest size of a single object ([capabilities](gpu-capabilities.md)); both are set by the driver. Created, blob and imported objects all count. A creation that would exceed either limit fails before the driver allocates anything. A single `GPU_RESOURCE_READ` or `GPU_RESOURCE_WRITE` moves at most `GPU_COPY_MAX` bytes, whatever the size of the resource.

`GPU_DEVICE_QUERY` answers which node an open is on, independently of what the node can render. The kernel gives every registered node a `device_id` when the node is registered. Two opens that report the same `device_id` are on the same node, and a node that is removed and registered again gets a new one. `device_id` belongs to the node and not to the physical GPU: the render node and the display node of one GPU have different values. A driver may name the render node that a display node prefers to receive images from in `companion_id`; no driver in the tree does so today, so a program reads `roles` and asks a display node for its scanout constraints to decide which nodes pair up before it sets up shared images ([sharing](gpu-sharing.md), [drivers](gpu-drivers.md)).

## Mapping into a process

A program does not receive an address from the kernel; it receives a token and maps it. `GPU_RESOURCE_MAP` asks the driver for a view of the object (the first time only) and returns a session-local `offset` and a `bytes` length. The offset is not a physical address and not a file position that means anything outside this open: it is a token that names the object's view. The program then calls `mmap(2)` on the same descriptor with that `offset` and a length of up to `bytes`:

```c
struct gpu_resource_map map = { .version = GPU_ABI_VERSION, .size = sizeof(map), .handle = handle };

ioctl(fd, GPU_RESOURCE_MAP, &map);
void *p = mmap(NULL, map.bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)map.offset);
```

Which objects can be mapped is the driver's decision, made when `GPU_RESOURCE_MAP` is called. The kernel itself checks nothing about the kind, so a storage resource and a blob are asked the same question. The PowerVR, Mali, BCM2711, Venus and ghci drivers map a blob only when it was created with `GPU_BLOB_MAPPABLE` and refuse one created without it (`ENOTSUP`, or `EINVAL` from ghci). The amdrdna, nvrtx and i915 drivers do not read the flag: they map any blob whose memory the processor can reach, and amdrdna refuses a blob outside its window with `EINVAL`. Whether a storage resource can be mapped depends on the driver. The node must offer `GPU_CAP_MAPPING`; a node that does not answers `EOPNOTSUPP` to `GPU_RESOURCE_MAP`, and on such a node no token exists, so `mmap(2)` fails with `EINVAL`. The view the driver returns must cover the whole object and must consist of whole pages, and the kernel refuses a view that does not meet this with `EIO`. The `bytes` that `GPU_RESOURCE_MAP` returns is the size of that view, which is a whole number of pages and at least the size of the resource. Asking again for the same resource returns the same `offset` and `bytes`. Offsets are handed out in increasing order from one page, advance by the size of each view, and are never reused within the open, so the token of a destroyed object never names a later one.

The mapping is made by `mmap(2)` on the descriptor of the open. The call must pass `MAP_SHARED`; a private mapping, a mapping with `PROT_EXEC`, and a mapping of an open that is write-only fail with `EACCES`, as does `PROT_WRITE` on an open that is read-only. Mapping needs the same readable open that `GPU_RESOURCE_MAP` needs, and the protection of the mapping, now and after any later `mprotect(2)`, can never exceed the access mode the file was opened with (read always, write only when the open is writable). `offset` must be a multiple of the page size and not negative. The length is rounded up to whole pages by the system call. For a GPU mapping the address argument is a hint even with `MAP_FIXED`: only `MAP_FIXED_NOREPLACE` places the mapping exactly, and fails with `EEXIST` when the range is in use. The pages must lie inside one object's view: a range may start inside the view and need not cover it, so a program can map part of an object, and can map the same object more than once, but it cannot map a range that leaves the view or that spans two views. An offset that is not a token of this open, or that was a token of an object that is gone, fails with `EINVAL`. A token of another open fails the same way; mapping another open's memory is possible only through export and import.

The cache attributes of the mapping come from the driver's view, not from the program. A view is ordinary cached memory, non-cacheable RAM, or device memory (with write combining when the driver asks for it). The PowerVR and Mali renderers map their blobs as non-cacheable RAM, the BCM2711 V3D does so for the blobs that back Vulkan device memory (its other blobs are cached RAM), and ghci does so for host memory on a system where the device does not snoop the processor's caches, so a program should not expect fast cached reads of such memory. The amdrdna, nvrtx and Venus drivers map blobs as device memory. A node that offers `GPU_CAP_HOST_BLOB` may back a blob created with `GPU_BLOB_HOST` with cached system memory, which the CPU reads much faster than a device window; a node that does not offer it clears the flag and makes the blob as any other. A mapping holds the object: until the last mapping of an object is removed, `GPU_RESOURCE_DESTROY` fails with `EBUSY` and the open file description stays alive. Closing the descriptor does not remove a mapping.

The errors of `mmap(2)` are the ones of the system call, and those the GPU node adds are as follows.

- `EACCES`: the mapping is not `MAP_SHARED`, asks for `PROT_EXEC`, or asks for write when the open is read-only; the open is write-only; or the protection exceeds the open's access mode.
- `EINVAL`: `offset` is negative or not a multiple of the page size, the length is zero, `offset` is not a token of this open, or the range does not lie inside one object's view.
- `ENODEV`: the open failed, or the device was withdrawn.
- `EINTR`: a signal arrived while the call waited for the open's admission.
- `EOVERFLOW`: the object already has the largest possible number of mappings.
- Other values: the open's recorded failure (`EIO`, `ETIMEDOUT` or a value a driver recorded) when the device or the open has failed, as for every request that runs under admission ([submission](gpu-submission.md#session-failure-and-recovery)); and the errors of the virtual-memory layer, for example `ENOMEM`, `EEXIST` for `MAP_FIXED_NOREPLACE` over an existing mapping, and `ENOTSUP` when a view asks for write combining on a system that does not provide it.

## Requests

### `GPU_RESOURCE_CREATE`

Requires: open mode W; capability `GPU_CAP_RESOURCE`.

Creates one storage resource of `bytes` bytes and returns its handle. The resource belongs to the calling open. `bytes` must be non-zero and at most `max_resource_bytes` of `GPU_GET_INFO`, and the open must hold fewer than `max_resources` objects. `usage` must be `GPU_RESOURCE_USAGE_STORAGE`, the only usage that exists, `flags` must be zero, and `handle` must be zero on input. The kernel takes the handle from its counter before it calls the driver and writes it back only after the driver has succeeded; if the write back fails, the driver releases the object and the creation has no effect, although the handle number stays used.

The request runs under the open's admission.

Errors:

- `ENODEV`: the device was withdrawn, or the device or this open failed with `ENODEV`.
- `EIO`: the device or this open has failed and the recorded errno is returned; or the driver reported success without supplying an object.
- `EINTR`: a signal arrived while the request waited for the open's admission.
- `EACCES`: the open is not writable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_RESOURCE`.
- `EFAULT`: the argument cannot be read or written back.
- `EINVAL`: wrong `version` or `size`; `usage` is not `GPU_RESOURCE_USAGE_STORAGE`; `flags` is not zero; `bytes` is zero or larger than the node's `max_resource_bytes`; `handle` is not zero.
- `ENOSPC`: the open already holds `max_resources` objects.
- `ENOMEM`: the kernel could not allocate the record of the resource.
- `EOVERFLOW`: the counter that issues handles is exhausted.
- Other values: the driver's information and creation operations return their own errno unchanged (for example `ENOMEM` when its memory is exhausted).

Sources: [gpu.h](../../include/uapi/gpu.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_create_ioctl`, `gpu_resource_reserve`, `gpu_handle_allocate`), [main.c](../../userland/tests/gpu-admission/main.c).

### `GPU_RESOURCE_DESTROY`

Requires: open mode W; capability none.

Releases one resource or blob of the calling open. The handle stops naming anything at once and never names an object again. The request destroys the object in the driver before it returns, and it frees the object's place against `max_resources`.

An object that has a mapping cannot be destroyed: the request fails with `EBUSY` and changes nothing. A program removes its mappings with `munmap(2)` first. An object that a display request is using at that moment (one of the requests that run outside the open's admission) is not refused; the request waits until that use ends, and a signal ends the wait with `EINTR` and leaves the object as it was. The request does not check whether submitted work refers to the object, and it does not wait for such work. Descriptors that were exported from the object are not revoked.

The node has to be able to hold objects: the request answers `EOPNOTSUPP` on a node that offers neither `GPU_CAP_RESOURCE` nor `GPU_CAP_BLOB`, unless the node accepts imported scanout images, which are destroyed through this request as well. Because that last condition is not a capability bit, the `Requires:` line names no capability.

The request runs under the open's admission.

Errors:

- `ENODEV`: the device was withdrawn, or the device or this open failed with `ENODEV`.
- `EIO`: the device or this open has failed and the recorded errno is returned.
- `EINTR`: a signal arrived while the request waited for the open's admission, or while it waited for a display request to stop using the object.
- `EACCES`: the open is not writable.
- `EOPNOTSUPP`: the node holds no resources or blobs and accepts no scanout import.
- `EFAULT`: the argument cannot be read.
- `EINVAL`: wrong `version` or `size`; `handle` is zero, unknown, already destroyed, or belongs to another open (this request answers `EINVAL` and not `ENOENT`).
- `EBUSY`: the object is mapped into a process.
- Other values: the recorded errno of a failed device or open when it is neither `EIO` nor `ENODEV` (for example `ETIMEDOUT`). The driver's destroy operation returns nothing, so no errno comes from the driver.

Sources: [gpu.h](../../include/uapi/gpu.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_destroy_ioctl`, `gpu_resource_lookup`, `gpu_resource_pin`, `gpu_resource_unpin`, `gpu_close`, `gpu_mapping_release`), [main.c](../../userland/tests/gpu-admission/main.c).

### `GPU_BLOB_CREATE`

Requires: open mode W; capability `GPU_CAP_BLOB`.

Creates one blob of `bytes` bytes and returns its `handle` and its `resource_id`. The structure is 40 bytes. `GPU_BLOB_CREATE_PLACED` is a different request with a different structure of 64 bytes; this request does not read or write a placement. The blob belongs to the calling open and counts against `max_resources`; `bytes` must be non-zero and at most `max_resource_bytes`. `handle` and `resource_id` must be zero on input.

`handle` is the number with which every request of the calling open names the blob. `resource_id` is a different name: it is the number that the renderer's command stream uses to refer to the blob. The driver chooses it, it is never zero on success (a driver that supplies none fails the creation with `EIO`), and it has meaning only for this node's renderer. A program uses `handle` in ioctl requests and `resource_id` in the command stream ([command stream](gpu-command-stream.md)).

`blob_id` tells the driver what the blob stands for. The kernel does not interpret it. Zero asks for a blob of the program's own. A non-zero value names a device memory object that the program has set up through the command stream under that identity, and the blob is that object's storage; the drivers that back Vulkan device memory use it this way, and a driver that has no use for it ignores it or refuses a non-zero value with `EINVAL`.

`flags` selects what the blob can do. `GPU_BLOB_MAPPABLE` allows a mapping. `GPU_BLOB_SHAREABLE` declares that the blob is to be exported, and `GPU_BLOB_CROSS_DEVICE` that the export is to be used with another device; both need the node to offer `GPU_CAP_SHARE` or `GPU_CAP_ALLOCATION_SHARE`. The kernel checks only those capabilities and the combination rules: the export requests do not read either flag, and the drivers decide what a missing flag costs (the Venus and BCM2711 drivers refuse the export of a blob made without `GPU_BLOB_SHAREABLE`, the other in-tree drivers export it all the same, and the Venus driver refuses the image export of a blob made without `GPU_BLOB_CROSS_DEVICE` unless its host reports that it scans out; see [sharing](gpu-sharing.md)). `GPU_BLOB_CROSS_DEVICE` is valid only together with `GPU_BLOB_SHAREABLE`. `GPU_BLOB_HOST` marks a blob that only the CPU touches, and it cannot be combined with the two sharing flags; it needs no capability, because a node that does not offer `GPU_CAP_HOST_BLOB` makes the blob as it makes any other and the kernel clears the flag, which then also reads as clear in the structure that comes back. A driver can refuse flag combinations that it cannot provide. No bit other than the four is defined.

The request runs under the open's admission. If the structure cannot be written back, the blob is destroyed and the request fails, with no effect except that the handle number stays used.

Errors:

- `ENODEV`: the device was withdrawn, or the device or this open failed with `ENODEV`.
- `EIO`: the device or this open has failed and the recorded errno is returned; or the driver reported success without an object or without a `resource_id`.
- `EINTR`: a signal arrived while the request waited for the open's admission.
- `EACCES`: the open is not writable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_BLOB`; or `flags` has `GPU_BLOB_SHAREABLE` or `GPU_BLOB_CROSS_DEVICE` and the node offers neither `GPU_CAP_SHARE` nor `GPU_CAP_ALLOCATION_SHARE`.
- `EFAULT`: the argument cannot be read or written back.
- `EINVAL`: wrong `version` or `size` (the size must be 40 for this request); an unknown bit in `flags`; `GPU_BLOB_HOST` together with `GPU_BLOB_SHAREABLE` or `GPU_BLOB_CROSS_DEVICE`; `GPU_BLOB_CROSS_DEVICE` without `GPU_BLOB_SHAREABLE`; `handle` or `resource_id` is not zero; `bytes` is zero or larger than the node's `max_resource_bytes`.
- `ENOSPC`: the open already holds `max_resources` objects.
- `ENOMEM`: the kernel could not allocate the record of the blob.
- `EOVERFLOW`: the counter that issues handles is exhausted.
- Other values: the driver's information and blob operations return their own errno unchanged. In-tree drivers return `EINVAL` for a flag combination or a `blob_id` they cannot honour, `EOPNOTSUPP` for flags the node cannot provide, and `ENOMEM` or `ENOSPC` when their memory or window is exhausted.

Sources: [gpu.h](../../include/uapi/gpu.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_blob_ioctl`, `gpu_resource_reserve`), [client.c](../../userland/tests/venus-frame/client.c).

### `GPU_BLOB_CREATE_PLACED`

Requires: open mode W; capability `GPU_CAP_BLOB`.

Creates a blob like `GPU_BLOB_CREATE` and in addition requires physical conditions on its backing memory. The structure is 64 bytes: a `struct gpu_blob_create` followed by a `struct gpu_placement`. The nested blob keeps its own layout and fields, with one change: its `size` field holds the size of the whole structure, 64, not 40. A request whose structure is 40 bytes is `GPU_BLOB_CREATE`, and the two requests do not accept each other's size. Everything said about the blob in `GPU_BLOB_CREATE` (flags, `blob_id`, `handle`, `resource_id`, limits) applies here.

The placement states what the memory must be. `GPU_PLACEMENT_DMA32` requires that the blob can be addressed with 32-bit DMA addresses, `GPU_PLACEMENT_CONTIGUOUS` that the memory is one physically contiguous run, and `GPU_PLACEMENT_COHERENT` that the memory is coherent with the CPU caches. `max_dma_address` is the highest DMA address the blob may occupy (the limit is inclusive); zero means no limit. `alignment` is the alignment of the first DMA address of the blob: zero or one asks for none, and any other value must be a power of two. `reserved` must be zero and no flag other than the three is defined.

The conditions are requirements, not hints. A node that makes the blob has established every condition on the memory it actually uses, and a node that cannot do so fails the creation with `ENOTSUP`, leaving nothing behind. A request that sets no condition at all (zero flags, zero `max_dma_address` and `alignment` of zero or one) is handled with the ordinary allocator of the node, as `GPU_BLOB_CREATE` would handle it. A request that sets any condition needs the node's placed allocator and fails with `ENOTSUP` on a node that has none. In-tree, the placed allocator exists on the PowerVR render node and on the BCM2711 render and display nodes; every other node answers `ENOTSUP` for a condition. Those three nodes refuse `GPU_PLACEMENT_COHERENT`: the PowerVR node reaches its buffers only through uncached views, and the BCM2711 nodes use cached memory that does not snoop. The BCM2711 nodes also refuse an `alignment` above 1 GiB with `ENOTSUP`.

The request runs under the open's admission.

Errors:

- `ENODEV`: the device was withdrawn, or the device or this open failed with `ENODEV`.
- `EIO`: the device or this open has failed and the recorded errno is returned; or the driver reported success without an object or without a `resource_id`.
- `EINTR`: a signal arrived while the request waited for the open's admission.
- `EACCES`: the open is not writable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_BLOB`; or `flags` has `GPU_BLOB_SHAREABLE` or `GPU_BLOB_CROSS_DEVICE` and the node offers neither `GPU_CAP_SHARE` nor `GPU_CAP_ALLOCATION_SHARE`. The name `ENOTSUP` below is the same value.
- `ENOTSUP`: a condition is set and the node has no placed allocator; or the allocator cannot establish a condition.
- `EFAULT`: the argument cannot be read or written back.
- `EINVAL`: wrong `version` or `size` (the size of the whole structure, 64, is checked against the nested `size`); `placement.reserved` is not zero; an unknown bit in `placement.flags`; `placement.alignment` is not zero and not a power of two; the other conditions listed for `GPU_BLOB_CREATE`.
- `ENOSPC`: the open already holds `max_resources` objects.
- `ENOMEM`: the kernel could not allocate the record of the blob.
- `EOVERFLOW`: the counter that issues handles is exhausted.
- Other values: the driver's placed or blob operation returns its own errno unchanged (for example `ENOMEM` when it cannot allocate the memory).

Sources: [gpu-scanout.h](../../include/uapi/gpu-scanout.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_blob_ioctl`), [render.c](../../src/drivers/gpu/powervr/render.c) (`render_blob_create_placed`), [render-device.c](../../src/drivers/gpu/bcm2711/render-device.c) (`render_placed`), [display-device.c](../../src/drivers/gpu/bcm2711/display/display-device.c) (`blob_create_placed`), no source-tree test.

### `GPU_RESOURCE_READ`

Requires: open mode R; capability `GPU_CAP_TRANSFER`.

Copies `bytes` bytes from the resource `handle`, starting at byte `offset` of the resource, to the caller's memory at the user address `address`. The address is carried as a 64-bit value and must fit the address width of the system. `bytes` is from 1 to `GPU_COPY_MAX`; the range from `offset` to `offset + bytes` must lie inside the resource. A larger transfer is made with several requests. The kernel reads into a private zeroed buffer and copies it out afterwards, so the caller's buffer is not touched until the driver has succeeded. `reserved` must be zero.

The request is not a synchronization point. The kernel does not wait for submitted work and does not order the copy after it; the copy is the driver's read of the memory as it is at that moment, and a program that needs the result of earlier work waits for its completion first ([submission](gpu-submission.md)). Which resources can be read is the driver's decision, and a resource whose memory the CPU cannot reach is refused.

The request runs under the open's admission.

Errors:

- `ENODEV`: the device was withdrawn, or the device or this open failed with `ENODEV`.
- `EIO`: the device or this open has failed and the recorded errno is returned.
- `EINTR`: a signal arrived while the request waited for the open's admission.
- `EACCES`: the open is not readable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_TRANSFER`.
- `EFAULT`: the argument cannot be read; or `address` is zero, does not fit the system's address width, or the range of `bytes` bytes wraps or cannot be written. The address is checked after the handle and the range of the resource.
- `EINVAL`: wrong `version` or `size`; `reserved` is not zero; `bytes` is zero or larger than `GPU_COPY_MAX`; `handle` is zero, unknown, destroyed or belongs to another open; `offset` is past the end of the resource, or `offset` plus `bytes` is past the end.
- `ENOMEM`: the kernel could not allocate the buffer.
- Other values: the driver's read operation returns its own errno unchanged. In-tree drivers return `EOPNOTSUPP` for memory that the CPU cannot read and `EINVAL` for a range they refuse.

Sources: [gpu.h](../../include/uapi/gpu.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_transfer_ioctl`, `gpu_user_range`), [main.c](../../userland/tests/gpu-admission/main.c).

### `GPU_RESOURCE_WRITE`

Requires: open mode W; capability `GPU_CAP_TRANSFER`.

Copies `bytes` bytes from the caller's memory at `address` to the resource `handle`, starting at byte `offset`. The rules of `GPU_RESOURCE_READ` for the fields, the size limit, the range of the resource, the address and the lack of any ordering with submitted work apply unchanged. The differences are these. The open must be writable instead of readable. The kernel copies the caller's bytes into a private buffer before it calls the driver, so the driver never sees the caller's memory, and a request whose memory cannot be read leaves the resource unchanged. When the driver itself fails, the kernel does not define what the range holds. The caller may reuse its buffer as soon as the request returns.

The request runs under the open's admission.

Errors:

- `ENODEV`: the device was withdrawn, or the device or this open failed with `ENODEV`.
- `EIO`: the device or this open has failed and the recorded errno is returned.
- `EINTR`: a signal arrived while the request waited for the open's admission.
- `EACCES`: the open is not writable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_TRANSFER`.
- `EFAULT`: the argument cannot be read; or `address` is zero, does not fit the system's address width, or the range of `bytes` bytes wraps or cannot be read.
- `EINVAL`: wrong `version` or `size`; `reserved` is not zero; `bytes` is zero or larger than `GPU_COPY_MAX`; `handle` is zero, unknown, destroyed or belongs to another open; `offset` is past the end of the resource, or `offset` plus `bytes` is past the end.
- `ENOMEM`: the kernel could not allocate the buffer.
- Other values: the driver's write operation returns its own errno unchanged. In-tree drivers return `EOPNOTSUPP` for memory that the CPU cannot write and `EINVAL` for a range they refuse.

Sources: [gpu.h](../../include/uapi/gpu.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_transfer_ioctl`, `gpu_user_range`), [client.c](../../userland/tests/venus-frame/client.c).

### `GPU_RESOURCE_MAP`

Requires: open mode R; capability `GPU_CAP_MAPPING`.

Returns the token with which a resource is mapped into the process. On input `handle` names the resource and `offset` and `bytes` are zero. On output `offset` is the token and `bytes` is the length of the resource's view, which is a whole number of pages and at least the size of the resource. The program passes `offset` as the file offset of `mmap(2)` (see Mapping into a process). The request does not map anything and creates no reference: it only fixes the token. The first call for a resource asks the driver for a view and keeps it for the life of the resource; every later call returns the same two numbers without asking again, and a call whose result could not be written back can be repeated. The open must be readable even for a program that only writes through the mapping.

A node that has no mapping offers no `GPU_CAP_MAPPING`; one that has it can still refuse a particular resource, for instance a blob that was not created with `GPU_BLOB_MAPPABLE`.

The request runs under the open's admission.

Errors:

- `ENODEV`: the device was withdrawn, or the device or this open failed with `ENODEV`.
- `EIO`: the device or this open has failed and the recorded errno is returned; or the driver returned a view that does not cover the resource, has no address, uses attributes the system does not know or a combination of them it does not accept, or is not made of whole pages.
- `EINTR`: a signal arrived while the request waited for the open's admission.
- `EACCES`: the open is not readable.
- `EOPNOTSUPP`: the node does not offer `GPU_CAP_MAPPING`.
- `EFAULT`: the argument cannot be read or written back.
- `EINVAL`: wrong `version` or `size`; `offset` or `bytes` is not zero; `handle` is zero, unknown, destroyed or belongs to another open.
- `EOVERFLOW`: the view cannot be addressed by this system, or the open has used up the range of tokens.
- Other values: the driver's map operation returns its own errno unchanged. In-tree drivers return `ENOTSUP` for a resource that is not host visible or a blob created without `GPU_BLOB_MAPPABLE`; ghci returns `EINVAL` for such a blob, amdrdna `EINVAL` for a blob outside its window, and i915 `EFAULT` when the memory has no kernel address.

Sources: [gpu.h](../../include/uapi/gpu.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_map_ioctl`, `gpu_mmap`, `gpu_mapping_release`), [gpu-check.c](../../userland/tests/amdrdnactl/gpu-check.c).

### `GPU_DEVICE_QUERY`

Requires: open mode none; capability none.

Reports the identity of the node the descriptor is open on and what it is for. Only `version` and `size` are inputs; every other field must be zero on input, and all of them are filled by the kernel and the driver. The request needs neither read nor write permission, no capability bit, and no renderer: a display-only node answers it as well as a render node does.

`device_id` identifies the node. The kernel assigns it when the node is registered; it is non-zero, it is the same on every open of the node for as long as the node exists, and it differs from the `device_id` of every other node. A removed node's number is never given out again. `companion_id` is the `device_id` of the render node that the node prefers to work with, as the driver reports it; it is a hint for choosing a pair. Zero means that the node states no preference, and it does not prove that sharing with any node fails: a program may still try an import. `roles` says what the node does, as a bit mask of `GPU_DEVICE_RENDER` and `GPU_DEVICE_DISPLAY`. A node whose driver supplies its own description reports what the driver says. For any other node the kernel derives the roles from the node's capabilities: `GPU_DEVICE_RENDER` when it offers `GPU_CAP_COMMAND`, and `GPU_DEVICE_DISPLAY` when it offers `GPU_CAP_DISPLAY` ([capabilities](gpu-capabilities.md)). The in-tree drivers report `companion_id` zero. `flags` and `reserved` are returned as zero.

The request runs under the open's admission.

Errors:

- `ENODEV`: the device was withdrawn, or the device or this open failed with `ENODEV`.
- `EIO`: the device or this open has failed and the recorded errno is returned; or the driver reported a role other than the two defined, or a non-zero `flags` or `reserved`.
- `EINTR`: a signal arrived while the request waited for the open's admission.
- `EFAULT`: the argument cannot be read or written back.
- `EINVAL`: wrong `version` or `size`; `device_id`, `companion_id`, `roles`, `flags` or `reserved` is not zero on input.
- Other values: the description operation of a driver that supplies one returns its own errno unchanged. The in-tree drivers return none.

Sources: [gpu-scanout.h](../../include/uapi/gpu-scanout.h), [gpu.c](../../src/drivers/gpu/gpu.c) (`gpu_device_query_ioctl`), [main.c](../../userland/tests/display-scanout/main.c).

## Constants

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_RESOURCE_USAGE_STORAGE` | 1 | The only `usage` of `GPU_RESOURCE_CREATE`: a plain storage resource that is read and written with the copy requests, and that `GPU_PRESENT` can show. Any other value is `EINVAL`. |
| `GPU_BLOB_MAPPABLE` | 1 | Blob flag. The blob can be mapped into the process. A driver may refuse `GPU_RESOURCE_MAP` for a blob created without it. |
| `GPU_BLOB_SHAREABLE` | 2 | Blob flag. Declares that the blob is to be exported as a file descriptor; the Venus and BCM2711 drivers refuse an export without it. The node must offer `GPU_CAP_SHARE` or `GPU_CAP_ALLOCATION_SHARE`, else `EOPNOTSUPP`. |
| `GPU_BLOB_CROSS_DEVICE` | 4 | Blob flag. Declares that the export is to be imported by another device; the kernel does not read it on export or import. Valid only together with `GPU_BLOB_SHAREABLE`; the same capability is needed. |
| `GPU_BLOB_HOST` | 8 | Blob flag. Only the CPU touches the blob (the process through its mapping and the renderer's decoder in the kernel), never GPU work, a display or another device. Cannot be combined with `GPU_BLOB_SHAREABLE` or `GPU_BLOB_CROSS_DEVICE`. A node without `GPU_CAP_HOST_BLOB` clears it. |
| `GPU_COPY_MAX` | 0x10000 | The most bytes one `GPU_RESOURCE_READ` or `GPU_RESOURCE_WRITE` moves, 65536. A larger or empty transfer is `EINVAL`. |
| `GPU_PLACEMENT_DMA32` | 1 | Placement flag. The blob can be addressed with 32-bit DMA addresses. |
| `GPU_PLACEMENT_CONTIGUOUS` | 2 | Placement flag. The blob is one physically contiguous run. |
| `GPU_PLACEMENT_COHERENT` | 4 | Placement flag. The blob is coherent with the CPU caches. The in-tree nodes that implement placement cannot provide it and answer `ENOTSUP`. |
| `GPU_DEVICE_RENDER` | 1 | Role bit of `GPU_DEVICE_QUERY`. The node renders: it runs command streams. |
| `GPU_DEVICE_DISPLAY` | 2 | Role bit of `GPU_DEVICE_QUERY`. The node drives a display. |

## Structures

#### `struct gpu_resource_create`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of this structure, 32. |
| 8 | 8 | `uint64_t` | `bytes` | in | Size of the resource in bytes. Non-zero and at most `max_resource_bytes` of `GPU_GET_INFO`. |
| 16 | 4 | `uint32_t` | `usage` | in | Must be `GPU_RESOURCE_USAGE_STORAGE`. |
| 20 | 4 | `uint32_t` | `flags` | zero | Reserved; must be zero. |
| 24 | 8 | `uint64_t` | `handle` | out | The handle of the new resource, non-zero and unique across the system. Must be zero on input. Written only when the request succeeds. |

#### `struct gpu_resource_destroy`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of this structure, 16. |
| 8 | 8 | `uint64_t` | `handle` | in | Handle of a resource or blob of this open. Must not be zero. |

#### `struct gpu_blob_create`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of the request: 40 for `GPU_BLOB_CREATE`, and 64 (the size of the whole `struct gpu_blob_create_placed`) when this structure is the first part of that one. |
| 8 | 8 | `uint64_t` | `bytes` | in | Size of the blob in bytes. Non-zero and at most `max_resource_bytes` of `GPU_GET_INFO`. |
| 16 | 8 | `uint64_t` | `blob_id` | in | What the blob stands for, interpreted by the driver. Zero for a blob of the program's own; a non-zero value names a device memory object set up through the command stream. The kernel does not interpret it. |
| 24 | 8 | `uint64_t` | `handle` | out | The handle of the new blob, non-zero and unique across the system. Must be zero on input. Written only when the request succeeds. |
| 32 | 4 | `uint32_t` | `flags` | in/out | Any of `GPU_BLOB_MAPPABLE`, `GPU_BLOB_SHAREABLE`, `GPU_BLOB_CROSS_DEVICE` and `GPU_BLOB_HOST`. On return `GPU_BLOB_HOST` is clear when the node does not offer `GPU_CAP_HOST_BLOB`. |
| 36 | 4 | `uint32_t` | `resource_id` | out | The number by which the renderer's command stream names the blob, chosen by the driver and never zero on success. Must be zero on input. |

#### `struct gpu_blob_create_placed`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 40 | `struct gpu_blob_create` | `blob` | in/out | The blob to create, with the fields and rules of `GPU_BLOB_CREATE` except that its `size` is 64. `handle` and `resource_id` are outputs. |
| 40 | 24 | `struct gpu_placement` | `placement` | in | The physical conditions the blob must meet. All zero asks for none. |

#### `struct gpu_placement`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `flags` | in | Any of `GPU_PLACEMENT_DMA32`, `GPU_PLACEMENT_CONTIGUOUS` and `GPU_PLACEMENT_COHERENT`. Each set bit is a requirement. |
| 4 | 4 | `uint32_t` | `reserved` | zero | Must be zero. |
| 8 | 8 | `uint64_t` | `max_dma_address` | in | The highest DMA address the blob may occupy, inclusive. Zero means no limit. |
| 16 | 8 | `uint64_t` | `alignment` | in | Alignment of the first DMA address in bytes. Zero or one asks for none; any other value must be a power of two. |

#### `struct gpu_transfer`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of this structure, 40. |
| 8 | 8 | `uint64_t` | `handle` | in | Handle of the resource or blob to read or write. Must not be zero. |
| 16 | 8 | `uint64_t` | `offset` | in | Byte offset in the resource at which the transfer starts. At most the size of the resource, and `offset` plus `bytes` must not exceed it. |
| 24 | 8 | `uint64_t` | `address` | in | User address of the caller's buffer, as a 64-bit value. Non-zero, and the whole range of `bytes` bytes must be writable for a read and readable for a write. |
| 32 | 4 | `uint32_t` | `bytes` | in | Number of bytes to move, from 1 to `GPU_COPY_MAX`. |
| 36 | 4 | `uint32_t` | `reserved` | zero | Must be zero. |

#### `struct gpu_resource_map`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of this structure, 32. |
| 8 | 8 | `uint64_t` | `handle` | in | Handle of the resource or blob to map. Must not be zero. |
| 16 | 8 | `uint64_t` | `offset` | out | The token to pass as the file offset of `mmap(2)`: a multiple of the page size, not a physical address, valid for this open only. Must be zero on input. |
| 24 | 8 | `uint64_t` | `bytes` | out | The length of the view that the token names, a whole number of pages and at least the size of the resource. Must be zero on input. |

#### `struct gpu_device_info`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | in | Must be `GPU_ABI_VERSION`. |
| 4 | 4 | `uint32_t` | `size` | in | Must be the size of this structure, 40. |
| 8 | 8 | `uint64_t` | `device_id` | out | The identity of the node, non-zero, assigned by the kernel, unique among nodes and never reused. Must be zero on input. |
| 16 | 8 | `uint64_t` | `companion_id` | out | The `device_id` of the render node this node prefers to work with. Zero means no preference and is not proof that sharing fails. Must be zero on input. |
| 24 | 4 | `uint32_t` | `roles` | out | Bit mask of `GPU_DEVICE_RENDER` and `GPU_DEVICE_DISPLAY`. Must be zero on input. |
| 28 | 4 | `uint32_t` | `flags` | out | No flag is defined; returned as zero. Must be zero on input. |
| 32 | 8 | `uint64_t` | `reserved` | zero | Must be zero, and is returned as zero. |

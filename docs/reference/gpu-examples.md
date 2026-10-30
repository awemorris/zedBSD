# GPU interface examples

Status: experimental reference for GPU ABI version 1.

<!-- spec: One paragraph: complete C programs that use the requests
directly, as a reference for the calling conventions; real programs use
Vulkan through libvulkan. Every example is a ```c block whose first line
is /* example: NAME */ and compiles as C11 for every GPU target with
-Wall -Wextra -Werror; each asserts the sizes of the structures it uses
with _Static_assert. Link back to gpu.md. -->

PENDING

## Opening a node and reading its capabilities

<!-- spec: Example "info": open /dev/gpu0 read-write, GPU_GET_INFO,
print driver_name and the capability bits by name, GPU_DEVICE_QUERY,
close. Shows version and size, the -1/errno convention and the check of
a capability bit before a request. -->

PENDING

## Allocating, writing and reading back storage

<!-- spec: Example "storage": GPU_RESOURCE_CREATE, GPU_RESOURCE_WRITE,
GPU_RESOURCE_READ, compare, GPU_RESOURCE_DESTROY; and, when
GPU_CAP_MAPPING and GPU_CAP_BLOB are set, GPU_BLOB_CREATE with
GPU_BLOB_MAPPABLE, GPU_RESOURCE_MAP and mmap. -->

PENDING

## Submitting and waiting with a timeout

<!-- spec: Example "submit-wait": the normal path GPU_COMMAND_SUBMIT then
GPU_COMMAND_WAIT with GPU_WAIT_CONSUME and a finite timeout, and the
failure handling a program needs: the timeout result (the exact errno),
EINTR (retry), and a session or device failure (the errno, close and
reopen as gpu-submission.md describes). The command bytes themselves may
be a minimal stream from gpu-op.h; say in the prose that a real stream
comes from libvulkan. -->

PENDING

## Presenting to a display

<!-- spec: Example "display": GPU_DISPLAY_QUERY with
GPU_DISPLAY_COUNT_ONLY then by index, GPU_DISPLAY_CLAIM, one
GPU_DISPLAY_PRESENT of a storage resource in FIFO mode, GPU_DISPLAY_WAIT
with a timeout, GPU_DISPLAY_RELEASE; handle ESTALE (re-query) and EBUSY
(another owner). -->

PENDING

## Where the tests are

<!-- spec: Links to the test programs under userland/tests/ that exercise
these requests (gpu-selftest, gpu-admission, gpu-recovery, gpu-share,
gpu-fence, acquire-fence, display-control, display-events,
display-inventory, display-scanout; check each exists) with one line
each on what it covers. -->

PENDING

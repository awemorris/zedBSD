# GPU sysctl variables

Status: experimental reference for GPU ABI version 1.

Four variables under `hw.` report on the GPU devices of the machine or control their start-up. They are read and written through `sysctl(3)` (`sysctlbyname` takes the dotted name) or the `sysctl` command, and they are defined in [sysctl.h](../../include/uapi/sysctl.h). They are not part of the `/dev/gpuN` interface that [the device interface](gpu.md) describes. A program that wants to know whether a GPU node will appear, to start a GPU that the boot held back, to read how busy the GPUs are, or to see how far the PowerVR driver came, uses them. The kernel side is [sysctl.c](../../src/kern/sysctl.c).

## Variables

| Name | MIB | Type | Access | Meaning |
| --- | --- | --- | --- | --- |
| `hw.gpu.attaching` | `{ CTL_HW, HW_GPU_ATTACHING }` | `uint32_t` | Read-only. | The number of GPU devices that a driver has attached but has not yet published a node (`/dev/gpuN`) for, or given up on. |
| `hw.gpu.start` | `{ CTL_HW, HW_GPU_START }` | `uint64_t` | Read by anyone. Written only by the superuser, and only with the value 1. | Read: the number of GPU devices that a driver holds back for a start that root asks for. Write 1: start them. |
| `hw.gputelemetry` | `{ CTL_HW, HW_GPUTELEMETRY }` | A `struct gpu_telemetry_header` followed by `count` `struct gpu_telemetry_entry`. | Read-only. | The work of each GPU as its driver keeps it. See Telemetry below. |
| `hw.gpu.powervr.stages` | `{ CTL_HW, HW_GPU_STAGES }` | A NUL-terminated string of at most 256 bytes, the NUL included. | Read-only. | How far the PowerVR driver has come since boot. `ENOENT` in a kernel without that driver. |

Every variable follows the usual `sysctl(3)` protocol for reading. With no buffer, the call stores the length that the value needs in the length argument and copies nothing. With a buffer that is too small, it stores the needed length and fails with `ENOMEM`. A call that passes a buffer but no length argument fails with `EINVAL`. A write to a read-only variable fails with `EPERM`, whoever the caller is. Reading needs no privilege.

##### hw.gpu.attaching

A driver counts a device from its attach, which happens before the device has finished its start, until the node is published or the start has failed. A driver whose start runs on a worker thread, or that loads firmware from the root file system, may therefore publish its node some time after init has started the services. The count never goes below zero. A device that is held for `hw.gpu.start` (below) is counted from the moment root starts it, not while it is held. The count is 4 bytes; read it into a `uint32_t`.

The graphical login uses the count to decide how long to wait for the display. The session manager ([main.c](../../userland/desktop/sessiond/main.c), `main_wait_display`, `main_gpu_attaching`) looks for `/dev/gpu0` and reads the count at each look. While the count is not zero it keeps looking, for at most 120 seconds, and uses the display as soon as it is there with the count at zero. When the 120 seconds pass with the count still not zero, it uses `/dev/gpu0` if it exists and otherwise falls back to the console login. When the count is zero and `/dev/gpu0` does not exist, it stops waiting at once and falls back to the console login. If the variable cannot be read, or has another size than 4 bytes, the session manager takes the count as zero. The decision is made by `sessiond_display_next` in [fallback-rules.c](../../userland/desktop/sessiond/fallback-rules.c). Drivers that count a device are amdrdna, i915, mali, nvrtx and powervr; bcm2711 and the others do not.

##### hw.gpu.start

A driver may hold a GPU device back after it has attached, until root asks for the start. Two drivers do, and a kernel has the operations of at most one of them because they are built for different architectures:

- i915 on amd64, when the boot parameter `i915.start=manual` is given. Each Intel GPU that attached waits, and a write of 1 launches the start of all of them. A device that attaches after the write starts by itself.
- bcm2711 on the Raspberry Pi 5, in a kernel built with `CONFIG_DRIVER_BCM2712_GPU`. The V3D engine and the display wait. The boot parameter `rpi5gpu.start=boot` starts them at boot instead.

A read returns how many devices are still held, which is 0 when no driver offers a start. For i915 it is the number of waiting devices; for bcm2711 it is 1 until the start has been taken. A write must be one 8-byte value and the value must be 1; the call then starts the held devices, and a read in the same call returns what is held afterwards. The write is refused with these errors:

- `EPERM`: the caller is not the superuser.
- `EINVAL`: the length is not 8 bytes, the value is not 1, or a length was given without a value.
- `ENODEV`: no driver offers a start, or none holds a device (for i915 the boot parameter was not given or root has already started them; for bcm2711 no engine was found or the start was already taken).
- Other values: the driver's start passes through. bcm2711 runs the whole start in the call and returns the error of the first stage that failed; i915 returns once the start workers are launched, and a failure of a device's start shows later as the device never publishing a node.

From the command line, `sysctl hw.gpu.start=1` writes the value and prints what is held afterwards.

##### hw.gpu.powervr.stages

The text is one word for each of seven stages, in this order and separated by single spaces: `firmware`, `render-node`, `compute`, `ta`, `3d`, `transfer` and `present`. Each word is `<stage>=<state>`. The state is `ok` for a stage that succeeded, `-` for a stage that has not been reached, and `fail:<reason>` for a stage whose latest result was a failure. A reason is one of a fixed set of words, such as `timeout`, `enoent`, `fault` or `reset`, with the spaces of a multi-word reason written as `-`. For example:

```
firmware=ok render-node=ok compute=- ta=- 3d=- transfer=- present=-
```

The text shows each stage's latest state, so a stage that failed and later succeeded reads `ok`. The `sysctl hw.gpu.powervr.stages` command prints the text. The driver is [stage-log.c](../../src/drivers/gpu/powervr/stage-log.c) (`stages_read`) with the text built in [stage.c](../../src/drivers/gpu/powervr/stage.c) (`powervr_stage_text`).

The MIB macros are these constants, all of them the second component of a name whose first component is `CTL_HW`:


| Name | Value | Meaning |
| --- | ---: | --- |
| `HW_GPU_ATTACHING` | 4 | The MIB component of `hw.gpu.attaching`. |
| `HW_GPU_START` | 5 | The MIB component of `hw.gpu.start`. |
| `HW_GPUTELEMETRY` | 8 | The MIB component of `hw.gputelemetry`. |
| `HW_GPU_STAGES` | 11 | The MIB component of `hw.gpu.powervr.stages`. |

## Telemetry

The value of `hw.gputelemetry` is a `struct gpu_telemetry_header` of 24 bytes followed by `count` `struct gpu_telemetry_entry` of 72 bytes each. It lists one entry for each GPU whose driver reports telemetry, in the order in which the drivers registered them, and at most 8 GPUs are listed. A GPU whose driver reports nothing has no entry, and a driver that keeps nothing (venus, amdrdna, ghci, mali and nvrtx) never has one, so the value may be only the header with `count` 0. A GPU whose driver fails to produce its entry when the value is read is left out of that read.

A reader sizes a buffer by asking for the length first; that length covers every listed GPU, and a successful read stores the length of the entries that are present. It then checks that `version` is `GPU_TELEMETRY_VERSION`, that `struct_size` and `element_size` are the sizes of the structures it was built with, and that `struct_size` plus `count` times `element_size` equals the length it got. Entry number `n` starts at `struct_size` plus `n` times `element_size`. The `sysctl hw.gputelemetry` command does exactly this check and prints one line for each GPU. The fields are 32-bit and 64-bit throughout, so the layout is the same for 32-bit and 64-bit processes.

`valid` says which fields the driver filled; a field whose bit is not set is 0 and means nothing. `time_ns` and `busy_ns` are in nanoseconds. `busy_ns` adds up the time during which any engine ran a request, up to `time_ns`, which is the driver's own monotonic clock when the entry was read (0 if the kernel has no such clock). The change of `busy_ns` between two reads, divided by the change of `time_ns`, is the use of the GPU in that interval. The frequencies are in MHz: `cur_mhz` is the one the hardware runs at, `req_mhz` the one the driver last asked for, and `min_mhz` and `max_mhz` the range that the driver asks within. `objects_bytes` is the memory that the GPU's objects hold, in bytes, and `objects_limit` is the limit it counts against.

The three drivers that report telemetry fill different fields:

| Driver | `driver` field | Fields filled |
| --- | --- | --- |
| i915 | `i915` | `GPU_TELEMETRY_BUSY`. `GPU_TELEMETRY_REQ_MHZ` while the driver's frequency control runs and has written a frequency. `GPU_TELEMETRY_RANGE_MHZ` once the limits of that control are known. Not `GPU_TELEMETRY_CUR_MHZ` or `GPU_TELEMETRY_OBJECTS`. |
| powervr | `powervr` | `GPU_TELEMETRY_BUSY`, which counts the time from a submission's kick to its delivery. `GPU_TELEMETRY_CUR_MHZ`, `GPU_TELEMETRY_REQ_MHZ` and `GPU_TELEMETRY_RANGE_MHZ` with the same value, the core clock, when it is known: the core has no frequency scaling. `GPU_TELEMETRY_OBJECTS`: the render buffers in use, not counting those kept for reuse, against 256 MiB. |
| bcm2711 (V3D) | `v3d` | `GPU_TELEMETRY_BUSY`, which counts the time the jobs ran. `GPU_TELEMETRY_REQ_MHZ` and `GPU_TELEMETRY_RANGE_MHZ` with the same value, the clock the driver asked for, when it is known. `GPU_TELEMETRY_OBJECTS`: the Vulkan allocations that the sessions hold, against 512 MiB. Not `GPU_TELEMETRY_CUR_MHZ`. |

Sources: the readers are `drv_i915_rps_telemetry_read` in [gt-power.c](../../src/drivers/gpu/i915/gt-power.c), `render_telemetry` in [render.c](../../src/drivers/gpu/powervr/render.c) and `render_telemetry` in [render-device.c](../../src/drivers/gpu/bcm2711/render-device.c); the list and the value are `kern_gpu_telemetry_register` and `sysctl_gputelemetry` in [sysctl.c](../../src/kern/sysctl.c).


| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_TELEMETRY_VERSION` | 1 | The version of the `hw.gputelemetry` layout, stored in `version` of the header. |
| `GPU_TELEMETRY_BUSY` | 1 | `valid` bit: `busy_ns` and `time_ns` are filled. |
| `GPU_TELEMETRY_CUR_MHZ` | 2 | `valid` bit: `cur_mhz` is filled. |
| `GPU_TELEMETRY_REQ_MHZ` | 4 | `valid` bit: `req_mhz` is filled. |
| `GPU_TELEMETRY_RANGE_MHZ` | 8 | `valid` bit: `min_mhz` and `max_mhz` are filled. |
| `GPU_TELEMETRY_OBJECTS` | 16 | `valid` bit: `objects_bytes` and `objects_limit` are filled. |

## Structures

#### `struct gpu_telemetry_header`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | out | The layout version, `GPU_TELEMETRY_VERSION`. |
| 4 | 4 | `uint32_t` | `struct_size` | out | The size of this header in bytes, 24. Entries start at this offset. |
| 8 | 4 | `uint32_t` | `element_size` | out | The size of one entry in bytes, 72. |
| 12 | 4 | `uint32_t` | `count` | out | The number of entries that follow. 0 when no driver reports telemetry. |
| 16 | 8 | `uint32_t[2]` | `reserved` | out | Written as zero. |

#### `struct gpu_telemetry_entry`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 16 | `char[16]` | `driver` | out | The name of the driver that reports this GPU, NUL-terminated. A longer name is cut to 15 characters. |
| 16 | 4 | `uint32_t` | `valid` | out | A set of `GPU_TELEMETRY_BUSY`, `GPU_TELEMETRY_CUR_MHZ`, `GPU_TELEMETRY_REQ_MHZ`, `GPU_TELEMETRY_RANGE_MHZ` and `GPU_TELEMETRY_OBJECTS`: the fields below that the driver filled. A field whose bit is clear is 0. |
| 20 | 4 | `uint32_t` | `reserved` | out | Written as zero. |
| 24 | 8 | `uint64_t` | `time_ns` | out | The driver's monotonic clock in nanoseconds when the entry was read. 0 when the kernel has no such clock. Valid with `GPU_TELEMETRY_BUSY`. |
| 32 | 8 | `uint64_t` | `busy_ns` | out | The time in nanoseconds during which any engine ran a request, up to `time_ns`. Valid with `GPU_TELEMETRY_BUSY`. |
| 40 | 8 | `uint64_t` | `objects_bytes` | out | The memory in bytes that the GPU's objects hold. Valid with `GPU_TELEMETRY_OBJECTS`. |
| 48 | 8 | `uint64_t` | `objects_limit` | out | The limit in bytes that `objects_bytes` counts against. Valid with `GPU_TELEMETRY_OBJECTS`. |
| 56 | 4 | `uint32_t` | `cur_mhz` | out | The frequency in MHz that the hardware runs at. Valid with `GPU_TELEMETRY_CUR_MHZ`. |
| 60 | 4 | `uint32_t` | `req_mhz` | out | The frequency in MHz that the driver last asked for. Valid with `GPU_TELEMETRY_REQ_MHZ`. |
| 64 | 4 | `uint32_t` | `min_mhz` | out | The lowest frequency in MHz that the driver asks within. Valid with `GPU_TELEMETRY_RANGE_MHZ`. |
| 68 | 4 | `uint32_t` | `max_mhz` | out | The highest frequency in MHz that the driver asks within. Valid with `GPU_TELEMETRY_RANGE_MHZ`. |

# GPU sysctl variables

Status: experimental reference for GPU ABI version 1.

<!-- spec: One paragraph: the GPU variables under hw. that sysctl(3) and
the sysctl command read, defined in include/uapi/sysctl.h. Link back to
gpu.md. -->

PENDING

## Variables

<!-- spec: A table | Name | MIB | Type | Access | Meaning | for
hw.gpu.attaching, hw.gpu.start, hw.gputelemetry and
hw.gpu.powervr.stages: their MIB macros (the constants table below), the
value type, read-only or writable and by whom (hw.gpu.start: writing 1
as the superuser; ENODEV with none held), and the meaning from the header
comments and the kernel handlers (grep HW_GPU_ in src/). Say how the
graphical login uses hw.gpu.attaching. Then one subsection per variable
where more than a line is needed. -->

PENDING

| Name | Value | Meaning |
| --- | ---: | --- |
| `HW_GPU_ATTACHING` | 4 | PENDING |
| `HW_GPU_START` | 5 | PENDING |
| `HW_GPUTELEMETRY` | 8 | PENDING |
| `HW_GPU_STAGES` | 11 | PENDING |

## Telemetry

<!-- spec: The value of hw.gputelemetry: a struct gpu_telemetry_header
then count entries; how a reader checks version, struct_size and
element_size before reading; one entry per GPU whose driver reports
telemetry (none for a driver that keeps nothing); valid says which fields
the driver filled (GPU_TELEMETRY_* bits); units (nanoseconds, MHz,
bytes) and what each field means; which drivers fill which fields (grep
the drivers for the telemetry hook). Fill the Meaning column of the
constants table. -->

PENDING

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_TELEMETRY_VERSION` | 1 | PENDING |
| `GPU_TELEMETRY_BUSY` | 1 | PENDING |
| `GPU_TELEMETRY_CUR_MHZ` | 2 | PENDING |
| `GPU_TELEMETRY_REQ_MHZ` | 4 | PENDING |
| `GPU_TELEMETRY_RANGE_MHZ` | 8 | PENDING |
| `GPU_TELEMETRY_OBJECTS` | 16 | PENDING |

## Structures

#### `struct gpu_telemetry_header`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `struct_size` | PENDING | PENDING |
| 8 | 4 | `uint32_t` | `element_size` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `count` | PENDING | PENDING |
| 16 | 8 | `uint32_t[2]` | `reserved` | PENDING | PENDING |

#### `struct gpu_telemetry_entry`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 16 | `char[16]` | `driver` | PENDING | PENDING |
| 16 | 4 | `uint32_t` | `valid` | PENDING | PENDING |
| 20 | 4 | `uint32_t` | `reserved` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `time_ns` | PENDING | PENDING |
| 32 | 8 | `uint64_t` | `busy_ns` | PENDING | PENDING |
| 40 | 8 | `uint64_t` | `objects_bytes` | PENDING | PENDING |
| 48 | 8 | `uint64_t` | `objects_limit` | PENDING | PENDING |
| 56 | 4 | `uint32_t` | `cur_mhz` | PENDING | PENDING |
| 60 | 4 | `uint32_t` | `req_mhz` | PENDING | PENDING |
| 64 | 4 | `uint32_t` | `min_mhz` | PENDING | PENDING |
| 68 | 4 | `uint32_t` | `max_mhz` | PENDING | PENDING |

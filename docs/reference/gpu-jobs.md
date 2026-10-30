# GPU jobs

Status: experimental reference for GPU ABI version 1.

<!-- spec: One paragraph: a job is producer-owned GPU work. A producer
(the Vulkan driver submitting natively) first reserves completion storage
and backend capacity, then submits natively, then commits or cancels the
reservation; the kernel supervises reserved and accepted work with
administrator-selected deadlines. Capabilities GPU_CAP_JOB and
GPU_CAP_JOB_CAPACITY. Link back to gpu.md. -->

PENDING

## Job life cycle

<!-- spec: From src/drivers/gpu/gpu.c (gpu_job_reserve_ioctl,
gpu_job_action_ioctl, gpu_job_bind, gpu_job_unbind, gpu_job_admit,
gpu_job_observe, gpu_monitor_*, gpu_capacity_changed_locked,
gpu_retirement_changed_locked): (1) A state diagram in words or a table:
reserved, committed, cancelled, faulted, terminal, consumed; which
request moves between them; GPU_COMMAND_WAIT observes and consumes the
terminal record of a committed job (gpu-submission.md). (2) The optional
fence (fd and generation, -1 and zero when absent): reserved generations
have a producer deadline but are not GPU dependencies; what CANCEL and
CANCEL_FAULT do to the fence. (3) Supervision: the reservation deadline,
the execution deadline and the stop deadline; their values come from the
kernel build configuration (CONFIG_GPU_JOB_RESERVATION_MS,
CONFIG_GPU_JOB_EXECUTION_MS, CONFIG_GPU_JOB_STOP_MS with their defaults
in gpu.c) and GPU_JOB_POLICY reports them; what the kernel does when each
expires (the session or device failure of gpu-submission.md). Clients
cannot extend deadlines. (4) Capacity: GPU_JOB_CAPACITY's QUERY and WAIT
modes (WAIT has no macro: it is flags zero), the domain, that neither
mode owns a job or guarantees the next reservation, and EAGAIN from a
reservation without capacity (read the code). (5) A node may advertise
GPU_CAP_JOB while GPU_JOB_RESERVE and GPU_JOB_CAPACITY return ENOTSUP
(read venus/transport.c: when the host has no strict queue); a program
checks the capset's STRICT_QUEUE flag (gpu-capabilities.md). -->

PENDING

## Requests

### `GPU_JOB_RESERVE`

<!-- spec: Argument struct gpu_job_reserve (Structures). Inputs fd, flags (zero), generation, timeline, reserved; output
sequence. The backend validates the timeline's completion domain. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_JOB_COMMIT`

<!-- spec: Argument struct gpu_job_action (Structures). After native success; flags zero only; publishes using the reserved
storage. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_JOB_CANCEL`

<!-- spec: Argument struct gpu_job_action (Structures). Flags zero (definite native nonacceptance: discards the record, fence
unsignaled and unbound) or GPU_JOB_CANCEL_FAULT (uncertain native work or
device loss: terminal ERROR retained, uncertain DMA until checked
recovery). -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_JOB_CAPACITY`

<!-- spec: Argument struct gpu_job_capacity (Structures). QUERY (GPU_JOB_CAPACITY_QUERY) and WAIT (flags zero) inputs and
outputs; timeout semantics; available. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

### `GPU_JOB_POLICY`

<!-- spec: Argument struct gpu_job_policy (Structures). Inputs version and size only; outputs the three intervals in
nanoseconds and flags. -->

Requires: PENDING

PENDING

Errors:

- PENDING

Sources: PENDING

## Constants

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_JOB_CANCEL_FAULT` | 1 | PENDING |
| `GPU_JOB_CAPACITY_QUERY` | 1 | PENDING |

## Structures

#### `struct gpu_job_reserve`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `int32_t` | `fd` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `generation` | PENDING | PENDING |
| 24 | 4 | `uint32_t` | `timeline` | PENDING | PENDING |
| 28 | 4 | `uint32_t` | `reserved` | PENDING | PENDING |
| 32 | 8 | `uint64_t` | `sequence` | PENDING | PENDING |

#### `struct gpu_job_action`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `sequence` | PENDING | PENDING |
| 16 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 20 | 4 | `uint32_t` | `reserved` | PENDING | PENDING |

#### `struct gpu_job_capacity`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 12 | 4 | `uint32_t` | `domain` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `timeout_ns` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `observed_sequence` | PENDING | PENDING |
| 32 | 8 | `uint64_t` | `sequence` | PENDING | PENDING |
| 40 | 4 | `uint32_t` | `available` | PENDING | PENDING |
| 44 | 4 | `uint32_t` | `reserved` | PENDING | PENDING |

#### `struct gpu_job_policy`

| Offset | Bytes | Type | Field | Direction | Meaning |
| ---: | ---: | --- | --- | --- | --- |
| 0 | 4 | `uint32_t` | `version` | PENDING | PENDING |
| 4 | 4 | `uint32_t` | `size` | PENDING | PENDING |
| 8 | 8 | `uint64_t` | `reservation_timeout_ns` | PENDING | PENDING |
| 16 | 8 | `uint64_t` | `execution_timeout_ns` | PENDING | PENDING |
| 24 | 8 | `uint64_t` | `stop_timeout_ns` | PENDING | PENDING |
| 32 | 4 | `uint32_t` | `flags` | PENDING | PENDING |
| 36 | 4 | `uint32_t` | `reserved` | PENDING | PENDING |

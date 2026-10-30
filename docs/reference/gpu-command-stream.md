# GPU command stream

Status: experimental reference for GPU ABI version 1.

<!-- spec: One paragraph: the bytes a program passes to GPU_COMMAND and
GPU_COMMAND_SUBMIT are a stream of Vulkan commands in the zedBSD GPU
command protocol, defined command by command in
include/uapi/gpu-op.h. This document gives the form only; the header is
the per-command reference and is not repeated here. Link back to gpu.md
and gpu-submission.md. -->

PENDING

## Protocol version and numbering

<!-- spec: From the comments at the top of include/uapi/gpu-op.h:
(1) GPU_OP_PROTOCOL_VERSION and what version 2 added (the video commands;
only a backend whose capset declares video is sent them). The capset's
wire_version word (gpu-capabilities.md) versus this protocol version.
(2) Version 1's numbers are those of the Venus protocol's wire format 1
as of virglrenderer 1.1.0 (venus-protocol commit
ca19b6358d7cc491bc3e4de76f04c6700876a8fa), so that a Venus renderer on a
virtual machine's host accepts the stream; zedBSD's own commands start
at GPU_OP_OWN_FIRST and are never sent to a Venus host. Name the version
exactly; do not call the stream "Venus compatible" without it. -->

PENDING

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_OP_PROTOCOL_VERSION` | 2 | PENDING |
| `GPU_OP_OWN_FIRST` | 0x10000 | PENDING |
| `GPU_OP_FLAG_REPLY` | 1 | PENDING |

## Wire form

<!-- spec: From the header comments: little-endian values; u32 and u64
sizes; object identities chosen by the sender; pointer arguments as a
presence word; arrays as count plus element count plus elements;
structures in declaration order with sType and pNext; the command header
[u32 number][u32 flags] with GPU_OP_FLAG_REPLY; no length field, so an
unknown number stops the stream; the reply layout; the create and destroy
forms. Summarise in your own words with a short byte diagram; refer to
gpu-op.h for each command. -->

PENDING

## One submission

<!-- spec: The framing libvulkan uses for each submission
(GPU_OP_SET_REPLY_STREAM, the command or GPU_OP_EXECUTE_STREAMS,
GPU_OP_SEEK_REPLY_STREAM, the completion trailer
GPU_OP_ENUMERATE_INSTANCE_VERSION) and how completion and the Vulkan
result are read from the reply blob; what a refused command does to the
session (VK_ERROR_DEVICE_LOST in libvulkan). Name only commands that
exist in the header. -->

PENDING

## Executors

<!-- spec: Which nodes decode the stream in the kernel (the native
executors named in the header comment) and which pass it to a virtual
machine's host unread (venus). One short table | Node | Who decodes |
Unknown command |. -->

PENDING

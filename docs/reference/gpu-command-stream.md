# GPU command stream

Status: experimental reference for GPU ABI version 1.

The bytes that a program passes to `GPU_COMMAND` and `GPU_COMMAND_SUBMIT` are a stream of Vulkan commands in the zedBSD GPU command protocol. The protocol is defined command by command in [gpu-op.h](../../include/uapi/gpu-op.h): for each command that header gives its number, its arguments, its reply, which library function sends it and which function of each kernel executor decodes it. This document describes only the form of the stream and the way a program frames a submission. It does not repeat the per-command reference. The requests that carry the stream are in [submission](gpu-submission.md), and the rules that every GPU node shares are in [the device interface](gpu.md). The library that produces the stream is libvulkan, which is the sender this document describes; a program that talks to the node directly has to produce the same bytes.

## Protocol version and numbering

`GPU_OP_PROTOCOL_VERSION` is the protocol's revision, and it is 2. Version 1 is the set of numbers below `GPU_OP_OWN_FIRST` together with the wire form described here. Version 2 adds the video commands, which are numbered from `GPU_OP_OWN_FIRST`; the numbers of version 1 did not change. Only a renderer whose capability set declares video decode is sent the video commands. No code in the tree compares the constant: it records the revision of the header, and a program decides what to send from the capability set. The `wire_version` word of capability set 4 ([capabilities](gpu-capabilities.md)) is a different number. It names the version of the wire form, and libvulkan accepts only 1; it is not `GPU_OP_PROTOCOL_VERSION`.

The numbers of version 1 are those of the Venus protocol's wire format 1 as shipped in virglrenderer 1.1.0 (venus-protocol commit `ca19b6358d7cc491bc3e4de76f04c6700876a8fa`), for the same Vulkan commands, so that a Venus renderer on a virtual machine's host accepts the stream as it is. There are 145 such commands, numbered from 0 to 180 with gaps. The names, the header and every later number are zedBSD's own. A zedBSD command that has no Venus counterpart is numbered from `GPU_OP_OWN_FIRST`; today these are the 14 video decode commands, and libvulkan sends them only to a renderer whose capability set declares video decode. The compatibility that this protocol claims is with the numbers and the wire form of Venus wire format 1 as of virglrenderer 1.1.0, and with nothing else.

Two private records may be chained to a command, with structure types `GPU_OP_RECORD_IMPORT_RESOURCE` and `GPU_OP_RECORD_QUEUE_TIMELINE`. The first, followed by the `u32` identity of a resource that the context imported, makes `vkAllocateMemory` import that resource. The second, followed by a `u32` ring index, binds the queue that `vkGetDeviceQueue2` returns to that timeline. They are listed in the header with the commands that use them.

| Name | Value | Meaning |
| --- | ---: | --- |
| `GPU_OP_PROTOCOL_VERSION` | 2 | The revision of the protocol. Version 2 added the video commands. No code compares it. |
| `GPU_OP_OWN_FIRST` | 0x10000 | The first number of zedBSD's own commands, which are sent only to a renderer whose capability set declares them. Numbers below it are those of Venus wire format 1. |
| `GPU_OP_FLAG_REPLY` | 1 | Bit 0 of a command's flag word: the executor writes the command's reply. |

## Wire form

Every value is little-endian. A `u32` is 4 bytes and a `u64` is 8 bytes. Enumerations, flags, counts and indices are `u32`. Sizes and offsets are `u64`. Floating-point values are their 32 IEEE bits. A command is always a whole number of 4-byte words, and a byte array is padded with zeros to a multiple of 4 bytes.

An object is named by a `u64` identity that the sender chooses and the executor adopts. libvulkan never reuses an identity within a session. The null handle is 0.

A pointer argument is a `u64` presence word, 1 or 0, followed by what the pointer points to when it is 1. An array argument is its `u32` count parameter, then a `u64` element count, then the elements. A structure is its fields in declaration order: its `sType` as a `u32`, its `pNext` as a presence word followed by any chained record, then the other fields.

Every command starts with a header of two `u32` words:

```
+---------+---------+------------------------------+
| number  | flags   | arguments in declaration     |
| u32     | u32     | order                        |
+---------+---------+------------------------------+
```

`flags` is `GPU_OP_FLAG_REPLY` for a command whose reply the sender wants, and 0 for a recorded command and for the transport commands that frame a submission. A sender sets only these two values: the executors other than i915 refuse any other flag word with `EINVAL`, while the i915 executor treats any non-zero word as the reply request. A command has no length field. An executor that does not know a number therefore cannot find the start of the next command, and it stops the stream at that command.

A reply is written at the reply cursor that the transport commands below set. It is the command's number as a `u32`, then a `u32` Vulkan result when the Vulkan command returns one, then the command's outputs. The reply of a command that returns nothing is its number alone. The sender checks that the first word of a reply is the number it sent and treats any other value as a lost device.

Two forms recur in the header. In the create form the arguments are the device, the create-info pointer and structure, a null allocator word (`u64` 0), then a pointer word and the identity of the new object; the reply is the number, the result, a pointer word and the same identity. In the destroy form the arguments are the device, the object's identity and a null allocator word, and the reply is the number alone. The header names the library function that implements each form.

## One submission

The kernel runs one command stream per request, so libvulkan wraps every command it needs a reply to in a small frame of transport commands. All of them are in the header. A submission, in order, is:

1. `GPU_OP_SET_REPLY_STREAM`, which names the reply blob: the resource identity, an offset of 0 and the blob's capacity. The reply cursor starts at the beginning of that region.
2. The command itself. A command that is longer than `GPU_COMMAND_MAX` less 128 bytes is first copied into a second session blob and replaced by `GPU_OP_EXECUTE_STREAMS`, which names that blob and runs it in place of this command. Both are mapped blobs of the session, created with `GPU_BLOB_MAPPABLE` and `GPU_BLOB_HOST` ([memory](gpu-memory.md)).
3. `GPU_OP_SEEK_REPLY_STREAM`, which moves the reply cursor to the last 20 bytes of the reply blob.
4. `GPU_OP_ENUMERATE_INSTANCE_VERSION` with `GPU_OP_FLAG_REPLY`, the completion trailer.

The whole frame goes to the node in one request, and how libvulkan sends it depends on the node. When the capability set promises that the renderer runs a command at once and the node has `GPU_CAP_NOTIFICATION`, it sends the frame with `GPU_COMMAND`; if that request answers `EAGAIN`, because earlier work of the open is still queued, it sends the same bytes the queued way. The queued way is `GPU_COMMAND_SUBMIT` with `GPU_COMMAND_CONTEXT_FENCE` on timeline 0 when the node has `GPU_CAP_NOTIFICATION`, and `GPU_COMMAND` otherwise. A recorded command and its `vkEndCommandBuffer` travel in one frame; every other frame holds one command. A session sends one frame at a time. A frame whose request fails with `ENOMEM` or `ENOSPC` fails only that call with an out-of-memory result; any other failure of the request, or of the reply, ends the session.

The trailer is how a program knows that the executor reached the end of the frame. Its reply is 20 bytes at the cursor that step 3 chose: the command number as a `u32`, a Vulkan result of 0, the presence word 1 as a `u64`, and a `u32` API version of 1.1.0 or later. The executor writes the version word last, so a reader that sees a version of at least 1.1.0 and then finds the other three words as expected knows that every earlier command of the frame ran and that its reply is complete. The reply of the caller's own command is in the first bytes of the blob, ahead of the trailer. The blob holds at least the caller's reply capacity plus the 20 bytes, rounded up to a whole page, and libvulkan reuses a blob that is already large enough.

With `GPU_CAP_NOTIFICATION`, libvulkan waits for the decoder's completion record ([submission](gpu-submission.md)) and then reads the trailer; if the trailer is not complete at that point, the session is lost. Without it, libvulkan reads the trailer every millisecond for up to 10 seconds and loses the session when it is still incomplete. The Vulkan result of the command is read from its reply as described above. A successful request, a terminal completion record and a successful Vulkan result are three different facts.

When an executor refuses a command, it stops the frame there. The trailer is never written, and libvulkan reports `VK_ERROR_DEVICE_LOST`, or the out-of-memory result when the request failed with `ENOMEM` or `ENOSPC`. Because the failure is recorded in the session, every later command of the same session also fails with `VK_ERROR_DEVICE_LOST` without being sent.

## Executors

The stream is decoded in one of two places. In the first, the driver in the kernel decodes it; in the second, the driver passes it unread to the host of a virtual machine.

| Node | Who decodes | Unknown command |
| --- | --- | --- |
| amdrdna, bcm2711, ghci, i915, mali, nvrtx, powervr | The driver's own executor in the kernel, which decodes each command and runs it. | The executor refuses the command with `ENOTSUP` and stops the stream there. |
| venus | The Venus renderer on the virtual machine's host. The driver checks only that the length is a multiple of 4 and sends the bytes to the host in a virtio-gpu `SUBMIT_3D` command; no zedBSD code decodes the stream. | The host's renderer decides. The driver does not look at the commands. |

Each executor reads the commands in order, one at a time, and the first command that it refuses ends the stream; the commands before it have already run. A refused command is not skipped, because its length cannot be known. A stream is a whole number of 4-byte words and at most `GPU_COMMAND_MAX` bytes; `GPU_COMMAND` refuses any other length with `EINVAL` before it reaches the driver.

The video commands, numbered from `GPU_OP_OWN_FIRST`, are decoded by the i915, nvrtx and bcm2711 executors (the last only when the driver is built with video support). The other executors refuse them like any unknown command. libvulkan sends them only when the capability set declares video decode, so a renderer that does not declare it is never sent them.

The i915 node also accepts a second kind of stream, a native batch in the driver's own format. The driver tells the two kinds apart by the first word of the stream; a stream that does not start with the native batch's magic word is read as the Vulkan command stream of this document. The native batch is not part of this protocol.

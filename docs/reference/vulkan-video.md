# Vulkan Video decode

zedBSD's libvulkan offers H.264 video decode through the Khronos Vulkan Video
extensions on Intel Gen12 graphics (Alder Lake and later integrated GPUs). A
program decodes on the GPU's video engine (VCS0, the MFX unit) and reads the
decoded pictures back from memory; there is no separate video API such as
VA-API.

## Availability

Video decode is offered only when all of these hold:

- the GPU is driven by zedBSD's native i915 driver (not the Venus path of a
  virtual machine), and the GT has the video engine VCS0;
- the kernel was booted with `i915.debug=video` (or `i915.debug=display,video`).
  Without it the device looks exactly as before: no video queue family, no
  video extension, no `VK_KHR_synchronization2`.

The boot word is a safety gate while the video engine's recovery from a hang is
being verified on hardware. When it is removed, this page will say so.

A program checks for video decode as on any Vulkan implementation: a queue
family whose `queueFlags` has `VK_QUEUE_VIDEO_DECODE_BIT_KHR`, and the device
extensions below. `vkvideo-probe --list` prints both.

## Extensions

| Extension | Notes |
| --- | --- |
| `VK_KHR_synchronization2` | Translated to Vulkan 1.0 commands: a stage or access bit that 1.0 does not have is widened (`ALL_COMMANDS`, `MEMORY_READ` and `MEMORY_WRITE`). |
| `VK_KHR_video_queue` | Sessions, session parameters, coding scopes. |
| `VK_KHR_video_decode_queue` | Decode. |
| `VK_KHR_video_decode_h264` | Header `VK_STD_vulkan_video_codec_h264_decode` version 1.0.0. |

Each depends on the one above it; enable them together.

## Queue family

Family 0 is graphics, compute and transfer. Family 1 is video decode: one queue,
`videoCodecOperations` = `VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR`, no
timestamps (`timestampValidBits` 0), and `queryResultStatusSupport` `VK_TRUE`.
Get its queue with `vkGetDeviceQueue` or `vkGetDeviceQueue2`. Video commands
submitted on family 0 lose the device (`VK_ERROR_DEVICE_LOST`).

## What decodes

| Property | Value |
| --- | --- |
| Profiles (`stdProfileIdc`) | Baseline (66), Main (77), High (100) |
| Chroma, bit depth | 4:2:0, 8 bits luma and chroma |
| Picture layout | Progressive only (`VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR`) |
| Largest coded extent | 4096 x 4096, at most 36864 macroblocks a picture |
| Alignment | 16 x 16 (`pictureAccessGranularity`, `minCodedExtent`) |
| DPB slots, active references | 17, 16 |
| Level | 5.1 (`maxLevelIdc`) |
| DPB and output | Coincide (`VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR`), one image for each slot (`VK_VIDEO_CAPABILITY_SEPARATE_REFERENCE_IMAGES_BIT_KHR`) |
| Bitstream buffer | Offset alignment 32 bytes, size alignment 1 byte; 3- and 4-byte start codes are both accepted |
| Session memory | Row-store and motion-vector buffers, 4 KiB aligned, reported by `vkGetVideoSessionMemoryRequirementsKHR` |

Interlaced pictures (fields, MBAFF, PAFF), 4:2:2, 4:4:4, more than 8 bits and
H.264 encode are not supported, nor are H.265 and AV1.

## Pictures

The decode output and reference format is `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM`
(NV12), `VK_IMAGE_TILING_OPTIMAL`, 2D, one level, one layer, one sample, with
usage `VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR` and
`VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR` only. Sampling the picture, copying
it, or blitting it is not offered.

To read a decoded picture, bind the image to host-visible memory, wait for the
decode, and ask `vkGetImageSubresourceLayout` with the aspect
`VK_IMAGE_ASPECT_PLANE_0_BIT` (luma) and `VK_IMAGE_ASPECT_PLANE_1_BIT`
(interleaved CbCr). The answer gives each plane's offset and row pitch in the
decoder's own tiled layout (Intel Tile Y: 128-byte by 32-row tiles, each made
of 16-byte by 32-row columns). `vkvideo-probe` shows how to de-tile it.

## Results of a decode

A decode whose bitstream values the decoder does not take is **skipped**: the
decoder writes nothing into the picture, the submission still succeeds, and the
DPB slots move as the command says, so later pictures can still be decoded.
The picture's contents are then undefined. A decode is skipped when, for
example, its parameter sets are missing or out of the ranges above, its slices'
offsets do not increase or leave the bitstream buffer, a reference picture has a
different size or layout from the output, or the picture has more than 256
slices.

A result status query tells a program whether its decode ran. Create a query
pool of `VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR` (with the video profile chained),
reset it outside the coding scope with `vkCmdResetQueryPool`, and put
`vkCmdBeginQuery` and `vkCmdEndQuery` around the one decode inside the scope.
`vkGetQueryPoolResults` with `VK_QUERY_RESULT_WITH_STATUS_BIT_KHR` gives:

| Status | Meaning |
| --- | --- |
| `VK_QUERY_RESULT_STATUS_COMPLETE_KHR` (1) | The decode ran on the video engine. |
| `VK_QUERY_RESULT_STATUS_ERROR_KHR` (-1) | The decode was skipped. |
| `VK_QUERY_RESULT_STATUS_NOT_READY_KHR` (0) | The query has not ended (the call returns `VK_NOT_READY`). |

Inline queries (`VK_KHR_video_maintenance1`) are not offered.

A command that breaks the API's rules (for example a decode before the session
was reset, a reference slot that does not hold a picture, a coding scope left
open, a query begun outside a scope) refuses the whole submission with
`VK_ERROR_DEVICE_LOST` before anything runs. If the video engine stops
responding, the submission also returns `VK_ERROR_DEVICE_LOST`; the engine is
reset and video decode is offered to new devices again, up to three times
before the next full GPU reset. Drawing on family 0 continues either way.

## Differences from the specification

- The device's `apiVersion` stays 1.0 while it offers the video extensions,
  which the specification bases on Vulkan 1.1 and `VK_KHR_synchronization2`.
- NV12 and the `PLANE_0` and `PLANE_1` aspects are used without
  `VK_KHR_sampler_ycbcr_conversion`.
- `vkGetImageSubresourceLayout` answers for an `OPTIMAL` decode picture (above).
- A picture of more than 256 slices is skipped.
- A duplicate parameter set key, or an identifier out of the H.264 range, is
  refused with `VK_ERROR_INITIALIZATION_FAILED`.
- A result status query pool's video profile is not checked against the
  session it is used with (there is one codec).

## Example program

`vkvideo-probe` (in the test images) lists what a device offers and decodes an
H.264 elementary stream (Annex B), printing the SHA-256 of each frame in display
order:

```
vkvideo-probe --list
vkvideo-probe [--frames=N] [--expect=FILE.sha256] STREAM.h264
```

With `--expect` it compares each frame with a line of the file. Where the video
family reports result status, each decode is in a result status query, and a
decode that did not complete is printed as `picture N status S`. The run exits
with a nonzero status when a frame differs or a decode did not complete.

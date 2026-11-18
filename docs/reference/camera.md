# Camera devices (/dev/camN)

Status: reference.

A camera is a character device `/dev/cam0` to `/dev/cam3`. The camera
framework owns the node, the buffers and the stream; a source driver (today
`uvc`, the USB Video Class driver) negotiates the mode with the device and
fills the frames. The requests, records and constants are in
[`<uapi/camera.h>`](../../include/uapi/camera.h).

## Nodes and permissions

- The nodes are `cam0` to `cam3` (`CAM_DEVICES_MAX`). A camera takes the
  lowest free number when it is attached; a number is reused only after the
  previous camera's node is gone. A program finds its camera by asking each
  node `CAM_GET_INFO`, not by its number.
- The nodes are mode 0600, owned by root. The session manager gives the
  seat's user the camera nodes the way it gives the GPU nodes, and takes them
  back when the seat changes hands. A camera that is plugged in raises the
  `/dev/system` event class `KERN_SYSTEM_EVENT_CAMERA` with the subject
  `camN` (add and remove), so the seat can give it at once.
- Any number of opens are allowed. Every open may ask: `CAM_GET_INFO`,
  `CAM_ENUM_FORMAT`, `CAM_ENUM_MODE`, `CAM_ENUM_INTERVAL`, `CAM_GET_FORMAT`,
  `CAM_QUERY_BUFFER`, `CAM_GET_CONTROL_INFO`, `CAM_GET_CONTROL` and
  `CAM_GET_COUNTERS`.

## Ownership

The first open that calls `CAM_SET_FORMAT` or `CAM_REQUEST_BUFFERS` with a
nonzero count becomes the owner. While there is an owner, another open's
`CAM_SET_FORMAT`, `CAM_REQUEST_BUFFERS`, `CAM_QUEUE_BUFFER`,
`CAM_DEQUEUE_BUFFER`, `CAM_STREAM_ON`, `CAM_STREAM_OFF`, `CAM_SET_CONTROL`
and `mmap` fail with `EBUSY`. `CAM_TRY_FORMAT` from another open is answered
while the owner only holds a format, and fails with `EBUSY` while the owner
has buffers or streams (the device is not asked while it is in use).

The owner gives the camera up with `CAM_REQUEST_BUFFERS` of count 0 or by
closing. An owner that closes while streaming stops the stream and frees the
buffers. A mapped buffer keeps the file open: the camera sees the close only
after the last `munmap`, so the stream goes on until then.

## Records

Every record starts with `version` (`CAM_ABI_VERSION`, 1) and `size` (the
record's own size), and its `reserved` fields are zero. No record holds a
pointer; every 64-bit field is aligned to 8 bytes; the sizes are fixed by
static assertions in the header, so one layout serves ILP32 and LP64
callers. A wrong version or size fails with `EINVAL`.

## Requests

All requests are in the ioctl group `'C'`.

| Number | Request | Record | What it does |
| --- | --- | --- | --- |
| 1 | `CAM_GET_INFO` | `cam_info` | The driver, product name, location, capability bits (`CAM_CAP_RAW`, `YUV`, `RGB`, `COMPRESSED`, `IMPORT_GPU`, `EXPORT`, `CONTROLS`), `max_buffers`, `format_count`, the device clock `clock_hz`, and `host_payload_max`: the most payload one service interval of the transport carries (0 is no limit). |
| 2 | `CAM_ENUM_FORMAT` | `cam_format_desc` | The format at `index`: its code, flags (`CAM_FORMAT_SUPPORTED`, `CAM_FORMAT_COMPRESSED`), bits per pixel, pixel group and number of modes. `EINVAL` past the last. |
| 3 | `CAM_ENUM_MODE` | `cam_mode_desc` | The frame size at `index` of a format, its interval type (`CAM_INTERVAL_DISCRETE` or `CONTINUOUS`), the first eight discrete intervals (default first) and their total, or the continuous range, and the largest frame. Intervals are in units of 100 ns. |
| 4, 5, 6 | `CAM_GET_FORMAT`, `CAM_SET_FORMAT`, `CAM_TRY_FORMAT` | `cam_format` | In: format, width, height and interval (0 is the mode's default). Out: the interval used, line stride (0 when compressed), image bytes, the payload the device needs per service interval (`payload_bytes`) and the mode index. The size must be one of the modes. `SET` works only without buffers (`EBUSY`) and takes ownership; `TRY` changes nothing. A mode whose `payload_bytes` exceeds `host_payload_max` fails with `ENOSPC`, and the output fields are still filled so a program can tell why. |
| 7 | `CAM_REQUEST_BUFFERS` | `cam_request_buffers` | Makes up to `count` buffers of `memory` (`CAM_MEMORY_DEVICE` or `CAM_MEMORY_IMPORT`) and answers how many and their size in `bytes`. Count 0 frees them and gives up ownership. `EBUSY` while streaming or while a buffer is mapped; `ENOMEM` when not one buffer can be had; `EOPNOTSUPP` for memory the camera cannot fill. |
| 8 | `CAM_QUERY_BUFFER` | `cam_buffer` | The buffer at `index`: its memory, size, `mmap_offset` and state flags (`CAM_BUFFER_QUEUED`, `ACTIVE`, `DONE`, `MAPPED`, `IMPORTED`). |
| 9 | `CAM_QUEUE_BUFFER` | `cam_buffer` | Gives an unused buffer to the camera to fill. A buffer that is queued, being filled or waiting to be dequeued fails with `EBUSY`. For imported memory see below. |
| 10 | `CAM_DEQUEUE_BUFFER` | `cam_buffer` | Takes the oldest finished frame and its metadata. It waits up to `timeout_ms` (0 waits without limit; `O_NONBLOCK` does not wait and fails with `EAGAIN`). The wait ends with `ETIMEDOUT`, `EINTR` on a signal, `EINVAL` when the stream stops, `EIO` when the camera faulted and `ENODEV` when it was unplugged. |
| 11, 12 | `CAM_STREAM_ON`, `CAM_STREAM_OFF` | none | ON needs at least one queued buffer (`EINVAL`) and starts the device; a transport that cannot carry the mode fails with `ENOSPC`. OFF stops the device and returns every buffer to unused, including frames not yet dequeued. OFF also clears a fault. |
| 13 | `CAM_GET_CONTROL_INFO` | `cam_control_info` | The range, step, default and flags of control `id`. |
| 14, 15 | `CAM_GET_CONTROL`, `CAM_SET_CONTROL` | `cam_control` | A control's value. Only the owner sets. |
| 16 | `CAM_GET_COUNTERS` | `cam_counters` | Frames, frames dropped (no buffer, overflow, error, transport), the transport's errors, sequence gaps, the times no frame came (`no_signal`) and how long ago the last frame finished. |
| 17 | `CAM_EXPORT_BUFFER` | — | Reserved; `CAM_CAP_EXPORT` is never set. |
| 18 | `CAM_ENUM_INTERVAL` | `cam_interval_desc` | A discrete interval of a mode beyond the eight `CAM_ENUM_MODE` reports. |

`read` and `write` fail with `EOPNOTSUPP`. An unknown request fails with
`ENOTTY`. After the camera is unplugged every request and `mmap` fails with
`ENODEV`.

`poll` reports `POLLIN` to the owner when a frame can be dequeued, `POLLERR`
when the camera faulted, and `POLLERR | POLLHUP` when it was unplugged. An
open that is not the owner never sees `POLLIN`.

## Formats

| Code | Name | Layout |
| --- | --- | --- |
| `0x0101`–`0x0106` | `CAM_FORMAT_RAW8`, `RAW10P`, `RAW10`, `RAW12P`, `RAW12`, `RAW16` | Bayer mosaic; the order is in `bayer_order`. The unpacked forms hold one sample in 16 bits, shifted by `sample_shift`. |
| `0x0201` | `CAM_FORMAT_YUYV` | Two pixels in four bytes: Y0 U Y1 V. |
| `0x0202` | `CAM_FORMAT_UYVY` | U Y0 V Y1. |
| `0x0203` | `CAM_FORMAT_NV12` | A Y plane, then an interleaved UV plane at half resolution. |
| `0x0204` | `CAM_FORMAT_YUV420P` | Y, U and V planes, U and V at half resolution. |
| `0x0301`–`0x0303` | `CAM_FORMAT_RGB565`, `RGB888`, `XRGB8888` | Packed RGB. |
| `0x0401` | `CAM_FORMAT_MJPEG` | One JPEG image per frame, SOI to EOI. A frame may lack its Huffman tables; the reader then uses the standard tables. Its length is `used_bytes`. |
| `0x0402` | `CAM_FORMAT_H264` | Reserved. |

The `uvc` driver delivers YUYV and MJPEG.

## Buffers

### Camera memory (`CAM_MEMORY_DEVICE`)

Each buffer is one physically contiguous run of whole pages, zeroed when it
is made, so a new buffer never shows an earlier owner's frame. The owner maps
a buffer read-only with `mmap(NULL, bytes, PROT_READ, MAP_SHARED, fd,
mmap_offset)`; a mapping must stay inside one buffer (`EINVAL`), and a
writable mapping fails with `EACCES`. The CPU sees the frame through the
cache; no cache operation is needed.

### Imported GPU memory (`CAM_MEMORY_IMPORT`)

When `CAM_GET_INFO` reports `CAM_CAP_IMPORT_GPU`, the camera can write the
frames straight into memory a Vulkan program allocated, with no copy:

1. The program allocates memory with `VkExportMemoryAllocateInfo`
   (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT`) of at least the format's
   `image_bytes`, and gets a descriptor with `vkGetMemoryFdKHR`.
2. `CAM_REQUEST_BUFFERS` with `memory = CAM_MEMORY_IMPORT` makes the slots,
   still without memory.
3. The first `CAM_QUEUE_BUFFER` of each slot passes that descriptor in `fd`
   (and `memory = CAM_MEMORY_IMPORT`). The camera keeps its own reference
   to the memory until the buffers are freed, so the program may close the
   descriptor. Every later queue of the slot passes `fd = -1`; anything else
   is `EINVAL`.
4. The memory must be one contiguous run of RAM that the GPU maps without
   caching and that holds a whole frame. Other memory fails with `EXDEV`,
   and a descriptor that is not a GPU allocation with `EINVAL`.
5. The program queues a slot again only after the GPU work that reads it has
   finished (after its fence). The camera does not wait for the GPU.

Imported buffers are not mapped through the camera (`mmap` fails with
`EINVAL`); the program maps the memory through Vulkan if it needs the CPU's
view. `CAM_QUERY_BUFFER` and `CAM_DEQUEUE_BUFFER` report
`memory = CAM_MEMORY_IMPORT`, `mmap_offset = 0` and, once bound,
`CAM_BUFFER_IMPORTED`.

## Frame metadata

`CAM_DEQUEUE_BUFFER` fills `cam_frame_metadata`:

- `sequence` counts every frame the camera began, including frames dropped
  for want of a buffer, so a gap is the number of frames lost.
- `timestamp_ns` is the monotonic time the host saw the frame's first data;
  `CAM_METADATA_APPROXIMATE` says it is not the moment of capture (for USB it
  is late by up to a few milliseconds).
- `used_bytes` is the bytes written: the image size for uncompressed frames,
  the JPEG's length for MJPEG.
- `device_pts` and `device_scr` hold the device's presentation time and
  source clock when `CAM_METADATA_PTS` and `CAM_METADATA_SCR` say so.
- `exposure` and `analogue_gain` are `CAM_METADATA_UNKNOWN` for USB cameras.
- `CAM_FRAME_ERROR`: the device or the transport reported an error during
  the frame, or an MJPEG frame does not start with SOI.
  `CAM_FRAME_INCOMPLETE`: the frame is likely short; the lines that were not
  written keep an older frame's pixels, or an MJPEG frame has no EOI.

## Controls

The control identifiers are fixed: 1 to 10 for image sensors, 32 onward for a
USB camera's processing unit and camera terminal (brightness 32, contrast 33,
hue 34, saturation 35, sharpness 36, gamma 37, white balance temperature 38
and its automatic mode 39, backlight compensation 40, gain 41, power line
frequency 42, exposure 48 and its automatic mode 49, focus 50 and its
automatic mode 51, zoom 52). A control the camera does not have reports
`CAM_CONTROL_UNSUPPORTED`, and reading or setting it fails with
`EOPNOTSUPP`. A camera with at least one control sets `CAM_CAP_CONTROLS`.

- The range, step and default come from the device. A switch
  (`CAM_CONTROL_BOOLEAN`: the automatic white balance, exposure and focus)
  is 0 or 1. The power line frequency is 0 (off), 1 (50 Hz) or 2 (60 Hz).
- The exposure time is in units of 100 µs. Automatic exposure 1 selects the
  device's automatic mode (aperture priority where it has one), 0 manual.
- A value outside the range fails with `EINVAL`. A control the device
  reports read-only, or one disabled while its automatic mode is on (the
  exposure time under automatic exposure), fails with `EACCES`.

## USB cameras

- An isochronous USB camera states the payload it sends per service interval
  for each mode. The host has a limit per interval (`host_payload_max`); a
  mode above it fails with `ENOSPC` rather than streaming with losses. A
  lower resolution, a lower frame rate or MJPEG usually fits.
- A high-speed camera may use up to three transactions per microframe
  (3072 bytes). SuperSpeed isochronous cameras are not supported.
- Bulk cameras have no per-interval limit (`host_payload_max` is 0).

## Suspend

While a camera streams over isochronous USB, the USB host refuses the system
suspend, so the machine does not sleep (the attempt fails and is logged). A
program that streams stops the stream (`CAM_STREAM_OFF`) when the lid closes
or the system is about to sleep, and starts it again after resume.

## Test programs

- `camcap` captures frames and writes them as JPEG or PPM, lists the
  formats and modes (`-l`), and reports the counters.
- `camcheck` checks the colours, the frame counter and the drops against a
  known test pattern shown on the source, and checks for leaks over repeated
  start and stop.
- `camvk` captures YUYV frames into imported Vulkan memory, turns them gray
  with a compute shader and compares the result with the CPU's reading of the
  same memory.

# Audio Architecture and Sound Subsystem

Status: current

This document describes zedBSD's audio architecture: the kernel sound interface,
the userland sound daemon (`/sbin/audiod`), the lock-free shared memory ring buffer
protocol, and the `libpulse` compatibility layer that enables modern applications
(such as Chromium and media players) to run unmodified.

---

## 1. Design Rationale

Traditional Unix audio systems often suffer from high latency, complex IPC, or
unbounded context-switching overhead:
- **Audio Over Sockets**: Streaming PCM samples over UNIX domain or TCP sockets
  (as in early sound daemons) incurs repeated kernel copies and context switches.
- **Direct Hardware Access**: Allowing multiple client applications to open
  `/dev/dsp0` directly leads to hardware device contention and lacks software
  mixing, volume normalization, and sample rate conversion.

zedBSD adopts a high-efficiency hybrid architecture:
1. **Userland Mixing with Hardware Separation**:
   Only `/sbin/audiod` opens `/dev/dsp0` and `/dev/mixer0`. All applications
   communicate exclusively with `audiod`.
2. **Zero-Streaming Control Socket**:
   No PCM audio samples flow across the UNIX domain socket. The socket is reserved
   strictly for control messages (stream creation, play/pause, volume changes).
3. **Lock-Free Shared Memory Ring Buffers**:
   Audio samples are transferred via dedicated, per-stream shared memory ring
   buffers created with anonymous POSIX shared memory and passed using `SCM_RIGHTS`.
4. **Seamless Compatibility**:
   Instead of porting heavyweight external audio servers, zedBSD provides a
   lightweight, clean-room `libpulse.so` compatibility library that translates
   PulseAudio API calls directly into `audiod` shared memory operations.

---

## 2. Audio Pipeline Overview

```
+--------------------------------------------------------------------------+
| Client Applications (Chromium, mpv, Keiland Desktop, Games)              |
|                                                                          |
|   libpulse.so (API Compatibility Shim)        libkeiland-audio           |
+--------------------------------------------------------------------------+
       |                                              |
       | 1. Control: UNIX Socket (/run/audiod.sock)   |
       | 2. Memory:  SCM_RIGHTS (Anonymous SHM fd)   |
       v                                              v
+--------------------------------------------------------------------------+
| /sbin/audiod (Sound Server Daemon)                                       |
|   - Multi-channel software mixer (32-bit float / 16-bit integer)         |
|   - Asynchronous Sample Rate Converter (ASRC)                            |
|   - Per-stream volume scaling and master volume coordination             |
|   - Latency tracking & A/V sync timestamps                               |
+--------------------------------------------------------------------------+
       |
       | OSS mmap buffer or periodic write()
       v
+--------------------------------------------------------------------------+
| Kernel Audio Subsystem (src/drivers/audio/)                              |
|   - /dev/dsp0 (Intel HDA, USB Audio, AC'97, SoC I2S)                     |
|   - /dev/mixer0                                                          |
+--------------------------------------------------------------------------+
```

---

## 3. The `audiod` Protocol and Shared Memory Ring

[`/sbin/audiod`](../../userland/base/audiod/main.c) listens on `/run/audiod.sock`
(mode `0666`).

### 3.1 Establishing Streams

1. **Handshake**:
   The client connects and sends `HELLO { version }`. `audiod` responds with
   `WELCOME` reporting the hardware device's native format, sample rate (e.g.
   48 kHz), channel count, and hardware period size.
2. **Stream Creation (`STREAM_CREATE`)**:
   The client requests a playback or capture stream, specifying desired sample
   format, channel layout, and target latency.
3. **Anonymous Shared Memory Allocation**:
   - `audiod` creates a shared memory segment:
     `shm_open("/audiod-<pid>-<id>", O_RDWR|O_CREAT|O_EXCL, 0600)`
   - Immediately calls `shm_unlink()`: the file name is removed from `/tmp` /
     `/run`, making the memory object completely anonymous and ensuring it is
     freed when all descriptors are closed.
   - `ftruncate()` sizes the segment to header size (4096 bytes) plus ring buffer
     capacity.
   - `audiod` sends `STREAM_CREATED` with the shared memory file descriptor via
     `SCM_RIGHTS`.

### 3.2 Shared Memory Layout

```
Offset 0       +----------------------------------------------------------+
               | struct audiod_shm_header (4096 bytes, 1 memory page)     |
               | - Stream format, rate, channels, capacity                |
               | - Cacheline-isolated atomic read/write/played positions  |
Offset 4096    +----------------------------------------------------------+
               | Ring Buffer Storage                                      |
               | (capacity_frames * frame_bytes, rounded to page size)    |
               +----------------------------------------------------------+
```

### 3.3 Cache-Line Isolation and Lock-Free Synchronization

To eliminate CPU cache-line bouncing (false sharing) between client and server,
the header fields are aligned to 64-byte boundaries:

```c
struct audiod_shm_header {
    uint32_t magic;            /* 'AUDD' */
    uint32_t version;
    uint32_t format;
    uint32_t channels;
    uint32_t rate;
    uint32_t frame_bytes;
    uint32_t capacity_frames;
    uint32_t period_frames;

    /* Written only by the client (playback) or audiod (capture) */
    _Alignas(64) uint64_t write_position;

    /* Written only by audiod (playback) or client (capture) */
    _Alignas(64) uint64_t read_position;

    /* Device playback timestamp and progress written by audiod */
    _Alignas(64) uint64_t played_position;
    int64_t               played_time_ns;

    uint32_t underruns;
    uint32_t overruns;
};
```

- **Single-Writer Seqlock**:
  On 32-bit platforms (such as `i386`) where 64-bit integer writes are not
  natively lock-free, `audiod` uses a single-writer sequence word
  (`audiod_position_store` / `audiod_position_load`) ensuring torn 64-bit reads
  are detected and retried without mutexes.
- **Accurate Latency Tracking**:
  `played_position` paired with `played_time_ns` (`CLOCK_MONOTONIC`) provides
  microsecond-accurate delay computation, allowing media players to maintain
  exact lip-sync during video playback.

---

## 4. The `libpulse` Compatibility Layer

Modern cross-platform applications (including Chromium, Firefox, WebRTC, and
SDL2) typically output sound via the PulseAudio client library (`libpulse.so`).

Instead of running a heavy PulseAudio or PipeWire server, zedBSD provides a
built-in, clean-room implementation of `libpulse.so` that translates PulseAudio
API primitives to `audiod` shared memory operations:

### 4.1 Function Mapping

| PulseAudio API | `audiod` Protocol / SHM Action |
| --- | --- |
| `pa_context_connect()` | Connects to `/run/audiod.sock` and exchanges `HELLO`/`WELCOME`. |
| `pa_stream_new()` | Allocates client-side stream tracking structure. |
| `pa_stream_connect_playback()` | Sends `STREAM_CREATE`, receives SHM fd via `SCM_RIGHTS`, and calls `mmap()`. |
| `pa_stream_write()` | Copies caller samples into the shared memory ring buffer and updates `write_position`. |
| `pa_stream_begin_write()` | **Zero-copy path**: Returns a direct pointer into the shared memory ring buffer, allowing clients to decode audio directly into shared memory. |
| `pa_stream_cork()` | Sends `STREAM_START` / `STREAM_STOP` control messages. |
| `pa_stream_get_time()` / `_latency()` | Computes current delay from `played_position` and `played_time_ns`. |
| `pa_stream_drain()` | Sends `STREAM_DRAIN` and waits for the `DRAINED` notification. |
| `pa_stream_flush()` | Sends `STREAM_FLUSH` and resets ring buffer positions. |

### 4.2 Advantages of the Compatibility Shim

1. **Lightweight**: Zero background overhead—applications talk directly to
   `audiod` without intermediate routing daemons or D-Bus dependencies.
2. **Binary Compatibility**: Standard dynamically linked binaries looking for
   `libpulse.so.0` run without source modifications.
3. **Low Latency**: Because `pa_stream_begin_write` accesses the shared memory
   ring directly, buffer round-trip latency is bounded only by the hardware
   mixing period (typically 5 to 10 ms).

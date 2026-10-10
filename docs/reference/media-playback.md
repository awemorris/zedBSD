# Media playback

Video Player and Music first use zedBSD's native media library. Its audio decoder supports AAC-LC. Its video decoder supports progressive, 8-bit, 4:2:0 H.264 Baseline, Main and High, using Vulkan Video.

## Hardware video

The library asks Vulkan for H.264 profile, level, image format, reference capacity and transfer capabilities. A GPU without the required capabilities cannot use native H.264 decoding. The current implementation is intended for the i915 video decoder; physical playback verification is still pending. Other drivers can provide the same standard Vulkan capabilities without adding device-specific code to the media library.

Decoded video is copied through the standard Vulkan image-to-buffer operation into CPU NV12 pictures. Applications can retain and scale those pictures after closing the decoder. Sample aspect ratio and BT.601, BT.709 or BT.2020 colour coefficients accompany the picture. Hardware decoding errors stop playback; a successful seek resets decoder state.

## Optional application codecs

Video Player and Music can load an installed, compatible libavcodec with `dlopen` when a native codec, profile or device is unsupported. The optional software codec is owned by the applications; libmedia does not link to or load it. If it is absent, an unsupported video cannot be played. Supported native AAC-LC does not require libavcodec.

HE-AAC, SBR and Parametric Stereo are not native AAC-LC support. They are rejected, including SBR signalled within a compressed packet. The applications may use the optional software codec for these formats.

## Current limits

- Native H.264 excludes interlaced pictures, 10-bit video, multiple slice groups, arbitrary slice ordering and redundant coded pictures. It admits all active reference lists before submitting GPU work; pictures requiring unavailable references are dropped.
- AAC coupling and gain control are unsupported. The AAC decoder accepts one to eight channels and produces stereo output; the LFE channel is omitted from the stereo downmix.
- The player applies MP4 edit-list start and end trimming and retains one AAC block of overlap preroll for seeking. It does not read `iTunSMPB` metadata.
- Normal media playback, hardware pixel identity, performance and simultaneous physical decoder sessions remain subject to verification on the target machine.

## Native diagnostics

Test images can include `media-probe`. It always uses native libmedia and has no application codec fallback.

```
media-probe --video-hash --time movie.mp4
media-probe --video-hash --twice --expect=movie.video.sha256 movie.mp4
media-probe --video-hash --seek=1 --expect=movie.video.sha256 movie.mp4
media-probe --audio-rms movie.mp4
```

Video output contains the presentation timestamp in microseconds and the SHA-256 of visible NV12 luma followed by interleaved chroma. Audio output contains normalized stereo RMS for successive blocks of 1024 output frames at 48 kHz, with a shorter final block. `--time` reports native open time and elapsed processing time; it does not separate GPU decode from transfer time.

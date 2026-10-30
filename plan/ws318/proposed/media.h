/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * libmedia: the one way the desktop's programs decode, encode and capture
 * media.  A program makes a context, optionally on its own Vulkan device
 * (media-vulkan.h), and opens decoders, encoders and cameras on it.  Each
 * codec is served by the first implementation that takes it, in the order
 * hardware (Vulkan Video on the context's device), libmedia's own software,
 * then FFmpeg's libavcodec, loaded by libmedia with dlopen when the system
 * has it.  A program may ask what the implementations can do and name the
 * one it wants.
 *
 * Pictures travel as frames (struct media_frame): reference-counted, in
 * the CPU's memory or as an image of the context's Vulkan device.  The
 * same frame type comes out of decoders and cameras and goes into
 * encoders, so a camera's frame can be shown and encoded without a copy
 * by the CPU.  No file descriptor or other operating-system handle is part
 * of this interface.
 *
 * Threads: the decoder, encoder and frame calls run on the caller's thread
 * and may wait for the GPU for a bounded time, so a program calls them from
 * its own worker thread, never from its event loop.  One object is used by
 * one thread at a time; different objects may be used at once.  The
 * engines and cameras have named threads of their own and tell the program
 * that something is waiting through its notify function (below).
 *
 * The library's internal API, for the desktop's programs; not a part of
 * any SDK.
 *
 * This is the proposed final form.  During the migration the calls of the
 * old interface (media_decoder_open, media_decoder_gpu_pictures,
 * media_frame_gpu, media_frame_free, media_codec_load, media_codec_reason,
 * media_engine_open, media_engine_wake_fd) stay beside these and are
 * removed in the last migration step.
 */

#ifndef LIBMEDIA_MEDIA_H
#define LIBMEDIA_MEDIA_H

#include "userland/desktop/mediafile/mediafile.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Why a codec could not be used.  The values are above every errno value
 * of the systems the desktop runs on, because the same calls also answer
 * an errno value (ENOMEM, EINVAL, EAGAIN, ENODEV, ...).
 *
 * When every implementation refuses a codec, the answer is chosen in this
 * order: MISSING when libavcodec would have been asked but is not
 * installed; else the refusal of the first implementation that knows the
 * codec (DEVICE, PROFILE, BUSY, CONFIG, VERSION); FORMAT only when no
 * implementation knows the codec at all.
 */
#define MEDIA_PROBLEM_MISSING	1001	/* libavcodec is not installed */
#define MEDIA_PROBLEM_VERSION	1002	/* a version of libavcodec libmedia does not know */
#define MEDIA_PROBLEM_FORMAT	1003	/* no implementation knows the codec */
#define MEDIA_PROBLEM_DEVICE	1004	/* no device with the needed Vulkan Video capabilities */
#define MEDIA_PROBLEM_PROFILE	1005	/* the stream's profile or tools are outside the implementations */
#define MEDIA_PROBLEM_BUSY	1006	/* all hardware video sessions are in use */
#define MEDIA_PROBLEM_CONFIG	1007	/* the encoder settings are outside every implementation of the codec */
#define MEDIA_PROBLEM_LOST	1008	/* the context's Vulkan device was lost; make the context again */

/* A codec's direction (media_codec_query). */
#define MEDIA_DECODE		1U
#define MEDIA_ENCODE		2U

/* The kinds of implementation, in the order they are tried. */
#define MEDIA_KIND_HARDWARE	1U	/* Vulkan Video on the context's device */
#define MEDIA_KIND_SOFTWARE	2U	/* libmedia's own code */
#define MEDIA_KIND_LIBAVCODEC	3U	/* FFmpeg's libavcodec, opened with dlopen */

/* Which kinds a decoder or encoder may use (bits; 0 means all). */
#define MEDIA_ALLOW_HARDWARE	0x1U
#define MEDIA_ALLOW_SOFTWARE	0x2U
#define MEDIA_ALLOW_LIBAVCODEC	0x4U
#define MEDIA_ALLOW_ANY		0x7U

/*
 * Profiles, numbered per codec (MEDIA_CODEC_* of mediafile.h).  A query's
 * profiles field has bit (1 << profile) set for each profile supported.
 * 0 asks for the implementation's default.
 */
#define MEDIA_PROFILE_DEFAULT			0U
#define MEDIA_PROFILE_H264_CONSTRAINED_BASELINE	1U
#define MEDIA_PROFILE_H264_MAIN			2U
#define MEDIA_PROFILE_H264_HIGH			3U
#define MEDIA_PROFILE_HEVC_MAIN			1U
#define MEDIA_PROFILE_HEVC_MAIN_10		2U	/* named so a query can report it; not produced yet */
#define MEDIA_PROFILE_AAC_LC			1U

/*
 * The layouts of a frame's pixels, without Vulkan's names (media-vulkan.h
 * maps them).  4:2:0 formats have even width and height.
 */
#define MEDIA_FORMAT_NONE	0U
#define MEDIA_FORMAT_NV12	1U	/* 8-bit Y plane, then one plane of Cb and Cr interleaved, 4:2:0 */
#define MEDIA_FORMAT_I420	2U	/* 8-bit Y, Cb and Cr planes, 4:2:0 */
#define MEDIA_FORMAT_YUYV	3U	/* 8-bit packed 4:2:2, bytes Y0 Cb Y1 Cr */
#define MEDIA_FORMAT_UYVY	4U	/* 8-bit packed 4:2:2, bytes Cb Y0 Cr Y1 */
#define MEDIA_FORMAT_BGRA8	5U	/* bytes B G R A (the canvas's 0xAARRGGBB on a little-endian CPU) */
#define MEDIA_FORMAT_RGBA8	6U	/* bytes R G B A */
#define MEDIA_FORMAT_P010	7U	/* reserved for 10-bit 4:2:0; not produced yet */
#define MEDIA_FORMAT_MJPEG	8U	/* a camera mode only: frames are always decoded before they are handed out */
#define MEDIA_FORMATS_MAX	8U	/* the most formats a request lists */

/* Where a frame's pixels are. */
#define MEDIA_MEMORY_CPU	1U	/* the CPU's memory: media_frame_planes */
#define MEDIA_MEMORY_VULKAN	2U	/* an image of the context's device: media_frame_vulkan (media-vulkan.h) */

/* The colour matrix of a frame (the values of the old interface kept). */
#define MEDIA_COLOUR_601	0U
#define MEDIA_COLOUR_709	1U
#define MEDIA_COLOUR_2020	2U

/*
 * A frame's colour: the matrix (MEDIA_COLOUR_*), whether the samples use
 * the full range, and the colour primaries, transfer characteristics and
 * chroma sample location as the codec signals them (ITU-T H.273 code
 * points; 2 = unspecified).
 */
struct media_colour {
	unsigned matrix;
	int full_range;
	unsigned primaries;
	unsigned transfer;
	unsigned chroma_location;
};

/*
 * A CPU frame's planes: the format, the picture's size, how many planes
 * the format has, and each plane's first byte and bytes per row.  The
 * memory belongs to the frame; it stays valid while the caller holds a
 * reference.
 */
struct media_planes {
	unsigned format;
	uint32_t width;
	uint32_t height;
	unsigned count;
	uint8_t *data[3];
	size_t stride[3];
};

/* What a notify function is told (the object is the engine or camera concerned, or the context). */
#define MEDIA_EVENT_ENGINE	1U	/* an engine's state changed or a picture came: read media_engine_status */
#define MEDIA_EVENT_CAMERA	2U	/* a camera has a frame waiting, or stopped: media_camera_take */
#define MEDIA_EVENT_LOST	3U	/* the context's device was lost */

/*
 * A program's notify function.  libmedia calls it on one of its own
 * threads, holding none of its locks, once when something starts waiting
 * (it is not called again for the same object until the program has
 * taken what waits).  It must return at once and must not call libmedia;
 * a Keiland program posts an event with kl_app_post and does the work on
 * its own thread.
 */
typedef void (*media_notify_fn)(void *data, unsigned event, void *object);

struct media_context;
struct media_decoder;
struct media_encoder;
struct media_camera;
struct media_engine;
struct media_frame;
struct media_scaler;
struct media_vulkan_device;

/* ---- Context ---- */

/* How a context reaches a GPU. */
#define MEDIA_DEVICE_NONE	0U	/* no GPU: software and libavcodec, CPU frames */
#define MEDIA_DEVICE_PRIVATE	1U	/* libmedia's own Vulkan device, made when first needed; frames are read back into CPU frames */
#define MEDIA_DEVICE_SHARED	2U	/* the program's device (media-vulkan.h); frames may be its images */

/*
 * What a context is made with: the device mode, the program's device for
 * MEDIA_DEVICE_SHARED (NULL otherwise), and the function told of the
 * context's own events (MEDIA_EVENT_LOST; NULL for none).
 */
struct media_context_info {
	unsigned device;
	const struct media_vulkan_device *vulkan;
	media_notify_fn notify;
	void *notify_data;
};

/*
 * Makes a context.  With MEDIA_DEVICE_SHARED the program's device must
 * have been made with what media_vulkan_device_requirements asked and
 * must outlive the context.  Returns 0; EINVAL for a missing or
 * inconsistent device description; ENOTSUP when a SHARED device lacks a
 * required extension or feature it says it enabled; ENOMEM.
 */
int media_context_create(const struct media_context_info *info, struct media_context **context);

/*
 * Reports 0, or MEDIA_PROBLEM_LOST once the context's device was lost
 * (every call on the context's objects then answers MEDIA_PROBLEM_LOST,
 * except the closing and unreferencing ones).
 */
int media_context_status(const struct media_context *context);

/*
 * Frees a context.  Returns 0, or EBUSY (and frees nothing) while a
 * decoder, encoder, camera, engine or frame made on it still lives.
 * NULL is left alone.
 */
int media_context_destroy(struct media_context *context);

/* ---- What the implementations can do ---- */

/*
 * One implementation of a codec in one direction, as the context can use
 * it.  id names it for a request (stable for the context's life); kind
 * and name ("vulkan-video", "h264-software", "aac-lc", "libavcodec:h264"
 * and so on); state is 0 when it can be used now, or the MEDIA_PROBLEM_*
 * that stops it (MISSING, VERSION, DEVICE).  The rest describe what it
 * takes: the profiles (bit 1 << MEDIA_PROFILE_*), the highest level
 * (level_idc of H.264, general_level_idc of H.265, 0 when not known), the
 * largest picture, for an encoder its rate controls (bit 1 <<
 * MEDIA_RATE_*), the most B-frames in a row and references, the formats
 * it reads (encode) or writes (decode) without a conversion in their
 * memory (memory, formats), and for sound the rates and channels.
 */
struct media_codec_info {
	unsigned codec;
	unsigned direction;
	unsigned id;
	unsigned kind;
	char name[32];
	int state;
	uint32_t profiles;
	unsigned max_level;
	uint32_t max_width;
	uint32_t max_height;
	uint32_t rate_controls;
	unsigned max_b_frames;
	unsigned max_references;
	unsigned memory;
	unsigned formats[MEDIA_FORMATS_MAX];
	unsigned format_count;
	uint32_t sample_rates;	/* bit n: the rate of MPEG-4 sampling frequency index n */
	unsigned max_channels;
};

/*
 * Lists the implementations of a codec in one direction, in the order
 * they are tried, up to capacity; count is the number there are (more
 * than capacity when the array was short).  Asking may load libavcodec
 * once.  Returns 0 (also with count 0), or EINVAL.
 */
int media_codec_query(struct media_context *context, unsigned direction, unsigned codec, struct media_codec_info *infos, unsigned capacity, unsigned *count);

/* ---- Decoding ---- */

/*
 * What a decoder is asked for: the kinds it may use (MEDIA_ALLOW_*, 0 for
 * all) or one implementation (id from media_codec_query, 0 for any); the
 * memory its frames should be in (MEDIA_MEMORY_VULKAN needs a SHARED
 * context); the formats the program takes, preferred first (none: the
 * decoder's own); and how many frames the program keeps at once at most
 * (the decoder keeps that many images free of its references).
 */
struct media_decoder_config {
	unsigned allow;
	unsigned implementation;
	unsigned memory;
	unsigned formats[MEDIA_FORMATS_MAX];
	unsigned format_count;
	unsigned hold;
};

/*
 * Opens a decoder for a track.  The track and its private data may go
 * away after the call (the decoder keeps a copy).  Returns 0; a
 * MEDIA_PROBLEM_* as described above; EINVAL; ENOMEM.  A decoder whose
 * implementation refuses the stream later (send answering ENOTSUP, ENODEV
 * or EBUSY inside libmedia) moves to the next implementation by itself.
 */
int media_decoder_create(struct media_context *context, const struct media_track *track, const struct media_decoder_config *config, struct media_decoder **decoder);

/* Names: the codec ("h264"), the implementation ("vulkan-video") and its kind. */
const char *media_decoder_name(const struct media_decoder *decoder);
const char *media_decoder_backend(const struct media_decoder *decoder);
unsigned media_decoder_kind(const struct media_decoder *decoder);

/*
 * Sends a packet (NULL drains the decoder at the end).  Returns 0, EAGAIN
 * when pictures or sound must be received first, EINVAL, ENOMEM, or a
 * MEDIA_PROBLEM_* when no implementation is left.
 */
int media_decoder_send(struct media_decoder *decoder, const struct media_packet *packet);

/* Receives the next picture or sound with its time: 1 with one, 0 when a packet is needed or the end came, < 0 a lasting error. */
int media_decoder_receive(struct media_decoder *decoder, int64_t *time_us);

/* Takes the picture received, with one reference the caller owns; NULL when it cannot be taken. */
struct media_frame *media_decoder_picture(struct media_decoder *decoder);

/* Converts the sound received into 16-bit interleaved stereo at a rate, up to capacity frames; returns how many. */
size_t media_decoder_sound(struct media_decoder *decoder, int16_t *samples, size_t capacity, uint32_t rate);

/* Trims sound before a time after a seek (ENOTSUP when the implementation cannot), the codec frame's duration for preroll. */
int media_decoder_trim(struct media_decoder *decoder, int64_t before_us);
int64_t media_decoder_frame_us(const struct media_decoder *decoder);

/* Empties a decoder after a seek; closes one (NULL is left alone; frames taken stay valid). */
void media_decoder_flush(struct media_decoder *decoder);
void media_decoder_close(struct media_decoder *decoder);

/* ---- Frames ---- */

/* Adds a reference; drops one (NULL is left alone).  The last drop gives the pixels back to their maker. */
void media_frame_ref(struct media_frame *frame);
void media_frame_unref(struct media_frame *frame);

/* The picture's size (the visible window), its sample aspect ratio, memory, format, colour and time. */
void media_frame_size(const struct media_frame *frame, int *width, int *height);
void media_frame_aspect(const struct media_frame *frame, int *num, int *den);
unsigned media_frame_memory(const struct media_frame *frame);
unsigned media_frame_format(const struct media_frame *frame);
void media_frame_colour(const struct media_frame *frame, struct media_colour *colour);
int64_t media_frame_time(const struct media_frame *frame);

/* Describes a CPU frame's planes: 0, or ENOTSUP for a frame in Vulkan memory. */
int media_frame_planes(const struct media_frame *frame, struct media_planes *planes);

/*
 * Makes an empty frame for the program to fill, with one reference: in
 * the CPU's memory (planes writable) or as an image of a SHARED context's
 * device usable as a colour attachment, a storage image or a transfer
 * destination (media-vulkan.h).  Returns 0, EINVAL (an odd size for a
 * 4:2:0 format, a format the memory cannot hold), ENOTSUP (Vulkan memory
 * without a SHARED context), ENOMEM.
 */
int media_frame_create(struct media_context *context, unsigned memory, unsigned format, uint32_t width, uint32_t height, struct media_frame **frame);

/*
 * Scales a CPU frame into 32-bit pixels (0xAARRGGBB) through the caller's
 * scaler, kept between calls.  For programs that draw with the CPU (the
 * browser's engine); 0, ENOTSUP for a Vulkan frame, EINVAL, ENOMEM.
 */
int media_frame_scale(const struct media_frame *frame, struct media_scaler **scaler, uint32_t *pixels, size_t stride, int width, int height);
void media_scaler_free(struct media_scaler *scaler);

/* ---- Encoding ---- */

/* Rate controls (media_encoder_config.rate_control, bits of media_codec_info.rate_controls). */
#define MEDIA_RATE_DEFAULT	0U	/* the implementation's own choice */
#define MEDIA_RATE_CQP		1U	/* a constant quantizer (qp) */
#define MEDIA_RATE_CBR		2U	/* a constant bit rate (bitrate) */
#define MEDIA_RATE_VBR		3U	/* a variable bit rate (bitrate on average, max_bitrate at most) */

/* How an encoder's packets are framed. */
#define MEDIA_OUTPUT_ANNEX_B	0U	/* H.264/H.265: start codes, parameter sets before each key frame; AAC: ADTS */
#define MEDIA_OUTPUT_RAW	1U	/* H.264/H.265: 4-byte NAL lengths, parameter sets only in codec data; AAC: raw access units */

/*
 * What an encoder is asked for.  Video: codec, profile, level (0: chosen
 * from the size and rate), the picture's size and frame rate, the rate
 * control and its numbers (bits per second, qp for CQP), the key frame
 * interval in frames (0: two seconds), B-frames in a row (0 only for
 * now), the colour signalled.  Sound (AAC): rate, channels, bitrate.
 * allow and implementation choose as for a decoder.  output frames the
 * packets (MEDIA_OUTPUT_*).
 */
struct media_encoder_config {
	unsigned codec;
	unsigned allow;
	unsigned implementation;
	unsigned profile;
	unsigned level;
	uint32_t width;
	uint32_t height;
	uint32_t rate_num;
	uint32_t rate_den;
	unsigned rate_control;
	uint32_t bitrate;
	uint32_t max_bitrate;
	unsigned qp;
	uint32_t key_interval;
	unsigned b_frames;
	struct media_colour colour;
	uint32_t sample_rate;
	unsigned channels;
	unsigned output;
};

/* A packet an encoder made: its bytes (the encoder's until the next receive or close), times and flags. */
struct media_encoded {
	const unsigned char *data;
	size_t size;
	int64_t time_us;
	int64_t decode_time_us;
	unsigned flags;
};

/* media_encoded.flags, and media_encoder_send_frame's flags. */
#define MEDIA_ENCODED_KEY	0x1U	/* the packet starts a key frame */
#define MEDIA_ENCODE_FORCE_KEY	0x1U	/* make this frame a key frame */

/* The codec data an encoder gives for a container. */
#define MEDIA_CODEC_DATA_PARAMETER_SETS	1U	/* H.264 SPS and PPS, H.265 VPS, SPS and PPS, with start codes */
#define MEDIA_CODEC_DATA_AVCC		2U	/* an AVCDecoderConfigurationRecord */
#define MEDIA_CODEC_DATA_HVCC		3U	/* an HEVCDecoderConfigurationRecord */
#define MEDIA_CODEC_DATA_AUDIO_CONFIG	4U	/* an AudioSpecificConfig */

/*
 * Opens an encoder: the first implementation, in the order of
 * media_codec_query, that takes the codec and the whole configuration.
 * Returns 0; MEDIA_PROBLEM_FORMAT, _CONFIG, _DEVICE, _BUSY, _MISSING,
 * _VERSION; EINVAL; ENOMEM.  An encoder never changes implementation after
 * it opened (its packets would not continue one stream).
 */
int media_encoder_create(struct media_context *context, const struct media_encoder_config *config, struct media_encoder **encoder);

/* Names of the implementation, its kind, and the frames it reads without a conversion. */
const char *media_encoder_backend(const struct media_encoder *encoder);
unsigned media_encoder_kind(const struct media_encoder *encoder);
void media_encoder_input(const struct media_encoder *encoder, unsigned *memory, unsigned *format);

/*
 * Sends a frame of the configured size at a time (NULL drains).  The
 * encoder takes its own reference and drops it when the GPU or CPU has
 * read the frame; a frame of another memory or format is converted once
 * on the GPU (or read back for a CPU implementation).  Returns 0, EAGAIN
 * when packets must be received first, EINVAL (size, time not after the
 * last), ENOMEM, MEDIA_PROBLEM_LOST.
 */
int media_encoder_send_frame(struct media_encoder *encoder, struct media_frame *frame, int64_t time_us, unsigned flags);

/* Sends interleaved 16-bit sound of the configured rate and channels (NULL drains); same answers. */
int media_encoder_send_sound(struct media_encoder *encoder, const int16_t *samples, size_t frames, int64_t time_us);

/* Receives the next packet: 1 with one, 0 when more input is needed or the drain is done, < 0 a lasting error. */
int media_encoder_receive(struct media_encoder *encoder, struct media_encoded *packet);

/* Copies a form of codec data (MEDIA_CODEC_DATA_*): 0, ENOTSUP for a form the codec has not, ENOBUFS (length says the size). */
int media_encoder_codec_data(const struct media_encoder *encoder, unsigned form, unsigned char *buffer, size_t size, size_t *length);

/* Closes an encoder (NULL is left alone); undrained input is dropped. */
void media_encoder_close(struct media_encoder *encoder);

/* ---- Cameras ---- */

/* A camera as listed: its index, the device's name and where it is plugged. */
struct media_camera_desc {
	unsigned index;
	char name[64];
	char location[64];
};

/* One mode a camera can deliver: the format it sends (MEDIA_FORMAT_*), size and frame interval. */
struct media_camera_mode {
	unsigned format;
	uint32_t width;
	uint32_t height;
	uint32_t interval_num;
	uint32_t interval_den;
};

/*
 * What a camera is opened with: the wanted size and frame rate (the
 * nearest mode is chosen), the memory and formats the program takes
 * (as for a decoder), how many frames may be held at once by the program
 * and an encoder together, an encoder the frames will go to (its input
 * format is preferred so that no conversion is needed; NULL for none),
 * and the notify function told of MEDIA_EVENT_CAMERA.
 */
struct media_camera_config {
	uint32_t width;
	uint32_t height;
	uint32_t rate_num;
	uint32_t rate_den;
	unsigned memory;
	unsigned formats[MEDIA_FORMATS_MAX];
	unsigned format_count;
	unsigned hold;
	const struct media_encoder *encoder;
	media_notify_fn notify;
	void *notify_data;
};

/* Lists the cameras (count), describes one, lists its modes (count may exceed capacity). */
int media_camera_count(struct media_context *context, unsigned *count);
int media_camera_describe(struct media_context *context, unsigned index, struct media_camera_desc *desc);
int media_camera_modes(struct media_context *context, unsigned index, struct media_camera_mode *modes, unsigned capacity, unsigned *count);

/*
 * Opens a camera.  Returns 0; ENOENT (no such camera), EACCES (the
 * session does not own it), EBUSY (another program streams from it),
 * ENOTSUP (no mode near the request in a format libmedia reads), ENOMEM.
 */
int media_camera_open(struct media_context *context, unsigned index, const struct media_camera_config *config, struct media_camera **camera);

/* The mode chosen at open, and whether the frames reach an encoder without a conversion. */
void media_camera_mode(const struct media_camera *camera, struct media_camera_mode *mode, int *direct);

/* Starts and stops the frames (the camera's thread waits on the device while started). */
int media_camera_start(struct media_camera *camera);
int media_camera_stop(struct media_camera *camera);

/*
 * Takes the oldest frame waiting, with one reference: 0; EAGAIN when none
 * waits; ENODEV when the camera went away; EIO when the stream failed.
 * When the program does not take frames in time the oldest are dropped
 * (counted in dropped).
 */
int media_camera_take(struct media_camera *camera, struct media_frame **frame, uint64_t *dropped);

/* Stops and closes a camera; frames taken stay valid until unreferenced. */
void media_camera_close(struct media_camera *camera);

/* ---- The engine: one file played ahead on a thread (the browser's <video> and <audio>) ---- */

/* An engine's states. */
#define MEDIA_OPENING		0U
#define MEDIA_PAUSED		1U
#define MEDIA_PLAYING		2U
#define MEDIA_ENDED		3U
#define MEDIA_FAILED		4U

/* What an engine is asked to play. */
#define MEDIA_SOUND		1U

/* An engine's state as the caller reads it (unchanged from the old interface). */
struct media_status {
	unsigned state;
	int error;
	int problem;
	int width;
	int height;
	int has_video;
	int has_audio;
	double duration;
	double position;
	uint64_t picture;
};

/*
 * Opens an engine on a context (NULL: a context of its own without a
 * GPU), playing a path or the caller's source.  The engine's thread calls
 * notify (MEDIA_EVENT_ENGINE) when its state changed or a picture came;
 * the caller then reads media_engine_status, which also re-arms notify.
 */
int media_engine_create(struct media_context *context, const char *path, const struct media_source *source, unsigned flags, media_notify_fn notify, void *notify_data, struct media_engine **engine);
void media_engine_status(struct media_engine *engine, struct media_status *status);
void media_engine_play(struct media_engine *engine);
void media_engine_pause(struct media_engine *engine);
void media_engine_seek(struct media_engine *engine, double seconds);
int media_engine_picture(struct media_engine *engine, uint32_t *pixels, size_t stride, int width, int height, double *next);
int media_engine_redraw(struct media_engine *engine, uint32_t *pixels, size_t stride, int width, int height);
void media_engine_close(struct media_engine *engine);

/* The log: each line of the library (without its end) goes to the caller's function, or nowhere. */
void media_set_log(void (*log)(void *context, const char *line), void *context);

#endif

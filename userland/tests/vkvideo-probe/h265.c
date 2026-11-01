/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The H.265 decode of vkvideo-probe.
 *
 * libmedia's H.265 parse does the stream's side: h265-stream splits the
 * Annex B stream into pictures in decoding order (with their order
 * counts and reference picture sets), h265-dpb keeps the decoded picture
 * buffer (the reference marking, the slot each picture is decoded into,
 * the output order of C.5.2) and h265-std makes the StdVideoH265
 * structures.  The probe gives every slot its own NV12 picture (slot n is
 * image n), decodes each picture into the slot the DPB takes for it with
 * every reference the DPB keeps, and hashes each frame when the DPB
 * outputs it, so the frames are printed in output order, as ffmpeg's
 * framehash lists them.
 *
 * An end of sequence does not flush the DPB: the picture after it is an
 * IRAP picture with NoRaslOutputFlag 1, whose h265_dpb_begin outputs the
 * prior pictures, or drops them when NoOutputOfPriorPicsFlag is 1, as the
 * reference decoder does.  Only the end of the stream flushes it.
 *
 * The session's parameters hold every parameter set the stream has given
 * so far; parameters are made again when the stream's store changes.
 */

#include "probe.h"
#include "frame.h"

#include "../../desktop/libmedia/h265-stream.h"
#include "../../desktop/libmedia/h265-dpb.h"
#include "../../desktop/libmedia/h265-std.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The slots the probe asks for: the most pictures of the DPB and the one being decoded. */
#define H265_PROBE_SLOTS	(H265_DPB_PICTURES + 1U)

/* The references a session may name at once: every picture of the DPB but the one decoded. */
#define H265_PROBE_REFERENCES	H265_DPB_PICTURES

/*
 * The decode's state besides struct probe: the reader, the picture it
 * returned, the DPB and the plan, the StdVideo forms of the parameter
 * sets and the arrays the parameters are made from, each slot's coded
 * size and display window (of the picture decoded into it), the slots
 * the session holds active, and whether the pictures were moved into the
 * DPB layout.  Kept off the stack (the reader is about 700 KiB).
 */
struct h265_probe {
	struct h265_stream stream;
	struct h265_picture picture;
	struct h265_dpb dpb;
	struct h265_dpb_plan plan;
	struct h265_dpb_output output[H265_DPB_MAX_OUTPUT];
	struct h265_std_vps std_vps[H265SW_VPS_IDS];
	struct h265_std_sps std_sps[H265SW_SPS_IDS];
	struct h265_std_pps std_pps[H265SW_PPS_IDS];
	StdVideoH265VideoParameterSet vps[H265SW_VPS_IDS];
	StdVideoH265SequenceParameterSet sps[H265SW_SPS_IDS];
	StdVideoH265PictureParameterSet pps[H265SW_PPS_IDS];
	VkExtent2D coded[DPB_SLOTS];
	struct frame_window window[DPB_SLOTS];
	uint32_t active;
	int laid_out;
	uint32_t slices[H265SW_MAX_SLICE_SEGMENTS];
};

/* What the first pass over the stream found: the pictures, the largest coded size and the first SPS's format. */
struct h265_survey {
	uint64_t pictures;
	uint32_t width;
	uint32_t height;
	int has_sps;
	struct h265sw_ptl ptl;
	unsigned chroma_format_idc;
	unsigned bit_depth_luma;
	unsigned bit_depth_chroma;
};

static int h265_survey(struct h265_probe *state, const uint8_t *data, size_t size, struct h265_survey *survey);
static int h265_profile(struct probe *probe, const struct h265_survey *survey, const char *name);
static int h265_parameters(struct probe *probe, struct h265_probe *state);
static int h265_decode(struct probe *probe, struct h265_probe *state, const struct h265sw_sps *sps);
static void h265_outputs(struct probe *probe, struct h265_probe *state, const struct h265_dpb_output *output, unsigned count, struct probe_output *printed, int hash);
static VkVideoComponentBitDepthFlagsKHR h265_depth(unsigned depth);

/*
 * Decodes an H.265 stream (data, size bytes, freed here): the first
 * pass for the profile and the largest picture, the device and the
 * decoder's objects, then each picture as the DPB plans it.  Returns the
 * run's exit status as main.c's run does.
 */
int
probe_h265_run(
	struct probe *probe,
	const struct probe_options *options,
	uint8_t *data,
	size_t size)
{
	static struct probe_output output;
	struct h265_probe *state;
	struct h265_survey survey;
	struct probe_timing timing;
	const struct h265sw_sps *sps;
	unsigned count;
	uint32_t frames;
	uint32_t skipped;
	uint64_t started;
	uint64_t elapsed;
	uint64_t missing;
	char line[80];
	int error;

	/* The decode's state, and the first pass. */
	state = calloc(1U, sizeof(*state));
	if (state == NULL) {
		free(data);
		return 1;
	}
	error = h265_survey(state, data, size, &survey);
	if (error == 0)
		error = h265_profile(probe, &survey, options->stream);
	if (error != 0) {
		free(state);
		free(data);
		return error;
	}

	/* The DPB's slots and references, which the decoder may lower. */
	probe->slots = H265_PROBE_SLOTS;
	probe->max_references = H265_PROBE_REFERENCES;

	/* The device and the decoder's objects. */
	error = probe_device(probe);
	if (error == 0)
		error = probe_capabilities(probe);
	if (error == 0 && probe->slots < H265_DPB_PICTURES) {
		fprintf(stderr, "vkvideo-probe: refused: the decoder has %u slots, fewer than the %u pictures of an H.265 DPB\n",
			probe->slots, H265_DPB_PICTURES);
		error = PROBE_REFUSED;
	}
	if (error == 0)
		error = probe_pictures(probe);
	if (error == 0)
		error = probe_bitstream(probe, size + 3U * H265SW_MAX_SLICE_SEGMENTS);
	if (error == 0)
		error = probe_session(probe);
	if (error == 0)
		error = probe_commands(probe);
	if (error != 0) {
		free(state);
		free(data);
		if (error == PROBE_REFUSED)
			return PROBE_REFUSED;
		return 1;
	}

	/* The expected hashes, when given. */
	memset(&output, 0, sizeof(output));
	if (options->expect != NULL) {
		output.expect = fopen(options->expect, "r");
		if (output.expect == NULL) {
			fprintf(stderr, "vkvideo-probe: %s: %s\n", options->expect, strerror(errno));
			free(state);
			free(data);
			return 1;
		}
	}

	/* The reader from the start, the DPB empty, no decode timed yet. */
	h265_stream_init(&state->stream);
	h265_stream_input(&state->stream, data, size);
	h265_dpb_init(&state->dpb, probe->slots);
	memset(&timing, 0, sizeof(timing));
	frames = 0U;
	skipped = 0U;
	missing = 0U;
	error = 0;
	for (;;) {
		/* The next picture, or the end. */
		if (options->frames != 0U && frames >= options->frames)
			break;
		error = h265_stream_next(&state->stream, &state->picture);
		if (error == ENOENT) {
			error = 0;
			break;
		}
		if (error != 0) {
			fprintf(stderr, "vkvideo-probe: picture %u: the reader failed (%d)\n", frames, error);
			error = 3;
			break;
		}
		sps = &state->stream.store.sps[state->picture.sps_id];

		/* Parameters with the sets given so far, when they changed. */
		if (probe->parameters == VK_NULL_HANDLE || state->stream.store.generation != probe->parameters_generation) {
			error = h265_parameters(probe, state);
			if (error != 0)
				break;
		}

		/*
		 * The marking and bumping before the picture (C.5.2.2); an end of
		 * sequence before it needs nothing more (see the file's comment).
		 * A picture the DPB cannot plan is skipped after its outputs.
		 */
		error = h265_dpb_begin(&state->dpb, sps, &state->picture.rps,
				       state->picture.irap != 0 && state->picture.no_rasl_output != 0,
				       state->picture.no_output_of_prior_pics, &state->plan);
		if (error != 0) {
			if (error == EINVAL)
				h265_outputs(probe, state, state->plan.output, state->plan.output_count, &output, !options->no_hash);
			printf("vkvideo-probe: picture %u: skipped, the DPB cannot hold it (%d)\n", frames, error);
			skipped++;
			error = 0;
			continue;
		}
		h265_outputs(probe, state, state->plan.output, state->plan.output_count, &output, !options->no_hash);
		missing += state->plan.missing;

		/* Its slot. */
		error = h265_dpb_take_slot(&state->dpb, &state->plan);
		if (error != 0) {
			h265_dpb_abort(&state->dpb);
			fprintf(stderr, "vkvideo-probe: picture %u: no free slot (%d)\n", frames, error);
			error = 3;
			break;
		}

		/* Decodes it; a decode that did not complete is named. */
		started = probe_now();
		error = h265_decode(probe, state, sps);
		elapsed = probe_now() - started;
		if (error != 0) {
			h265_dpb_abort(&state->dpb);
			break;
		}
		if (probe->status != VK_QUERY_RESULT_STATUS_COMPLETE_KHR) {
			printf("vkvideo-probe: picture %u status %d\n", frames, (int)probe->status);
			output.failed++;
		}

		/* Counts the decode's time, from its commands to its fence. */
		timing.count++;
		timing.total += elapsed;
		if (elapsed > timing.longest)
			timing.longest = elapsed;

		/* The picture in the DPB (C.5.2.3), and what is output after it. */
		error = h265_dpb_end(&state->dpb, &state->plan, state->picture.poc, state->picture.pic_output, state->picture.index,
				     state->output, &count);
		if (error != 0) {
			fprintf(stderr, "vkvideo-probe: picture %u: the DPB did not take it (%d)\n", frames, error);
			error = 3;
			break;
		}
		h265_outputs(probe, state, state->output, count, &output, !options->no_hash);
		frames++;
	}

	/* The end of the stream: the rest in output order. */
	if (h265_dpb_flush(&state->dpb, state->output, &count) == 0)
		h265_outputs(probe, state, state->output, count, &output, !options->no_hash);

	/* A reference frame the decode never output. */
	if (output.expect != NULL) {
		while (fgets(line, sizeof(line), output.expect) != NULL) {
			line[strcspn(line, "\n")] = '\0';
			if (line[0] == '\0')
				continue;
			printf("vkvideo-probe: reference frame %s not output\n", line);
			output.failed++;
		}
		fclose(output.expect);
	}

	/* The summary: the pictures decoded, the frames output, and matched when expected. */
	if (probe->parameters_made > 1U)
		printf("vkvideo-probe: parameters made %u times for the sets the stream sent\n", probe->parameters_made);
	if (state->stream.counts.rasl_skipped != 0U || state->stream.counts.before_irap != 0U || state->stream.counts.broken_pictures != 0U)
		printf("vkvideo-probe: not decoded: %llu RASL, %llu before the first IRAP, %llu broken\n",
		       (unsigned long long)state->stream.counts.rasl_skipped,
		       (unsigned long long)state->stream.counts.before_irap,
		       (unsigned long long)state->stream.counts.broken_pictures);
	if (missing != 0U)
		printf("vkvideo-probe: %llu references missing\n", (unsigned long long)missing);
	printf("vkvideo-probe: %u frames decoded, %u output", frames, output.printed);
	if (options->expect != NULL)
		printf(", %u match the reference", output.matched);
	if (skipped != 0U)
		printf(", %u skipped", skipped);
	if (output.failed != 0U)
		printf(", %u failed", output.failed);
	printf("\n");

	/* The decode times, when asked for. */
	if (options->time)
		probe_timing_print(&timing);
	h265_stream_destroy(&state->stream);
	free(state);
	free(data);

	/* Reports the run's failure: a stopped decode, a frame that did not match, a decode that did not complete. */
	if (error != 0)
		return error;
	if (options->expect != NULL && (output.matched != output.printed || output.printed == 0U))
		return 4;
	if (output.failed != 0U || skipped != 0U)
		return 5;

	/* Succeeded: the stream is decoded. */
	return 0;
}

/*
 * Reads the whole stream once: how many pictures it has, the largest
 * coded picture of their sequences, and the format of the first
 * picture's SPS (the session's profile).  1 for a stream without a
 * picture.
 */
static int
h265_survey(
	struct h265_probe *state,
	const uint8_t *data,
	size_t size,
	struct h265_survey *survey)
{
	const struct h265sw_sps *sps;
	int error;

	/* Every picture the reader returns. */
	memset(survey, 0, sizeof(*survey));
	h265_stream_init(&state->stream);
	h265_stream_input(&state->stream, data, size);
	for (;;) {
		error = h265_stream_next(&state->stream, &state->picture);
		if (error == ENOENT)
			break;
		if (error != 0)
			continue;

		/* Its sequence's size, and the format of the first. */
		sps = &state->stream.store.sps[state->picture.sps_id];
		survey->pictures++;
		if (sps->width > survey->width)
			survey->width = sps->width;
		if (sps->height > survey->height)
			survey->height = sps->height;
		if (!survey->has_sps) {
			survey->has_sps = 1;
			survey->ptl = sps->ptl;
			survey->chroma_format_idc = sps->chroma_format_idc;
			survey->bit_depth_luma = sps->bit_depth_luma;
			survey->bit_depth_chroma = sps->bit_depth_chroma;
		}
	}
	h265_stream_destroy(&state->stream);

	/* A stream without a picture to decode. */
	if (survey->pictures == 0U || !survey->has_sps) {
		fprintf(stderr, "vkvideo-probe: no H.265 picture the probe can read (%s)\n",
			state->stream.reason != NULL ? state->stream.reason : "no reason");
		return 1;
	}

	/* Succeeded: the stream is surveyed. */
	return 0;
}

/*
 * Makes the H.265 decode profile of the first SPS (its profile, 4:2:0,
 * its bit depths) and its list, and the session's extent.  A format the
 * Vulkan profile cannot name is refused (PROBE_REFUSED).
 */
static int
h265_profile(
	struct probe *probe,
	const struct h265_survey *survey,
	const char *name)
{
	StdVideoH265ProfileTierLevel ptl;
	VkVideoComponentBitDepthFlagsKHR luma;
	VkVideoComponentBitDepthFlagsKHR chroma;
	int error;

	/* A profile StdVideo names (Main, Main 10, Main Still Picture or one compatible with them, or the extensions). */
	error = h265_std_ptl(&survey->ptl, &ptl);
	if (error != 0) {
		fprintf(stderr, "vkvideo-probe: refused: the stream's H.265 profile (general_profile_idc %u) has no Vulkan profile\n",
			survey->ptl.profile_idc);
		return PROBE_REFUSED;
	}

	/* 4:2:0 of 8 or 10 bits. */
	luma = h265_depth(survey->bit_depth_luma);
	chroma = h265_depth(survey->bit_depth_chroma);
	if (survey->chroma_format_idc != 1U || luma == VK_VIDEO_COMPONENT_BIT_DEPTH_INVALID_KHR ||
	    chroma == VK_VIDEO_COMPONENT_BIT_DEPTH_INVALID_KHR) {
		fprintf(stderr, "vkvideo-probe: refused: the stream is not 4:2:0 of 8 or 10 bits (chroma_format_idc %u, %u and %u bits)\n",
			survey->chroma_format_idc, survey->bit_depth_luma, survey->bit_depth_chroma);
		return PROBE_REFUSED;
	}

	/* The profile, chained with its H.265 record, and its list. */
	probe->codec = PROBE_H265;
	memset(&probe->h265_profile, 0, sizeof(probe->h265_profile));
	probe->h265_profile.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR;
	probe->h265_profile.stdProfileIdc = ptl.general_profile_idc;
	memset(&probe->profile, 0, sizeof(probe->profile));
	probe->profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
	probe->profile.pNext = &probe->h265_profile;
	probe->profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR;
	probe->profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
	probe->profile.lumaBitDepth = luma;
	probe->profile.chromaBitDepth = chroma;
	memset(&probe->profile_list, 0, sizeof(probe->profile_list));
	probe->profile_list.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR;
	probe->profile_list.profileCount = 1U;
	probe->profile_list.pProfiles = &probe->profile;

	/* The largest coded picture. */
	probe->extent.width = survey->width;
	probe->extent.height = survey->height;
	printf("vkvideo-probe: stream %s H.265 profile %u level %u, %ux%u, %llu pictures\n",
	       name,
	       (unsigned)ptl.general_profile_idc,
	       survey->ptl.level_idc,
	       probe->extent.width,
	       probe->extent.height,
	       (unsigned long long)survey->pictures);

	/* Succeeded: the profile is made. */
	return 0;
}

/* The Vulkan bit depth of 8 or 10 bits; INVALID for another. */
static VkVideoComponentBitDepthFlagsKHR
h265_depth(
	unsigned depth)
{
	/* The two depths a profile names here. */
	if (depth == 8U)
		return VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	if (depth == 10U)
		return VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR;

	/* Another. */
	return VK_VIDEO_COMPONENT_BIT_DEPTH_INVALID_KHR;
}

/*
 * Makes the session's parameters from every parameter set the store
 * holds, in place of the parameters before (no decode is in flight).  A
 * set h265-std cannot make (a profile without a Vulkan profile) is left
 * out; a picture that names it is then an error of the decoder's.
 */
static int
h265_parameters(
	struct probe *probe,
	struct h265_probe *state)
{
	const struct h265sw_ps_store *store;
	VkVideoDecodeH265SessionParametersAddInfoKHR add;
	VkVideoDecodeH265SessionParametersCreateInfoKHR h265;
	VkVideoSessionParametersCreateInfoKHR info;
	VkResult result;
	uint32_t vps_count;
	uint32_t sps_count;
	uint32_t pps_count;
	uint32_t index;
	int error;

	/* The sets in StdVideo form, each made where it lives, then copied (shallow) into the arrays. */
	store = &state->stream.store;
	vps_count = 0U;
	for (index = 0U; index < H265SW_VPS_IDS; index++) {
		if (!store->vps_present[index])
			continue;
		error = h265_std_vps(&store->vps[index], &state->std_vps[index]);
		if (error != 0)
			continue;
		state->vps[vps_count] = state->std_vps[index].vps;
		vps_count++;
	}
	sps_count = 0U;
	for (index = 0U; index < H265SW_SPS_IDS; index++) {
		if (!store->sps_present[index])
			continue;
		error = h265_std_sps(&store->sps[index], &state->std_sps[index]);
		if (error != 0)
			continue;
		state->sps[sps_count] = state->std_sps[index].sps;
		sps_count++;
	}
	pps_count = 0U;
	for (index = 0U; index < H265SW_PPS_IDS; index++) {
		if (!store->pps_present[index] || !store->sps_present[store->pps[index].sps_id])
			continue;
		error = h265_std_pps(&store->pps[index], &store->sps[store->pps[index].sps_id], &state->std_pps[index]);
		if (error != 0)
			continue;
		state->pps[pps_count] = state->std_pps[index].pps;
		pps_count++;
	}

	/* The parameters object holding them. */
	memset(&add, 0, sizeof(add));
	add.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_ADD_INFO_KHR;
	add.stdVPSCount = vps_count;
	add.pStdVPSs = state->vps;
	add.stdSPSCount = sps_count;
	add.pStdSPSs = state->sps;
	add.stdPPSCount = pps_count;
	add.pStdPPSs = state->pps;
	memset(&h265, 0, sizeof(h265));
	h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_SESSION_PARAMETERS_CREATE_INFO_KHR;
	h265.maxStdVPSCount = vps_count;
	h265.maxStdSPSCount = sps_count;
	h265.maxStdPPSCount = pps_count;
	h265.pParametersAddInfo = &add;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR;
	info.pNext = &h265;
	info.videoSession = probe->session;
	if (probe->parameters != VK_NULL_HANDLE) {
		vkDestroyVideoSessionParametersKHR(probe->device, probe->parameters, NULL);
		probe->parameters = VK_NULL_HANDLE;
	}
	result = vkCreateVideoSessionParametersKHR(probe->device, &info, NULL, &probe->parameters);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkCreateVideoSessionParametersKHR");
	probe->parameters_generation = store->generation;
	probe->parameters_made++;

	/* The first parameters are named; the later ones are counted at the end. */
	if (probe->parameters_made == 1U)
		printf("vkvideo-probe: parameters %u VPS, %u SPS, %u PPS\n", vps_count, sps_count, pps_count);

	/* Succeeded: the parameters. */
	return 0;
}

/*
 * Decodes the current picture as the DPB planned it, and waits for it:
 * its slice segments copied into the buffer each behind a three-byte
 * start code; a coding scope binding every reference the DPB keeps with
 * its slot, deactivating the slots that held a picture and hold no
 * reference any more, and binding the written slot's picture without a
 * slot; the session's reset when the DPB starts; the decode with every
 * kept reference and the written slot set up; the scope's end.  The
 * first decode moves every slot's image into the DPB layout.
 */
static int
h265_decode(
	struct probe *probe,
	struct h265_probe *state,
	const struct h265sw_sps *sps)
{
	VkVideoPictureResourceInfoKHR resources[DPB_SLOTS];
	VkVideoReferenceSlotInfoKHR bound[DPB_SLOTS * 2U];
	VkVideoReferenceSlotInfoKHR references[H265_DPB_PICTURES];
	VkVideoDecodeH265DpbSlotInfoKHR reference_info[H265_DPB_PICTURES];
	StdVideoDecodeH265ReferenceInfo reference_std[H265_DPB_PICTURES];
	VkVideoDecodeH265DpbSlotInfoKHR setup_info;
	StdVideoDecodeH265ReferenceInfo setup_std;
	StdVideoDecodeH265PictureInfo picture_std;
	VkVideoDecodeH265PictureInfoKHR h265;
	VkVideoReferenceSlotInfoKHR setup;
	VkVideoBeginCodingInfoKHR begin;
	VkVideoCodingControlInfoKHR control;
	VkVideoDecodeInfoKHR decode;
	VkVideoEndCodingInfoKHR end;
	VkCommandBufferBeginInfo record;
	VkImageMemoryBarrier barriers[DPB_SLOTS];
	VkSubmitInfo submit;
	const struct h265_picture *picture;
	const struct h265_dpb_plan *plan;
	VkResult result;
	size_t at;
	uint32_t kept;
	uint32_t slice;
	uint32_t slot;
	uint32_t index;
	uint32_t count;
	int error;

	/* The picture information from the plan. */
	picture = &state->picture;
	plan = &state->plan;
	error = h265_std_picture(picture, plan, &picture_std);
	if (error != 0) {
		fprintf(stderr, "vkvideo-probe: a picture's information does not fit Vulkan's (%d)\n", error);
		return 3;
	}
	h265_std_references(plan, reference_std);
	h265_std_setup(picture->poc, &setup_std);

	/* The slice segments, each behind a start code, into the buffer. */
	at = 0U;
	for (slice = 0U; slice < picture->slice_count; slice++) {
		if (at + 3U + picture->slice_size[slice] > probe->buffer_size) {
			fprintf(stderr, "vkvideo-probe: a picture larger than the buffer\n");
			return 1;
		}
		state->slices[slice] = (uint32_t)at;
		probe->buffer_map[at] = 0U;
		probe->buffer_map[at + 1U] = 0U;
		probe->buffer_map[at + 2U] = 1U;
		memcpy(probe->buffer_map + at + 3U, state->stream.data + picture->slice_offset[slice], picture->slice_size[slice]);
		at += 3U + picture->slice_size[slice];
	}

	/* The written slot's picture: the sequence's coded size and display window. */
	slot = (uint32_t)plan->setup_slot;
	state->coded[slot].width = sps->width;
	state->coded[slot].height = sps->height;
	state->window[slot].x = sps->crop_x;
	state->window[slot].y = sps->crop_y;
	state->window[slot].width = sps->crop_width;
	state->window[slot].height = sps->crop_height;

	/* Every slot's picture resource: its image's coded picture. */
	memset(resources, 0, sizeof(resources));
	for (index = 0U; index < probe->slots; index++) {
		resources[index].sType = VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR;
		resources[index].codedExtent = state->coded[index];
		if (resources[index].codedExtent.width == 0U)
			resources[index].codedExtent = state->coded[slot];
		resources[index].imageViewBinding = probe->view[index];
	}

	/* The command buffer from its start; the status query starts unavailable, outside the scope. */
	memset(&record, 0, sizeof(record));
	record.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	record.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	result = vkBeginCommandBuffer(probe->command, &record);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkBeginCommandBuffer");
	if (probe->status_pool != VK_NULL_HANDLE)
		vkCmdResetQueryPool(probe->command, probe->status_pool, 0U, 1U);

	/* The first decode moves every slot's image into the DPB layout. */
	if (!state->laid_out) {
		memset(barriers, 0, sizeof(barriers));
		for (index = 0U; index < probe->slots; index++) {
			barriers[index].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
			barriers[index].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barriers[index].newLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
			barriers[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			barriers[index].image = probe->image[index];
			barriers[index].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			barriers[index].subresourceRange.levelCount = 1U;
			barriers[index].subresourceRange.layerCount = 1U;
		}
		vkCmdPipelineBarrier(probe->command,
				     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				     VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
				     0U,
				     0U,
				     NULL,
				     0U,
				     NULL,
				     probe->slots,
				     barriers);
		state->laid_out = 1;
	}

	/* A reset leaves no slot active. */
	if (plan->reset)
		state->active = 0U;

	/* The scope's slots: the references kept, the slots deactivated, the written picture without a slot. */
	memset(bound, 0, sizeof(bound));
	count = 0U;
	kept = 0U;
	for (index = 0U; index < plan->reference_count; index++) {
		bound[count].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
		bound[count].slotIndex = plan->reference_slot[index];
		bound[count].pPictureResource = &resources[plan->reference_slot[index]];
		kept |= 1U << (uint32_t)plan->reference_slot[index];
		count++;
	}
	for (index = 0U; index < probe->slots; index++) {
		if ((state->active & ~kept & (1U << index)) == 0U)
			continue;
		bound[count].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
		bound[count].slotIndex = (int32_t)index;
		bound[count].pPictureResource = NULL;
		count++;
	}
	bound[count].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	bound[count].slotIndex = -1;
	bound[count].pPictureResource = &resources[slot];
	count++;

	/* The scope, and the reset when the DPB starts. */
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR;
	begin.videoSession = probe->session;
	begin.videoSessionParameters = probe->parameters;
	begin.referenceSlotCount = count;
	begin.pReferenceSlots = bound;
	vkCmdBeginVideoCodingKHR(probe->command, &begin);
	if (plan->reset) {
		memset(&control, 0, sizeof(control));
		control.sType = VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR;
		control.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
		vkCmdControlVideoCodingKHR(probe->command, &control);
	}

	/* The references with their information. */
	memset(references, 0, sizeof(references));
	memset(reference_info, 0, sizeof(reference_info));
	for (index = 0U; index < plan->reference_count; index++) {
		reference_info[index].sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR;
		reference_info[index].pStdReferenceInfo = &reference_std[index];
		references[index].sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
		references[index].pNext = &reference_info[index];
		references[index].slotIndex = plan->reference_slot[index];
		references[index].pPictureResource = &resources[plan->reference_slot[index]];
	}

	/* The written slot, with the picture's own information. */
	memset(&setup_info, 0, sizeof(setup_info));
	setup_info.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR;
	setup_info.pStdReferenceInfo = &setup_std;
	memset(&setup, 0, sizeof(setup));
	setup.sType = VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR;
	setup.pNext = &setup_info;
	setup.slotIndex = (int32_t)slot;
	setup.pPictureResource = &resources[slot];

	/* The decode: the segments into the written slot's picture. */
	memset(&h265, 0, sizeof(h265));
	h265.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR;
	h265.pStdPictureInfo = &picture_std;
	h265.sliceSegmentCount = picture->slice_count;
	h265.pSliceSegmentOffsets = state->slices;
	memset(&decode, 0, sizeof(decode));
	decode.sType = VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR;
	decode.pNext = &h265;
	decode.srcBuffer = probe->buffer;
	decode.srcBufferOffset = 0U;
	decode.srcBufferRange = at;
	decode.dstPictureResource = resources[slot];
	decode.pSetupReferenceSlot = &setup;
	decode.referenceSlotCount = plan->reference_count;
	decode.pReferenceSlots = references;
	if (probe->status_pool != VK_NULL_HANDLE)
		vkCmdBeginQuery(probe->command, probe->status_pool, 0U, 0U);
	vkCmdDecodeVideoKHR(probe->command, &decode);
	if (probe->status_pool != VK_NULL_HANDLE)
		vkCmdEndQuery(probe->command, probe->status_pool, 0U);

	/* The scope's end; the written slot and the kept references are active from now on. */
	memset(&end, 0, sizeof(end));
	end.sType = VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR;
	vkCmdEndVideoCodingKHR(probe->command, &end);
	state->active = kept | (1U << slot);
	result = vkEndCommandBuffer(probe->command);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkEndCommandBuffer");

	/* Submits it and waits for it. */
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1U;
	submit.pCommandBuffers = &probe->command;
	result = vkQueueSubmit(probe->queue, 1U, &submit, probe->fence);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkQueueSubmit");
	result = vkWaitForFences(probe->device, 1U, &probe->fence, VK_TRUE, PROBE_WAIT_NS);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkWaitForFences");

	/* The fence and the buffer for the next picture. */
	result = vkResetFences(probe->device, 1U, &probe->fence);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkResetFences");
	result = vkResetCommandBuffer(probe->command, 0U);
	if (result != VK_SUCCESS)
		return probe_failed(result, "vkResetCommandBuffer");

	/* Whether the decode completed: its query's status, when there is one. */
	probe->status = VK_QUERY_RESULT_STATUS_COMPLETE_KHR;
	if (probe->status_pool != VK_NULL_HANDLE) {
		result = vkGetQueryPoolResults(probe->device, probe->status_pool, 0U, 1U, sizeof(probe->status), &probe->status,
					       sizeof(probe->status), VK_QUERY_RESULT_WITH_STATUS_BIT_KHR | VK_QUERY_RESULT_WAIT_BIT);
		if (result != VK_SUCCESS)
			return probe_failed(result, "vkGetQueryPoolResults");
	}

	/* Succeeded: the picture is in its slot's image, or its status says why not. */
	return 0;
}

/*
 * Prints the pictures the DPB output, in its order: each slot's image
 * hashed over the display window of the picture decoded into it (unless
 * hashing is off), compared with the next expected line, then released.
 */
static void
h265_outputs(
	struct probe *probe,
	struct h265_probe *state,
	const struct h265_dpb_output *output,
	unsigned count,
	struct probe_output *printed,
	int hash)
{
	VkImageSubresource subresource;
	VkSubresourceLayout luma;
	VkSubresourceLayout chroma;
	struct frame_planes planes;
	char text[65];
	unsigned index;
	int slot;

	/* Each output in order. */
	for (index = 0U; index < count; index++) {
		slot = output[index].slot;
		if (hash && slot >= 0 && (uint32_t)slot < probe->slots) {
			/* The image's two planes, hashed over the window; printed at once (one frame waits at a time). */
			memset(&subresource, 0, sizeof(subresource));
			subresource.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT;
			vkGetImageSubresourceLayout(probe->device, probe->image[slot], &subresource, &luma);
			subresource.aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT;
			vkGetImageSubresourceLayout(probe->device, probe->image[slot], &subresource, &chroma);
			planes.luma = probe->image_map[slot] + luma.offset;
			planes.luma_pitch = (size_t)luma.rowPitch;
			planes.chroma = probe->image_map[slot] + chroma.offset;
			planes.chroma_pitch = (size_t)chroma.rowPitch;
			planes.linear = probe->linear;
			frame_hash(&planes, &state->window[slot], text);
			probe_output_add(printed, output[index].poc, text);
			probe_output_flush(printed, 0U);
		}

		/* The slot is free for the DPB again. */
		h265_dpb_release(&state->dpb, slot);
	}
}

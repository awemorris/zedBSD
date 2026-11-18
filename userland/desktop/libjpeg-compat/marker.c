/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The markers of a JPEG file (ITU T.81 B.1): SOI, the frame (SOF0, SOF1
 * and SOF2, Huffman with 8-bit samples), the tables (DQT, DHT), the
 * restart interval (DRI), the scan header (SOS), and the APP0 (JFIF) and
 * APP14 (Adobe) markers whose facts decide the colour space.  APPn and
 * COM segments are kept when the program asked (jpeg_save_markers) and
 * skipped otherwise; the other frame types are refused.
 */

#include "internal.h"

#include <string.h>

/* The markers (the byte after 0xFF). */
#define JPEG_SOF0		0xC0
#define JPEG_SOF1		0xC1
#define JPEG_SOF2		0xC2
#define JPEG_DHT		0xC4
#define JPEG_JPG		0xC8
#define JPEG_DAC		0xCC
#define JPEG_SOF15		0xCF
#define JPEG_SOI		0xD8
#define JPEG_SOS		0xDA
#define JPEG_DQT		0xDB
#define JPEG_DNL		0xDC
#define JPEG_DRI		0xDD
#define JPEG_APP14		0xEE
#define JPEG_APP15		0xEF

/* The bytes of an APP0 (JFIF) and an APP14 (Adobe) segment that say what it is. */
#define JPEG_JFIF_HEAD		14
#define JPEG_ADOBE_HEAD		12
#define JPEG_TEM		0x01

/*
 * The natural (row by row) position of each coefficient in zigzag order,
 * followed by 16 extra entries so that a corrupt run past 63 stays in the
 * block.  The table is constant for the life of the program.
 */
const int jpeg_compat_natural_order[DCTSIZE2 + 16] = {
	0, 1, 8, 16, 9, 2, 3, 10,
	17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34,
	27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46,
	53, 60, 61, 54, 47, 55, 62, 63,
	63, 63, 63, 63, 63, 63, 63, 63,
	63, 63, 63, 63, 63, 63, 63, 63
};

static unsigned jpeg_compat_read_word(j_decompress_ptr cinfo);
static unsigned jpeg_compat_segment_length(j_decompress_ptr cinfo);
static void jpeg_compat_skip_segment(j_decompress_ptr cinfo, unsigned length);
static void jpeg_compat_read_frame(j_decompress_ptr cinfo, int marker);
static void jpeg_compat_read_scan(j_decompress_ptr cinfo);
static void jpeg_compat_read_dqt(j_decompress_ptr cinfo);
static void jpeg_compat_read_dht(j_decompress_ptr cinfo);
static void jpeg_compat_read_dri(j_decompress_ptr cinfo);
static void jpeg_compat_read_other(j_decompress_ptr cinfo, int marker);
static int jpeg_compat_saved_kind(int marker);
static void jpeg_compat_parse_app0(j_decompress_ptr cinfo, const unsigned char *head);
static void jpeg_compat_parse_app14(j_decompress_ptr cinfo, const unsigned char *head);
static void jpeg_compat_refuse_frame(j_decompress_ptr cinfo, int marker);

/*
 * Reads the next byte of the source (a fake EOI once a program's own
 * source has nothing more).
 */
int
jpeg_compat_read_byte(
	j_decompress_ptr cinfo)
{
	struct jpeg_source_mgr *source;
	struct jpeg_decomp_master *master;
	boolean filled;
	int byte;

	/* More bytes when the buffer is empty; none (a suspending source) reads as EOI. */
	source = cinfo->src;
	if (source->bytes_in_buffer == 0) {
		filled = source->fill_input_buffer(cinfo);
		if (!filled || source->bytes_in_buffer == 0) {
			master = cinfo->master;
			if (!master->eof_warned)
				jpeg_compat_warn((j_common_ptr)cinfo, JWRN_JPEG_EOF);
			master->eof_warned = 1;
			master->fake_eoi[0] = 0xFF;
			master->fake_eoi[1] = JPEG_EOI;
			source->next_input_byte = master->fake_eoi;
			source->bytes_in_buffer = sizeof(master->fake_eoi);
		}
	}

	/* Succeeded: the byte. */
	byte = *source->next_input_byte;
	source->next_input_byte++;
	source->bytes_in_buffer--;
	return byte;
}

/*
 * Asks for the APPn or COM markers of a kind to be kept in marker_list
 * (up to length_limit bytes of each; 0 keeps none), before
 * jpeg_read_header.
 */
void
jpeg_save_markers(
	j_decompress_ptr cinfo,
	int marker_code,
	unsigned int length_limit)
{
	int kind;

	/* Only APP0 to APP15 and COM can be kept. */
	kind = jpeg_compat_saved_kind(marker_code);
	if (kind < 0)
		jpeg_compat_fail_number((j_common_ptr)cinfo, JERR_UNKNOWN_MARKER, marker_code, 0);

	/* The limit, remembered for the markers read from now on. */
	cinfo->master->save_limit[kind] = length_limit;
}

/*
 * Reads markers and their segments up to the next SOS (whose header is
 * read) or EOI.  in_image is 0 for the markers before the first scan
 * (starting with SOI) and 1 between scans (starting with the marker the
 * entropy decoder met, if any).  Returns JPEG_SOS or JPEG_EOI.
 */
int
jpeg_compat_read_markers(
	j_decompress_ptr cinfo,
	int in_image)
{
	struct jpeg_decomp_master *master;
	unsigned length;
	int first;
	int second;
	int marker;

	/* The file starts with SOI. */
	master = cinfo->master;
	if (!in_image) {
		first = jpeg_compat_read_byte(cinfo);
		second = jpeg_compat_read_byte(cinfo);
		if (first != 0xFF || second != JPEG_SOI)
			jpeg_compat_fail_number((j_common_ptr)cinfo, JERR_NO_SOI, first, second);
	}

	/* Each marker until a scan or the end of the image. */
	for (;;) {
		marker = master->marker;
		master->marker = 0;
		if (marker == 0)
			marker = jpeg_compat_next_marker(cinfo);

		/* Decide by the marker's kind. */
		switch (marker) {
		case JPEG_SOF0:
		case JPEG_SOF1:
		case JPEG_SOF2:
			jpeg_compat_read_frame(cinfo, marker);
			break;
		case JPEG_DHT:
			jpeg_compat_read_dht(cinfo);
			break;
		case JPEG_DQT:
			jpeg_compat_read_dqt(cinfo);
			break;
		case JPEG_DRI:
			jpeg_compat_read_dri(cinfo);
			break;
		case JPEG_SOS:
			if (!master->frame_seen)
				jpeg_compat_fail((j_common_ptr)cinfo, JERR_SOS_NO_SOF);
			jpeg_compat_read_scan(cinfo);
			return JPEG_SOS;
		case JPEG_EOI:
			return JPEG_EOI;
		case JPEG_SOI:
		case JPEG_TEM:
			break;
		default:
			if (marker >= JPEG_SOF0 && marker <= JPEG_SOF15 && marker != JPEG_DHT && marker != JPEG_JPG) {
				jpeg_compat_refuse_frame(cinfo, marker);
			} else if (marker >= JPEG_RST0 && marker <= JPEG_RST0 + 7) {
				/* A restart marker outside a scan carries nothing. */
				continue;
			} else if (marker >= JPEG_APP0 && marker <= JPEG_APP15) {
				/* APPn: kept when asked, and JFIF and Adobe read. */
				jpeg_compat_read_other(cinfo, marker);
			} else if (marker == JPEG_COM) {
				/* A comment: kept when asked. */
				jpeg_compat_read_other(cinfo, marker);
			} else {
				/* APPn, COM, DNL and the rest: their segments are skipped. */
				length = jpeg_compat_segment_length(cinfo);
				jpeg_compat_skip_segment(cinfo, length);
			}

			/* The marker is handled. */
			break;
		}
	}
}

/* Finds the next marker, skipping (and reporting) any bytes before it and the 0xFF fill. */
int
jpeg_compat_next_marker(
	j_decompress_ptr cinfo)
{
	int skipped;
	int byte;

	/* Up to a 0xFF, then past the fill bytes to the code. */
	skipped = 0;
	for (;;) {
		byte = jpeg_compat_read_byte(cinfo);
		while (byte != 0xFF) {
			skipped++;
			byte = jpeg_compat_read_byte(cinfo);
		}

		/* Past the fill bytes to the code. */
		do {
			byte = jpeg_compat_read_byte(cinfo);
		} while (byte == 0xFF);

		/* 0xFF 0x00 is data, not a marker. */
		if (byte != 0)
			break;
		skipped += 2;
	}

	/* Garbage before the marker is corrupt data, but decoding goes on. */
	if (skipped != 0) {
		cinfo->err->msg_parm.i[0] = skipped;
		cinfo->err->msg_parm.i[1] = byte;
		jpeg_compat_warn((j_common_ptr)cinfo, JWRN_EXTRANEOUS_DATA);
	}

	/* Succeeded: the marker's code. */
	return byte;
}

/* Reads a big-endian 16-bit word. */
static unsigned
jpeg_compat_read_word(
	j_decompress_ptr cinfo)
{
	unsigned high;
	unsigned low;

	/* The high byte, then the low one. */
	high = (unsigned)jpeg_compat_read_byte(cinfo);
	low = (unsigned)jpeg_compat_read_byte(cinfo);
	return (high << 8) | low;
}

/* Reads a segment's length and returns the bytes that follow it (the length counts its own two). */
static unsigned
jpeg_compat_segment_length(
	j_decompress_ptr cinfo)
{
	unsigned length;

	/* A length under 2 is corrupt. */
	length = jpeg_compat_read_word(cinfo);
	if (length < 2U)
		jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_LENGTH);

	/* Succeeded: the bytes after the length. */
	return length - 2U;
}

/* Skips the rest of a segment. */
static void
jpeg_compat_skip_segment(
	j_decompress_ptr cinfo,
	unsigned length)
{
	/* One byte at a time (the segments skipped are short). */
	while (length > 0) {
		jpeg_compat_read_byte(cinfo);
		length--;
	}
}

/* Reads SOF0, SOF1 or SOF2: the precision, the size and each component's sampling and table. */
static void
jpeg_compat_read_frame(
	j_decompress_ptr cinfo,
	int marker)
{
	struct jpeg_decomp_master *master;
	jpeg_component_info *component;
	unsigned length;
	int sampling;
	int index;

	/* The frame's header. */
	master = cinfo->master;
	length = jpeg_compat_segment_length(cinfo);
	cinfo->data_precision = jpeg_compat_read_byte(cinfo);
	cinfo->image_height = jpeg_compat_read_word(cinfo);
	cinfo->image_width = jpeg_compat_read_word(cinfo);
	cinfo->num_components = jpeg_compat_read_byte(cinfo);

	/* 8-bit samples, a size given in the frame, and 1, 3 or 4 components. */
	if (cinfo->data_precision != 8)
		jpeg_compat_fail_number((j_common_ptr)cinfo, JERR_BAD_PRECISION, cinfo->data_precision, 0);
	if (cinfo->image_height == 0 || cinfo->image_width == 0)
		jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_SIZE);
	if (cinfo->num_components != 1 && cinfo->num_components != 3 && cinfo->num_components != 4)
		jpeg_compat_fail_number((j_common_ptr)cinfo, JERR_BAD_COMPONENTS, cinfo->num_components, 0);
	if (length != 6U + 3U * (unsigned)cinfo->num_components)
		jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_LENGTH);

	/* Each component's identity, sampling (1 to 4) and quantization table. */
	for (index = 0; index < cinfo->num_components; index++) {
		component = &master->components[index];
		memset(component, 0, sizeof(*component));
		component->component_index = index;
		component->component_id = jpeg_compat_read_byte(cinfo);
		sampling = jpeg_compat_read_byte(cinfo);
		component->h_samp_factor = sampling >> 4;
		component->v_samp_factor = sampling & 15;
		component->quant_tbl_no = jpeg_compat_read_byte(cinfo);
		if (component->h_samp_factor < 1 || component->h_samp_factor > 4 ||
		    component->v_samp_factor < 1 || component->v_samp_factor > 4)
			jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_SAMPLING);
		if (component->quant_tbl_no >= NUM_QUANT_TBLS)
			jpeg_compat_fail_number((j_common_ptr)cinfo, JERR_NO_TABLE, component->quant_tbl_no, 0);
		component->component_needed = TRUE;
		component->DCT_scaled_size = DCTSIZE;
	}

	/* Succeeded: the frame is known, progressive for SOF2. */
	cinfo->comp_info = master->components;
	cinfo->progressive_mode = FALSE;
	if (marker == JPEG_SOF2)
		cinfo->progressive_mode = TRUE;
	cinfo->arith_code = FALSE;
	master->frame_seen = 1;
}

/* Reads SOS: the scan's components and their Huffman tables, and the spectral selection. */
static void
jpeg_compat_read_scan(
	j_decompress_ptr cinfo)
{
	struct jpeg_decomp_master *master;
	jpeg_component_info *component;
	unsigned length;
	int count;
	int index;
	int found;
	int identity;
	int tables;
	int approximation;

	/* The number of components (1 to 4, among the frame's). */
	master = cinfo->master;
	length = jpeg_compat_segment_length(cinfo);
	count = jpeg_compat_read_byte(cinfo);
	if (count < 1 || count > MAX_COMPS_IN_SCAN || count > cinfo->num_components || length != 4U + 2U * (unsigned)count)
		jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_SCAN);

	/* Each component, found by its identity, with its DC and AC tables. */
	for (index = 0; index < count; index++) {
		identity = jpeg_compat_read_byte(cinfo);
		tables = jpeg_compat_read_byte(cinfo);
		for (found = 0; found < cinfo->num_components; found++) {
			if (master->components[found].component_id == identity)
				break;
		}

		/* A component the frame does not have. */
		if (found == cinfo->num_components)
			jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_SCAN);
		component = &master->components[found];
		component->dc_tbl_no = tables >> 4;
		component->ac_tbl_no = tables & 15;
		if (component->dc_tbl_no >= NUM_HUFF_TBLS || component->ac_tbl_no >= NUM_HUFF_TBLS)
			jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_SCAN);
		master->scan_components[index] = found;
	}

	/* The spectral selection and the successive approximation (high bits, low bits). */
	master->scan_count = count;
	master->spectral_start = jpeg_compat_read_byte(cinfo);
	master->spectral_end = jpeg_compat_read_byte(cinfo);
	approximation = jpeg_compat_read_byte(cinfo);
	master->approximation = approximation;

	/* A sequential scan is the whole block, once. */
	if (!cinfo->progressive_mode) {
		if (master->spectral_start != 0 || master->spectral_end != DCTSIZE2 - 1 || approximation != 0)
			jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_SCAN);
	}

	/* A progressive scan is the DC alone or a band of AC of one component, with at most 13 bits each way (T.81 G.1.1.1). */
	if (cinfo->progressive_mode) {
		if (master->spectral_start > master->spectral_end || master->spectral_end > DCTSIZE2 - 1)
			jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_SCAN);
		if (master->spectral_start == 0 && master->spectral_end != 0)
			jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_SCAN);
		if (master->spectral_start != 0 && count != 1)
			jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_SCAN);
		if ((approximation >> 4) > 13 || (approximation & 15) > 13)
			jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_SCAN);
	}

	/* Succeeded: one more scan. */
	cinfo->input_scan_number++;
}

/* Reads DQT: tables of 8-bit or 16-bit values, stored in natural order. */
static void
jpeg_compat_read_dqt(
	j_decompress_ptr cinfo)
{
	JQUANT_TBL *table;
	unsigned length;
	unsigned needed;
	int precision;
	int number;
	int value;
	int index;

	/* Each table of the segment. */
	length = jpeg_compat_segment_length(cinfo);
	while (length > 0) {
		value = jpeg_compat_read_byte(cinfo);
		precision = value >> 4;
		number = value & 15;
		needed = 1U + DCTSIZE2;
		if (precision != 0)
			needed = 1U + 2U * DCTSIZE2;
		if (number >= NUM_QUANT_TBLS || precision > 1 || length < needed)
			jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_TABLE);

		/* The values, from zigzag into natural order. */
		table = &cinfo->master->quant[number];
		for (index = 0; index < DCTSIZE2; index++) {
			if (precision != 0) {
				value = (int)jpeg_compat_read_word(cinfo);
			} else {
				value = jpeg_compat_read_byte(cinfo);
			}

			/* Stored at its natural position. */
			table->quantval[jpeg_compat_natural_order[index]] = (UINT16)value;
		}

		/* The table is known. */
		table->sent_table = TRUE;
		cinfo->quant_tbl_ptrs[number] = table;
		length -= needed;
	}
}

/* Reads DHT: Huffman tables (their code counts and values), made ready for decoding. */
static void
jpeg_compat_read_dht(
	j_decompress_ptr cinfo)
{
	struct jpeg_decomp_master *master;
	JHUFF_TBL *table;
	struct jpeg_huffman *huffman;
	unsigned length;
	unsigned total;
	int value;
	int number;
	int index;

	/* Each table of the segment. */
	master = cinfo->master;
	length = jpeg_compat_segment_length(cinfo);
	while (length > 0) {
		/* Its class (DC or AC) and number. */
		value = jpeg_compat_read_byte(cinfo);
		number = value & 15;
		if ((value >> 4) > 1 || number >= NUM_HUFF_TBLS || length < 17U)
			jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_TABLE);
		table = &master->dc_tables[number];
		huffman = &master->dc[number];
		if ((value >> 4) == 1) {
			table = &master->ac_tables[number];
			huffman = &master->ac[number];
		}

		/* The count of codes of each length, then the values. */
		table->bits[0] = 0;
		total = 0;
		for (index = 1; index <= 16; index++) {
			table->bits[index] = (UINT8)jpeg_compat_read_byte(cinfo);
			total += table->bits[index];
		}

		/* At most 256 values, all in the segment. */
		if (total > 256U || length < 17U + total)
			jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_TABLE);
		memset(table->huffval, 0, sizeof(table->huffval));
		for (index = 0; index < (int)total; index++)
			table->huffval[index] = (UINT8)jpeg_compat_read_byte(cinfo);

		/* The table, ready for decoding. */
		table->sent_table = TRUE;
		jpeg_compat_build_huffman(cinfo, table, huffman);
		if ((value >> 4) == 1) {
			cinfo->ac_huff_tbl_ptrs[number] = table;
		} else {
			cinfo->dc_huff_tbl_ptrs[number] = table;
		}

		/* The next table of the segment. */
		length -= 17U + total;
	}
}

/* Reads DRI: the number of MCUs between restart markers (0 for none). */
static void
jpeg_compat_read_dri(
	j_decompress_ptr cinfo)
{
	unsigned length;

	/* A two-byte interval. */
	length = jpeg_compat_segment_length(cinfo);
	if (length != 2U)
		jpeg_compat_fail((j_common_ptr)cinfo, JERR_BAD_LENGTH);
	cinfo->restart_interval = jpeg_compat_read_word(cinfo);
}

/*
 * Reads an APPn or COM segment: the bytes the program asked to keep go
 * into a saved marker at the end of marker_list, and the start of an
 * APP0 or APP14 segment is read for its JFIF or Adobe facts.
 */
static void
jpeg_compat_read_other(
	j_decompress_ptr cinfo,
	int marker)
{
	struct jpeg_decomp_master *master;
	jpeg_saved_marker_ptr saved;
	unsigned char head[JPEG_JFIF_HEAD];
	unsigned length;
	unsigned keep;
	unsigned index;
	int kind;
	int byte;

	/* The segment's length, and how much of it the program keeps. */
	master = cinfo->master;
	length = jpeg_compat_segment_length(cinfo);
	kind = jpeg_compat_saved_kind(marker);
	keep = master->save_limit[kind];
	if (keep > length)
		keep = length;

	/* A kept marker: its record and bytes in the image's pool. */
	saved = NULL;
	if (master->save_limit[kind] != 0) {
		saved = jpeg_compat_alloc(cinfo, JPOOL_IMAGE, sizeof(*saved) + keep);
		saved->next = NULL;
		saved->marker = (UINT8)marker;
		saved->original_length = length;
		saved->data_length = keep;
		saved->data = (JOCTET *)(saved + 1);
	}

	/* Each byte: into the record while it keeps them, and the first ones for JFIF and Adobe. */
	memset(head, 0, sizeof(head));
	for (index = 0; index < length; index++) {
		byte = jpeg_compat_read_byte(cinfo);
		if (saved != NULL && index < keep)
			saved->data[index] = (JOCTET)byte;
		if (index < sizeof(head))
			head[index] = (unsigned char)byte;
	}

	/* The kept marker goes at the end of the list. */
	if (saved != NULL) {
		if (master->last_saved == NULL) {
			cinfo->marker_list = saved;
		} else {
			master->last_saved->next = saved;
		}

		/* The next kept marker follows this one. */
		master->last_saved = saved;
	}

	/* JFIF's and Adobe's facts, when the segment is long enough to hold them. */
	if (marker == JPEG_APP0 && length >= JPEG_JFIF_HEAD)
		jpeg_compat_parse_app0(cinfo, head);
	if (marker == JPEG_APP14 && length >= JPEG_ADOBE_HEAD)
		jpeg_compat_parse_app14(cinfo, head);
}

/* Tells which kind of kept marker a code is (APP0 to APP15 are 0 to 15, COM 16), or -1. */
static int
jpeg_compat_saved_kind(
	int marker)
{
	/* APPn. */
	if (marker >= JPEG_APP0 && marker <= JPEG_APP15)
		return marker - JPEG_APP0;

	/* COM. */
	if (marker == JPEG_COM)
		return JPEG_SAVED_KINDS - 1;

	/* Any other marker cannot be kept. */
	return -1;
}

/* Reads a JFIF header's version and density from the start of an APP0 segment. */
static void
jpeg_compat_parse_app0(
	j_decompress_ptr cinfo,
	const unsigned char *head)
{
	int differs;

	/* "JFIF\0", the version, the unit and the densities; another APP0 says nothing. */
	differs = memcmp(head, "JFIF", 5);
	if (differs != 0)
		return;

	/* The header's facts. */
	cinfo->saw_JFIF_marker = TRUE;
	cinfo->JFIF_major_version = head[5];
	cinfo->JFIF_minor_version = head[6];
	cinfo->density_unit = head[7];
	cinfo->X_density = (UINT16)((head[8] << 8) | head[9]);
	cinfo->Y_density = (UINT16)((head[10] << 8) | head[11]);
}

/* Reads Adobe's colour transform (0 for RGB or CMYK, 1 for YCbCr, 2 for YCCK) from the start of an APP14 segment. */
static void
jpeg_compat_parse_app14(
	j_decompress_ptr cinfo,
	const unsigned char *head)
{
	int differs;

	/* "Adobe", the version and flags, then the transform; another APP14 says nothing. */
	differs = memcmp(head, "Adobe", 5);
	if (differs != 0)
		return;

	/* The marker's transform. */
	cinfo->saw_Adobe_marker = TRUE;
	cinfo->Adobe_transform = head[11];
}

/* Refuses a frame this library does not decode: arithmetic, lossless and hierarchical. */
static void
jpeg_compat_refuse_frame(
	j_decompress_ptr cinfo,
	int marker)
{
	/* Arithmetic coding (SOF9 and up, and DAC). */
	if (marker >= 0xC9 || marker == JPEG_DAC)
		jpeg_compat_fail((j_common_ptr)cinfo, JERR_ARITHMETIC);

	/* Lossless and hierarchical. */
	jpeg_compat_fail_number((j_common_ptr)cinfo, JERR_UNSUPPORTED_SOF, marker, 0);
}

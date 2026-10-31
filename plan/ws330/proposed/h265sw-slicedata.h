/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * slice_segment_data() of the software H.265 decoder (7.3.8.1): the
 * CTUs of one slice segment in tile scan order, the initialization,
 * storage and synchronization of the CABAC state at the starts of
 * slices, tiles and wavefront rows (9.3.1, 9.3.2.1, 9.3.2.4, 9.3.2.5),
 * end_of_slice_segment_flag, end_of_subset_one_bit with byte_alignment()
 * and the restart of the engine for the next substream, and the QP
 * prediction resets (8.6.1 step 1).  Each CTU is parsed
 * (h265sw_ctu_parse) and reconstructed (h265sw_recon_ctu) before the
 * next.
 *
 * Substreams are read one after another from the slice data;
 * entry_point_offset_minus1 is not needed for that (and counts
 * emulation prevention bytes the RBSP no longer has).
 */

#ifndef LIBMEDIA_H265SW_SLICEDATA_H
#define LIBMEDIA_H265SW_SLICEDATA_H

#include "h265sw.h"
#include "h265sw-cabac-init.h"
#include "h265sw-ctu.h"
#include "h265sw-recon.h"

/*
 * The CABAC context variables kept across CTUs and slice segments of a
 * picture: wpp (TableStateIdxWpp and TableMpsValWpp, stored after the
 * second CTU of a row of a tile, 9.3.1) with wpp_valid, and ds
 * (TableStateIdxDs, stored at the end of every slice segment when
 * dependent_slice_segments_enabled_flag is 1) with ds_valid.  Owned by
 * the decoder; wpp_valid is cleared at each picture.
 */
struct h265sw_cabac_store {
	uint8_t wpp[H265SW_CABAC_CONTEXTS];
	int wpp_valid;
	uint8_t ds[H265SW_CABAC_CONTEXTS];
	int ds_valid;
};

/*
 * Decodes the slice data of one slice segment.
 *
 * data and size are the RBSP bytes from header->data_offset to the end
 * (the trailing bits and cabac_zero_words included); ctu is the
 * caller's scratch for one CTU; recon and ctu_ctx describe the slice
 * (their cabac is the one this function starts); store the kept
 * contexts.
 *
 * From CtbAddrInTs = CtbAddrRsToTs[ slice_segment_address ]: at the
 * start, the contexts by 9.3.2.1 in its order of priority: the first CTU
 * of a tile is initialized (h265sw_cabac_init_contexts with initType and
 * SliceQpY); else the first CTU of a wavefront row is synchronized from
 * store->wpp when the CTB above-right (9-3) is available, else
 * initialized; else a dependent segment is synchronized from store->ds;
 * else initialized.  Then the engine
 * (vcodec_cabac_start); then for each CTU: the initialization at the
 * first CTU of a tile, the synchronization from store->wpp (when the CTB
 * above-right is available by 6.4.1, else initialization) at the first
 * CTU of a row with entropy_coding_sync_enabled_flag; the QP state reset
 * at the first quantization group of a slice, a tile and a wavefront
 * row; pic->ctb[ addr ] given SliceAddrRs and the segment before the
 * parse (the availability of 6.4.1 inside the CTU depends on it);
 * h265sw_ctu_parse; h265sw_recon_ctu; pic->ctb marked decoded; the storage to store->wpp after the
 * second CTU of a row; end_of_slice_segment_flag; when the next CTU
 * starts a tile or a wavefront row, end_of_subset_one_bit (must be 1)
 * and the engine restarted at the next byte boundary
 * (vcodec_cabac_position, vcodec_cabac_start).  At the end, the storage
 * to store->ds when the PPS allows dependent segments.
 *
 * Returns the number of CTUs decoded (also on an error: the CTUs before
 * the failing one are complete); *error is 0, or EINVAL when a CTU
 * failed to parse or reconstruct, end_of_subset_one_bit was 0, the
 * slice ran past the last CTB of the picture, or a CTB was already
 * decoded (overlapping slice segments).  The caller conceals the CTBs no
 * segment decoded.
 */
unsigned h265sw_slice_data(const uint8_t *data, size_t size, const struct h265sw_ctu_ctx *ctu_ctx, const struct h265sw_recon_ctx *recon, struct h265sw_ctu *ctu, struct h265sw_cabac_store *store, int *error);

#endif

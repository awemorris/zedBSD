/* -*- mode: c; c-file-style: "linux"; tab-width: 8; -*- */

/*
 * zedBSD Intel AX211 private MMIO contract
 *
 * Portions derived from OpenBSD sys/dev/pci/if_iwxreg.h and if_iwx.c at
 * commit 0f464d413c50396e4e6cd70948f15613d6a73081.
 * Copyright (c) 2014, 2016 genua gmbh <info@genua.de>
 * Copyright (c) 2014 Fixup Software Ltd.
 * Copyright (c) 2017, 2019, 2020 Stefan Sperling <stsp@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Copyright(c) 2017 Intel Deutschland GmbH
 * Copyright(c) 2018 - 2019 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_MMIO_H
#define ZEDBSD_DRIVERS_INTEL_AX211_MMIO_H

#include <stdint.h>

#define INTEL_AX211_MMIO_MAC_SO                    0x37U
#define INTEL_AX211_MMIO_MAC_SOF                   0x43U
#define INTEL_AX211_MMIO_RF_GF                    0x10dU
#define INTEL_AX211_MMIO_UMAC_PRPH_OFFSET       0x300000U
#define INTEL_AX211_MMIO_IML_SIZE                  13944U

enum intel_ax211_mmio_result {
	INTEL_AX211_MMIO_OK = 0,
	INTEL_AX211_MMIO_INVALID = 1,
	INTEL_AX211_MMIO_IO = 2,
	INTEL_AX211_MMIO_TIMEOUT = 3,
	INTEL_AX211_MMIO_CLOCK = 4,
	INTEL_AX211_MMIO_ORDER = 5,
	INTEL_AX211_MMIO_NOT_OWNER = 6
};

enum intel_ax211_mmio_wait {
	INTEL_AX211_MMIO_WAIT_HW_READY = 1,
	INTEL_AX211_MMIO_WAIT_APM_CLOCK = 2,
	INTEL_AX211_MMIO_WAIT_NIC_OWNERSHIP = 3,
	INTEL_AX211_MMIO_WAIT_MASTER_DISABLED = 4
};

struct intel_ax211_mmio_profile {
	uint16_t mac_type;
	uint16_t rf_type;
	uint8_t cdb;
	uint8_t integrated;
	uint32_t umac_prph_offset;
};

struct intel_ax211_mmio_ops {
	int (*csr_read32)(void *argument, uint32_t offset, uint32_t *value);
	int (*csr_write32)(void *argument, uint32_t offset, uint32_t value);
	int (*prph_read32)(void *argument, uint32_t address, uint32_t *value);
	int (*prph_write32)(void *argument, uint32_t address, uint32_t value);
	int (*delay_us)(void *argument, uint32_t duration_us);
	int (*clock_us)(void *argument, uint64_t *time_us);
	void (*trace_deadline)(void *argument, enum intel_ax211_mmio_wait wait,
		uint64_t start_us, uint64_t deadline_us);
};

struct intel_ax211_mmio_boot {
	uint64_t context_address;
	uint64_t iml_address;
	uint32_t iml_size;
};

struct intel_ax211_mmio {
	const struct intel_ax211_mmio_ops *ops;
	void *argument;
	struct intel_ax211_mmio_profile profile;
	unsigned int nic_lock_depth;
	int prepared;
	int reset_done;
	int apm_ready;
};

int intel_ax211_mmio_init(struct intel_ax211_mmio *mmio,
	const struct intel_ax211_mmio_ops *ops, void *argument,
	const struct intel_ax211_mmio_profile *profile);
int intel_ax211_mmio_prepare_card_hw(struct intel_ax211_mmio *mmio);
int intel_ax211_mmio_sw_reset(struct intel_ax211_mmio *mmio);
int intel_ax211_mmio_apm_init(struct intel_ax211_mmio *mmio);
/* Success guarantees STOP_MASTER, APM stop, and a settled software reset. */
int intel_ax211_mmio_stop(struct intel_ax211_mmio *mmio);
int intel_ax211_mmio_nic_lock(struct intel_ax211_mmio *mmio);
int intel_ax211_mmio_nic_unlock(struct intel_ax211_mmio *mmio);
int intel_ax211_mmio_read_mac(struct intel_ax211_mmio *mmio,
	uint8_t mac_address[6]);
int intel_ax211_mmio_prph_read32(struct intel_ax211_mmio *mmio,
	uint32_t address, uint32_t *value);
int intel_ax211_mmio_prph_write32(struct intel_ax211_mmio *mmio,
	uint32_t address, uint32_t value);
int intel_ax211_mmio_publish_gen3(struct intel_ax211_mmio *mmio,
	const struct intel_ax211_mmio_boot *boot);

#endif

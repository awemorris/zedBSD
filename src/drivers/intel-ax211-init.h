/*
 * zedBSD Intel AX211 private initialization protocol contract
 *
 * Portions derived from OpenBSD sys/dev/pci/if_iwxreg.h and if_iwx.c.
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
 *
 * OpenBSD source provenance:
 *   sys/dev/pci/if_iwx.c, if_iwxreg.h at
 *   0f464d413c50396e4e6cd70948f15613d6a73081
 */

#ifndef ZEDBSD_DRIVERS_INTEL_AX211_INIT_H
#define ZEDBSD_DRIVERS_INTEL_AX211_INIT_H

#include <stddef.h>
#include <stdint.h>

#include "intel-ax211-protocol.h"

#define INTEL_AX211_INIT_SYSTEM_GROUP                     2U
#define INTEL_AX211_INIT_EXTENDED_CFG_OPCODE            0x03U
#define INTEL_AX211_INIT_EXTENDED_CFG_VERSION              1U
#define INTEL_AX211_INIT_EXTENDED_CFG_SIZE                 4U
#define INTEL_AX211_INIT_EXTENDED_CFG_NVM_FLAG          0x02U

#define INTEL_AX211_INIT_NVM_ACCESS_COMMAND_VERSION        1U
#define INTEL_AX211_INIT_NVM_ACCESS_NOTIFICATION_VERSION   0U
#define INTEL_AX211_INIT_NVM_GET_INFO_COMMAND_VERSION      1U

enum intel_ax211_init_profile {
	INTEL_AX211_INIT_PROFILE_RUNTIME = 0,
	INTEL_AX211_INIT_PROFILE_READ_NVM = 1
};

int intel_ax211_init_api89_validate(
	const struct intel_ax211_protocol_command_table *table);

int intel_ax211_init_extended_cfg_encode(
	enum intel_ax211_init_profile profile,
	uint8_t output[INTEL_AX211_INIT_EXTENDED_CFG_SIZE]);
int intel_ax211_init_extended_cfg_decode(
	const uint8_t *bytes, size_t length,
	enum intel_ax211_init_profile *profile);

int intel_ax211_init_complete_validate(
	const struct intel_ax211_protocol_message *message,
	uint32_t generation);

#endif

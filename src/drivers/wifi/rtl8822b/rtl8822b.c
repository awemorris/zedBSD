/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * RTL8822B bounded firmware, efuse, and receive codecs
 */

#include "rtl8822b-internal.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#ifndef RTL8822B_HOST_TEST
#include "kern/file.h"
#include "kern/kmem.h"
#include "kern/vfs.h"
#include <fcntl.h>
#endif

#define RTL8822B_FW_SIGNATURE 0x8822U
#define RTL8822B_FW_VERSION 30U
#define RTL8822B_FW_SUBVERSION 20U
#define RTL8822B_FW_SUBINDEX 0U
#define RTL8822B_FW_H2C_FORMAT_VERSION 14U
#define RTL8822B_FW_MEM_USAGE 0x08U
#define RTL8822B_FW_DMEM_HEADER_ADDRESS 0x80200000U
#define RTL8822B_FW_IMEM_HEADER_ADDRESS 0x80000000U

#define RTL8822B_SYS_CFG1_CUT_SHIFT 12U
#define RTL8822B_SYS_CFG1_CUT_MASK 0x0fU
#define RTL8822B_SYS_CFG1_RTL_ID 0x00800000U
#define RTL8822B_SYS_CFG1_RF_2T2R 0x08000000U
#define RTL8822B_CUT_G 6U
#define RTL8822B_CUT_D 3U

#define RTL8822B_RX_PKT_LENGTH_MASK 0x3fffU
#define RTL8822B_RX_CRC_ERROR 0x4000U
#define RTL8822B_RX_ICV_ERROR 0x8000U
#define RTL8822B_RX_DRV_INFO_SHIFT 16U
#define RTL8822B_RX_DRV_INFO_MASK 0x0fU
#define RTL8822B_RX_SHIFT_SHIFT 24U
#define RTL8822B_RX_SHIFT_MASK 0x03U
#define RTL8822B_RX_PHY_STATUS 0x04000000U
#define RTL8822B_RX_ENCRYPTION_SHIFT 20U
#define RTL8822B_RX_ENCRYPTION_MASK 0x07U
#define RTL8822B_RX_SOFTWARE_DECRYPT 0x08000000U
#define RTL8822B_RX_MAC_ID_MASK 0x7fU
#define RTL8822B_RX_C2H_FLAG 0x10000000U
#define RTL8822B_RX_RATE_MASK 0x7fU
#define RTL8822B_RX_RATE_MAX 0x54U
#define RTL8822B_RX_BANDWIDTH_SHIFT 4U
#define RTL8822B_RX_BANDWIDTH_MASK 0x03U

#define RTL8822B_POWER_POLL_MAX 20000U
#define RTL8822B_POWER_POLL_DELAY_US 50U

#define RTL8822B_REG_SYS_FUNC_EN 0x0002U
#define RTL8822B_REG_SYS_POWER_CTRL 0x0004U
#define RTL8822B_REG_RSV_CTRL 0x001cU
#define RTL8822B_REG_RF_CTRL 0x001fU
#define RTL8822B_REG_AFE_CTRL1 0x0024U
#define RTL8822B_REG_GPIO_MUX 0x0040U
#define RTL8822B_REG_LED_CFG 0x004cU
#define RTL8822B_REG_COEX_OWNER 0x0073U
#define RTL8822B_REG_MCUFW_CTRL 0x0080U
#define RTL8822B_REG_SYS_STATUS1 0x00f4U
#define RTL8822B_REG_WLRF1 0x00ecU
#define RTL8822B_REG_CR 0x0100U
#define RTL8822B_REG_TXDMA_PQ_MAP 0x010cU
#define RTL8822B_REG_RXFF_BOUNDARY 0x011cU
#define RTL8822B_REG_TRXFF_BOUNDARY 0x0114U
#define RTL8822B_REG_FIFO_PAGE_CTRL2 0x0204U
#define RTL8822B_REG_AUTO_LLT 0x0208U
#define RTL8822B_REG_TXDMA_OFFSET_CHECK 0x020cU
#define RTL8822B_REG_RQPN_CTRL2 0x022cU
#define RTL8822B_REG_FIFO_PAGE_HIGH 0x0230U
#define RTL8822B_REG_FIFO_PAGE_LOW 0x0234U
#define RTL8822B_REG_FIFO_PAGE_NORMAL 0x0238U
#define RTL8822B_REG_FIFO_PAGE_EXTRA 0x023cU
#define RTL8822B_REG_FIFO_PAGE_PUBLIC 0x0240U
#define RTL8822B_REG_H2C_HEAD 0x0244U
#define RTL8822B_REG_H2C_TAIL 0x0248U
#define RTL8822B_REG_H2C_READ 0x024cU
#define RTL8822B_REG_H2C_INFO 0x0254U
#define RTL8822B_REG_RXDMA_AGGREGATION 0x0280U
#define RTL8822B_REG_RX_PACKET_NUMBER 0x0284U
#define RTL8822B_REG_RXDMA_MODE 0x0290U
#define RTL8822B_REG_CR_EXT 0x1100U
#define RTL8822B_REG_CPU_DMEM_CON 0x1080U
#define RTL8822B_REG_H2C_PACKET_READ 0x10d0U
#define RTL8822B_REG_H2C_PACKET_WRITE 0x10d4U
#define RTL8822B_REG_H2CQ_CSR 0x1330U
#define RTL8822B_REG_DATA_SC 0x0483U
#define RTL8822B_REG_AMPDU_MAX_TIME 0x0455U
#define RTL8822B_REG_TX_HANG_CTRL 0x045eU
#define RTL8822B_REG_FW_HW_TXQ_CTRL 0x0420U
#define RTL8822B_REG_BCNQ_BOUNDARY 0x0424U
#define RTL8822B_REG_BCNQ1_BOUNDARY 0x0456U
#define RTL8822B_REG_TX_PAUSE 0x0522U
#define RTL8822B_REG_SW_AMPDU_BURST 0x04bcU
#define RTL8822B_REG_PROTECTION_MODE 0x04c8U
#define RTL8822B_REG_BAR_MODE 0x04ccU
#define RTL8822B_REG_EDCA_VO 0x0500U
#define RTL8822B_REG_EDCA_VI 0x0504U
#define RTL8822B_REG_PIFS 0x0512U
#define RTL8822B_REG_SIFS 0x0514U
#define RTL8822B_REG_SLOT 0x051bU
#define RTL8822B_REG_TX_PROTOCOL 0x0520U
#define RTL8822B_REG_TBTT_PROHIBIT 0x0540U
#define RTL8822B_REG_NAV 0x0544U
#define RTL8822B_REG_BEACON_CTRL 0x0550U
#define RTL8822B_REG_DRIVER_EARLY_INT 0x0558U
#define RTL8822B_REG_BEACON_DMA_TIME 0x0559U
#define RTL8822B_REG_USTIME_TSF 0x055cU
#define RTL8822B_REG_RX_TSF_OFFSET 0x055eU
#define RTL8822B_REG_TIMER0_SOURCE 0x05b4U
#define RTL8822B_REG_TCR 0x0604U
#define RTL8822B_REG_RCR 0x0608U
#define RTL8822B_REG_RX_PACKET_LIMIT 0x060cU
#define RTL8822B_REG_RX_DRIVER_INFO 0x060fU
#define RTL8822B_REG_PORT0_MAC 0x0610U
#define RTL8822B_REG_USTIME_EDCA 0x0638U
#define RTL8822B_REG_WMAC_TRX_PROTOCOL 0x0668U
#define RTL8822B_REG_RX_FILTER0 0x06a0U
#define RTL8822B_REG_RX_FILTER1 0x06a2U
#define RTL8822B_REG_RX_FILTER2 0x06a4U
#define RTL8822B_REG_WMAC_OPTION 0x07d0U
#define RTL8822B_REG_SOUNDING_PROTOCOL 0x0718U
#define RTL8822B_REG_HT_STF_WEIGHT 0x0800U
#define RTL8822B_REG_RX_PATH_SELECT 0x0808U
#define RTL8822B_REG_TX_PATH_SELECT 0x080cU
#define RTL8822B_REG_RX_CCA_MASK 0x0814U
#define RTL8822B_REG_CCA_SELECT 0x082cU
#define RTL8822B_REG_PD_MATCH_THRESHOLD 0x0830U
#define RTL8822B_REG_CCA_SECONDARY 0x0838U
#define RTL8822B_REG_L1_WEIGHT 0x083cU
#define RTL8822B_REG_CLOCK_TRACK 0x0860U
#define RTL8822B_REG_ADC_CLOCK 0x08acU
#define RTL8822B_REG_ADC_160 0x08c4U
#define RTL8822B_REG_ACBB0 0x0948U
#define RTL8822B_REG_CDD_TX_PATH 0x093cU
#define RTL8822B_REG_TX_PATH_SELECT1 0x0940U
#define RTL8822B_REG_ACBB_RX_FIR 0x094cU
#define RTL8822B_REG_ACG_G2_TABLE 0x0958U
#define RTL8822B_REG_ENABLE_TX_CCK 0x0a80U
#define RTL8822B_REG_ADC_INITIAL 0x0a04U
#define RTL8822B_REG_RX_DESCRIPTOR 0x0a2cU
#define RTL8822B_REG_TX_SHAPING2 0x0a24U
#define RTL8822B_REG_TX_SHAPING6 0x0a28U
#define RTL8822B_REG_TX_DFIR 0x0c20U
#define RTL8822B_REG_RX_IGI_A 0x0c50U
#define RTL8822B_REG_AGC_TRX_A 0x0c08U
#define RTL8822B_REG_TR_SWITCH 0x0ca0U
#define RTL8822B_REG_RFE_SELECT0 0x0cb0U
#define RTL8822B_REG_RFE_SELECT8 0x0cb4U
#define RTL8822B_REG_RFE_CONTROL 0x0cb8U
#define RTL8822B_REG_RFE_INVERT 0x0cbcU
#define RTL8822B_REG_RX_IGI_B 0x0e50U
#define RTL8822B_REG_AGC_TRX_B 0x0e08U
#define RTL8822B_REG_CCK_CHECK 0x0454U
#define RTL8822B_REG_FAST_EDCA_VOVI 0x1448U
#define RTL8822B_REG_FAST_EDCA_BEBK 0x144cU
#define RTL8822B_REG_ANTENNA_WEIGHT 0x1904U
#define RTL8822B_REG_RFE_PATH_SOURCE 0x1990U
#define RTL8822B_REG_RFE_IO_DIRECTION 0x0974U
#define RTL8822B_REG_MRC 0x0850U
#define RTL8822B_REG_COEX_ACCESS 0x1700U
#define RTL8822B_REG_COEX_WRITE 0x1704U
#define RTL8822B_REG_COEX_READ 0x1708U
#define RTL8822B_REG_USB_CPWM 0xfe57U
#define RTL8822B_REG_USB_RPWM 0xfe58U
#define RTL8822B_REG_USB3_PHY_ADDRESS 0xff0cU
#define RTL8822B_REG_USB3_PHY_DATA_LOW 0xff0dU
#define RTL8822B_REG_USB3_PHY_DATA_HIGH 0xff0eU

#define RTL8822B_MCUFW_INIT_READY 0x8000U
#define RTL8822B_RPWM_ACK 0x40U
#define RTL8822B_RPWM_TOGGLE 0x80U
#define RTL8822B_RPWM_ACK_POLL_US 100U
#define RTL8822B_RPWM_ACK_POLL_MAX 150U

#define RTL8822B_RF_SIPI_A 0x0c90U
#define RTL8822B_RF_SIPI_B 0x0e90U
#define RTL8822B_RF_DIRECT_A 0x2800U
#define RTL8822B_RF_DIRECT_B 0x2c00U
#define RTL8822B_RF_VALUE_MASK 0x000fffffU

#define RTL8822B_TXAGC_A 0x1d00U
#define RTL8822B_TXAGC_B 0x1d80U
#define RTL8822B_TXAGC_LAST_RATE 0x3fU
#define RTL8822B_EFUSE_TX_POWER_OFFSET 0x10U
#define RTL8822B_EFUSE_TX_POWER_STRIDE 0x2aU
#define RTL8822B_EFUSE_2G_HT1_DIFF_OFFSET 0x0bU
#define RTL8822B_EFUSE_5G_BW40_OFFSET 0x12U
#define RTL8822B_EFUSE_5G_HT1_DIFF_OFFSET 0x20U
#define RTL8822B_LEGACY_RATE_COUNT 12U
#define RTL8822B_CHANNEL_PLAN_WORLD_ETSI1 0x26U
#define RTL8822B_CHANNEL_PLAN_MKK1_MKK1 0x27U
#define RTL8822B_CHANNEL_PLAN_REALTEK_DEFINE 0x7fU
#define RTL8822B_CHANNEL_PLAN_HARDWARE_ONLY 0x80U

#define RTL8822B_CR_ALL_ENABLE 0xffU
#define RTL8822B_CR_RX_ENABLE_MASK 0x8aU
#define RTL8822B_RX_RELEASE_ENABLE 0x00040000U
#define RTL8822B_RXDMA_IDLE 0x00020000U
#define RTL8822B_RX_GENERATION_POLL_MAX 150U
#define RTL8822B_RX_GENERATION_POLL_US 100U
#define RTL8822B_BB_RESET_BITS 0x03U
#define RTL8822B_RF_ENABLE_BITS 0x07U
#define RTL8822B_WLRF_ENABLE_BITS 0x07000000U
#define RTL8822B_RX_PATH_RESET 0x30000000U
#define RTL8822B_RCR_SCAN_PROFILE 0xe400220eU

/* The initial RTL8822BU target exposes exactly three bulk OUT endpoints. */
#define RTL8822B_USB3_TXDMA_MAP 0xf5a0U
#define RTL8822B_TX_FIFO_PAGES 2048U
#define RTL8822B_RESERVED_PAGES 52U
#define RTL8822B_RESERVED_BOUNDARY 1996U
#define RTL8822B_PUBLIC_QUEUE_PAGES 1803U
#define RTL8822B_RX_FIFO_BOUNDARY 0x5effU
#define RTL8822B_H2C_QUEUE_ADDRESS 0x3fa00U
#define RTL8822B_H2C_QUEUE_END 0x3fe00U
#define RTL8822B_H2C_QUEUE_SIZE 1024U
#define RTL8822B_LLT_POLL_MAX 1000U
#define RTL8822B_LLT_POLL_DELAY_US 10U
#define RTL8822B_COEX_READY 0x20000000U
#define RTL8822B_COEX_POLL_MAX 1000U
#define RTL8822B_COEX_POLL_DELAY_US 10U
#define RTL8822B_COEX_GRANT_MASK 0xff80U
#define RTL8822B_COEX_WLAN_GRANT 0x7700U

struct rtl8822b_sha256_context {
	uint32_t state[8];
	uint64_t byte_count;
	uint8_t block[64];
	size_t block_length;
};

static const uint32_t sha256_constants[64] = {
	0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
	0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
	0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
	0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
	0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
	0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
	0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
	0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
	0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
	0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
	0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
	0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
	0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

static const uint8_t rtl8822b_firmware_digest[32] = {
	0xa7, 0x2d, 0xa6, 0x90, 0x59, 0x7b, 0xfa, 0x99, 0xd8, 0xeb, 0x6f,
	0xc2, 0xab, 0x09, 0x0d, 0x18, 0xd8, 0xad, 0x92, 0xac, 0x2b, 0xef,
	0xd3, 0x5d, 0xb1, 0xc9, 0xe3, 0x66, 0x2d, 0x8d, 0x84, 0x18};

#include "rtl8822b-tables.inc"

static int radio_usb_phy_profile(struct rtl8822b_radio *radio, uint64_t deadline_ticks);
static int radio_usb_profile(struct rtl8822b_radio *radio, uint64_t deadline_ticks);
static int radio_board_wlan_only(const struct rtl8822b_radio *radio);
static int radio_coex_ready(struct rtl8822b_radio *radio, uint64_t deadline_ticks);
static int radio_coex_grant_read(struct rtl8822b_radio *radio, uint32_t *value, uint64_t deadline_ticks);
static int radio_wlan_only_profile(struct rtl8822b_radio *radio, uint64_t deadline_ticks);
static int radio_txagc_legacy_profile(struct rtl8822b_radio *radio, uint8_t channel, uint64_t deadline_ticks);

static uint16_t load_le16(const uint8_t *bytes);

/* Reads a 16-bit field, least significant byte first. */
static uint16_t
load_le16(
	const uint8_t *bytes)
{
	/* Returns the computed result. */
	return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t load_le32(const uint8_t *bytes);

/* Reads a 32-bit field, least significant byte first. */
static uint32_t
load_le32(
	const uint8_t *bytes)
{
	/* Returns the computed result. */
	return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
	       ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static void store_le16(uint8_t *bytes, uint16_t value);

/* Writes a 16-bit field, least significant byte first. */
static void
store_le16(
	uint8_t *bytes,
	uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static void store_le32(uint8_t *bytes, uint32_t value);

/* Writes a 32-bit field, least significant byte first. */
static void
store_le32(
	uint8_t *bytes,
	uint32_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
	bytes[2] = (uint8_t)(value >> 16);
	bytes[3] = (uint8_t)(value >> 24);
}

static uint32_t rotate_right(uint32_t value, unsigned shift);

/* Rotates a word right, as the hash function needs. */
static uint32_t
rotate_right(
	uint32_t value,
	unsigned shift)
{
	/* Returns the computed result. */
	return (value >> shift) | (value << (32U - shift));
}

static void sha256_transform(struct rtl8822b_sha256_context *context, const uint8_t block[64]);

/* Folds one block into the running hash state. */
static void
sha256_transform(
	struct rtl8822b_sha256_context *context,
	const uint8_t block[64])
{
	const uint8_t *word;
	uint32_t small0, small1;
	uint32_t words[64];
	uint32_t a, b, c, d, e, f, g, h;
	uint32_t choose, majority, sigma0, sigma1, temporary1, temporary2;
	unsigned index;

	/* Process each remaining element. */
	for (index = 0; index < 16U; index++) {
		word = block + index * 4U;

		words[index] = ((uint32_t)word[0] << 24) |
			       ((uint32_t)word[1] << 16) |
			       ((uint32_t)word[2] << 8) | (uint32_t)word[3];
	}

	/* Process each remaining element. */
	for (; index < 64U; index++) {
		small0 = rotate_right(words[index - 15U], 7U) ^
			 rotate_right(words[index - 15U], 18U) ^
			 (words[index - 15U] >> 3);
		small1 = rotate_right(words[index - 2U], 17U) ^
			 rotate_right(words[index - 2U], 19U) ^
			 (words[index - 2U] >> 10);
		words[index] = words[index - 16U] + small0 + words[index - 7U] +
			       small1;
	}

	/* Works on a copy of the running state. */
	a = context->state[0];
	b = context->state[1];
	c = context->state[2];
	d = context->state[3];
	e = context->state[4];
	f = context->state[5];
	g = context->state[6];
	h = context->state[7];
	/* Process each remaining element. */
	for (index = 0; index < 64U; index++) {
		sigma1 = rotate_right(e, 6U) ^ rotate_right(e, 11U) ^
			 rotate_right(e, 25U);
		choose = (e & f) ^ ((~e) & g);
		temporary1 = h + sigma1 + choose + sha256_constants[index] +
			     words[index];
		sigma0 = rotate_right(a, 2U) ^ rotate_right(a, 13U) ^
			 rotate_right(a, 22U);
		majority = (a & b) ^ (a & c) ^ (b & c);
		temporary2 = sigma0 + majority;
		h = g;
		g = f;
		f = e;
		e = d + temporary1;
		d = c;
		c = b;
		b = a;
		a = temporary1 + temporary2;
	}

	context->state[0] += a;
	context->state[1] += b;
	context->state[2] += c;
	context->state[3] += d;
	context->state[4] += e;
	context->state[5] += f;
	context->state[6] += g;
	context->state[7] += h;
}

static void sha256_init(struct rtl8822b_sha256_context *context);

/* Starts a hash with the constants the algorithm defines. */
static void
sha256_init(
	struct rtl8822b_sha256_context *context)
{
	static const uint32_t initial_state[8] = {
		0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
		0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

	memcpy(context->state, initial_state, sizeof(initial_state));
	context->byte_count = 0;
	context->block_length = 0;
}

static void sha256_update(struct rtl8822b_sha256_context *context, const uint8_t *data, size_t length);

/* Takes more bytes into a running hash. */
static void
sha256_update(
	struct rtl8822b_sha256_context *context,
	const uint8_t *data,
	size_t length)
{
	size_t available;
	size_t copy;

	/* Process each remaining element. */
	while (length != 0U) {
		available = sizeof(context->block) - context->block_length;
		copy = length < available ? length : available;

		memcpy(context->block + context->block_length, data, copy);
		context->block_length += copy;
		context->byte_count += copy;
		data += copy;
		length -= copy;

		/* Handles the context condition. */
		if (context->block_length == sizeof(context->block)) {
			sha256_transform(context, context->block);
			context->block_length = 0;
		}
	}
}

static void sha256_final(struct rtl8822b_sha256_context *context, uint8_t digest[32]);

/* Finishes a hash and reports its digest. */
static void
sha256_final(
	struct rtl8822b_sha256_context *context,
	uint8_t digest[32])
{
	uint64_t bit_count = context->byte_count * 8U;
	unsigned index;

	context->block[context->block_length++] = 0x80U;

	/* Handles the context condition. */
	if (context->block_length > 56U) {
		memset(context->block + context->block_length, 0,
		       sizeof(context->block) - context->block_length);
		sha256_transform(context, context->block);
		context->block_length = 0;
	}

	memset(context->block + context->block_length, 0,
	       56U - context->block_length);
	/* Process each remaining element. */
	for (index = 0; index < 8U; index++) {
		context->block[63U - index] =
			(uint8_t)(bit_count >> (index * 8U));
	}

	sha256_transform(context, context->block);
	/* Process each remaining element. */
	for (index = 0; index < 8U; index++) {
		digest[index * 4U] = (uint8_t)(context->state[index] >> 24);
		digest[index * 4U + 1U] =
			(uint8_t)(context->state[index] >> 16);
		digest[index * 4U + 2U] = (uint8_t)(context->state[index] >> 8);
		digest[index * 4U + 3U] = (uint8_t)context->state[index];
	}

	memset(context, 0, sizeof(*context));
}

/*
 * Hashes a run of bytes in one call.
 */
int
drv_rtl8822b_sha256(
	const void *data,
	size_t length,
	uint8_t digest[32])
{
	struct rtl8822b_sha256_context context;

	/* Handles the digest availability. */
	if (digest == NULL || (data == NULL && length != 0U))
		return EINVAL;
#if SIZE_MAX > UINT64_MAX / 8U

	/* Checks the current data length. */
	if (length > (size_t)(UINT64_MAX / 8U))
		return EOVERFLOW;
#endif
	sha256_init(&context);

	/* Checks the current data length. */
	if (length != 0U)
		sha256_update(&context, data, length);
	sha256_final(&context, digest);

	/* Succeeded. */
	return 0;
}

static int firmware_validate_expected(const uint8_t *data, size_t length, const uint8_t expected_digest[32], struct rtl8822b_firmware_view *view);

/* Reports the digest the firmware image should hash to. */
static int
firmware_validate_expected(
	const uint8_t *data,
	size_t length,
	const uint8_t expected_digest[32],
	struct rtl8822b_firmware_view *view)
{
	struct rtl8822b_firmware_view result;
	uint8_t actual_digest[32];
	int error;

	/* Handles the data availability. */
	if (data == NULL || expected_digest == NULL || view == NULL)
		return EINVAL;
	memset(view, 0, sizeof(*view));

	/* Checks the current data length. */
	if (length != RTL8822B_FIRMWARE_SIZE)
		return EINVAL;

	/* Checks the load le16 result. */
	if (load_le16(data) != RTL8822B_FW_SIGNATURE || data[2] != 0U ||
	    data[3] != 0U || load_le16(data + 4U) != RTL8822B_FW_VERSION ||
	    data[6] != RTL8822B_FW_SUBVERSION ||
	    data[7] != RTL8822B_FW_SUBINDEX ||
	    data[24] != RTL8822B_FW_MEM_USAGE ||
	    load_le16(data + 28U) != RTL8822B_FW_H2C_FORMAT_VERSION ||
	    load_le32(data + 32U) != RTL8822B_FW_DMEM_HEADER_ADDRESS ||
	    load_le32(data + 36U) != RTL8822B_FIRMWARE_DMEM_SIZE ||
	    load_le32(data + 48U) != RTL8822B_FIRMWARE_IMEM_SIZE ||
	    load_le32(data + 52U) != 0U ||
	    load_le32(data + 60U) != RTL8822B_FW_IMEM_HEADER_ADDRESS) {
		/* Failed. */
		return EINVAL;
	}

	/* Checks the operation status. */
	error = drv_rtl8822b_sha256(data, length, actual_digest);
	if (error != 0)
		return error;

	/* Handles the memcmp condition. */
	if (memcmp(actual_digest, expected_digest, sizeof(actual_digest)) !=
	    0) {
		memset(actual_digest, 0, sizeof(actual_digest));

		/* Failed. */
		return EILSEQ;
	}

	memset(actual_digest, 0, sizeof(actual_digest));

	/* Describes the image the fixed layout says this is. */
	result.bytes = data;
	result.size = length;
	result.version = RTL8822B_FW_VERSION;
	result.subversion = RTL8822B_FW_SUBVERSION;
	result.subindex = RTL8822B_FW_SUBINDEX;
	result.dmem_offset = RTL8822B_FIRMWARE_HEADER_SIZE;
	result.dmem_length =
		RTL8822B_FIRMWARE_DMEM_SIZE + RTL8822B_FIRMWARE_CHECKSUM_SIZE;
	result.dmem_address = RTL8822B_FIRMWARE_DMEM_ADDRESS;
	result.imem_offset = result.dmem_offset + result.dmem_length;
	result.imem_length =
		RTL8822B_FIRMWARE_IMEM_SIZE + RTL8822B_FIRMWARE_CHECKSUM_SIZE;
	result.imem_address = RTL8822B_FIRMWARE_IMEM_ADDRESS;

	/* Checks the operation result. */
	if (result.imem_offset > length ||
	    result.imem_length > length - result.imem_offset ||
	    result.imem_offset + result.imem_length != length) {
		/* Failed. */
		return EINVAL;
	}
	*view = result;
	/* Succeeded. */
	return 0;
}

/*
 * Refuses a firmware image that is not the expected one.
 */
int
drv_rtl8822b_firmware_validate(
	const uint8_t *data,
	size_t length,
	struct rtl8822b_firmware_view *view)
{
	int error;

	/* Obtains the firmware validate expected result. */
	error = firmware_validate_expected(
		data, length, rtl8822b_firmware_digest, view);

	/* Returns the computed result. */
	return error;
}

static int firmware_blob_state(const struct rtl8822b_firmware_blob *firmware, int *owned);

/* Reports whether the firmware image is present and usable. */
static int
firmware_blob_state(
	const struct rtl8822b_firmware_blob *firmware,
	int *owned)
{
	/* Handles the firmware availability. */
	if (firmware == NULL || owned == NULL)
		return EINVAL;
	*owned = 0;
	/* Handles the bytes availability. */
	if (firmware->bytes == NULL)
		return firmware->size == 0U ? 0 : EINVAL;

	/* Handles the firmware condition. */
	if (firmware->size != RTL8822B_FIRMWARE_SIZE ||
	    firmware->view.bytes != firmware->bytes ||
	    firmware->view.size != firmware->size) {
		/* Failed. */
		return EINVAL;
	}
	*owned = 1;
	/* Succeeded. */
	return 0;
}

#ifndef RTL8822B_HOST_TEST
static void firmware_scrub(void *memory, size_t length);

/* Clears the firmware image out of memory. */
static void
firmware_scrub(
	void *memory,
	size_t length)
{
	volatile uint8_t *bytes = memory;

	/* Process each remaining element. */
	while (length-- != 0U)
		*bytes++ = 0U;
}

/*
 * Gives the firmware image back.
 */
void
drv_rtl8822b_firmware_release(
	struct rtl8822b_firmware_blob *firmware)
{
	/* Handles the firmware availability. */
	if (firmware == NULL)
		return;

	/* Handles the bytes availability. */
	if (firmware->bytes != NULL) {
		firmware_scrub(firmware->bytes, RTL8822B_FIRMWARE_SIZE);
		kern_free(firmware->bytes);
	}

	memset(firmware, 0, sizeof(*firmware));
}

/*
 * Reads the firmware image and checks it before use.
 */
int
drv_rtl8822b_firmware_load(
	struct rtl8822b_firmware_blob *firmware)
{
	struct file_content_lease lease;
	struct rtl8822b_firmware_blob result;
	struct file *file = NULL;
	ssize_t count;
	size_t offset = 0;
	int replace_owned;
	int close_error;
	int error;

	/* Checks the operation status. */
	error = firmware_blob_state(firmware, &replace_owned);
	if (error != 0)
		return error;
	memset(&lease, 0, sizeof(lease));
	memset(&result, 0, sizeof(result));

	/* Checks the operation status. */
	error = file_openat(&kern_cwdinfo, RTL8822B_FIRMWARE_PATH,
			    O_RDONLY | O_NOFOLLOW, 0, &file);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = file_content_lease_begin(file, &lease);
	if (error != 0)
		goto close_file;

	/* Handles the lease condition. */
	if (lease.size < 0 || (uint64_t)lease.size != RTL8822B_FIRMWARE_SIZE) {
		error = EINVAL;
		goto end_lease;
	}

	result.bytes = kern_malloc(RTL8822B_FIRMWARE_SIZE);

	/* Handles the bytes availability. */
	if (result.bytes == NULL) {
		error = ENOMEM;
		goto end_lease;
	}

	result.size = RTL8822B_FIRMWARE_SIZE;
	/* Process each remaining element. */
	while (offset < result.size) {
		/* Checks the remaining item count. */
		count = file_content_lease_pread(&lease, result.bytes + offset,
						 result.size - offset,
						 (off_t)offset);
		if (count < 0) {
			error = (int)-count;
			goto end_lease;
		}

		/* Checks the remaining item count. */
		if (count == 0 || (size_t)count > result.size - offset) {
			error = EIO;
			goto end_lease;
		}

		offset += (size_t)count;
	}

end_lease:
	file_content_lease_end(&lease);
close_file:
	close_error = file_close(file);
	file = NULL;

	/* Checks the operation status. */
	if (error == 0 && close_error != 0)
		error = close_error;
	if (error == 0) {
		error = drv_rtl8822b_firmware_validate(
			result.bytes, result.size, &result.view);
	}

	/* Checks the operation status. */
	if (error != 0) {
		drv_rtl8822b_firmware_release(&result);

		/* Failed. */
		return error;
	}

	/* Handles the replace owned condition. */
	if (replace_owned)
		drv_rtl8822b_firmware_release(firmware);
	*firmware = result;
	/* Succeeded. */
	return 0;
}
#endif

#ifdef RTL8822B_TESTING
/*
 * Exposes the firmware check to the host test harness.
 */
int
drv_rtl8822b_test_firmware_validate(
	const uint8_t *data,
	size_t length,
	const uint8_t expected_digest[32],
	struct rtl8822b_firmware_view *view)
{
	int error;

	/* Obtains the firmware validate expected result. */
	error =
		firmware_validate_expected(data, length, expected_digest, view);

	/* Returns the computed result. */
	return error;
}

/*
 * Exposes the image state to the host test harness.
 */
int
drv_rtl8822b_test_firmware_blob_state(
	const struct rtl8822b_firmware_blob *firmware,
	int *owned)
{
	int error;

	/* Obtains the firmware blob state result. */
	error = firmware_blob_state(firmware, owned);

	/* Returns the computed result. */
	return error;
}
#endif

static int firmware_walk_segment(const struct rtl8822b_firmware_view *view, enum rtl8822b_firmware_segment segment, size_t file_offset, size_t segment_length, uint32_t destination, rtl8822b_firmware_chunk_fn callback, void *context);

/* Walks one segment of the firmware image. */
static int
firmware_walk_segment(
	const struct rtl8822b_firmware_view *view,
	enum rtl8822b_firmware_segment segment,
	size_t file_offset,
	size_t segment_length,
	uint32_t destination,
	rtl8822b_firmware_chunk_fn callback,
	void *context)
{
	struct rtl8822b_firmware_chunk chunk;
	size_t remaining;
	size_t length;
	int error;
	size_t offset = 0;

	/* Process each remaining element. */
	while (offset < segment_length) {
		remaining = segment_length - offset;

		/* Handles the file offset condition. */
		length = remaining < RTL8822B_FIRMWARE_CHUNK_MAX
				 ? remaining
				 : RTL8822B_FIRMWARE_CHUNK_MAX;
		if (file_offset > view->size ||
		    offset > view->size - file_offset ||
		    length > view->size - file_offset - offset ||
		    offset > UINT32_MAX - destination) {
			/* Failed. */
			return EOVERFLOW;
		}
		memset(&chunk, 0, sizeof(chunk));
		chunk.segment = segment;
		chunk.file_offset = file_offset + offset;
		chunk.destination = destination + (uint32_t)offset;
		chunk.length = (uint32_t)length;
		chunk.wire_payload_length = (uint32_t)length;

		/*
		 * Avoid full packets on both 512-byte HS and 1024-byte SS
		 * pipes.
		 */
		if ((length + RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE) % 512U ==
		    0U)
			chunk.wire_payload_length++;
		chunk.first = offset == 0U;
		chunk.last = length == remaining;
		chunk.checksum_continue = offset != 0U;

		/* Checks the operation status. */
		error = callback(context, &chunk);
		if (error != 0)
			return error;
		offset += length;
	}

	/* Succeeded. */
	return 0;
}

static int firmware_view_equal(const struct rtl8822b_firmware_view *left, const struct rtl8822b_firmware_view *right);

/* Compares two views of the firmware image. */
static int
firmware_view_equal(
	const struct rtl8822b_firmware_view *left,
	const struct rtl8822b_firmware_view *right)
{
	/* Returns the computed result. */
	return left->bytes == right->bytes && left->size == right->size &&
	       left->version == right->version &&
	       left->subversion == right->subversion &&
	       left->subindex == right->subindex &&
	       left->dmem_offset == right->dmem_offset &&
	       left->dmem_length == right->dmem_length &&
	       left->dmem_address == right->dmem_address &&
	       left->imem_offset == right->imem_offset &&
	       left->imem_length == right->imem_length &&
	       left->imem_address == right->imem_address;
}

static int firmware_walk_expected(const struct rtl8822b_firmware_view *view, const uint8_t expected_digest[32], rtl8822b_firmware_chunk_fn callback, void *context);

/* Reports the segments the image should be made of. */
static int
firmware_walk_expected(
	const struct rtl8822b_firmware_view *view,
	const uint8_t expected_digest[32],
	rtl8822b_firmware_chunk_fn callback,
	void *context)
{
	int function_result;
	struct rtl8822b_firmware_view validated;
	int error;

	/* Handles the view availability. */
	if (view == NULL || expected_digest == NULL || callback == NULL ||
	    view->bytes == NULL) {
		/* Failed. */
		return EINVAL;
	}

	/* Checks the operation status. */
	error = firmware_validate_expected(view->bytes, view->size,
					   expected_digest, &validated);
	if (error != 0)
		return error;

	/* Checks the firmware view equal result. */
	if (!firmware_view_equal(view, &validated))
		return EINVAL;

	/* Checks the operation status. */
	error = firmware_walk_segment(
		&validated, RTL8822B_FIRMWARE_SEGMENT_DMEM,
		validated.dmem_offset, validated.dmem_length,
		validated.dmem_address, callback, context);
	if (error != 0)
		return error;

	/* Obtains the firmware walk segment result. */
	function_result = firmware_walk_segment(
		&validated, RTL8822B_FIRMWARE_SEGMENT_IMEM,
		validated.imem_offset, validated.imem_length,
		validated.imem_address, callback, context);

	/* Returns the computed result. */
	return function_result;
}

/*
 * Walks the firmware image segment by segment.
 */
int
drv_rtl8822b_firmware_walk(
	const struct rtl8822b_firmware_view *view,
	rtl8822b_firmware_chunk_fn callback,
	void *context)
{
	int error;

	/* Obtains the firmware walk expected result. */
	error = firmware_walk_expected(view, rtl8822b_firmware_digest,
						 callback, context);

	/* Returns the computed result. */
	return error;
}

#ifdef RTL8822B_TESTING
/*
 * Exercises production segment geometry independently of the pinned image size.
 */
int
drv_rtl8822b_test_firmware_segment(
	const struct rtl8822b_firmware_view *view,
	size_t length,
	rtl8822b_firmware_chunk_fn callback,
	void *context)
{
	int error;

	/* Reject incomplete fixtures before entering the production walker. */
	if (view == NULL || callback == NULL)
		return EINVAL;

	/* Walk a single DMEM segment through the real chunk planner. */

	/* Checks the operation status. */
	error = firmware_walk_segment(view, RTL8822B_FIRMWARE_SEGMENT_DMEM, 0U,
				      length, 0x10000U, callback, context);
	if (error != 0)
		return error;

	/* Report the completed geometry check. */
	return 0;
}

/*
 * Exposes that walk to the host test harness.
 */
int
drv_rtl8822b_test_firmware_walk(
	const struct rtl8822b_firmware_view *view,
	const uint8_t expected_digest[32],
	rtl8822b_firmware_chunk_fn callback,
	void *context)
{
	int error;

	/* Obtains the firmware walk expected result. */
	error = firmware_walk_expected(view, expected_digest,
						 callback, context);

	/* Returns the computed result. */
	return error;
}
#endif

/*
 * Builds the descriptor a firmware download is written under.
 */
int
drv_rtl8822b_firmware_tx_descriptor(
	uint8_t descriptor[48],
	size_t payload_length)
{
	uint16_t checksum = 0;
	unsigned index;

	/* Handles the descriptor availability. */
	if (descriptor == NULL || payload_length == 0U ||
	    payload_length > UINT16_MAX) {
		/* Failed. */
		return EINVAL;
	}
	memset(descriptor, 0, RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE);
	store_le32(
		descriptor,
		(uint32_t)payload_length |
			((uint32_t)RTL8822B_FIRMWARE_TX_DESCRIPTOR_SIZE << 16) |
			(1U << 26));
	store_le32(descriptor + 4U, 16U << 8);
	/* Process each remaining element. */
	for (index = 0; index < 16U; index++)
		checksum ^= load_le16(descriptor + index * 2U);
	store_le16(descriptor + 28U, checksum);

	/* Succeeded. */
	return 0;
}

/*
 * Decodes the packed contents of the device's fuse map.
 */
int
drv_rtl8822b_efuse_decode(
	const uint8_t *physical,
	size_t physical_length,
	uint8_t *logical,
	size_t logical_length)
{
	uint8_t header2;
	size_t logical_index;
	uint8_t header1;
	uint8_t word_enable;
	unsigned block;
	unsigned word;
	const size_t usable =
		RTL8822B_EFUSE_PHYSICAL_SIZE - RTL8822B_EFUSE_PROTECTED_SIZE;
	size_t physical_index = 0;

	/* Handles the physical availability. */
	if (physical == NULL || logical == NULL ||
	    physical_length != RTL8822B_EFUSE_PHYSICAL_SIZE ||
	    logical_length != RTL8822B_EFUSE_LOGICAL_SIZE) {
		/* Failed. */
		return EINVAL;
	}
	memset(logical, 0xff, logical_length);
	/* Process each remaining element. */
	while (physical_index < usable) {
		/* Handles the header1 condition. */
		header1 = physical[physical_index++];
		if (header1 == 0xffU)
			return 0;

		/* Handles the header1 condition. */
		if ((header1 & 0x1fU) == 0x0fU) {
			/* Handles the physical index condition. */
			if (physical_index >= usable)
				return EINVAL;

			/* Handles the header2 condition. */
			header2 = physical[physical_index++];
			if (header2 == 0xffU)
				return 0;
			block = ((unsigned)(header2 & 0xf0U) >> 1) |
				((unsigned)header1 >> 5);
			word_enable = header2 & 0x0fU;
		} else {
			block = (unsigned)(header1 & 0xf0U) >> 4;
			word_enable = header1 & 0x0fU;
		}

		/* Process each element required by the operation. */
		for (word = 0; word < 4U; word++) {
			/* Handles the word enable condition. */
			if ((word_enable & (1U << word)) != 0U)
				continue;

			/* Handles the physical index condition. */
			logical_index = (size_t)block * 8U + word * 2U;
			if (physical_index > usable ||
			    usable - physical_index < 2U ||
			    logical_index > logical_length ||
			    logical_length - logical_index < 2U) {
				/* Failed. */
				return EINVAL;
			}
			logical[logical_index] = physical[physical_index++];
			logical[logical_index + 1U] =
				physical[physical_index++];
		}
	}

	/* Succeeded. */
	return 0;
}

/*
 * Reads which revision of the chip this is.
 */
int
drv_rtl8822b_chip_identity_parse(
	uint32_t sys_cfg1,
	struct rtl8822b_chip_identity *identity)
{
	struct rtl8822b_chip_identity result;
	uint32_t cut;

	/* Handles the identity availability. */
	if (identity == NULL)
		return EINVAL;
	memset(identity, 0, sizeof(*identity));

	/* Handles the cut condition. */
	cut = (sys_cfg1 >> RTL8822B_SYS_CFG1_CUT_SHIFT) &
	      RTL8822B_SYS_CFG1_CUT_MASK;
	if (cut > RTL8822B_CUT_G)
		return EOPNOTSUPP;
	memset(&result, 0, sizeof(result));
	result.cut = (uint8_t)cut;
	result.rf_path_count =
		(sys_cfg1 & RTL8822B_SYS_CFG1_RF_2T2R) != 0U ? 2U : 1U;
	result.mass_production =
		(sys_cfg1 & RTL8822B_SYS_CFG1_RTL_ID) == 0U ? 1U : 0U;
	*identity = result;
	/* Succeeded. */
	return 0;
}

static int mac_address_valid(const uint8_t address[6]);

/* Refuses a hardware address no station could have. */
static int
mac_address_valid(
	const uint8_t address[6])
{
	unsigned index;
	int all_zero = 1;
	int all_ff = 1;

	/* Handles the address condition. */
	if ((address[0] & 1U) != 0U)
		return 0;
	/* Process each remaining element. */
	for (index = 0; index < 6U; index++) {
		/* Handles the address condition. */
		if (address[index] != 0U)
			all_zero = 0;

		/* Handles the address condition. */
		if (address[index] != 0xffU)
			all_ff = 0;
	}

	/* Returns the computed result. */
	return !all_zero && !all_ff;
}

static int board_tx_power_2g_valid(const struct rtl8822bu_board_info *board);

/* Refuses a 2.4 GHz power table the board could not hold. */
static int
board_tx_power_2g_valid(
	const struct rtl8822bu_board_info *board)
{
	const struct rtl8822b_2g_tx_power *power;
	unsigned group;
	unsigned path;

	/* Handles the board availability. */
	if (board == NULL ||
	    (board->chip.rf_path_count != 1U &&
	     board->chip.rf_path_count != RTL8822B_RF_PATH_COUNT)) {
		/* Succeeded. */
		return 0;
	}
	/* Process each remaining element. */
	for (path = 0U; path < board->chip.rf_path_count; path++) {
		power = &board->tx_power_2g[path];

		/* Process each element required by the operation. */
		for (group = 0U; group < 4U; group++) {
			/* Handles the power condition. */
			if (power->cck_base[group] > 0x3fU ||
			    power->bw40_base[group] > 0x3fU) {
				/* Succeeded. */
				return 0;
			}
		}

		/* Handles the power condition. */
		if (power->ofdm_diff < -8 || power->ofdm_diff > 7)
			return 0;
	}

	/* Reports operation failure. */
	return 1;
}

static int board_tx_power_5g_w52_valid(const struct rtl8822bu_board_info *board);

/* Refuses a 5 GHz power table the board could not hold. */
static int
board_tx_power_5g_w52_valid(
	const struct rtl8822bu_board_info *board)
{
	const struct rtl8822b_5g_tx_power *power;
	unsigned group;
	unsigned path;

	/* Handles the board availability. */
	if (board == NULL ||
	    (board->chip.rf_path_count != 1U &&
	     board->chip.rf_path_count != RTL8822B_RF_PATH_COUNT)) {
		/* Succeeded. */
		return 0;
	}
	/* Process each remaining element. */
	for (path = 0U; path < board->chip.rf_path_count; path++) {
		power = &board->tx_power_5g[path];

		/*
		 * W52 consumes only groups 0 and 1 from the fourteen-group map.
		 */
		for (group = 0U; group < 2U; group++) {
			/* Handles the power condition. */
			if (power->bw40_base[group] > 0x3fU)
				return 0;
		}

		/* Handles the power condition. */
		if (power->ofdm_diff < -8 || power->ofdm_diff > 7)
			return 0;
	}

	/* Reports operation failure. */
	return 1;
}

static int channel_is_w52(uint8_t channel);

/* Asks whether a channel is one of the lower 5 GHz band. */
static int
channel_is_w52(
	uint8_t channel)
{
	/* Returns the computed result. */
	return channel == 36U || channel == 40U || channel == 44U ||
	       channel == 48U;
}

/*
 * Admits calibrated channels from the supported factory policy profiles.
 */
int
drv_rtl8822b_board_active_channel_allowed(
	const struct rtl8822bu_board_info *board,
	uint8_t channel)
{
	int error;

	/* Reject absent board facts before evaluating channel policy. */
	if (board == NULL)
		return 0;

	/* Preserve the existing calibrated 2.4-GHz subset. */
	if (channel >= 1U && channel <= 11U) {
		/* Obtains the board tx power 2g valid result. */
		error = board_tx_power_2g_valid(board);

		/* Failed. */
		return error;
	}

	/* Require both the non-DFS subset and its factory power calibration. */
	if (!channel_is_w52(channel) || !board_tx_power_5g_w52_valid(board))
		return 0;

	/*
	 * The measured erased-country profile enforces hardware plan 0x26.
	 * Bit 7 forbids software override; the remaining bits name WORLD_ETSI1,
	 * whose non-DFS channels include 36/40/44/48. Retain the raw board
	 * facts and the existing minimum FCC/ETSI/MKK power ceilings.
	 */
	if (board->country_code[0] == 0xffU &&
	    board->country_code[1] == 0xffU &&
	    board->channel_plan == (RTL8822B_CHANNEL_PLAN_HARDWARE_ONLY |
				    RTL8822B_CHANNEL_PLAN_WORLD_ETSI1)) {
		/* Reports operation failure. */
		return 1;
	}

	/* Preserve the original Japan policy for all other admitted boards. */
	if (board->country_code[0] != 'J' || board->country_code[1] != 'P')
		return 0;

	/* 0x7f delegates the channel plan to the retained JP country code. */
	return board->channel_plan == RTL8822B_CHANNEL_PLAN_MKK1_MKK1 ||
	       board->channel_plan == RTL8822B_CHANNEL_PLAN_REALTEK_DEFINE;
}

/*
 * Reads the board's own calibration out of its fuse map.
 */
int
drv_rtl8822bu_board_parse(
	const uint8_t *logical,
	size_t logical_length,
	uint32_t sys_cfg1,
	struct rtl8822bu_board_info *board)
{
	const uint8_t *power;
	uint8_t nibble;
	struct rtl8822bu_board_info result;
	unsigned path;
	int error;

	/* Handles the logical availability. */
	if (logical == NULL || board == NULL ||
	    logical_length != RTL8822B_EFUSE_LOGICAL_SIZE) {
		/* Failed. */
		return EINVAL;
	}
	memset(board, 0, sizeof(*board));
	memset(&result, 0, sizeof(result));

	/* Checks the operation status. */
	error = drv_rtl8822b_chip_identity_parse(sys_cfg1, &result.chip);
	if (error != 0)
		return error;
	memcpy(result.mac_address, logical + RTL8822BU_EFUSE_MAC_OFFSET,
	       sizeof(result.mac_address));

	/* Checks the mac address valid result. */
	if (!mac_address_valid(result.mac_address))
		return EINVAL;
	result.channel_plan = logical[0xb8U];
	result.crystal_cap = logical[0xb9U];
	result.thermal_meter = logical[0xbaU];
	result.rf_board_option = logical[0xc1U];
	result.rfe_option = logical[0xcaU];
	result.country_code[0] = logical[0xcbU];
	result.country_code[1] = logical[0xccU];

	/* Checks the operation result. */
	if (result.rfe_option != 2U && result.rfe_option != 3U &&
	    result.rfe_option != 5U) {
		/* Failed. */
		return EOPNOTSUPP;
	}
	/* Process each remaining element. */
	for (path = 0U; path < result.chip.rf_path_count; path++) {
		power = logical + RTL8822B_EFUSE_TX_POWER_OFFSET +
			path * RTL8822B_EFUSE_TX_POWER_STRIDE;

		/* Reads the per-path power tables out of the fuse block. */
		memcpy(result.tx_power_2g[path].cck_base, power,
		       sizeof(result.tx_power_2g[path].cck_base));
		memcpy(result.tx_power_2g[path].bw40_base,
		       power + RTL8822B_2G_CCK_GROUP_COUNT,
		       sizeof(result.tx_power_2g[path].bw40_base));
		nibble = power[RTL8822B_EFUSE_2G_HT1_DIFF_OFFSET] & 0x0fU;
		result.tx_power_2g[path].ofdm_diff =
			(int8_t)(nibble < 8U ? nibble : (int)nibble - 16);
		memcpy(result.tx_power_5g[path].bw40_base,
		       power + RTL8822B_EFUSE_5G_BW40_OFFSET,
		       sizeof(result.tx_power_5g[path].bw40_base));
		nibble = power[RTL8822B_EFUSE_5G_HT1_DIFF_OFFSET] & 0x0fU;
		result.tx_power_5g[path].ofdm_diff =
			(int8_t)(nibble < 8U ? nibble : (int)nibble - 16);
	}

	/* Checks the board tx power 2g valid result. */
	if (!board_tx_power_2g_valid(&result))
		return EINVAL;
	*board = result;
	/* Succeeded. */
	return 0;
}

static int32_t clamp_rssi(int32_t value);

/* Holds a signal strength inside the range the kernel reports. */
static int32_t
clamp_rssi(
	int32_t value)
{
	/* Validates the current value. */
	if (value < -120)
		return -120;

	/* Validates the current value. */
	if (value > 0)
		return 0;

	/* Returns the computed result. */
	return value;
}

/*
 * Parses one bounded USB receive record and its applicable metadata.
 */
int
drv_rtl8822b_rx_packet_parse(
	const uint8_t *bytes,
	size_t length,
	struct rtl8822b_rx_packet *packet)
{
	struct rtl8822b_rx_packet result;
	uint32_t word0, word1, word2, word3, word4;
	size_t packet_length, driver_info_length, shift, payload_offset;
	size_t occupied, aligned;
	int32_t power_a, power_b;
	uint8_t page;
	int is_c2h;

	/* Refuses missing storage before clearing the output record. */
	if (bytes == NULL || packet == NULL)
		return EINVAL;
	memset(packet, 0, sizeof(*packet));

	/* Requires the complete descriptor before decoding its layout. */
	if (length < RTL8822B_RX_DESCRIPTOR_SIZE)
		return EINVAL;
	word0 = load_le32(bytes);
	word1 = load_le32(bytes + 4U);
	word2 = load_le32(bytes + 8U);
	word3 = load_le32(bytes + 12U);
	word4 = load_le32(bytes + 16U);
	is_c2h = (word2 & RTL8822B_RX_C2H_FLAG) != 0U;
	packet_length = word0 & RTL8822B_RX_PKT_LENGTH_MASK;
	driver_info_length = ((word0 >> RTL8822B_RX_DRV_INFO_SHIFT) &
			      RTL8822B_RX_DRV_INFO_MASK) *
			     8U;
	shift = (word0 >> RTL8822B_RX_SHIFT_SHIFT) & RTL8822B_RX_SHIFT_MASK;

	/* Requires a nonempty payload for both frames and firmware messages. */
	if (packet_length == 0U)
		return EINVAL;

	/* Applies over-the-air receive semantics only to ordinary frames. */
	if (!is_c2h) {
		/* Retains the supported frame, rate and PHY layout checks. */
		if (packet_length > RTL8822B_RX_MPDU_MAX ||
		    (word0 & RTL8822B_RX_CRC_ERROR) != 0U ||
		    (driver_info_length != 0U &&
		     driver_info_length != RTL8822B_RX_PHY_INFO_SIZE) ||
		    ((word0 & RTL8822B_RX_PHY_STATUS) != 0U &&
		     driver_info_length == 0U) ||
		    (word3 & RTL8822B_RX_RATE_MASK) >= RTL8822B_RX_RATE_MAX) {
			/* Failed. */
			return EINVAL;
		}
	}

	/* Bounds descriptor-provided offsets even for firmware messages. */
	if (shift > SIZE_MAX - RTL8822B_RX_DESCRIPTOR_SIZE ||
	    driver_info_length > SIZE_MAX - RTL8822B_RX_DESCRIPTOR_SIZE - shift) {
		/* Failed. */
		return EOVERFLOW;
	}
	payload_offset =
		RTL8822B_RX_DESCRIPTOR_SIZE + shift + driver_info_length;

	/* Keeps the entire payload inside the received transfer. */
	if (payload_offset > length || packet_length > length - payload_offset)
		return EINVAL;

	/* Checks the aggregate alignment before rounding the record length. */
	occupied = payload_offset + packet_length;
	if (occupied > SIZE_MAX - 7U)
		return EOVERFLOW;

	/* Allows an unpadded final record only at the exact transfer end. */
	aligned = (occupied + 7U) & ~(size_t)7U;
	if (aligned > length) {
		/* Rejects a partial trailing alignment region. */
		if (occupied != length)
			return EINVAL;
		aligned = occupied;
	}

	/* Initializes shared layout fields and absent receive metadata. */
	memset(&result, 0, sizeof(result));
	result.aggregate_length = aligned;
	result.payload = bytes + payload_offset;
	result.rssi_dbm = -128;

	/* Firmware notifications have no meaningful frame or PHY status. */
	if (is_c2h) {
		/*
		 * Requires the command identifier and sequence before
		 * publication.
		 */
		if (packet_length < 2U)
			return EINVAL;
		result.kind = RTL8822B_RX_C2H;
		result.payload_length = packet_length;
		result.c2h_id = result.payload[0];
		result.c2h_sequence = result.payload[1];
		*packet = result;

		/*
		 * Publishes only the bounded command and its transport layout.
		 */
		return 0;
	}

	/* Decodes metadata belonging to an ordinary received frame. */
	result.rate = (uint8_t)(word3 & RTL8822B_RX_RATE_MASK);
	result.bandwidth = (uint8_t)((word4 >> RTL8822B_RX_BANDWIDTH_SHIFT) &
				     RTL8822B_RX_BANDWIDTH_MASK);
	result.tsf_low = load_le32(bytes + 20U);
	result.encryption_type =
		(uint8_t)((word0 >> RTL8822B_RX_ENCRYPTION_SHIFT) &
			  RTL8822B_RX_ENCRYPTION_MASK);
	result.software_decrypted =
		(word0 & RTL8822B_RX_SOFTWARE_DECRYPT) != 0U;
	result.mac_id = (uint8_t)(word1 & RTL8822B_RX_MAC_ID_MASK);
	result.icv_error = (word0 & RTL8822B_RX_ICV_ERROR) != 0U;

	/*
	 * Interprets the validated PHY record only when the frame carries it.
	 */
	if ((word0 & RTL8822B_RX_PHY_STATUS) != 0U) {
		result.phy_info = bytes + RTL8822B_RX_DESCRIPTOR_SIZE + shift;
		result.phy_info_length = driver_info_length;
		page = result.phy_info[0] & 0x0fU;

		/*
		 * Refuses unsupported PHY pages before reading their power
		 * fields.
		 */
		if (page != 0U && page != 1U)
			return EINVAL;
		power_a = (int32_t)result.phy_info[1] - 110;
		result.rssi_dbm = clamp_rssi(power_a);

		/*
		 * Selects the stronger path when the PHY page reports both
		 * paths.
		 */
		if (page == 1U) {
			power_b = (int32_t)result.phy_info[2] - 110;

			/*
			 * Preserves the first path when the second path is
			 * weaker.
			 */
			if (power_b > result.rssi_dbm)
				result.rssi_dbm = clamp_rssi(power_b);
		}
	}

	/* Excludes the trailing FCS from a nonempty received frame. */
	if (packet_length <= RTL8822B_RX_FCS_SIZE)
		return EINVAL;
	result.kind = RTL8822B_RX_FRAME;
	result.payload_length = packet_length - RTL8822B_RX_FCS_SIZE;
	*packet = result;

	/*
	 * Publishes the validated frame with its applicable receive metadata.
	 */
	return 0;
}

/*
 * Walks the frames the device packed into one transfer.
 */
int
drv_rtl8822b_rx_aggregate_walk(
	const uint8_t *bytes,
	size_t length,
	rtl8822b_rx_packet_fn callback,
	void *context,
	size_t *packet_count)
{
	struct rtl8822b_rx_packet packet_local;
	int error_local;
	struct rtl8822b_rx_packet packet_local1;
	int error_local2;
	size_t offset = 0;
	size_t count = 0;

	/* Handles the bytes availability. */
	if (bytes == NULL || callback == NULL || packet_count == NULL ||
	    length == 0U || length > RTL8822B_RX_AGGREGATE_MAX) {
		/* Failed. */
		return EINVAL;
	}
	*packet_count = 0;
	/* Process each remaining element. */
	while (offset < length) {
		/* Checks the operation status. */
		error_local = drv_rtl8822b_rx_packet_parse(
			bytes + offset, length - offset, &packet_local);
		if (error_local != 0)
			return error_local;

		/* Handles the packet local condition. */
		if (packet_local.aggregate_length == 0U ||
		    packet_local.aggregate_length > length - offset) {
			/* Failed. */
			return EINVAL;
		}
		offset += packet_local.aggregate_length;
	}

	offset = 0;
	/* Process each remaining element. */
	while (offset < length) {
		/* Checks the operation status. */
		error_local2 = drv_rtl8822b_rx_packet_parse(
			bytes + offset, length - offset, &packet_local1);
		if (error_local2 != 0)
			return error_local2;

		/* Handles the packet local1 condition. */
		if (packet_local1.aggregate_length == 0U ||
		    packet_local1.aggregate_length > length - offset) {
			/* Failed. */
			return EINVAL;
		}

		/* Checks the operation status. */
		error_local2 = callback(context, &packet_local1);
		if (error_local2 != 0)
			return error_local2;
		offset += packet_local1.aggregate_length;
		count++;
		*packet_count = count;
	}

	/* Succeeded. */
	return 0;
}

enum rtl8822b_power_operation {
	RTL8822B_POWER_WRITE = 1,
	RTL8822B_POWER_POLL = 2,
	RTL8822B_POWER_DELAY = 3
};

struct rtl8822b_power_command {
	uint16_t address;
	uint16_t delay_microseconds;
	uint8_t cut_mask;
	uint8_t operation;
	uint8_t mask;
	uint8_t value;
};

#define RTL8822B_CUT_ALL 0xffU
#define RTL8822B_CUT_C 0x08U
#define RTL8822B_PWR_WRITE(address, cut, mask, value)                          \
	{(address), 0U, (cut), RTL8822B_POWER_WRITE, (mask), (value)}
#define RTL8822B_PWR_POLL(address, cut, mask, value)                           \
	{(address), 0U, (cut), RTL8822B_POWER_POLL, (mask), (value)}
#define RTL8822B_PWR_DELAY(microseconds)                                       \
	{0U, (microseconds), RTL8822B_CUT_ALL, RTL8822B_POWER_DELAY, 0U, 0U}

/* USB-only projection of the RTL8822B card-disable -> active sequence. */
static const struct rtl8822b_power_command rtl8822b_power_enable[] = {
	RTL8822B_PWR_WRITE(0x004aU, RTL8822B_CUT_ALL, 0x01U, 0x00U),
	RTL8822B_PWR_WRITE(0x0005U, RTL8822B_CUT_ALL, 0x98U, 0x00U),
	RTL8822B_PWR_WRITE(0x0012U, RTL8822B_CUT_ALL, 0x02U, 0x00U),
	RTL8822B_PWR_WRITE(0x0012U, RTL8822B_CUT_ALL, 0x01U, 0x01U),
	RTL8822B_PWR_WRITE(0x0020U, RTL8822B_CUT_ALL, 0x01U, 0x01U),
	RTL8822B_PWR_DELAY(1000U),
	RTL8822B_PWR_WRITE(0x0000U, RTL8822B_CUT_ALL, 0x20U, 0x00U),
	RTL8822B_PWR_WRITE(0x0005U, RTL8822B_CUT_ALL, 0x1cU, 0x00U),
	RTL8822B_PWR_POLL(0x0006U, RTL8822B_CUT_ALL, 0x02U, 0x02U),
	RTL8822B_PWR_WRITE(0xff1aU, RTL8822B_CUT_ALL, 0xffU, 0x00U),
	RTL8822B_PWR_WRITE(0x0006U, RTL8822B_CUT_ALL, 0x01U, 0x01U),
	RTL8822B_PWR_WRITE(0x0005U, RTL8822B_CUT_ALL, 0x80U, 0x00U),
	RTL8822B_PWR_WRITE(0x0005U, RTL8822B_CUT_ALL, 0x18U, 0x00U),
	RTL8822B_PWR_WRITE(0x10c3U, RTL8822B_CUT_ALL, 0x01U, 0x01U),
	RTL8822B_PWR_WRITE(0x0005U, RTL8822B_CUT_ALL, 0x01U, 0x01U),
	RTL8822B_PWR_POLL(0x0005U, RTL8822B_CUT_ALL, 0x01U, 0x00U),
	RTL8822B_PWR_WRITE(0x0020U, RTL8822B_CUT_ALL, 0x08U, 0x08U),
	RTL8822B_PWR_WRITE(0x10a8U, RTL8822B_CUT_C, 0xffU, 0x00U),
	RTL8822B_PWR_WRITE(0x10a9U, RTL8822B_CUT_C, 0xffU, 0xefU),
	RTL8822B_PWR_WRITE(0x10aaU, RTL8822B_CUT_C, 0xffU, 0x0cU),
	RTL8822B_PWR_WRITE(0x0029U, RTL8822B_CUT_ALL, 0xffU, 0xf9U),
	RTL8822B_PWR_WRITE(0x0024U, RTL8822B_CUT_ALL, 0x04U, 0x00U),
	RTL8822B_PWR_WRITE(0x00afU, RTL8822B_CUT_ALL, 0x20U, 0x20U)};

/* USB-only projection of active -> card-disable. */
static const struct rtl8822b_power_command rtl8822b_power_disable[] = {
	RTL8822B_PWR_WRITE(0x0093U, RTL8822B_CUT_ALL, 0x08U, 0x00U),
	RTL8822B_PWR_WRITE(0x001fU, RTL8822B_CUT_ALL, 0xffU, 0x00U),
	RTL8822B_PWR_WRITE(0x00efU, RTL8822B_CUT_ALL, 0xffU, 0x00U),
	RTL8822B_PWR_WRITE(0xff1aU, RTL8822B_CUT_ALL, 0xffU, 0x30U),
	RTL8822B_PWR_WRITE(0x0049U, RTL8822B_CUT_ALL, 0x02U, 0x00U),
	RTL8822B_PWR_WRITE(0x0006U, RTL8822B_CUT_ALL, 0x01U, 0x01U),
	RTL8822B_PWR_WRITE(0x0002U, RTL8822B_CUT_ALL, 0x02U, 0x00U),
	RTL8822B_PWR_WRITE(0x10c3U, RTL8822B_CUT_ALL, 0x01U, 0x00U),
	RTL8822B_PWR_WRITE(0x0005U, RTL8822B_CUT_ALL, 0x02U, 0x02U),
	RTL8822B_PWR_POLL(0x0005U, RTL8822B_CUT_ALL, 0x02U, 0x00U),
	RTL8822B_PWR_WRITE(0x0020U, RTL8822B_CUT_ALL, 0x08U, 0x00U),
	RTL8822B_PWR_WRITE(0x0000U, RTL8822B_CUT_ALL, 0x20U, 0x20U),
	RTL8822B_PWR_WRITE(0x0007U, RTL8822B_CUT_ALL, 0xffU, 0x20U),
	RTL8822B_PWR_WRITE(0x0067U, RTL8822B_CUT_ALL, 0x20U, 0x00U),
	RTL8822B_PWR_WRITE(0x004aU, RTL8822B_CUT_ALL, 0x01U, 0x00U),
	RTL8822B_PWR_WRITE(0x0081U, RTL8822B_CUT_ALL, 0xc0U, 0x00U),
	RTL8822B_PWR_WRITE(0x0005U, RTL8822B_CUT_ALL, 0x18U, 0x08U),
	RTL8822B_PWR_WRITE(0x0090U, RTL8822B_CUT_ALL, 0x02U, 0x00U)};

static int radio_error(int error);

/* Records the first failure a radio sequence met. */
static int
radio_error(
	int error)
{
	/* Returns the computed result. */
	return error < 0 ? EIO : error;
}

static int radio_deadline_check(const struct rtl8822b_radio *radio, uint64_t deadline_ticks);

/* Refuses to keep waiting past a sequence's deadline. */
static int
radio_deadline_check(
	const struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	int error;

	/* Handles the radio availability. */
	if (radio == NULL || radio->transport.now_ticks == NULL)
		return EINVAL;

	/* Computes the function result. */
	error = radio->transport.now_ticks(
				  radio->transport.context) >= deadline_ticks
				  ? ETIMEDOUT
				  : 0;

	/* Returns the computed result. */
	return error;
}

static int radio_read(struct rtl8822b_radio *radio, uint16_t address, unsigned width, uint32_t *value, uint64_t deadline_ticks);

/* Reads one radio register. */
static int
radio_read(
	struct rtl8822b_radio *radio,
	uint16_t address,
	unsigned width,
	uint32_t *value,
	uint64_t deadline_ticks)
{
	int function_result;
	int error;

	/* Handles the radio availability. */
	if (radio == NULL || value == NULL || radio->transport.read == NULL ||
	    (width != 1U && width != 2U && width != 4U)) {
		/* Failed. */
		return EINVAL;
	}

	/* Checks the operation status. */
	error = radio_deadline_check(radio, deadline_ticks);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = radio_error(radio->transport.read(radio->transport.context,
						  address, width, value,
						  deadline_ticks));
	if (error != 0)
		return error;

	/* Handles the width condition. */
	if (width == 1U)
		*value &= 0xffU;
	else if (width == 2U)
		*value &= 0xffffU;
	/* Obtains the radio deadline check result. */
	function_result = radio_deadline_check(radio, deadline_ticks);

	/* Returns the computed result. */
	return function_result;
}

static int radio_write(struct rtl8822b_radio *radio, uint16_t address, unsigned width, uint32_t value, uint64_t deadline_ticks);

/* Writes one radio register. */
static int
radio_write(
	struct rtl8822b_radio *radio,
	uint16_t address,
	unsigned width,
	uint32_t value,
	uint64_t deadline_ticks)
{
	int function_result;
	int error;

	/* Handles the radio availability. */
	if (radio == NULL || radio->transport.write == NULL ||
	    (width != 1U && width != 2U && width != 4U)) {
		/* Failed. */
		return EINVAL;
	}

	/* Checks the operation status. */
	error = radio_deadline_check(radio, deadline_ticks);
	if (error != 0)
		return error;

	/* Handles the width condition. */
	if (width == 1U)
		value &= 0xffU;
	else if (width == 2U)
		value &= 0xffffU;

	/* Checks the operation status. */
	error = radio_error(radio->transport.write(radio->transport.context,
						   address, width, value,
						   deadline_ticks));
	if (error != 0)
		return error;

	/* Obtains the radio deadline check result. */
	function_result = radio_deadline_check(radio, deadline_ticks);

	/* Returns the computed result. */
	return function_result;
}

static int radio_delay(struct rtl8822b_radio *radio, uint32_t microseconds, uint64_t deadline_ticks);

/* Waits the moment a radio step needs. */
static int
radio_delay(
	struct rtl8822b_radio *radio,
	uint32_t microseconds,
	uint64_t deadline_ticks)
{
	int function_result;
	int error;

	/* Handles the radio availability. */
	if (radio == NULL || radio->transport.delay_us == NULL)
		return EINVAL;

	/* Checks the operation status. */
	error = radio_deadline_check(radio, deadline_ticks);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = radio_error(radio->transport.delay_us(
		radio->transport.context, microseconds, deadline_ticks));
	if (error != 0)
		return error;

	/* Obtains the radio deadline check result. */
	function_result = radio_deadline_check(radio, deadline_ticks);

	/* Returns the computed result. */
	return function_result;
}

static int radio_update(struct rtl8822b_radio *radio, uint16_t address, unsigned width, uint32_t mask, uint32_t value, uint64_t deadline_ticks);

/* Changes selected bits of one radio register. */
static int
radio_update(
	struct rtl8822b_radio *radio,
	uint16_t address,
	unsigned width,
	uint32_t mask,
	uint32_t value,
	uint64_t deadline_ticks)
{
	int function_result;
	uint32_t old;
	int error;

	/* Checks the operation status. */
	error = radio_read(radio, address, width, &old, deadline_ticks);
	if (error != 0)
		return error;

	/* Obtains the radio write result. */
	function_result =
		radio_write(radio, address, width,
			    (old & ~mask) | (value & mask), deadline_ticks);

	/* Returns the computed result. */
	return function_result;
}

static int radio_power_commands(struct rtl8822b_radio *radio, const struct rtl8822b_power_command *commands, size_t count, uint64_t deadline_ticks);

/* Reports the power sequence for one direction. */
static int
radio_power_commands(
	struct rtl8822b_radio *radio,
	const struct rtl8822b_power_command *commands,
	size_t count,
	uint64_t deadline_ticks)
{
	const struct rtl8822b_power_command *command;
	uint32_t value;
	unsigned attempt;
	int error;
	uint8_t cut_mask;
	size_t index;

	/* Handles the radio condition. */
	if (radio->board.chip.cut > RTL8822B_CUT_G)
		return EOPNOTSUPP;
	cut_mask = (uint8_t)(1U << (radio->board.chip.cut + 1U));
	/* Process each remaining element. */
	for (index = 0; index < count; index++) {
		/* Handles the command condition. */
		command = &commands[index];
		if ((command->cut_mask & cut_mask) == 0U)
			continue;

		/* Checks the operation status. */
		error = radio_deadline_check(radio, deadline_ticks);
		if (error != 0)
			return error;
		/* Dispatch the selected operation case. */
		switch (command->operation) {
		case RTL8822B_POWER_WRITE:
			error = radio_update(radio, command->address, 1U,
					     command->mask, command->value,
					     deadline_ticks);
			break;
		case RTL8822B_POWER_DELAY:
			error = radio_delay(radio, command->delay_microseconds,
					    deadline_ticks);
			break;
		case RTL8822B_POWER_POLL:
			error = ETIMEDOUT;
			/* Process each element required by the operation. */
			for (attempt = 0; attempt < RTL8822B_POWER_POLL_MAX;
			     attempt++) {
				/* Checks the operation status. */
				error = radio_read(radio, command->address, 1U,
						   &value, deadline_ticks);
				if (error != 0)
					break;

				/* Validates the current value. */
				if ((value & command->mask) ==
				    (command->value & command->mask)) {
					error = 0;
					break;
				}

				/* Checks the operation status. */
				error = radio_delay(
					radio, RTL8822B_POWER_POLL_DELAY_US,
					deadline_ticks);
				if (error != 0)
					break;

				/* Handles the yield availability. */
				if (radio->transport.yield != NULL) {
					radio->transport.yield(
						radio->transport.context);
				}
			}

			break;
		default:
			/* Failed. */
			return EINVAL;
		}

		/* Checks the operation status. */
		if (error != 0)
			return error;
	}

	/* Succeeded. */
	return 0;
}

static int radio_power_state_is_on(struct rtl8822b_radio *radio, int *powered, uint64_t deadline_ticks);

/* Asks whether the radio is already powered. */
static int
radio_power_state_is_on(
	struct rtl8822b_radio *radio,
	int *powered,
	uint64_t deadline_ticks)
{
	uint32_t control;
	uint32_t status;
	int error;

	/* Handles the powered availability. */
	if (powered == NULL)
		return EINVAL;
	*powered = 0;

	/* Checks the operation status. */
	error = radio_read(radio, RTL8822B_REG_CR, 1U, &control,
			   deadline_ticks);
	if (error == 0) {
		error = radio_read(radio, RTL8822B_REG_SYS_STATUS1 + 1U, 1U,
				   &status, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0)
		*powered = control != 0xeaU && (status & 0x01U) == 0U;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static int radio_warm_firmware_ack(struct rtl8822b_radio *radio, uint64_t deadline_ticks);

/* A USB RTL8822B may survive a host-side rebind with its WCPU and firmware still running.  Before the checked card-disable sequence touches that warm instance, send the reviewed RPWM acknowledgement request and wait for firmware to toggle CPWM.  A missing acknowledgement does not authorize reuse: power_on() still performs the full disable/enable transaction and the caller downloads a fresh, pinned firmware image afterwards. Fully powered-off and powered-but-without-firmware states deliberately skip RPWM.  Those states have no firmware peer which could acknowledge it. */
static int
radio_warm_firmware_ack(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	uint32_t firmware;
	uint32_t request;
	uint32_t confirm;
	uint32_t polling;
	unsigned attempt;
	int error;

	/* Checks the operation status. */
	error = radio_read(radio, RTL8822B_REG_MCUFW_CTRL, 2U, &firmware,
			   deadline_ticks);
	if (error != 0 || (firmware & RTL8822B_MCUFW_INIT_READY) == 0U)
		return error;

	/* Checks the operation status. */
	error = radio_read(radio, RTL8822B_REG_USB_RPWM, 1U, &request,
			   deadline_ticks);
	if (error == 0) {
		error = radio_read(radio, RTL8822B_REG_USB_CPWM, 1U, &confirm,
				   deadline_ticks);
	}

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Clear every mode bit, invert only the toggle, and request an ACK. */
	request = ((request & RTL8822B_RPWM_TOGGLE) ^ RTL8822B_RPWM_TOGGLE) |
		  RTL8822B_RPWM_ACK;

	/* Checks the operation status. */
	error = radio_write(radio, RTL8822B_REG_USB_RPWM, 1U, request,
			    deadline_ticks);
	if (error != 0)
		return error;

	/*
	 * now_ticks is expressed in kernel ticks while delay_us is explicitly
	 * microseconds.  Bound this secondary wait by count, never by adding
	 * microseconds to an opaque tick value.
	 */
	for (attempt = 0U; attempt < RTL8822B_RPWM_ACK_POLL_MAX; attempt++) {
		/* Checks the operation status. */
		error = radio_read(radio, RTL8822B_REG_USB_CPWM, 1U, &polling,
				   deadline_ticks);
		if (error != 0)
			return error;

		/* Handles the polling condition. */
		if (((polling ^ confirm) & RTL8822B_RPWM_TOGGLE) != 0U)
			return 0;

		/* Checks the operation status. */
		error = radio_delay(radio, RTL8822B_RPWM_ACK_POLL_US,
				    deadline_ticks);
		if (error != 0)
			return error;

		/* Handles the yield availability. */
		if (radio->transport.yield != NULL)
			radio->transport.yield(radio->transport.context);
	}

	/* Failed. */
	return ETIMEDOUT;
}

static int radio_condition_matches(const struct rtl8822b_radio *radio, uint32_t word);

/* Asks whether one power step applies to this chip revision. */
static int
radio_condition_matches(
	const struct rtl8822b_radio *radio,
	uint32_t word)
{
	uint8_t cut = (uint8_t)((word >> 24) & 0x0fU);
	uint8_t platform = (uint8_t)((word >> 16) & 0x0fU);
	uint8_t package = (uint8_t)((word >> 12) & 0x0fU);
	uint8_t interface = (uint8_t)((word >> 8) & 0x0fU);
	uint8_t rfe = (uint8_t)(word & 0xffU);
	uint8_t actual_cut =
		radio->board.chip.cut != 0U ? radio->board.chip.cut : 15U;

	/* Handles the cut condition. */
	if (cut != 0U && cut != actual_cut)
		return 0;

	/* Package is not encoded by the current bounded board contract. */
	if (package != 0U && package != 15U)
		return 0;

	/* Handles the platform condition. */
	if (platform != 0U && platform != 4U)
		return 0;

	/* Handles the interface condition. */
	if (interface != 0U && interface != 2U)
		return 0;

	/* Returns the computed result. */
	return rfe == radio->board.rfe_option;
}

static int radio_rf_write(struct rtl8822b_radio *radio, uint8_t path, uint16_t rf_address, uint32_t value, uint64_t deadline_ticks);

/* Writes one register of the radio front end. */
static int
radio_rf_write(
	struct rtl8822b_radio *radio,
	uint8_t path,
	uint16_t rf_address,
	uint32_t value,
	uint64_t deadline_ticks)
{
	int function_result;
	uint16_t sipi;
	int error;

	/* Handles the path condition. */
	if (path >= radio->board.chip.rf_path_count || path > 1U ||
	    rf_address > 0xffU) {
		/* Failed. */
		return EINVAL;
	}
	sipi = path == 0U ? RTL8822B_RF_SIPI_A : RTL8822B_RF_SIPI_B;

	/* Checks the operation status. */
	error = radio_write(radio, sipi, 4U,
			    ((uint32_t)rf_address << 20) |
				    (value & RTL8822B_RF_VALUE_MASK),
			    deadline_ticks);
	if (error != 0)
		return error;

	/* Obtains the radio delay result. */
	function_result = radio_delay(radio, 13U, deadline_ticks);

	/* Returns the computed result. */
	return function_result;
}

static int radio_table_write(struct rtl8822b_radio *radio, uint8_t domain, uint8_t width, uint8_t rf_path, uint32_t address, uint32_t value, uint64_t deadline_ticks);

/* Writes one entry of a radio initialization table. */
static int
radio_table_write(
	struct rtl8822b_radio *radio,
	uint8_t domain,
	uint8_t width,
	uint8_t rf_path,
	uint32_t address,
	uint32_t value,
	uint64_t deadline_ticks)
{
	int function_result;
	uint32_t delay = 0U;
	int error;

	/* Handles the domain condition. */
	if (domain == RTL8822B_TABLE_DOMAIN_BB) {
		/* Dispatch the selected operation case. */
		switch (address) {
		case 0xfeU:
			delay = 50000U;
			break;
		case 0xfdU:
			delay = 5000U;
			break;
		case 0xfcU:
			delay = 1000U;
			break;
		case 0xfbU:
			delay = 50U;
			break;
		case 0xfaU:
			delay = 5U;
			break;
		case 0xf9U:
			delay = 1U;
			break;
		default:
			break;
		}

		/* Handles the delay condition. */
		if (delay != 0U) {
			/* Obtains the radio delay result. */
			function_result =
				radio_delay(radio, delay, deadline_ticks);

			/* Returns the computed result. */
			return function_result;
		}
	} else if (domain == RTL8822B_TABLE_DOMAIN_RF) {
		/* Handles the address condition. */
		if (address == 0xffeU) {
			/* Obtains the radio delay result. */
			function_result =
				radio_delay(radio, 50000U, deadline_ticks);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the address condition. */
		if (address == 0xfeU) {
			/* Obtains the radio delay result. */
			function_result =
				radio_delay(radio, 100U, deadline_ticks);

			/* Returns the computed result. */
			return function_result;
		}

		/* Handles the width condition. */
		if (width != RTL8822B_TABLE_WIDTH_RF20)
			return EINVAL;

		/* Checks the operation status. */
		error = radio_rf_write(radio, rf_path, (uint16_t)address, value,
				       deadline_ticks);
		if (error != 0)
			return error;

		/* Obtains the radio delay result. */
		function_result = radio_delay(radio, 1U, deadline_ticks);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the address condition. */
	if (address > UINT16_MAX)
		return EOVERFLOW;

	/* Handles the domain condition. */
	if (domain == RTL8822B_TABLE_DOMAIN_MAC &&
	    width == RTL8822B_TABLE_WIDTH_8) {
		/* Obtains the radio write result. */
		function_result = radio_write(radio, (uint16_t)address, 1U,
					      value, deadline_ticks);

		/* Returns the computed result. */
		return function_result;
	}

	/* Handles the domain condition. */
	if ((domain == RTL8822B_TABLE_DOMAIN_AGC ||
	     domain == RTL8822B_TABLE_DOMAIN_BB) &&
	    width == RTL8822B_TABLE_WIDTH_32) {
		/* Obtains the radio write result. */
		function_result = radio_write(radio, (uint16_t)address, 4U,
					      value, deadline_ticks);

		/* Returns the computed result. */
		return function_result;
	}

	/* Failed. */
	return EINVAL;
}

static int radio_table_apply(struct rtl8822b_radio *radio, uint8_t domain, uint8_t width, uint8_t rf_path, const uint32_t *words, size_t word_count, uint64_t deadline_ticks);

/* Writes a whole radio initialization table. */
static int
radio_table_apply(
	struct rtl8822b_radio *radio,
	uint8_t domain,
	uint8_t width,
	uint8_t rf_path,
	const uint32_t *words,
	size_t word_count,
	uint64_t deadline_ticks)
{
	uint32_t address;
	uint32_t value;
	uint8_t branch;
	int error;
	uint32_t condition = 0U;
	int in_branch = 0;
	int awaiting_condition = 0;
	int matched = 1;
	int skipped = 0;
	size_t index;

	/* Handles the radio availability. */
	if (radio == NULL || words == NULL || word_count == 0U ||
	    (word_count & 1U) != 0U) {
		/* Failed. */
		return EINVAL;
	}

	/* Handles the domain condition. */
	if (domain == RTL8822B_TABLE_DOMAIN_RF &&
	    rf_path >= radio->board.chip.rf_path_count) {
		/* Succeeded. */
		return 0;
	}
	/* Process each remaining element. */
	for (index = 0; index < word_count; index += 2U) {
		address = words[index];
		value = words[index + 1U];

		/* Checks the operation status. */
		error = radio_deadline_check(radio, deadline_ticks);
		if (error != 0)
			return error;

		/* Handles the address condition. */
		if ((address & 0x80000000U) != 0U) {
			/* Handles the address condition. */
			if ((address & 0x40000000U) != 0U)
				return EINVAL;

			/* Handles the branch condition. */
			branch = (uint8_t)((address >> 28) & 3U);
			if (branch == 3U) {
				/* Handles the in branch condition. */
				if (!in_branch || awaiting_condition)
					return EINVAL;
				in_branch = 0;
				matched = 1;
				skipped = 0;
			} else if (branch == 2U) {
				/* Handles the in branch condition. */
				if (!in_branch || awaiting_condition)
					return EINVAL;
				matched = !skipped;
			} else {
				/* Handles the branch condition. */
				if ((branch == 0U && in_branch) ||
				    (branch == 1U &&
				     (!in_branch || awaiting_condition))) {
					/* Failed. */
					return EINVAL;
				}

				/* Handles the branch condition. */
				if (branch == 0U) {
					in_branch = 1;
					skipped = 0;
				}

				condition = address;
				(void)value; /* cond2 is unused by RTL8822B. */
				awaiting_condition = 1;
			}

			continue;
		}

		/* Handles the address condition. */
		if ((address & 0x40000000U) != 0U) {
			/* Handles the in branch condition. */
			if (!in_branch || !awaiting_condition)
				return EINVAL;

			/* Checks the radio condition matches result. */
			if (!skipped &&
			    radio_condition_matches(radio, condition)) {
				matched = 1;
				skipped = 1;
			} else {
				matched = 0;
			}

			awaiting_condition = 0;
			continue;
		}

		/* Handles the awaiting condition condition. */
		if (awaiting_condition)
			return EINVAL;

		/* Handles the matched condition. */
		if (!matched)
			continue;

		/* Checks the operation status. */
		error = radio_table_write(radio, domain, width, rf_path,
					  address, value, deadline_ticks);
		if (error != 0)
			return error;
	}

	/* Returns the computed result. */
	return in_branch || awaiting_condition ? EINVAL : 0;
}

#ifdef RTL8822B_TESTING
/*
 * Exposes that to the host test harness.
 */
int
drv_rtl8822b_test_radio_table_apply(
	struct rtl8822b_radio *radio,
	uint8_t domain,
	uint8_t width,
	uint8_t rf_path,
	const uint32_t *words,
	size_t word_count,
	uint64_t deadline_ticks)
{
	int error;

	/* Obtains the radio table apply result. */
	error = radio_table_apply(radio, domain, width, rf_path,
					    words, word_count, deadline_ticks);

	/* Returns the computed result. */
	return error;
}
#endif

#define RTL8822B_CHANNEL_JOURNAL_MAX 64U

enum rtl8822b_journal_kind {
	RTL8822B_JOURNAL_REGISTER = 1,
	RTL8822B_JOURNAL_RF = 2
};

struct rtl8822b_journal_entry {
	uint32_t value;
	uint16_t address;
	uint8_t width;
	uint8_t kind;
	uint8_t rf_path;
};

struct rtl8822b_journal {
	struct rtl8822b_journal_entry entries[RTL8822B_CHANNEL_JOURNAL_MAX];
	size_t count;
};

static unsigned mask_shift(uint32_t mask);

/* Reports how far a field mask is shifted. */
static unsigned
mask_shift(
	uint32_t mask)
{
	unsigned shift = 0U;

	/* Continue while the operation condition remains true. */
	while ((mask & 1U) == 0U) {
		mask >>= 1;
		shift++;
	}

	/* Returns the computed result. */
	return shift;
}

static uint32_t mask_value(uint32_t mask, uint32_t value);

/* Renders a value as the bits a field mask selects. */
static uint32_t
mask_value(
	uint32_t mask,
	uint32_t value)
{
	uint32_t function_result;

	/* Computes the function result. */
	function_result = (value << mask_shift(mask)) & mask;

	/* Returns the computed result. */
	return function_result;
}

static int journal_add(struct rtl8822b_journal *journal, uint8_t kind, uint16_t address, uint8_t width, uint8_t rf_path, uint32_t value);

/* Records one register write so it can be undone. */
static int
journal_add(
	struct rtl8822b_journal *journal,
	uint8_t kind,
	uint16_t address,
	uint8_t width,
	uint8_t rf_path,
	uint32_t value)
{
	struct rtl8822b_journal_entry *entry;

	/* Handles the journal condition. */
	if (journal->count >= RTL8822B_CHANNEL_JOURNAL_MAX)
		return EOVERFLOW;
	entry = &journal->entries[journal->count++];
	entry->kind = kind;
	entry->address = address;
	entry->width = width;
	entry->rf_path = rf_path;
	entry->value = value;

	/* Succeeded. */
	return 0;
}

static int journal_update(struct rtl8822b_radio *radio, struct rtl8822b_journal *journal, uint16_t address, uint8_t width, uint32_t mask, uint32_t value, uint64_t deadline_ticks);

/* Records one masked register change so it can be undone. */
static int
journal_update(
	struct rtl8822b_radio *radio,
	struct rtl8822b_journal *journal,
	uint16_t address,
	uint8_t width,
	uint32_t mask,
	uint32_t value,
	uint64_t deadline_ticks)
{
	int function_result;
	uint32_t old;
	int error;

	/* Handles the mask condition. */
	if (mask == 0U)
		return EINVAL;

	/* Checks the operation status. */
	error = radio_read(radio, address, width, &old, deadline_ticks);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = journal_add(journal, RTL8822B_JOURNAL_REGISTER, address, width,
			    0U, old);
	if (error != 0)
		return error;

	/* Obtains the radio write result. */
	function_result = radio_write(radio, address, width,
				      (old & ~mask) | mask_value(mask, value),
				      deadline_ticks);

	/* Returns the computed result. */
	return function_result;
}

static int journal_update_phy_paths(struct rtl8822b_radio *radio, struct rtl8822b_journal *journal, uint16_t address, uint32_t mask, uint32_t value, uint64_t deadline_ticks);

/* Records the same change on every physical path. */
static int
journal_update_phy_paths(
	struct rtl8822b_radio *radio,
	struct rtl8822b_journal *journal,
	uint16_t address,
	uint32_t mask,
	uint32_t value,
	uint64_t deadline_ticks)
{
	int error;

	/* Handles the address condition. */
	if (address < 0x0c00U || address > 0x0cffU)
		return EINVAL;

	/* The second RF path mirrors the A-path BB window at +0x200. */
	error = journal_update(radio, journal, address, 4U, mask, value,
			       deadline_ticks);
	if (error == 0 && radio->board.chip.rf_path_count > 1U) {
		error = journal_update(radio, journal,
				       (uint16_t)(address + 0x0200U), 4U, mask,
				       value, deadline_ticks);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static int radio_rf_read(struct rtl8822b_radio *radio, uint8_t path, uint16_t rf_address, uint32_t *value, uint64_t deadline_ticks);

/* Reads one register of the radio front end. */
static int
radio_rf_read(
	struct rtl8822b_radio *radio,
	uint8_t path,
	uint16_t rf_address,
	uint32_t *value,
	uint64_t deadline_ticks)
{
	int error;
	uint32_t direct;

	/* Handles the value availability. */
	if (path >= radio->board.chip.rf_path_count || path > 1U ||
	    rf_address > 0xffU || value == NULL) {
		/* Failed. */
		return EINVAL;
	}

	/* Handles the direct condition. */
	direct = (path == 0U ? RTL8822B_RF_DIRECT_A : RTL8822B_RF_DIRECT_B) +
		 (uint32_t)rf_address * 4U;
	if (direct > UINT16_MAX)
		return EOVERFLOW;

	/* Obtains the radio read result. */
	error =
		radio_read(radio, (uint16_t)direct, 4U, value, deadline_ticks);

	/* Returns the computed result. */
	return error;
}

static int journal_rf_update(struct rtl8822b_radio *radio, struct rtl8822b_journal *journal, uint8_t path, uint16_t address, uint32_t mask, uint32_t value, uint64_t deadline_ticks);

/* Records one front-end change so it can be undone. */
static int
journal_rf_update(
	struct rtl8822b_radio *radio,
	struct rtl8822b_journal *journal,
	uint8_t path,
	uint16_t address,
	uint32_t mask,
	uint32_t value,
	uint64_t deadline_ticks)
{
	int function_result;
	uint32_t old;
	int error;

	/* Handles the mask condition. */
	if (mask == 0U || (mask & ~RTL8822B_RF_VALUE_MASK) != 0U)
		return EINVAL;

	/* Checks the operation status. */
	error = radio_rf_read(radio, path, address, &old, deadline_ticks);
	if (error != 0)
		return error;
	old &= RTL8822B_RF_VALUE_MASK;

	/* Checks the operation status. */
	error = journal_add(journal, RTL8822B_JOURNAL_RF, address, 4U, path,
			    old);
	if (error != 0)
		return error;

	/* Obtains the radio rf write result. */
	function_result = radio_rf_write(
		radio, path, address, (old & ~mask) | mask_value(mask, value),
		deadline_ticks);

	/* Returns the computed result. */
	return function_result;
}

static int journal_rollback(struct rtl8822b_radio *radio, struct rtl8822b_journal *journal);

/* Undoes every recorded change, in reverse. */
static int
journal_rollback(
	struct rtl8822b_radio *radio,
	struct rtl8822b_journal *journal)
{
	uint16_t sipi;
	const struct rtl8822b_journal_entry *entry;
	int error;
	int first_error = 0;

	/*
	 * Give rollback transfers the transport's finite local cleanup budget.
	 */
	while (journal->count != 0U) {
		/* Handles the entry condition. */
		entry = &journal->entries[--journal->count];
		if (entry->kind == RTL8822B_JOURNAL_RF) {
			sipi = entry->rf_path == 0U ? RTL8822B_RF_SIPI_A
						    : RTL8822B_RF_SIPI_B;

			/* Restores a radio register through the indirect interface. */
			error = radio_error(radio->transport.write(
				radio->transport.context, sipi, 4U,
				((uint32_t)entry->address << 20) |
					(entry->value & RTL8822B_RF_VALUE_MASK),
				UINT64_MAX));
		} else {
			error = radio_error(radio->transport.write(
				radio->transport.context, entry->address,
				entry->width, entry->value, UINT64_MAX));
		}

		/* Checks the operation status. */
		if (error != 0 && first_error == 0)
			first_error = error;
	}

	/* Returns the computed result. */
	return first_error;
}

static void radio_emergency_off(struct rtl8822b_radio *radio);

/* Powers the radio down after a sequence has failed. */
static void
radio_emergency_off(
	struct rtl8822b_radio *radio)
{
	uint32_t value;

	/* Handles the radio availability. */
	if (radio == NULL || radio->transport.read == NULL ||
	    radio->transport.write == NULL) {
		/* Returns the computed result. */
		return;
	}

	/*
	 * Stop RF even after the failed operation's absolute deadline has
	 * elapsed.
	 */
	(void)radio->transport.write(radio->transport.context,
				     RTL8822B_REG_TX_PAUSE, 1U, 0xffU,
				     UINT64_MAX);
	(void)radio->transport.write(radio->transport.context, RTL8822B_REG_CR,
				     1U, 0U, UINT64_MAX);

	/* Checks the read result. */
	if (radio->transport.read(radio->transport.context,
				  RTL8822B_REG_SYS_FUNC_EN, 1U, &value,
				  UINT64_MAX) == 0) {
		(void)radio->transport.write(
			radio->transport.context, RTL8822B_REG_SYS_FUNC_EN, 1U,
			value & ~RTL8822B_BB_RESET_BITS, UINT64_MAX);
	}

	/* Checks the read result. */
	if (radio->transport.read(radio->transport.context,
				  RTL8822B_REG_RF_CTRL, 1U, &value,
				  UINT64_MAX) == 0) {
		(void)radio->transport.write(
			radio->transport.context, RTL8822B_REG_RF_CTRL, 1U,
			value & ~RTL8822B_RF_ENABLE_BITS, UINT64_MAX);
	}

	/* Checks the read result. */
	if (radio->transport.read(radio->transport.context, RTL8822B_REG_WLRF1,
				  4U, &value, UINT64_MAX) == 0) {
		(void)radio->transport.write(
			radio->transport.context, RTL8822B_REG_WLRF1, 4U,
			value & ~RTL8822B_WLRF_ENABLE_BITS, UINT64_MAX);
	}
}

static int radio_pre_power(struct rtl8822b_radio *radio, uint64_t deadline_ticks);

/* Prepares the chip before the power sequence runs. */
static int
radio_pre_power(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	int error;

	/* Checks the operation status. */
	error = radio_write(radio, RTL8822B_REG_RSV_CTRL, 1U, 0U,
			    deadline_ticks);
	if (error == 0) {
		error = radio_update(radio, 0x0064U, 4U, 0x30000000U,
				     0x30000000U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_LED_CFG, 4U,
				     0x06000000U, 0U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_GPIO_MUX, 4U,
				     0x00000004U, 0x00000004U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_SYS_FUNC_EN, 1U,
				     RTL8822B_BB_RESET_BITS, 0U,
				     deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_RF_CTRL, 1U,
				     RTL8822B_RF_ENABLE_BITS, 0U,
				     deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_WLRF1, 4U,
				     RTL8822B_WLRF_ENABLE_BITS, 0U,
				     deadline_ticks);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static int radio_post_power(struct rtl8822b_radio *radio, uint64_t deadline_ticks);

/* Finishes the chip's setup after that sequence. */
static int
radio_post_power(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	uint32_t value;
	int error;

	/* Checks the operation status. */
	error = radio_update(radio, RTL8822B_REG_SYS_STATUS1 + 1U, 1U, 0x01U,
			     0U, deadline_ticks);
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_CPU_DMEM_CON, 4U,
				     0x00010100U, 0x00010100U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_SYS_FUNC_EN + 1U, 1U,
				     0xdcU, 0xdcU, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		/* Checks the operation status. */
		error = radio_read(radio, RTL8822B_REG_CR_EXT + 3U, 1U, &value,
				   deadline_ticks);
		if (error == 0) {
			error = radio_write(radio, RTL8822B_REG_CR_EXT + 3U, 1U,
					    (value & 0xf0U) | 0x0cU,
					    deadline_ticks);
		}
	}

	/* Checks the operation status. */
	if (error == 0) {
		/* Checks the operation status. */
		error = radio_read(radio, RTL8822B_REG_MCUFW_CTRL, 4U, &value,
				   deadline_ticks);
		if (error == 0 && (value & 0x00100000U) != 0U) {
			/* Checks the operation status. */
			error = radio_write(radio, RTL8822B_REG_MCUFW_CTRL, 4U,
					    value & ~0x00100000U,
					    deadline_ticks);
			if (error == 0) {
				error = radio_update(
					radio, RTL8822B_REG_GPIO_MUX, 4U,
					0x00080000U, 0U, deadline_ticks);
			}
		}
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static int radio_mac_channel_20(struct rtl8822b_radio *radio, struct rtl8822b_journal *journal, uint8_t channel, uint64_t deadline_ticks);

/* Sets the media access registers for a 20 MHz channel. */
static int
radio_mac_channel_20(
	struct rtl8822b_radio *radio,
	struct rtl8822b_journal *journal,
	uint8_t channel,
	uint64_t deadline_ticks)
{
	int error;

	/* Checks the operation status. */
	error = journal_update(radio, journal, RTL8822B_REG_DATA_SC, 1U, 0xffU,
			       0U, deadline_ticks);
	if (error == 0) {
		error = journal_update(radio, journal,
				       RTL8822B_REG_WMAC_TRX_PROTOCOL, 4U,
				       0x00000180U, 0U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = journal_update(radio, journal, RTL8822B_REG_AFE_CTRL1,
				       4U, 0x00300000U, 0U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = journal_update(radio, journal, RTL8822B_REG_USTIME_TSF,
				       1U, 0xffU, 80U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = journal_update(radio, journal, RTL8822B_REG_USTIME_EDCA,
				       1U, 0xffU, 80U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = journal_update(
			radio, journal, RTL8822B_REG_CCK_CHECK, 1U, 0x80U,
			channel_is_w52(channel) ? 1U : 0U, deadline_ticks);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static int radio_bb_channel_20(struct rtl8822b_radio *radio, struct rtl8822b_journal *journal, uint8_t channel, uint64_t deadline_ticks);

/* Sets the baseband registers for a 20 MHz channel. */
static int
radio_bb_channel_20(
	struct rtl8822b_radio *radio,
	struct rtl8822b_journal *journal,
	uint8_t channel,
	uint64_t deadline_ticks)
{
	uint32_t value;
	int w52 = channel_is_w52(channel);
	int error;

	/* Checks the operation status. */
	error = journal_update(radio, journal, RTL8822B_REG_RX_PATH_SELECT, 4U,
			       0x10000000U, w52 ? 0U : 1U, deadline_ticks);
	if (error == 0) {
		error = journal_update(
			radio, journal, RTL8822B_REG_ENABLE_TX_CCK, 4U,
			0x00040000U, w52 ? 1U : 0U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = journal_update(radio, journal, RTL8822B_REG_RX_CCA_MASK,
				       4U, 0x0000fc00U, w52 ? 34U : 15U,
				       deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = journal_update(
			radio, journal, RTL8822B_REG_ACG_G2_TABLE, 4U,
			0x0000001fU, w52 ? 1U : 0U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = journal_update(radio, journal, RTL8822B_REG_CLOCK_TRACK,
				       4U, 0x1ffe0000U, w52 ? 0x494U : 0x96aU,
				       deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0 && !w52) {
		error = journal_update(radio, journal, RTL8822B_REG_TX_SHAPING2,
				       4U, 0xffffffffU, 0x384f6577U,
				       deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0 && !w52) {
		error = journal_update(radio, journal, RTL8822B_REG_TX_SHAPING6,
				       4U, 0x0000ffffU, 0x1525U,
				       deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = journal_update_phy_paths(
			radio, journal, RTL8822B_REG_RFE_INVERT, 0x00000300U,
			w52 ? 1U : 2U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = radio_read(radio, RTL8822B_REG_ADC_CLOCK, 4U, &value,
			   deadline_ticks);
	if (error != 0)
		return error;

	/* Checks the operation status. */
	error = journal_update(radio, journal, RTL8822B_REG_ADC_CLOCK, 4U,
			       0xffffffffU, (value & 0xffcffc00U),
			       deadline_ticks);
	if (error == 0) {
		error = journal_update(radio, journal, RTL8822B_REG_ADC_160, 4U,
				       0x40000000U, 1U, deadline_ticks);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static int radio_rf_channel_20(struct rtl8822b_radio *radio, struct rtl8822b_journal *journal, uint8_t channel, uint64_t deadline_ticks);

/* Sets the front-end registers for a 20 MHz channel. */
static int
radio_rf_channel_20(
	struct rtl8822b_radio *radio,
	struct rtl8822b_journal *journal,
	uint8_t channel,
	uint64_t deadline_ticks)
{
	uint32_t rf18;
	unsigned path;
	int error;

	/* Checks the operation status. */
	error = radio_rf_read(radio, 0U, 0x18U, &rf18, deadline_ticks);
	if (error != 0)
		return error;
	rf18 &= ~(0x00010000U | 0x00000300U | 0x00060000U | 0x00000c00U |
		  0x000000ffU);
	rf18 |= (uint32_t)channel | 0x00000c00U;

	/* Handles the channel is w52 condition. */
	if (channel_is_w52(channel))
		rf18 |= 0x00010100U;

	/* Checks the operation status. */
	error = journal_rf_update(
		radio, journal, 0U, 0xbeU, 0x00038000U,
		channel_is_w52(channel)
			? rtw8822b_w52_rf_be[(channel - 36U) / 4U]
			: 0U,
		deadline_ticks);
	if (error == 0) {
		error = journal_rf_update(radio, journal, 0U, 0xdfU,
					  0x00040000U, 0U, deadline_ticks);
	}

	/* Process each remaining element. */
	for (path = 0U; error == 0 && path < radio->board.chip.rf_path_count;
	     path++) {
		error = journal_rf_update(radio, journal, (uint8_t)path, 0x18U,
					  RTL8822B_RF_VALUE_MASK, rf18,
					  deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = journal_rf_update(radio, journal, 0U, 0xb8U,
					  0x00080000U, 0U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = journal_rf_update(radio, journal, 0U, 0xb8U,
					  0x00080000U, 1U, deadline_ticks);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static int radio_rxdfir_20(struct rtl8822b_radio *radio, struct rtl8822b_journal *journal, uint64_t deadline_ticks);

/* Sets the receive filter for a 20 MHz channel. */
static int
radio_rxdfir_20(
	struct rtl8822b_radio *radio,
	struct rtl8822b_journal *journal,
	uint64_t deadline_ticks)
{
	int error;

	/* Checks the operation status. */
	error = journal_update(radio, journal, RTL8822B_REG_ACBB0, 4U,
			       0x30000000U, 2U, deadline_ticks);
	if (error == 0) {
		error = journal_update(radio, journal, RTL8822B_REG_ACBB_RX_FIR,
				       4U, 0x30000000U, 2U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = journal_update_phy_paths(
			radio, journal, RTL8822B_REG_TX_DFIR, 0x80000000U, 1U,
			deadline_ticks);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static int radio_toggle_igi(struct rtl8822b_radio *radio, struct rtl8822b_journal *journal, uint64_t deadline_ticks);

/* Nudges the automatic gain control so it re-settles. */
static int
radio_toggle_igi(
	struct rtl8822b_radio *radio,
	struct rtl8822b_journal *journal,
	uint64_t deadline_ticks)
{
	uint32_t igi;
	uint32_t antenna = radio->board.chip.rf_path_count > 1U ? 3U : 1U;
	int error;

	/* Checks the operation status. */
	error = radio_read(radio, RTL8822B_REG_RX_IGI_A, 4U, &igi,
			   deadline_ticks);
	if (error != 0)
		return error;
	igi &= 0x7fU;

	/* Checks the operation status. */
	error = journal_update(radio, journal, RTL8822B_REG_RX_IGI_A, 4U, 0x7fU,
			       igi > 1U ? igi - 2U : 0U, deadline_ticks);
	if (error == 0) {
		error = journal_update(radio, journal, RTL8822B_REG_RX_IGI_A,
				       4U, 0x7fU, igi, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0 && radio->board.chip.rf_path_count > 1U) {
		error = journal_update(radio, journal, RTL8822B_REG_RX_IGI_B,
				       4U, 0x7fU, igi > 1U ? igi - 2U : 0U,
				       deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0 && radio->board.chip.rf_path_count > 1U) {
		error = journal_update(radio, journal, RTL8822B_REG_RX_IGI_B,
				       4U, 0x7fU, igi, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = journal_update(radio, journal,
				       RTL8822B_REG_RX_PATH_SELECT, 4U, 0xffU,
				       0U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = journal_update(
			radio, journal, RTL8822B_REG_RX_PATH_SELECT, 4U, 0xffU,
			antenna | (antenna << 4), deadline_ticks);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static int radio_cca_20(struct rtl8822b_radio *radio, struct rtl8822b_journal *journal, uint8_t channel, uint64_t deadline_ticks);

/* Sets the clear-channel assessment for a 20 MHz channel. */
static int
radio_cca_20(
	struct rtl8822b_radio *radio,
	struct rtl8822b_journal *journal,
	uint8_t channel,
	uint64_t deadline_ticks)
{
	unsigned path;
	uint32_t cca_select;
	uint32_t pd_threshold;
	uint32_t cca_secondary;
	int error;

	/* Handles the channel is w52 condition. */
	if (channel_is_w52(channel)) {
		const uint32_t(*table)[3] = radio->board.rfe_option == 2U
						    ? rtw8822b_w52_cca_type2
						    : rtw8822b_w52_cca_type35;
		path = radio->board.chip.rf_path_count > 1U ? 1U : 0U;

		cca_select = table[path][0];
		pd_threshold = table[path][1];
		cca_secondary = table[path][2];

		/* Handles the radio condition. */
		if (radio->board.rfe_option == 5U && path == 1U)
			pd_threshold = 0x79a0ea28U;
	} else if (radio->board.rfe_option == 2U) {
		cca_select = radio->board.chip.rf_path_count > 1U ? 0x75c97010U
								  : 0x75c97010U;
		pd_threshold = radio->board.chip.rf_path_count > 1U
				       ? 0x79a0eaacU
				       : 0x79a0eaaaU;
		cca_secondary = radio->board.chip.rf_path_count > 1U
					? 0x87746341U
					: 0x87765541U;
	} else {
		cca_select = 0x75da8010U;
		pd_threshold = radio->board.chip.rf_path_count > 1U
				       ? 0x97a0eaacU
				       : 0x79a0eaaaU;
		cca_secondary = radio->board.chip.rf_path_count > 1U
					? 0x86666341U
					: 0x87765541U;
	}

	/* Checks the operation status. */
	error = journal_update(radio, journal, RTL8822B_REG_CCA_SELECT, 4U,
			       0xffffffffU, cca_select, deadline_ticks);
	if (error == 0) {
		error = journal_update(
			radio, journal, RTL8822B_REG_PD_MATCH_THRESHOLD, 4U,
			0xffffffffU, pd_threshold, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = journal_update(
			radio, journal, RTL8822B_REG_CCA_SECONDARY, 4U,
			0xffffffffU, cca_secondary, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0 && channel_is_w52(channel) &&
	    radio->board.rfe_option == 2U && radio->board.chip.cut != 1U) {
		error = journal_update(radio, journal, RTL8822B_REG_L1_WEIGHT,
				       4U, 0xffffffffU, 0x9194b2b9U,
				       deadline_ticks);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static int radio_rfe_channel(struct rtl8822b_radio *radio, struct rtl8822b_journal *journal, uint8_t channel, uint64_t deadline_ticks);

/* Sets the front-end switch for the band a channel is in. */
static int
radio_rfe_channel(
	struct rtl8822b_radio *radio,
	struct rtl8822b_journal *journal,
	uint8_t channel,
	uint64_t deadline_ticks)
{
	int w52 = channel_is_w52(channel);
	uint32_t antenna =
		w52 && radio->board.rfe_option != 2U
			? 0xa5a5U
			: (radio->board.chip.rf_path_count > 1U ? 0xa501U
								: 0xa500U);
	uint32_t select0;
	int error;

	/* Handles the radio condition. */
	if (radio->board.rfe_option == 2U)
		select0 = w52 ? 0x177517U : 0x705770U;
	else if (radio->board.rfe_option == 3U || radio->board.rfe_option == 5U)
		select0 = w52 ? 0x477547U : 0x745774U;
	else {
		/* Failed. */
		return EOPNOTSUPP;
	}

	/* Checks the operation status. */
	error = journal_update_phy_paths(radio, journal,
					 RTL8822B_REG_RFE_SELECT0, 0x00ffffffU,
					 select0, deadline_ticks);
	if (error == 0) {
		error = journal_update_phy_paths(
			radio, journal, RTL8822B_REG_RFE_SELECT8, 0x0000ff00U,
			w52 ? 0x75U : 0x57U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0 && radio->board.rfe_option == 2U) {
		error = journal_update_phy_paths(
			radio, journal, RTL8822B_REG_RFE_CONTROL,
			w52 ? 0x20U : 0x10U, 0U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = journal_update_phy_paths(
			radio, journal, RTL8822B_REG_RFE_INVERT, 0x00000c3fU,
			0U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = journal_update_phy_paths(
			radio, journal, RTL8822B_REG_TR_SWITCH, 0x0000ffffU,
			antenna, deadline_ticks);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Puts the radio on one channel. */
static int
radio_channel_apply(
	struct rtl8822b_radio *radio,
	uint8_t channel,
	int unpause,
	uint64_t deadline_ticks)
{
	struct rtl8822b_journal journal;
	int error;

	/* Checks the drv rtl8822b board active channel allowed result. */
	if (!drv_rtl8822b_board_active_channel_allowed(&radio->board, channel))
		return EOPNOTSUPP;
	radio->power_limits_valid = 0U;
	memset(&journal, 0, sizeof(journal));

	/* Checks the operation status. */
	error = journal_update(radio, &journal, RTL8822B_REG_TX_PAUSE, 1U,
			       0xffU, 0xffU, deadline_ticks);
	if (error == 0) {
		error = radio_bb_channel_20(radio, &journal, channel,
					    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_mac_channel_20(radio, &journal, channel,
					     deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_rf_channel_20(radio, &journal, channel,
					    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0)
		error = radio_rxdfir_20(radio, &journal, deadline_ticks);
	if (error == 0)
		error = radio_toggle_igi(radio, &journal, deadline_ticks);
	if (error == 0)
		error = radio_cca_20(radio, &journal, channel, deadline_ticks);
	if (error == 0) {
		error = radio_rfe_channel(radio, &journal, channel,
					  deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_txagc_legacy_profile(radio, channel, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0 && unpause) {
		error = journal_update(radio, &journal, RTL8822B_REG_TX_PAUSE,
				       1U, 0xffU, 0U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error != 0) {
		(void)journal_rollback(radio, &journal);

		/*
		 * A transport fault makes continued RF operation untrustworthy.
		 */
		radio_emergency_off(radio);
		radio->state = RTL8822B_RADIO_STOPPING;

		/* Failed. */
		return error;
	}

	radio->channel = channel;

	/* Succeeded. */
	return 0;
}

/* Writes one section of the radio initialization tables. */
static int
radio_table_section_apply(
	struct rtl8822b_radio *radio,
	uint8_t domain,
	uint8_t rf_path,
	uint64_t deadline_ticks)
{
	int error;
	const struct rtl8822b_phy_table_section *section;
	const struct rtl8822b_phy_table_section *selected = NULL;
	size_t index;

	/* Process each remaining element. */
	for (index = 0; index < sizeof(rtw8822b_phy_sections) /
					sizeof(rtw8822b_phy_sections[0]);
	     index++) {
		/* Handles the section condition. */
		section = &rtw8822b_phy_sections[index];
		if (section->domain != domain ||
		    (domain == RTL8822B_TABLE_DOMAIN_RF &&
		     section->rf_path != rf_path))
			continue;

		/* Handles the selected availability. */
		if (selected != NULL)
			return EINVAL;
		selected = section;
	}

	/* Handles the selected availability. */
	if (selected == NULL || selected->reserved != 0U ||
	    selected->words == NULL || selected->word_count == 0U ||
	    (selected->word_count & 1U) != 0U) {
		/* Failed. */
		return EINVAL;
	}

	/* Obtains the radio table apply result. */
	error = radio_table_apply(
		radio, selected->domain, selected->width, selected->rf_path,
		selected->words, selected->word_count, deadline_ticks);

	/* Returns the computed result. */
	return error;
}

/* Writes every section of those tables. */
static int
radio_tables_apply(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	unsigned path;
	int error;

	/* Checks the operation status. */
	error = radio_table_section_apply(radio, RTL8822B_TABLE_DOMAIN_MAC, 0U,
					  deadline_ticks);
	if (error == 0) {
		error = radio_table_section_apply(
			radio, RTL8822B_TABLE_DOMAIN_BB, 0U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_table_section_apply(
			radio, RTL8822B_TABLE_DOMAIN_AGC, 0U, deadline_ticks);
	}

	/* Process each remaining element. */
	for (path = 0U; error == 0 && path < radio->board.chip.rf_path_count;
	     path++) {
		error = radio_table_section_apply(
			radio, RTL8822B_TABLE_DOMAIN_RF, (uint8_t)path,
			deadline_ticks);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Reports which 2.4 GHz power group a channel is in. */
static unsigned
radio_2g_power_group(
	uint8_t channel)
{
	/* Handles the channel condition. */
	if (channel <= 2U)
		return 0U;

	/* Handles the channel condition. */
	if (channel <= 5U)
		return 1U;

	/* Handles the channel condition. */
	if (channel <= 8U)
		return 2U;

	/* Returns the computed result. */
	return 3U;
}

/* Reports the gain table index of a legacy rate. */
static uint8_t
radio_txagc_legacy_index(
	const struct rtl8822bu_board_info *board,
	unsigned path,
	uint8_t channel,
	unsigned rate)
{
	const struct rtl8822b_2g_tx_power *power = &board->tx_power_2g[path];
	unsigned group = radio_2g_power_group(channel);
	unsigned section = rate < 4U ? 0U : 1U;
	int section_base = section == 0U ? 32 : 28;
	int limit_offset =
		(int)rtw8822b_2g_legacy_world_limit[channel - 1U][section] -
		section_base;
	int rate_offset = rtw8822b_2g_legacy_rate_offset[rate];
	int index;

	/* Handles the rate offset condition. */
	if (rate_offset > limit_offset)
		rate_offset = limit_offset;

	/* Handles the section condition. */
	if (section == 0U)
		index = power->cck_base[group] + rate_offset;
	else
		index = power->bw40_base[group] + power->ofdm_diff +
			rate_offset;

	/* Checks the current index. */
	if (index < 0)
		return 0U;

	/* Checks the current index. */
	if (index > 0x3f)
		return 0x3fU;

	/* Returns the computed result. */
	return (uint8_t)index;
}

/* Reports which 5 GHz power group a channel is in. */
static unsigned
radio_w52_power_group(
	uint8_t channel)
{
	/* Returns the computed result. */
	return channel <= 40U ? 0U : 1U;
}
/* Reports the 5 GHz gain table index of a legacy rate. */
static uint8_t
radio_txagc_5g_legacy_index(
	const struct rtl8822bu_board_info *board,
	unsigned path,
	uint8_t channel,
	unsigned rate)
{
	const struct rtl8822b_5g_tx_power *power = &board->tx_power_5g[path];
	unsigned group = radio_w52_power_group(channel);
	unsigned channel_index = (channel - 36U) / 4U;
	int section_base = board->rfe_option == 2U ? 32 : 26;
	int limit_offset = (int)rtw8822b_w52_legacy_world_limit[channel_index] -
			   section_base;
	int rate_offset = rtw8822b_5g_legacy_rate_offset[rate - 4U];
	int index;

	/* Handles the rate offset condition. */
	if (rate_offset > limit_offset)
		rate_offset = limit_offset;

	/* Checks the current index. */
	index = power->bw40_base[group] + power->ofdm_diff + rate_offset;
	if (index < 0)
		return 0U;

	/* Checks the current index. */
	if (index > 0x3f)
		return 0x3fU;

	/* Returns the computed result. */
	return (uint8_t)index;
}

/* Writes the transmit gain table for the legacy rates. */
static int
radio_txagc_legacy_profile(
	struct rtl8822b_radio *radio,
	uint8_t channel,
	uint64_t deadline_ticks)
{
	uint8_t index;
	unsigned current;
	uint32_t packed;
	unsigned lane;
	int error;
	uint16_t base;
	unsigned path;
	unsigned rate;

	/* Process each remaining element. */
	for (path = 0U; path < radio->board.chip.rf_path_count; path++) {
		base = path == 0U ? RTL8822B_TXAGC_A : RTL8822B_TXAGC_B;

		/* Process each element required by the operation. */
		for (rate = 0U; rate <= RTL8822B_TXAGC_LAST_RATE; rate += 4U) {
			packed = 0U;

			/* Process each element required by the operation. */
			for (lane = 0U; lane < 4U; lane++) {
				/* Handles the current condition. */
				current = rate + lane;
				if (current < RTL8822B_LEGACY_RATE_COUNT) {
					index = channel_is_w52(channel)
							? (current < 4U
								   ? 0U
								   : radio_txagc_5g_legacy_index(
									     &radio->board,
									     path,
									     channel,
									     current))
							: radio_txagc_legacy_index(
								  &radio->board,
								  path, channel,
								  current);

					packed |= (uint32_t)index
						  << (lane * 8U);
				}
			}

			/* Writes the packed lane indices for this rate. */

			/* Checks the operation status. */
			error = radio_write(radio, (uint16_t)(base + rate), 4U,
					    packed, deadline_ticks);
			if (error != 0)
				return error;
		}
	}

	radio->power_limits_valid = 1U;

	/* Succeeded. */
	return 0;
}

/* Sets the queue map this transport's three endpoints need. */
static int
radio_fifo_3bulkout_profile(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	uint32_t free_bytes;
	uint32_t read_pointer;
	uint32_t value;
	uint32_t write_pointer;
	unsigned attempt;
	int error;

	/*
	 * These values are the RTL8822B 3081-WCPU, 128-byte-page layout:
	 * 2048 total pages, 52 firmware/driver-reserved pages, and the
	 * upstream three-bulk-OUT HQ/LQ/NQ split 64/64/64/0 plus one gap.
	 */
	if (RTL8822B_TX_FIFO_PAGES - RTL8822B_RESERVED_PAGES !=
		    RTL8822B_RESERVED_BOUNDARY ||
	    RTL8822B_RESERVED_BOUNDARY - 64U - 64U - 64U - 1U !=
		    RTL8822B_PUBLIC_QUEUE_PAGES) {
		/* Failed. */
		return EINVAL;
	}

	/* Checks the operation status. */
	error = radio_write(radio, RTL8822B_REG_TXDMA_PQ_MAP, 2U,
			    RTL8822B_USB3_TXDMA_MAP, deadline_ticks);
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_CR, 1U, 0U,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_CR, 1U,
				    RTL8822B_CR_ALL_ENABLE, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_H2CQ_CSR, 4U,
				    0x80000000U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_TXDMA_PQ_MAP, 1U,
				     0x01U, 0x01U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_FIFO_PAGE_HIGH, 2U, 64U,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_FIFO_PAGE_LOW, 2U, 64U,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_FIFO_PAGE_NORMAL, 2U,
				    64U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_FIFO_PAGE_EXTRA, 2U, 0U,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_FIFO_PAGE_PUBLIC, 2U,
				    RTL8822B_PUBLIC_QUEUE_PAGES,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_RQPN_CTRL2, 4U,
				     0x80000000U, 0x80000000U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_FIFO_PAGE_CTRL2, 2U,
				    RTL8822B_RESERVED_BOUNDARY, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_FW_HW_TXQ_CTRL + 2U,
				     1U, 0x10U, 0x10U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_BCNQ_BOUNDARY, 2U,
				    RTL8822B_RESERVED_BOUNDARY, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_FIFO_PAGE_CTRL2 + 2U,
				    2U, RTL8822B_RESERVED_BOUNDARY,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_BCNQ1_BOUNDARY, 2U,
				    RTL8822B_RESERVED_BOUNDARY, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_RXFF_BOUNDARY, 4U,
				    RTL8822B_RX_FIFO_BOUNDARY, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_AUTO_LLT, 1U, 0xf0U,
				     0x30U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_AUTO_LLT + 3U, 1U, 3U,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio,
				     RTL8822B_REG_TXDMA_OFFSET_CHECK + 1U, 1U,
				     0x02U, 0x02U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_AUTO_LLT, 1U, 0x01U,
				     0x01U, deadline_ticks);
	}

	/* Process each element required by the operation. */
	for (attempt = 0U; error == 0 && attempt < RTL8822B_LLT_POLL_MAX;
	     attempt++) {
		/* Checks the operation status. */
		error = radio_read(radio, RTL8822B_REG_AUTO_LLT, 1U, &value,
				   deadline_ticks);
		if (error != 0 || (value & 0x01U) == 0U)
			break;

		/* Checks the operation status. */
		error = radio_delay(radio, RTL8822B_LLT_POLL_DELAY_US,
				    deadline_ticks);
		if (error == 0 && radio->transport.yield != NULL)
			radio->transport.yield(radio->transport.context);
	}

	/* Checks the operation status. */
	if (error == 0 && (value & 0x01U) != 0U)
		error = ETIMEDOUT;
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_CR + 3U, 1U, 0U,
				    deadline_ticks);
	}

	/* Firmware command-ring placement for the RTL8822B 3081 WCPU. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_H2C_HEAD, 4U,
				     0x0003ffffU, RTL8822B_H2C_QUEUE_ADDRESS,
				     deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_H2C_READ, 4U,
				     0x0003ffffU, RTL8822B_H2C_QUEUE_ADDRESS,
				     deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_H2C_TAIL, 4U,
				     0x0003ffffU, RTL8822B_H2C_QUEUE_END,
				     deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_H2C_INFO, 1U, 0x03U,
				     0x01U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_H2C_INFO, 1U, 0x04U,
				     0x04U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio,
				     RTL8822B_REG_TXDMA_OFFSET_CHECK + 1U, 1U,
				     0x80U, 0x80U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_read(radio, RTL8822B_REG_H2C_PACKET_WRITE, 4U,
				   &write_pointer, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_read(radio, RTL8822B_REG_H2C_PACKET_READ, 4U,
				   &read_pointer, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		write_pointer &= 0x0003ffffU;
		read_pointer &= 0x0003ffffU;

		/* Handles the free bytes condition. */
		free_bytes = write_pointer >= read_pointer
				     ? RTL8822B_H2C_QUEUE_SIZE -
					       (write_pointer - read_pointer)
				     : read_pointer - write_pointer;
		if (free_bytes != RTL8822B_H2C_QUEUE_SIZE)
			error = EIO;
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Configures the USB3 PHY adjustment required by RTL8822B cut D. */
static int
radio_usb_phy_profile(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	int error;

	/* Preserve all other cuts' PHY state. */
	if (radio->board.chip.cut != RTL8822B_CUT_D)
		return 0;

	/* Apply upstream's USB3 PHY table at either enumerated USB speed. */

	/* Checks the operation status. */
	error = radio_write(radio, RTL8822B_REG_USB3_PHY_DATA_LOW, 1U, 0x41U,
			    deadline_ticks);
	if (error != 0)
		return error;

	/* Stage the high byte before triggering the PHY register write. */

	/* Checks the operation status. */
	error = radio_write(radio, RTL8822B_REG_USB3_PHY_DATA_HIGH, 1U, 0xa8U,
			    deadline_ticks);
	if (error != 0)
		return error;

	/* Write PHY address one without requesting a USB mode switch. */

	/* Checks the operation status. */
	error = radio_write(radio, RTL8822B_REG_USB3_PHY_ADDRESS, 1U, 0x81U,
			    deadline_ticks);
	if (error != 0)
		return error;

	/* Report the completed PHY adjustment. */
	return 0;
}

/* Configures DMA and RTL8822B v1 aggregation for the actual USB speed. */
static int
radio_usb_profile(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	uint32_t rxdma_mode;
	int error;

	/*
	 * Select four-beat DMA bursts with the validated endpoint packet size.
	 */
	if (radio->transport.usb_bulk_max_packet_size == 1024U)
		rxdma_mode = 0x0eU;
	else
		rxdma_mode = 0x1eU;

	/* Program burst size before enabling the existing aggregate format. */

	/* Checks the operation status. */
	error = radio_write(radio, RTL8822B_REG_RXDMA_MODE, 1U, rxdma_mode,
			    deadline_ticks);
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_TXDMA_OFFSET_CHECK, 2U,
				     0x0200U, 0x0200U, deadline_ticks);
	}

	/* The receive walker consumes the enabled v1 USB aggregate format. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_TXDMA_PQ_MAP, 1U,
				     0x04U, 0x04U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_RXDMA_AGGREGATION + 3U,
				     1U, 0x80U, 0U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_RXDMA_AGGREGATION, 2U,
				    0x2005U, deadline_ticks);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static int radio_phy_trx_mode(struct rtl8822b_radio *radio, uint64_t deadline_ticks);

/* Sets the transmit and receive mode of the baseband. */
static int
radio_phy_trx_mode(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	uint32_t path_bits;
	uint32_t value;
	unsigned attempt;
	int error;

	/*
	 * The bounded USB profile uses path A or paths A+B, never path B alone.
	 */
	path_bits = radio->board.chip.rf_path_count == 2U ? 3U : 1U;

	/* Checks the operation status. */
	error = radio_update(radio, RTL8822B_REG_AGC_TRX_A, 4U, 0x0000ffffU,
			     0x00003231U, deadline_ticks);
	if (error == 0) {
		error = radio_update(
			radio, RTL8822B_REG_AGC_TRX_B, 4U, 0x0000ffffU,
			path_bits == 3U ? 0x00003231U : 0x00001111U,
			deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_CDD_TX_PATH, 4U,
				     0x000c0000U, 0x000c0000U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_TX_PATH_SELECT, 4U,
				     0x30000000U, 0x10000000U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_TX_PATH_SELECT, 4U,
				     0x40000000U, 0x40000000U, deadline_ticks);
	}

	/* Select path A for one stream whenever A is enabled, including A+B. */
	if (error == 0 && (path_bits & 1U) != 0U) {
		error = radio_update(radio, RTL8822B_REG_CDD_TX_PATH, 4U,
				     0xfff00000U, 0x00100000U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0 && (path_bits & 1U) != 0U) {
		error = radio_update(radio, RTL8822B_REG_ADC_INITIAL, 4U,
				     0xf0000000U, 0x80000000U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(
			radio, RTL8822B_REG_TX_PATH_SELECT1, 4U, 0x0000fff0U,
			path_bits == 1U ? 0x00000010U : 0x00000430U,
			deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(
			radio, RTL8822B_REG_TX_PATH_SELECT, 4U, 0x000000ffU,
			path_bits == 1U ? 0x11U : 0x33U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_RX_DESCRIPTOR, 4U,
				     0x00440000U, 0U, deadline_ticks);
	}

	/* Use path A for both CCK receive selections when A+B is enabled. */
	if (error == 0 && (path_bits & 1U) != 0U) {
		error = radio_update(radio, RTL8822B_REG_ADC_INITIAL, 4U,
				     0x0f000000U, 0U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(
			radio, RTL8822B_REG_RX_PATH_SELECT, 4U, 0x000000ffU,
			path_bits == 1U ? 0x11U : 0x33U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(
			radio, RTL8822B_REG_ANTENNA_WEIGHT, 4U, 0x00010000U,
			path_bits == 1U ? 0U : 0x00010000U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(
			radio, RTL8822B_REG_HT_STF_WEIGHT, 4U, 0x10000000U,
			path_bits == 1U ? 0U : 0x10000000U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_MRC, 4U, 0x00800000U,
				     path_bits == 1U ? 0U : 0x00800000U,
				     deadline_ticks);
	}

	/* Program and verify the path-A RF mode LUT with a finite poll. */
	for (attempt = 0U; error == 0 && attempt < 100U; attempt++) {
		/* Checks the operation status. */
		error = radio_rf_write(radio, 0U, 0xefU, 0x80000U,
				       deadline_ticks);
		if (error == 0) {
			error = radio_rf_write(radio, 0U, 0x33U, 0x00001U,
					       deadline_ticks);
		}

		/* Checks the operation status. */
		if (error == 0)
			error = radio_delay(radio, 2U, deadline_ticks);
		if (error == 0) {
			error = radio_rf_read(radio, 0U, 0x33U, &value,
					      deadline_ticks);
		}

		/* Checks the operation status. */
		if (error != 0 || (value & RTL8822B_RF_VALUE_MASK) == 1U)
			break;

		/* Handles the yield availability. */
		if (radio->transport.yield != NULL)
			radio->transport.yield(radio->transport.context);
	}

	/* Checks the operation status. */
	if (error == 0 && (value & RTL8822B_RF_VALUE_MASK) != 1U)
		error = ETIMEDOUT;
	if (error == 0) {
		error = radio_rf_write(radio, 0U, 0xefU, 0x80000U,
				       deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_rf_write(radio, 0U, 0x33U, 0x00001U,
				       deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_rf_write(radio, 0U, 0x3eU, 0x00034U,
				       deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_rf_write(radio, 0U, 0x3fU, 0x4080cU,
				       deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0)
		error = radio_rf_write(radio, 0U, 0xefU, 0U, deadline_ticks);
	if (error == 0)
		error = radio_rf_write(radio, 0U, 0xefU, 0U, deadline_ticks);

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static int radio_phy_rfe_post_table(struct rtl8822b_radio *radio, uint64_t deadline_ticks);

/* Writes the front-end registers the tables leave over. */
static int
radio_phy_rfe_post_table(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	int error;

	/* Checks the operation status. */
	error = radio_update(radio, 0x0064U, 4U, 0x30000000U, 0x30000000U,
			     deadline_ticks);
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_LED_CFG, 4U,
				     0x06000000U, 0U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_GPIO_MUX, 4U,
				     0x00000004U, 0x00000004U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_RFE_PATH_SOURCE, 4U,
				     0x00000c3fU, 0x00000c30U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_RFE_IO_DIRECTION, 4U,
				     0x00000c3fU, 0x00000c3fU, deadline_ticks);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Identifies a programmed board which explicitly has no Bluetooth coexistence. */
static int
radio_board_wlan_only(
	const struct rtl8822b_radio *radio)
{
	/* Preserve unprogrammed and Bluetooth-combination board behavior. */
	if (radio->board.rf_board_option == 0xffU ||
	    (radio->board.rf_board_option & 0xe0U) == 0x20U) {
		/* Succeeded. */
		return 0;
	}

	/* Report the vendor efuse definition of a known WLAN-only board. */
	return 1;
}

/* Waits for the indirect coexistence port within the shared startup deadline. */
static int
radio_coex_ready(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	uint32_t value;
	unsigned attempt;
	int error;

	/*
	 * Retain the upstream finite 1000-read, ten-microsecond polling limit.
	 */
	for (attempt = 0U; attempt < RTL8822B_COEX_POLL_MAX; attempt++) {
		/*
		 * Propagate transport and deadline failures before examining
		 * readiness.
		 */

		/* Checks the operation status. */
		error = radio_read(radio, RTL8822B_REG_COEX_ACCESS, 4U, &value,
				   deadline_ticks);
		if (error != 0)
			return error;

		/*
		 * Admit the next transaction only after the preceding command
		 * completes.
		 */
		if ((value & RTL8822B_COEX_READY) != 0U)
			return 0;

		/*
		 * Bound each retry by both elapsed time and the fixed poll
		 * count.
		 */

		/* Checks the operation status. */
		error = radio_delay(radio, RTL8822B_COEX_POLL_DELAY_US,
				    deadline_ticks);
		if (error != 0)
			return error;
	}

	/*
	 * Report a permanently busy indirect port without starting another
	 * command.
	 */
	return ETIMEDOUT;
}

/* Reads the grant-control register through the checked indirect port. */
static int
radio_coex_grant_read(
	struct rtl8822b_radio *radio,
	uint32_t *value,
	uint64_t deadline_ticks)
{
	int error;

	/*
	 * Join any preceding indirect command before selecting register 0x38.
	 */

	/* Checks the operation status. */
	error = radio_coex_ready(radio, deadline_ticks);
	if (error != 0)
		return error;

	/* Select the four-byte grant register for reading. */

	/* Checks the operation status. */
	error = radio_write(radio, RTL8822B_REG_COEX_ACCESS, 4U, 0x800f0038U,
			    deadline_ticks);
	if (error != 0)
		return error;

	/*
	 * Wait for completion before accepting the returned register contents.
	 */

	/* Checks the operation status. */
	error = radio_coex_ready(radio, deadline_ticks);
	if (error != 0)
		return error;

	/* Return the checked data transfer to the startup owner. */

	/* Reports the failure. */
	error = radio_read(radio, RTL8822B_REG_COEX_READ, 4U, value,
			   deadline_ticks);
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/* Gives a known WLAN-only board its software grants and antenna-switch ownership. */
static int
radio_wlan_only_profile(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	uint32_t value;
	int error;

	/*
	 * Avoid changing grants or switch ownership for unknown or Bluetooth
	 * boards.
	 */
	if (!radio_board_wlan_only(radio))
		return 0;

	/*
	 * Preserve unrelated grant-register state while disabling LTE
	 * arbitration.
	 */

	/* Checks the operation status. */
	error = radio_coex_grant_read(radio, &value, deadline_ticks);
	if (error != 0)
		return error;
	value = (value & ~RTL8822B_COEX_GRANT_MASK) | RTL8822B_COEX_WLAN_GRANT;

	/* Reserve the indirect port before publishing its write data. */

	/* Checks the operation status. */
	error = radio_coex_ready(radio, deadline_ticks);
	if (error != 0)
		return error;

	/* Set both GNT_WL controls high and both GNT_BT controls low. */

	/* Checks the operation status. */
	error = radio_write(radio, RTL8822B_REG_COEX_WRITE, 4U, value,
			    deadline_ticks);
	if (error != 0)
		return error;

	/* Commit the four grant fields through the chip's indirect command. */

	/* Checks the operation status. */
	error = radio_write(radio, RTL8822B_REG_COEX_ACCESS, 4U, 0xc00f0038U,
			    deadline_ticks);
	if (error != 0)
		return error;

	/*
	 * Require the requested grant state before transferring path ownership.
	 */

	/* Checks the operation status. */
	error = radio_coex_grant_read(radio, &value, deadline_ticks);
	if (error != 0)
		return error;

	/*
	 * Reject an accepted transfer whose hardware grant state did not
	 * change.
	 */
	if ((value & RTL8822B_COEX_GRANT_MASK) != RTL8822B_COEX_WLAN_GRANT)
		return EIO;

	/*
	 * Select WLAN ownership while preserving debug and other system mux
	 * bits.
	 */

	/* Checks the operation status. */
	error = radio_update(radio, RTL8822B_REG_COEX_OWNER, 1U, 0x04U, 0x04U,
			     deadline_ticks);
	if (error != 0)
		return error;

	/* Verify ownership before enabling the external WLAN antenna switch. */

	/* Checks the operation status. */
	error = radio_read(radio, RTL8822B_REG_COEX_OWNER, 1U, &value,
			   deadline_ticks);
	if (error != 0)
		return error;

	/* Reject an ownership write which did not reach the hardware. */
	if ((value & 0x04U) == 0U)
		return EIO;

	/* Route the external DPDT switch to BB software control. */

	/* Checks the operation status. */
	error = radio_update(radio, RTL8822B_REG_LED_CFG, 4U, 0x01800000U,
			     0x01000000U, deadline_ticks);
	if (error != 0)
		return error;

	/*
	 * Verify only the selected mux bits without interpreting unrelated
	 * state.
	 */

	/* Checks the operation status. */
	error = radio_read(radio, RTL8822B_REG_LED_CFG, 4U, &value,
			   deadline_ticks);
	if (error != 0)
		return error;

	/*
	 * Keep startup closed when switch ownership could not be established.
	 */
	if ((value & 0x01800000U) != 0x01000000U)
		return EIO;

	/*
	 * Report the checked WLAN-only profile before channel TX is unpaused.
	 */
	return 0;
}

static int radio_driver_info_profile(struct rtl8822b_radio *radio, uint64_t deadline_ticks);

/* Sets how much per-frame information the device reports. */
static int
radio_driver_info_profile(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	int error;

	/* Checks the operation status. */
	error = radio_write(radio, RTL8822B_REG_RX_DRIVER_INFO, 1U, 4U,
			    deadline_ticks);
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_TRXFF_BOUNDARY + 1U,
				     1U, 0x0fU, 0x0fU, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_RCR, 4U, 0x10000000U,
				     0x10000000U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_WMAC_OPTION + 4U, 4U,
				     0x00000300U, 0U, deadline_ticks);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

static int radio_minimum_mac_profile(struct rtl8822b_radio *radio, uint64_t deadline_ticks);

/* Writes the smallest media access setup that works. */
static int
radio_minimum_mac_profile(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	uint32_t mac_high;
	uint32_t mac_low;
	int error;

	/* Packs the address into the two hardware registers. */
	mac_low = (uint32_t)radio->board.mac_address[0] |
		  ((uint32_t)radio->board.mac_address[1] << 8) |
		  ((uint32_t)radio->board.mac_address[2] << 16) |
		  ((uint32_t)radio->board.mac_address[3] << 24);
	mac_high = (uint32_t)radio->board.mac_address[4] |
		   ((uint32_t)radio->board.mac_address[5] << 8);

	/* Pauses transmission while the profile is installed. */

	/* Checks the operation status. */
	error = radio_write(radio, RTL8822B_REG_TX_PAUSE, 1U, 0xffU,
			    deadline_ticks);
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_SW_AMPDU_BURST, 1U,
				     0x40U, 0U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_AMPDU_MAX_TIME, 1U,
				    0x70U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_TX_HANG_CTRL, 1U,
				     0x04U, 0x04U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_PROTECTION_MODE, 4U,
				    0x202008ffU, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_BAR_MODE + 2U, 2U,
				    0x0801U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_FAST_EDCA_VOVI, 1U,
				    0x06U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_FAST_EDCA_VOVI + 2U, 1U,
				    0x06U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_FAST_EDCA_BEBK, 1U,
				    0x06U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_FAST_EDCA_BEBK + 2U, 1U,
				    0x06U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_TIMER0_SOURCE, 1U,
				     0x70U, 0U, deadline_ticks);
	}

	/* Keep TX closed until the calibrated legacy TXAGC profile exists. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_TX_PAUSE + 1U, 1U, 0U,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_SLOT, 1U, 0x09U,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_PIFS, 1U, 0x19U,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_SIFS, 4U, 0x10100e0aU,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_EDCA_VO + 2U, 2U,
				    0x0186U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_EDCA_VI + 2U, 2U,
				    0x03bcU, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_NAV, 4U, 0x001b0005U,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_RX_TSF_OFFSET, 2U,
				    0x3030U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_BEACON_CTRL, 1U, 0x08U,
				     0x08U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_TBTT_PROHIBIT, 4U,
				    0x00006404U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_DRIVER_EARLY_INT, 1U,
				    0x04U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_BEACON_DMA_TIME, 1U,
				    0x02U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_TX_PROTOCOL + 1U, 1U,
				     0x10U, 0U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_PORT0_MAC, 4U, mac_low,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_PORT0_MAC + 4U, 2U,
				    mac_high, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_RX_FILTER0, 2U, 0xffffU,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_RX_FILTER1, 2U, 0x0fffU,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_RX_FILTER2, 2U, 0xffffU,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_RCR, 4U,
				    RTL8822B_RCR_SCAN_PROFILE, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_RX_PACKET_LIMIT, 1U,
				    24U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_TCR + 2U, 1U, 0x30U,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_TCR + 1U, 1U, 0x30U,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_WMAC_OPTION + 8U, 4U,
				    0xb0810041U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_WMAC_OPTION + 4U, 1U,
				    0x98U, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_SOUNDING_PROTOCOL, 1U,
				     0x40U, 0x40U, deadline_ticks);
	}

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Powers the radio up and runs its initialization.
 */
int
drv_rtl8822b_radio_power_on(
	struct rtl8822b_radio *radio,
	const struct rtl8822b_radio_transport *transport,
	const struct rtl8822bu_board_info *board,
	uint64_t deadline_ticks)
{
	int already_powered;
	int error;

	/* Checks the mac address valid result. */
	if (radio == NULL || transport == NULL || board == NULL ||
	    transport->read == NULL || transport->write == NULL ||
	    transport->now_ticks == NULL || transport->delay_us == NULL ||
	    (transport->usb_bulk_max_packet_size != 512U &&
	     transport->usb_bulk_max_packet_size != 1024U) ||
	    radio->state != RTL8822B_RADIO_OFF ||
	    !mac_address_valid(board->mac_address) ||
	    (board->chip.rf_path_count != 1U &&
	     board->chip.rf_path_count != 2U) ||
	    !board_tx_power_2g_valid(board) ||
	    (board->rfe_option != 2U && board->rfe_option != 3U &&
	     board->rfe_option != 5U)) {
		/* Failed. */
		return EINVAL;
	}
	memset(radio, 0, sizeof(*radio));
	radio->transport = *transport;
	radio->board = *board;

	/* Checks the operation status. */
	error = radio_deadline_check(radio, deadline_ticks);
	if (error == 0)
		error = radio_pre_power(radio, deadline_ticks);
	if (error == 0) {
		error = radio_power_state_is_on(radio, &already_powered,
						deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0 && already_powered) {
		/*
		 * A failed/missing firmware acknowledgement forces the same
		 * full power-cycle below; no warm state is ever reused.
		 */
		(void)radio_warm_firmware_ack(radio, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0 && already_powered) {
		error = radio_power_commands(
			radio, rtl8822b_power_disable,
			sizeof(rtl8822b_power_disable) /
				sizeof(rtl8822b_power_disable[0]),
			deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0 && already_powered)
		error = radio_pre_power(radio, deadline_ticks);
	if (error == 0) {
		error = radio_power_commands(
			radio, rtl8822b_power_enable,
			sizeof(rtl8822b_power_enable) /
				sizeof(rtl8822b_power_enable[0]),
			deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0)
		error = radio_post_power(radio, deadline_ticks);

	/* Prepare the cut-dependent USB PHY before firmware transfer starts. */
	if (error == 0)
		error = radio_usb_phy_profile(radio, deadline_ticks);
	if (error != 0) {
		radio_emergency_off(radio);
		radio->state = RTL8822B_RADIO_STOPPING;

		/* Failed. */
		return error;
	}

	radio->state = RTL8822B_RADIO_POWERED;

	/* Succeeded. */
	return 0;
}

/*
 * Brings the radio into service on a channel.
 */
int
drv_rtl8822b_radio_start(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	uint32_t crystal;
	int error;

	/* Handles the radio availability. */
	if (radio == NULL || radio->state != RTL8822B_RADIO_POWERED)
		return EINVAL;

	/* Firmware has already been checked and started by the caller. */

	/* Checks the operation status. */
	error = radio_fifo_3bulkout_profile(radio, deadline_ticks);
	if (error == 0)
		error = radio_usb_profile(radio, deadline_ticks);
	if (error == 0)
		error = radio_minimum_mac_profile(radio, deadline_ticks);
	if (error == 0)
		error = radio_driver_info_profile(radio, deadline_ticks);
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_SYS_FUNC_EN, 1U,
				     RTL8822B_BB_RESET_BITS,
				     RTL8822B_BB_RESET_BITS, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_RF_CTRL, 1U,
				     RTL8822B_RF_ENABLE_BITS,
				     RTL8822B_RF_ENABLE_BITS, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_WLRF1, 4U,
				     RTL8822B_WLRF_ENABLE_BITS,
				     RTL8822B_WLRF_ENABLE_BITS, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_RX_PATH_SELECT, 4U,
				     RTL8822B_RX_PATH_RESET, 0U,
				     deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0)
		error = radio_tables_apply(radio, deadline_ticks);

	/* Checks the operation status. */
	crystal = radio->board.crystal_cap & 0x3fU;
	if (error == 0) {
		error = radio_update(radio, 0x0024U, 4U, 0x7e000000U,
				     crystal << 25, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, 0x0028U, 4U, 0x0000007eU,
				     crystal << 1, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_update(radio, RTL8822B_REG_RX_PATH_SELECT, 4U,
				     RTL8822B_RX_PATH_RESET,
				     RTL8822B_RX_PATH_RESET, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0)
		error = radio_phy_trx_mode(radio, deadline_ticks);
	if (error == 0)
		error = radio_phy_rfe_post_table(radio, deadline_ticks);

	/*
	 * Establish grants and switch ownership while initialization TX is
	 * paused.
	 */
	if (error == 0)
		error = radio_wlan_only_profile(radio, deadline_ticks);

	/*
	 * Program the initial channel before the final transmitter admission.
	 */
	if (error == 0)
		error = radio_channel_apply(radio, 1U, 0, deadline_ticks);
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_CR, 1U,
				    RTL8822B_CR_ALL_ENABLE, deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_TX_PAUSE, 1U, 0U,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error != 0) {
		(void)drv_rtl8822b_radio_stop(radio, deadline_ticks);

		/* Failed. */
		return error;
	}

	radio->state = RTL8822B_RADIO_STARTED;

	/* Succeeded. */
	return 0;
}

/*
 * Moves the radio to another channel.
 */
int
drv_rtl8822b_radio_set_channel(
	struct rtl8822b_radio *radio,
	uint8_t channel,
	uint64_t deadline_ticks)
{
	int error;

	/* Handles the radio availability. */
	if (radio == NULL || radio->state != RTL8822B_RADIO_STARTED)
		return EINVAL;

	/* Obtains the radio channel apply result. */
	error =
		radio_channel_apply(radio, channel, 1, deadline_ticks);

	/* Returns the computed result. */
	return error;
}

/*
 * Stops the receive path taking new frames.
 */
int
drv_rtl8822b_radio_rx_generation_pause(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	uint32_t control;
	uint32_t receive;
	unsigned attempt;
	int error;

	/* Handles the radio availability. */
	if (radio == NULL || radio->state != RTL8822B_RADIO_STARTED)
		return EINVAL;

	/* Handles the radio condition. */
	if (radio->rx_generation_paused)
		return 0;

	/* Checks the operation status. */
	error = radio_read(radio, RTL8822B_REG_CR, 1U, &control,
			   deadline_ticks);
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_CR, 1U,
				    control & ~RTL8822B_CR_RX_ENABLE_MASK,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error != 0)
		return error;

	/*
	 * From this point forward a failure is fail-closed: resume or the
	 * checked radio-stop transaction is required before RX can run again.
	 */
	radio->rx_generation_paused = 1U;

	/* Checks the operation status. */
	error = radio_read(radio, RTL8822B_REG_RX_PACKET_NUMBER, 4U, &receive,
			   deadline_ticks);
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_RX_PACKET_NUMBER, 4U,
				    receive | RTL8822B_RX_RELEASE_ENABLE,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error != 0)
		return error;
	/* Process each element required by the operation. */
	for (attempt = 0U; attempt < RTL8822B_RX_GENERATION_POLL_MAX;
	     attempt++) {
		/* Checks the operation status. */
		error = radio_read(radio, RTL8822B_REG_RX_PACKET_NUMBER, 4U,
				   &receive, deadline_ticks);
		if (error != 0 || (receive & RTL8822B_RXDMA_IDLE) != 0U)
			return error;

		/* Checks the operation status. */
		error = radio_delay(radio, RTL8822B_RX_GENERATION_POLL_US,
				    deadline_ticks);
		if (error != 0)
			return error;
	}

	/* Failed. */
	return ETIMEDOUT;
}

/*
 * Lets it take frames again.
 */
int
drv_rtl8822b_radio_rx_generation_resume(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	uint32_t control;
	uint32_t receive;
	int error;

	/* Handles the radio availability. */
	if (radio == NULL || radio->state != RTL8822B_RADIO_STARTED)
		return EINVAL;

	/* Handles the radio condition. */
	if (!radio->rx_generation_paused)
		return 0;

	/* Checks the operation status. */
	error = radio_read(radio, RTL8822B_REG_RX_PACKET_NUMBER, 4U, &receive,
			   deadline_ticks);
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_RX_PACKET_NUMBER, 4U,
				    receive & ~RTL8822B_RX_RELEASE_ENABLE,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_read(radio, RTL8822B_REG_CR, 1U, &control,
				   deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0) {
		error = radio_write(radio, RTL8822B_REG_CR, 1U,
				    control | RTL8822B_CR_RX_ENABLE_MASK,
				    deadline_ticks);
	}

	/* Checks the operation status. */
	if (error == 0)
		radio->rx_generation_paused = 0U;

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

/*
 * Takes the radio out of service and powers it down.
 */
int
drv_rtl8822b_radio_stop(
	struct rtl8822b_radio *radio,
	uint64_t deadline_ticks)
{
	int first_error = 0;
	int error;

	/* Handles the radio availability. */
	if (radio == NULL)
		return EINVAL;

	/* Handles the radio condition. */
	if (radio->state == RTL8822B_RADIO_OFF)
		return 0;
	radio->state = RTL8822B_RADIO_STOPPING;
	radio->power_limits_valid = 0U;

	/* Checks the operation status. */
	error = radio_write(radio, RTL8822B_REG_TX_PAUSE, 1U, 0xffU,
			    deadline_ticks);
	if (error != 0)
		first_error = error;

	/* Checks the operation status. */
	error = radio_write(radio, RTL8822B_REG_CR, 1U, 0U, deadline_ticks);
	if (error != 0 && first_error == 0)
		first_error = error;

	/* Checks the operation status. */
	error = radio_update(radio, RTL8822B_REG_SYS_FUNC_EN, 1U,
			     RTL8822B_BB_RESET_BITS, 0U, deadline_ticks);
	if (error != 0 && first_error == 0)
		first_error = error;

	/* Checks the operation status. */
	error = radio_update(radio, RTL8822B_REG_RF_CTRL, 1U,
			     RTL8822B_RF_ENABLE_BITS, 0U, deadline_ticks);
	if (error != 0 && first_error == 0)
		first_error = error;

	/* Checks the operation status. */
	error = radio_update(radio, RTL8822B_REG_WLRF1, 4U,
			     RTL8822B_WLRF_ENABLE_BITS, 0U, deadline_ticks);
	if (error != 0 && first_error == 0)
		first_error = error;

	/* Checks the operation status. */
	error = radio_power_commands(radio, rtl8822b_power_disable,
				     sizeof(rtl8822b_power_disable) /
					     sizeof(rtl8822b_power_disable[0]),
				     deadline_ticks);
	if (error != 0 && first_error == 0)
		first_error = error;
	if (first_error != 0)
		radio_emergency_off(radio);

	/*
	 * Preserve transport and state after failure: OFF would make the next
	 * stop report success without issuing the still-unconfirmed hardware
	 * inverse.
	 */
	if (first_error == 0)
		memset(radio, 0, sizeof(*radio));

	/* Failed. */
	if (first_error != 0)
		return first_error;

	/* Succeeded. */
	return 0;
}

/*
 * Asks whether this channel may be probed actively.
 */
int
drv_rtl8822b_radio_active_scan_allowed(
	const struct rtl8822b_radio *radio,
	uint8_t channel)
{
	int error;

	/* Computes the function result. */
	error =
		radio != NULL && radio->state == RTL8822B_RADIO_STARTED &&
		radio->power_limits_valid != 0U &&
		drv_rtl8822b_board_active_channel_allowed(&radio->board,
							  channel) &&
		(radio->board.rfe_option == 2U ||
		 radio->board.rfe_option == 3U ||
		 radio->board.rfe_option == 5U);

	/* Returns the computed result. */
	return error;
}

/* Refuses a probe request frame that is malformed. */
static int
probe_request_valid(
	const struct rtl8822b_radio *radio,
	const uint8_t *frame,
	size_t length)
{
	uint8_t id;
	uint8_t ie_length;
	size_t offset;
	unsigned ssid_count = 0U;
	unsigned index;

	/* Checks the load le16 result. */
	if (radio == NULL || frame == NULL || length < 26U ||
	    length > RTL8822B_MANAGEMENT_MPDU_MAX ||
	    load_le16(frame) != 0x0040U ||
	    memcmp(frame + 10U, radio->board.mac_address, 6U) != 0) {
		/* Succeeded. */
		return 0;
	}
	/* Process each remaining element. */
	for (index = 0U; index < 6U; index++) {
		/* Handles the frame condition. */
		if (frame[4U + index] != 0xffU || frame[16U + index] != 0xffU)
			return 0;
	}

	offset = 24U;
	/* Process each remaining element. */
	while (offset < length) {
		/* Checks the current data length. */
		if (length - offset < 2U)
			return 0;
		id = frame[offset];
		ie_length = frame[offset + 1U];
		offset += 2U;

		/* Handles the ie length condition. */
		if ((size_t)ie_length > length - offset)
			return 0;

		/* Handles the id condition. */
		if (id == 0U) {
			/*
			 * IEEE 802.11 Probe Request contains one SSID IE.
			 * Length zero is a wildcard scan; 1..32 is the directed
			 * reconnect form emitted by the common WLAN engine.
			 */
			if (ssid_count != 0U || ie_length > 32U)
				return 0;
			ssid_count++;
		}

		offset += ie_length;
	}

	/* Returns the computed result. */
	return ssid_count == 1U;
}

static int radio_management_frame_prepare(const struct rtl8822b_radio *radio, uint8_t *wire, size_t capacity, const uint8_t *frame, size_t frame_length, size_t *wire_length);

/* Builds the descriptor one management frame is sent under. */
static int
radio_management_frame_prepare(
	const struct rtl8822b_radio *radio,
	uint8_t *wire,
	size_t capacity,
	const uint8_t *frame,
	size_t frame_length,
	size_t *wire_length)
{
	size_t total;
	uint16_t checksum = 0U;
	unsigned index;

	/* Handles the wire length availability. */
	if (wire_length == NULL)
		return EINVAL;
	*wire_length = 0U;
	/* Checks the drv rtl8822b radio active scan allowed result. */
	if (radio == NULL || wire == NULL || frame == NULL ||
	    !drv_rtl8822b_radio_active_scan_allowed(radio, radio->channel)) {
		/* Failed. */
		return EPERM;
	}

	/* Handles the frame length condition. */
	if (frame_length > SIZE_MAX - RTL8822B_MANAGEMENT_TX_DESCRIPTOR_SIZE)
		return EOVERFLOW;

	/* Avoid full packets on both supported USB bulk packet sizes. */
	total = RTL8822B_MANAGEMENT_TX_DESCRIPTOR_SIZE + frame_length;
	if (total % 512U == 0U) {
		/* Handles the total condition. */
		if (total == SIZE_MAX)
			return EOVERFLOW;
		total++;
	}

	/* Handles the capacity condition. */
	if (capacity < total)
		return ENOSPC;
	memset(wire, 0, total);
	store_le32(wire,
		   (uint32_t)frame_length |
			   ((uint32_t)RTL8822B_MANAGEMENT_TX_DESCRIPTOR_SIZE
			    << 16) |
			   (1U << 24) | (1U << 26) | (1U << 31));
	store_le32(wire + 4U, (18U << 8) | (8U << 16));
	store_le32(wire + 12U, (1U << 8) | (1U << 10));

	/* Use 6 Mbps OFDM on W52 and retain 1 Mbps CCK on 2.4 GHz. */
	if (radio->channel > 14U)
		store_le32(wire + 16U, 4U);

	/* Finalize the descriptor checksum after selecting the basic rate. */
	store_le32(wire + 32U, 1U << 15);
	/* Process each remaining element. */
	for (index = 0U; index < 16U; index++)
		checksum ^= load_le16(wire + index * 2U);
	store_le16(wire + 28U, checksum);
	memcpy(wire + RTL8822B_MANAGEMENT_TX_DESCRIPTOR_SIZE, frame,
	       frame_length);
	*wire_length = total;
	/* Succeeded. */
	return 0;
}

/*
 * Prepares a management frame for transmission.
 */
int
drv_rtl8822b_radio_management_frame_prepare(
	const struct rtl8822b_radio *radio,
	uint8_t *wire,
	size_t capacity,
	const uint8_t *frame,
	size_t frame_length,
	size_t *wire_length)
{
	int error;

	/* Checks the probe request valid result. */
	if (!probe_request_valid(radio, frame, frame_length)) {
		/* Handles the wire length availability. */
		if (wire_length != NULL)
			*wire_length = 0U;
		/* Failed. */
		return EINVAL;
	}

	/* Obtains the radio management frame prepare result. */
	error = radio_management_frame_prepare(
		radio, wire, capacity, frame, frame_length, wire_length);

	/* Returns the computed result. */
	return error;
}

/*
 * Prepares the frame that tells a station it is disconnected.
 */
int
drv_rtl8822b_radio_deauthentication_prepare(
	const struct rtl8822b_radio *radio,
	uint8_t *wire,
	size_t capacity,
	const uint8_t bssid[6],
	const uint8_t station[6],
	uint16_t reason,
	size_t *wire_length)
{
	uint8_t frame[26];
	int error;

	/* Handles the wire length availability. */
	if (wire_length == NULL)
		return EINVAL;
	*wire_length = 0U;

	/* Handles the bssid availability. */
	if (bssid == NULL || station == NULL || reason == 0U ||
	    (bssid[0] & 1U) != 0U || (station[0] & 1U) != 0U) {
		/* Failed. */
		return EINVAL;
	}

	memset(frame, 0, sizeof(frame));
	frame[0] = 0xc0U;
	memcpy(frame + 4U, bssid, 6U);
	memcpy(frame + 10U, station, 6U);
	memcpy(frame + 16U, bssid, 6U);
	store_le16(frame + 24U, reason);
	error = radio_management_frame_prepare(radio,
					       wire,
					       capacity,
					       frame,
					       sizeof(frame),
					       wire_length);
	memset(frame, 0, sizeof(frame));

	/* Reports the failure. */
	if (error != 0)
		return error;

	/* Succeeded. */
	return 0;
}

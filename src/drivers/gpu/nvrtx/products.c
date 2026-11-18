/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The products of the chips the nvrtx driver knows.
 *
 * Every PCI device ID of a Turing (TU102, TU104, TU106, TU116, TU117), Ampere
 * (GA102 to GA107) or Ada (AD102 to AD107) GPU function: desktop and laptop
 * boards of the GeForce RTX 20, 30 and 40 and GTX 16, and the workstation,
 * data-centre and mining boards built on the same chips.  An unnamed entry
 * carries no name.  The table only names a GPU in the stage marks: the chip,
 * and with it the driver's path, is decided by the identity register, so a
 * board missing here is still driven.
 */

#include <stddef.h>
#include <stdint.h>

#include "drivers/gpu/nvrtx/nvrtx-private.h"

/*
 * The products, by device ID in increasing order.
 *
 * Each row is the device ID, the chip number in the identity register, the
 * laptop flag and the product name.
 *
 * Values referenced: pci.ids of the PCI ID repository (pci-ids.ucw.cz) of
 * 2026-10-15 (the IDs and names of the NVIDIA devices).
 */
static const struct nvrtx_product products_table[] = {
	{ 0x1e02U, 0x162U, 0, "TITAN RTX" },
	{ 0x1e03U, 0x162U, 0, "GeForce RTX 2080 Ti 12GB" },
	{ 0x1e04U, 0x162U, 0, "GeForce RTX 2080 Ti" },
	{ 0x1e07U, 0x162U, 0, "GeForce RTX 2080 Ti Rev. A" },
	{ 0x1e09U, 0x162U, 0, "CMP 50HX" },
	{ 0x1e2dU, 0x162U, 0, "GeForce RTX 2080 Ti Engineering Sample" },
	{ 0x1e2eU, 0x162U, 0, "GeForce RTX 2080 Ti 12GB Engineering Sample" },
	{ 0x1e30U, 0x162U, 0, "Quadro RTX 6000/8000" },
	{ 0x1e35U, 0x162U, 0, "Tesla T10" },
	{ 0x1e36U, 0x162U, 0, "Quadro RTX 6000" },
	{ 0x1e37U, 0x162U, 0, "Tesla T10 16GB / GRID RTX T10-2/T10-4/T10-8" },
	{ 0x1e38U, 0x162U, 0, "Tesla T40 24GB" },
	{ 0x1e3cU, 0x162U, 0, NULL },
	{ 0x1e3dU, 0x162U, 0, NULL },
	{ 0x1e3eU, 0x162U, 0, NULL },
	{ 0x1e78U, 0x162U, 0, "Quadro RTX 6000/8000" },
	{ 0x1e81U, 0x164U, 0, "GeForce RTX 2080 SUPER" },
	{ 0x1e82U, 0x164U, 0, "GeForce RTX 2080" },
	{ 0x1e84U, 0x164U, 0, "GeForce RTX 2070 SUPER" },
	{ 0x1e87U, 0x164U, 0, "GeForce RTX 2080 Rev. A" },
	{ 0x1e89U, 0x164U, 0, "GeForce RTX 2060" },
	{ 0x1e90U, 0x164U, 1, "GeForce RTX 2080 Mobile" },
	{ 0x1e91U, 0x164U, 1, "GeForce RTX 2070 SUPER Mobile / Max-Q" },
	{ 0x1e93U, 0x164U, 1, "GeForce RTX 2080 SUPER Mobile / Max-Q" },
	{ 0x1eabU, 0x164U, 1, NULL },
	{ 0x1eaeU, 0x164U, 1, "GeForce GTX 2080 Engineering Sample" },
	{ 0x1eb0U, 0x164U, 0, "Quadro RTX 5000" },
	{ 0x1eb1U, 0x164U, 0, "Quadro RTX 4000" },
	{ 0x1eb4U, 0x164U, 0, "T4G" },
	{ 0x1eb5U, 0x164U, 1, "Quadro RTX 5000 Mobile / Max-Q" },
	{ 0x1eb6U, 0x164U, 1, "Quadro RTX 4000 Mobile / Max-Q" },
	{ 0x1eb8U, 0x164U, 0, "Tesla T4" },
	{ 0x1eb9U, 0x164U, 0, NULL },
	{ 0x1ebaU, 0x164U, 0, "PG189 SKU600" },
	{ 0x1ebeU, 0x164U, 0, NULL },
	{ 0x1ec2U, 0x164U, 0, "GeForce RTX 2070 SUPER" },
	{ 0x1ec7U, 0x164U, 0, "GeForce RTX 2070 SUPER" },
	{ 0x1ed0U, 0x164U, 1, "GeForce RTX 2080 Mobile" },
	{ 0x1ed1U, 0x164U, 1, "GeForce RTX 2070 SUPER Mobile / Max-Q" },
	{ 0x1ed3U, 0x164U, 1, "GeForce RTX 2080 SUPER Mobile / Max-Q" },
	{ 0x1ef5U, 0x164U, 1, "Quadro RTX 5000 Mobile Refresh" },
	{ 0x1f02U, 0x166U, 0, "GeForce RTX 2070" },
	{ 0x1f03U, 0x166U, 0, "GeForce RTX 2060 12GB" },
	{ 0x1f04U, 0x166U, 0, NULL },
	{ 0x1f06U, 0x166U, 0, "GeForce RTX 2060 SUPER" },
	{ 0x1f07U, 0x166U, 0, "GeForce RTX 2070 Rev. A" },
	{ 0x1f08U, 0x166U, 0, "GeForce RTX 2060 Rev. A" },
	{ 0x1f09U, 0x166U, 0, "GeForce GTX 1660 SUPER" },
	{ 0x1f0aU, 0x166U, 0, "GeForce GTX 1650" },
	{ 0x1f0bU, 0x166U, 0, "CMP 40HX" },
	{ 0x1f10U, 0x166U, 1, "GeForce RTX 2070 Mobile" },
	{ 0x1f11U, 0x166U, 1, "GeForce RTX 2060 Mobile" },
	{ 0x1f12U, 0x166U, 1, "GeForce RTX 2060 Max-Q" },
	{ 0x1f14U, 0x166U, 1, "GeForce RTX 2070 Mobile / Max-Q Refresh" },
	{ 0x1f15U, 0x166U, 1, "GeForce RTX 2060 Mobile" },
	{ 0x1f2eU, 0x166U, 1, NULL },
	{ 0x1f36U, 0x166U, 1, "Quadro RTX 3000 Mobile / Max-Q" },
	{ 0x1f42U, 0x166U, 0, "GeForce RTX 2060 SUPER" },
	{ 0x1f47U, 0x166U, 0, "GeForce RTX 2060 SUPER" },
	{ 0x1f50U, 0x166U, 1, "GeForce RTX 2070 Mobile / Max-Q" },
	{ 0x1f51U, 0x166U, 1, "GeForce RTX 2060 Mobile" },
	{ 0x1f54U, 0x166U, 1, "GeForce RTX 2070 Mobile" },
	{ 0x1f55U, 0x166U, 1, "GeForce RTX 2060 Mobile" },
	{ 0x1f76U, 0x166U, 1, "Quadro RTX 3000 Mobile Refresh" },
	{ 0x1f81U, 0x167U, 0, NULL },
	{ 0x1f82U, 0x167U, 0, "GeForce GTX 1650" },
	{ 0x1f83U, 0x167U, 0, "GeForce GTX 1630" },
	{ 0x1f91U, 0x167U, 1, "GeForce GTX 1650 Mobile / Max-Q" },
	{ 0x1f92U, 0x167U, 1, "GeForce GTX 1650 Mobile" },
	{ 0x1f94U, 0x167U, 1, "GeForce GTX 1650 Mobile" },
	{ 0x1f95U, 0x167U, 1, "GeForce GTX 1650 Ti Mobile" },
	{ 0x1f96U, 0x167U, 1, "GeForce GTX 1650 Mobile / Max-Q" },
	{ 0x1f97U, 0x167U, 1, "GeForce MX450" },
	{ 0x1f98U, 0x167U, 1, "GeForce MX450" },
	{ 0x1f99U, 0x167U, 1, "GeForce GTX 1650 Mobile / Max-Q" },
	{ 0x1f9cU, 0x167U, 1, "GeForce MX450" },
	{ 0x1f9dU, 0x167U, 1, "GeForce GTX 1650 Mobile / Max-Q" },
	{ 0x1f9fU, 0x167U, 1, "GeForce MX550" },
	{ 0x1fa0U, 0x167U, 1, "GeForce MX550" },
	{ 0x1fa1U, 0x167U, 1, NULL },
	{ 0x1faeU, 0x167U, 0, NULL },
	{ 0x1fb0U, 0x167U, 1, "Quadro T1000 Mobile" },
	{ 0x1fb1U, 0x167U, 0, "T600" },
	{ 0x1fb2U, 0x167U, 1, "Quadro T400 Mobile" },
	{ 0x1fb6U, 0x167U, 1, "T600 Laptop GPU" },
	{ 0x1fb7U, 0x167U, 1, "T550 Laptop GPU" },
	{ 0x1fb8U, 0x167U, 1, "Quadro T2000 Mobile / Max-Q" },
	{ 0x1fb9U, 0x167U, 1, "Quadro T1000 Mobile" },
	{ 0x1fbaU, 0x167U, 1, "T600 Mobile" },
	{ 0x1fbbU, 0x167U, 1, "Quadro T500 Mobile" },
	{ 0x1fbcU, 0x167U, 1, "T1200 Laptop GPU" },
	{ 0x1fbfU, 0x167U, 0, NULL },
	{ 0x1fd9U, 0x167U, 1, "GeForce GTX 1650 Mobile Refresh" },
	{ 0x1fddU, 0x167U, 1, "GeForce GTX 1650 Mobile Refresh" },
	{ 0x1ff0U, 0x167U, 0, "T1000 8GB" },
	{ 0x1ff2U, 0x167U, 0, "T400 4GB / T400E" },
	{ 0x1ff9U, 0x167U, 1, "Quadro T1000 Mobile" },
	{ 0x2182U, 0x168U, 0, "GeForce GTX 1660 Ti" },
	{ 0x2183U, 0x168U, 0, NULL },
	{ 0x2184U, 0x168U, 0, "GeForce GTX 1660" },
	{ 0x2186U, 0x168U, 0, "PG160 SKU18" },
	{ 0x2187U, 0x168U, 0, "GeForce GTX 1650 SUPER" },
	{ 0x2188U, 0x168U, 0, "GeForce GTX 1650" },
	{ 0x2189U, 0x168U, 0, "CMP 30HX" },
	{ 0x2191U, 0x168U, 1, "GeForce GTX 1660 Ti Mobile" },
	{ 0x2192U, 0x168U, 1, "GeForce GTX 1650 Ti Mobile" },
	{ 0x21aeU, 0x168U, 0, NULL },
	{ 0x21bfU, 0x168U, 0, NULL },
	{ 0x21c2U, 0x168U, 0, NULL },
	{ 0x21c3U, 0x168U, 0, NULL },
	{ 0x21c4U, 0x168U, 0, "GeForce GTX 1660 SUPER" },
	{ 0x21d1U, 0x168U, 1, "GeForce GTX 1660 Ti Mobile" },
	{ 0x2200U, 0x172U, 0, NULL },
	{ 0x2203U, 0x172U, 0, "GeForce RTX 3090 Ti" },
	{ 0x2204U, 0x172U, 0, "GeForce RTX 3090" },
	{ 0x2205U, 0x172U, 0, "GeForce RTX 3080 Ti 20GB" },
	{ 0x2206U, 0x172U, 0, "GeForce RTX 3080" },
	{ 0x2207U, 0x172U, 0, "GeForce RTX 3070 Ti" },
	{ 0x2208U, 0x172U, 0, "GeForce RTX 3080 Ti" },
	{ 0x220aU, 0x172U, 0, "GeForce RTX 3080 12GB" },
	{ 0x220dU, 0x172U, 0, "CMP 90HX" },
	{ 0x2216U, 0x172U, 0, "GeForce RTX 3080 Lite Hash Rate" },
	{ 0x222bU, 0x172U, 0, "GeForce RTX 3090 Engineering Sample" },
	{ 0x222fU, 0x172U, 0, "GeForce RTX 3080 11GB / 12GB Engineering Sample" },
	{ 0x2230U, 0x172U, 0, "RTX A6000" },
	{ 0x2231U, 0x172U, 0, "RTX A5000" },
	{ 0x2232U, 0x172U, 0, "RTX A4500" },
	{ 0x2233U, 0x172U, 0, "RTX A5500" },
	{ 0x2235U, 0x172U, 0, "A40" },
	{ 0x2236U, 0x172U, 0, "A10" },
	{ 0x2237U, 0x172U, 0, "A10G" },
	{ 0x2238U, 0x172U, 0, "A10M" },
	{ 0x223fU, 0x172U, 0, NULL },
	{ 0x2414U, 0x173U, 0, "GeForce RTX 3060 Ti" },
	{ 0x2420U, 0x173U, 1, "GeForce RTX 3080 Ti Mobile" },
	{ 0x2438U, 0x173U, 1, "RTX A5500 Laptop GPU" },
	{ 0x2460U, 0x173U, 1, "GeForce RTX 3080 Ti Laptop GPU" },
	{ 0x2480U, 0x174U, 0, "Reserved Dev ID A" },
	{ 0x2482U, 0x174U, 0, "GeForce RTX 3070 Ti" },
	{ 0x2483U, 0x174U, 0, NULL },
	{ 0x2484U, 0x174U, 0, "GeForce RTX 3070" },
	{ 0x2486U, 0x174U, 0, "GeForce RTX 3060 Ti" },
	{ 0x2487U, 0x174U, 0, "GeForce RTX 3060" },
	{ 0x2488U, 0x174U, 0, "GeForce RTX 3070 Lite Hash Rate" },
	{ 0x2489U, 0x174U, 0, "GeForce RTX 3060 Ti Lite Hash Rate" },
	{ 0x248aU, 0x174U, 0, "CMP 70HX" },
	{ 0x248cU, 0x174U, 0, "GeForce RTX 3070 Ti" },
	{ 0x248dU, 0x174U, 0, "GeForce RTX 3070" },
	{ 0x248eU, 0x174U, 0, "GeForce RTX 3060 Ti" },
	{ 0x249cU, 0x174U, 1, "GeForce RTX 3080 Mobile / Max-Q 8GB/16GB" },
	{ 0x249dU, 0x174U, 1, "GeForce RTX 3070 Mobile / Max-Q" },
	{ 0x249fU, 0x174U, 1, NULL },
	{ 0x24a0U, 0x174U, 1, "Geforce RTX 3070 Ti Laptop GPU" },
	{ 0x24a4U, 0x174U, 1, NULL },
	{ 0x24acU, 0x174U, 0, "GeForce RTX 30x0 Engineering Sample" },
	{ 0x24adU, 0x174U, 0, "GeForce RTX 3060 Engineering Sample" },
	{ 0x24afU, 0x174U, 0, "GeForce RTX 3070 Engineering Sample" },
	{ 0x24b0U, 0x174U, 0, "RTX A4000" },
	{ 0x24b1U, 0x174U, 0, "RTX A4000H" },
	{ 0x24b6U, 0x174U, 1, "RTX A5000 Mobile" },
	{ 0x24b7U, 0x174U, 1, "RTX A4000 Mobile" },
	{ 0x24b8U, 0x174U, 1, "RTX A3000 Mobile" },
	{ 0x24b9U, 0x174U, 1, "RTX A3000 12GB Laptop GPU" },
	{ 0x24baU, 0x174U, 1, "RTX A4500 Laptop GPU" },
	{ 0x24bbU, 0x174U, 1, "RTX A3000 Laptop GPU" },
	{ 0x24bfU, 0x174U, 0, "GeForce RTX 3070 Engineering Sample" },
	{ 0x24c0U, 0x174U, 0, "Initial Dev ID B" },
	{ 0x24c7U, 0x174U, 0, "GeForce RTX 3060 8GB" },
	{ 0x24c8U, 0x174U, 0, "GeForce RTX 3070 GDDR6X" },
	{ 0x24c9U, 0x174U, 0, "GeForce RTX 3060 Ti GDDR6X" },
	{ 0x24dcU, 0x174U, 1, "GeForce RTX 3080 Mobile / Max-Q 8GB/16GB" },
	{ 0x24ddU, 0x174U, 1, "GeForce RTX 3070 Mobile / Max-Q" },
	{ 0x24dfU, 0x174U, 1, NULL },
	{ 0x24e0U, 0x174U, 1, "Geforce RTX 3070 Ti Laptop GPU" },
	{ 0x24faU, 0x174U, 0, "RTX A4500 Embedded GPU" },
	{ 0x2501U, 0x176U, 0, "GeForce RTX 3060" },
	{ 0x2503U, 0x176U, 0, "GeForce RTX 3060" },
	{ 0x2504U, 0x176U, 0, "GeForce RTX 3060 Lite Hash Rate" },
	{ 0x2505U, 0x176U, 0, NULL },
	{ 0x2507U, 0x176U, 0, "Geforce RTX 3050" },
	{ 0x2508U, 0x176U, 0, "GeForce RTX 3050 OEM" },
	{ 0x2509U, 0x176U, 0, "GeForce RTX 3060 12GB Rev. 2" },
	{ 0x2520U, 0x176U, 1, "GeForce RTX 3060 Mobile / Max-Q" },
	{ 0x2521U, 0x176U, 1, "GeForce RTX 3060 Laptop GPU" },
	{ 0x2523U, 0x176U, 1, "GeForce RTX 3050 Ti Mobile / Max-Q" },
	{ 0x252fU, 0x176U, 0, "GeForce RTX 3060 Engineering Sample" },
	{ 0x2531U, 0x176U, 0, "RTX A2000" },
	{ 0x2544U, 0x176U, 0, "GeForce RTX 3060" },
	{ 0x2560U, 0x176U, 1, "GeForce RTX 3060 Mobile / Max-Q" },
	{ 0x2561U, 0x176U, 1, "GeForce RTX 3060 Laptop GPU" },
	{ 0x2563U, 0x176U, 1, "GeForce RTX 3050 Ti Mobile / Max-Q" },
	{ 0x2571U, 0x176U, 0, "RTX A2000 12GB" },
	{ 0x2582U, 0x177U, 0, "GeForce RTX 3050 8GB" },
	{ 0x2583U, 0x177U, 0, "GeForce RTX 3050 4GB" },
	{ 0x2584U, 0x177U, 0, "GeForce RTX 3050 6GB" },
	{ 0x25a0U, 0x177U, 1, "GeForce RTX 3050 Ti Mobile" },
	{ 0x25a2U, 0x177U, 1, "GeForce RTX 3050 Mobile" },
	{ 0x25a3U, 0x177U, 0, NULL },
	{ 0x25a4U, 0x177U, 0, NULL },
	{ 0x25a5U, 0x177U, 1, "GeForce RTX 3050 Mobile" },
	{ 0x25a6U, 0x177U, 1, "GeForce MX570" },
	{ 0x25a7U, 0x177U, 1, "GeForce MX570" },
	{ 0x25a9U, 0x177U, 1, "GeForce RTX 2050" },
	{ 0x25aaU, 0x177U, 1, "GeForce MX570 A" },
	{ 0x25abU, 0x177U, 1, "GeForce RTX 3050 4GB Laptop GPU" },
	{ 0x25acU, 0x177U, 1, "GeForce RTX 3050 6GB Laptop GPU" },
	{ 0x25adU, 0x177U, 0, "GeForce RTX 2050" },
	{ 0x25afU, 0x177U, 0, "GeForce RTX 3050 Engineering Sample" },
	{ 0x25b0U, 0x177U, 0, "RTX A1000" },
	{ 0x25b2U, 0x177U, 0, "RTX A400" },
	{ 0x25b5U, 0x177U, 1, "RTX A4 Mobile" },
	{ 0x25b6U, 0x177U, 0, "A2 / A16" },
	{ 0x25b8U, 0x177U, 1, "RTX A2000 Mobile" },
	{ 0x25b9U, 0x177U, 1, "RTX A1000 Laptop GPU" },
	{ 0x25baU, 0x177U, 1, "RTX A2000 8GB Laptop GPU" },
	{ 0x25bbU, 0x177U, 1, "RTX A500 Laptop GPU" },
	{ 0x25bcU, 0x177U, 1, "RTX A1000 6GB Laptop GPU" },
	{ 0x25bdU, 0x177U, 1, "RTX A500 Laptop GPU" },
	{ 0x25e0U, 0x177U, 1, "GeForce RTX 3050 Ti Mobile" },
	{ 0x25e2U, 0x177U, 1, "GeForce RTX 3050 Mobile" },
	{ 0x25e5U, 0x177U, 1, "GeForce RTX 3050 Mobile" },
	{ 0x25ecU, 0x177U, 1, "GeForce RTX 3050 6GB Laptop GPU" },
	{ 0x25edU, 0x177U, 0, "GeForce RTX 2050" },
	{ 0x25f9U, 0x177U, 0, "RTX A1000 Embedded GPU" },
	{ 0x25faU, 0x177U, 0, "RTX A2000 Embedded GPU" },
	{ 0x25fbU, 0x177U, 0, "RTX A500 Embedded GPU" },
	{ 0x2681U, 0x192U, 0, "RTX TITAN Ada" },
	{ 0x2684U, 0x192U, 0, "GeForce RTX 4090" },
	{ 0x2685U, 0x192U, 0, "GeForce RTX 4090 D" },
	{ 0x2689U, 0x192U, 0, "GeForce RTX 4070 Ti SUPER" },
	{ 0x26afU, 0x192U, 0, "PG137" },
	{ 0x26b1U, 0x192U, 0, "RTX 6000 Ada Generation" },
	{ 0x26b2U, 0x192U, 0, "RTX 5000 Ada Generation" },
	{ 0x26b3U, 0x192U, 0, "RTX 5880 Ada Generation" },
	{ 0x26b5U, 0x192U, 0, "L40" },
	{ 0x26b7U, 0x192U, 0, "L20" },
	{ 0x26b8U, 0x192U, 0, "L40G" },
	{ 0x26b9U, 0x192U, 0, "L40S" },
	{ 0x26baU, 0x192U, 0, "L20" },
	{ 0x26bbU, 0x192U, 0, "L30" },
	{ 0x26f5U, 0x192U, 0, "L40 CNX" },
	{ 0x2702U, 0x193U, 0, "GeForce RTX 4080 SUPER" },
	{ 0x2703U, 0x193U, 0, "GeForce RTX 4080 SUPER" },
	{ 0x2704U, 0x193U, 0, "GeForce RTX 4080" },
	{ 0x2705U, 0x193U, 0, "GeForce RTX 4070 Ti SUPER" },
	{ 0x2709U, 0x193U, 0, "GeForce RTX 4070" },
	{ 0x2717U, 0x193U, 1, "GeForce RTX 4090 Laptop GPU" },
	{ 0x2730U, 0x193U, 1, "RTX 5000 Ada Generation Laptop GPU" },
	{ 0x2757U, 0x193U, 1, "GeForce RTX 4090 Laptop GPU" },
	{ 0x2770U, 0x193U, 1, "RTX 5000 Ada Generation Embedded GPU" },
	{ 0x2782U, 0x194U, 0, "GeForce RTX 4070 Ti" },
	{ 0x2783U, 0x194U, 0, "GeForce RTX 4070 SUPER" },
	{ 0x2785U, 0x194U, 0, "AC AD104 20GB" },
	{ 0x2786U, 0x194U, 0, "GeForce RTX 4070" },
	{ 0x2788U, 0x194U, 0, "GeForce RTX 4060 Ti" },
	{ 0x27a0U, 0x194U, 1, "GeForce RTX 4080 Max-Q / Mobile" },
	{ 0x27b0U, 0x194U, 0, "RTX 4000 SFF Ada Generation" },
	{ 0x27b1U, 0x194U, 0, "RTX 4500 Ada Generation" },
	{ 0x27b2U, 0x194U, 0, "RTX 4000 Ada Generation" },
	{ 0x27b6U, 0x194U, 0, "L2" },
	{ 0x27b7U, 0x194U, 0, "L16" },
	{ 0x27b8U, 0x194U, 0, "L4" },
	{ 0x27baU, 0x194U, 1, "RTX 4000 Ada Generation Laptop GPU" },
	{ 0x27bbU, 0x194U, 1, "RTX 3500 Ada Generation Laptop GPU" },
	{ 0x27e0U, 0x194U, 1, "GeForce RTX 4080 Max-Q / Mobile" },
	{ 0x27faU, 0x194U, 1, "RTX 4000 Ada Generation Embedded GPU" },
	{ 0x27fbU, 0x194U, 1, "RTX 3500 Ada Generation Embedded GPU" },
	{ 0x2803U, 0x196U, 0, "GeForce RTX 4060 Ti" },
	{ 0x2805U, 0x196U, 0, "GeForce RTX 4060 Ti 16GB" },
	{ 0x2808U, 0x196U, 0, "GeForce RTX 4060" },
	{ 0x2820U, 0x196U, 1, "GeForce RTX 4070 Max-Q / Mobile" },
	{ 0x2822U, 0x196U, 1, "GeForce RTX 3050 A Laptop GPU" },
	{ 0x2838U, 0x196U, 1, "RTX 3000 Ada Generation Laptop GPU" },
	{ 0x2860U, 0x196U, 1, "GeForce RTX 4070 Max-Q / Mobile" },
	{ 0x2878U, 0x196U, 1, "RTX 3000 Ada Generation Embedded GPU" },
	{ 0x2882U, 0x197U, 0, "GeForce RTX 4060" },
	{ 0x28a0U, 0x197U, 1, "GeForce RTX 4060 Max-Q / Mobile" },
	{ 0x28a1U, 0x197U, 1, "GeForce RTX 4050 Max-Q / Mobile" },
	{ 0x28a3U, 0x197U, 1, "GeForce RTX 3050 A Laptop GPU" },
	{ 0x28b0U, 0x197U, 0, "RTX 2000 / 2000E Ada Generation" },
	{ 0x28b8U, 0x197U, 1, "RTX 2000 Ada Generation Laptop GPU" },
	{ 0x28b9U, 0x197U, 1, "RTX 1000 Ada Generation Laptop GPU" },
	{ 0x28baU, 0x197U, 1, "RTX 500 Ada Generation Laptop GPU" },
	{ 0x28bbU, 0x197U, 1, "RTX 500 Ada Generation Laptop GPU" },
	{ 0x28e0U, 0x197U, 1, "GeForce RTX 4060 Max-Q / Mobile" },
	{ 0x28e1U, 0x197U, 1, "GeForce RTX 4050 Max-Q / Mobile" },
	{ 0x28e3U, 0x197U, 1, "GeForce RTX 3050 A Laptop GPU" },
	{ 0x28f8U, 0x197U, 1, "RTX 2000 Ada Generation Embedded GPU" }
};

/*
 * Finds the product a PCI device ID names.
 *
 * Returns NULL for a device ID the table does not list.
 */
const struct nvrtx_product *
nvrtx_product_find(
	uint16_t device)
{
	size_t low;
	size_t high;
	size_t middle;

	/* Halves the sorted table until the ID is found or the range is empty. */
	low = 0U;
	high = sizeof(products_table) / sizeof(products_table[0]);
	while (low < high) {
		middle = low + (high - low) / 2U;

		/* The ID is in the upper half. */
		if (products_table[middle].device < device) {
			low = middle + 1U;
			continue;
		}

		/* The ID is in the lower half. */
		if (products_table[middle].device > device) {
			high = middle;
			continue;
		}

		/* Succeeded: the product with this ID. */
		return &products_table[middle];
	}

	/* No product has this ID. */
	return NULL;
}

/*
 * Reports how many products the table lists.
 */
size_t
nvrtx_product_count(void)
{
	/* The table's length. */
	return sizeof(products_table) / sizeof(products_table[0]);
}

/*
 * Reports one product of the table by its index.
 */
const struct nvrtx_product *
nvrtx_product_at(
	size_t index)
{
	/* An index past the end names nothing. */
	if (index >= sizeof(products_table) / sizeof(products_table[0]))
		return NULL;

	/* Succeeded: the product at the index. */
	return &products_table[index];
}

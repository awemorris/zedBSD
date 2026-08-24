/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_USERLAND_LZW_H
#define ZEDBSD_USERLAND_LZW_H

int lzw_compress(int input, int output, unsigned max_bits, int block_mode);
int lzw_decompress(int input, int output);

#endif

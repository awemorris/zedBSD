/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * m68k soft-float user ABI link contract fixture.
 */

	.text
	.globl _start
	.type _start,@function
_start:
	moveq #0,%d0
	rts
	.size _start,.-_start

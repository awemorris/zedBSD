/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Implements the aarch64 runtime-linker entry point.
 */

.text
.balign 16
.global _rtld_start
.type _rtld_start, %function
_rtld_start:
	mov x29, xzr
	mov x30, xzr
	mov x19, sp
	mov x0, x19
	bl rtld_main
	mov sp, x19
	br x0
.size _rtld_start, .-_rtld_start

.global rtld_syscall6
.type rtld_syscall6, %function
rtld_syscall6:
	mov x8, x0
	mov x0, x1
	mov x1, x2
	mov x2, x3
	mov x3, x4
	mov x4, x5
	mov x5, x6
	svc #0
	ret
.size rtld_syscall6, .-rtld_syscall6

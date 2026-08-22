/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_MUSL_FEATURES_H
#define ZEDBSD_MUSL_FEATURES_H

#define hidden __attribute__((__visibility__("hidden")))
#define weak __attribute__((__weak__))
#define weak_alias(old, new) \
	extern __typeof(old) new __attribute__((__weak__, __alias__(#old)))
#define strong_alias(old, new) \
	extern __typeof(old) new __attribute__((__alias__(#old)))

#endif

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/* Reuse the existing host-only video execution stand-in to exercise the actual native diagnostic CLI. */
#define main host_playback_fixture_main
#include "host-native-playback.c"
#undef main
#define main host_media_probe_main
#include "userland/tests/media-probe/main.c"
#undef main

/*
 * Run production diagnostic argument, packet, hash and RMS handling without claiming physical GPU output.
 */
int
main(
	int argc,
	char **argv)
{
	int status;

	/* The diagnostic owns its ordinary native decoder lifetime and error result. */
	status = host_media_probe_main(argc, argv);
	return status;
}

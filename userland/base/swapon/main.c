/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/swap-control/swap-command.h"

#include <zedbsd/system.h>

int
main(int argc, char **argv)
{
	return swap_command_main("swapon", ZEDBSD_SYSTEM_SWAP_ADD, argc, argv);
}

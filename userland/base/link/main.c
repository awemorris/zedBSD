/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/base/common/command.h"
#include <stdio.h>
#include <unistd.h>
int main(int argc, char **argv) { if (argc != 3) { fprintf(stderr, "usage: link source target\n"); return 1; } if (link(argv[1], argv[2])) { command_error("link", argv[2]); return 1; } return 0; }

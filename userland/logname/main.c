/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include "userland/common/command.h"
#include <stdio.h>
#include <unistd.h>
int main(void){char b[128];if(getlogin_r(b,sizeof(b))){command_error("logname",NULL);return 1;}puts(b);return 0;}

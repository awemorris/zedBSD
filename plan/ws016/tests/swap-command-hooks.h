/* SWAP-T009/T010 host syscall hooks. SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_SWAP_COMMAND_HOOKS_H
#define ZEDBSD_SWAP_COMMAND_HOOKS_H

int swap_test_open(const char *, int, ...);
int swap_test_ioctl(int, unsigned long, ...);
int swap_test_close(int);

#endif

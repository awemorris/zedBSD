/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    unsigned char buffer[512];
    int fd;
    ssize_t count;
    if (argc != 2) return 2;
    fd = open(argv[1], O_RDWR);
    if (fd < 0) return 3;
    if (pread(fd, buffer, sizeof(buffer), 4096) != sizeof(buffer)) return 4;
    if (fsync(fd) != 0) return 5;
    puts("UASHELD READY"); fflush(stdout);
    if (getchar() == EOF) return 6;
    count = pread(fd, buffer, sizeof(buffer), 4096);
    if (count >= 0) return 7;
    memset(buffer, 0x3c, sizeof(buffer));
    if (pwrite(fd, buffer, sizeof(buffer), 1048576) >= 0) return 8;
    if (fsync(fd) == 0) return 9;
    puts("UASHELD OLDREJECTED"); fflush(stdout);
    if (getchar() == EOF) return 10;
    if (close(fd) != 0) return 11;
    puts("UASHELD CLOSED");
    return 0;
}

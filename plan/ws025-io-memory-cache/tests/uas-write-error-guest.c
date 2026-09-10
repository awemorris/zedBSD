/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv)
{
    unsigned char data[4096];
    int fd, flushed, write_error;
    ssize_t written;
    unsigned i;
    if (argc != 2) return 2;
    fd = open(argv[1], O_RDWR);
    if (fd < 0) return 3;
    memset(data, 0x3c, sizeof(data));
    written = pwrite(fd, data, sizeof(data), 8 * 1024 * 1024);
    write_error = errno;
#ifdef UAS_WRITE_TIMEOUT
    if (written != -1 || write_error != ETIMEDOUT) return 10;
    puts("UASWRITE TIMEOUT");
#endif
    flushed = fsync(fd);
    printf("UASWRITE FIRST write=%ld errno=%d sync=%d\n", (long)written, write_error, flushed);
    if (written == sizeof(data) && flushed == 0) return 4;
    if (pread(fd, data, sizeof(data), 12 * 1024 * 1024) != sizeof(data)) return 5;
    for (i = 0; i < sizeof(data); i++) if (data[i] != 0xa5) return 6;
    puts("UASWRITE READOK");
    if (fsync(fd) == 0) return 7;
    memset(data, 0x69, sizeof(data));
    if (pwrite(fd, data, sizeof(data), 16 * 1024 * 1024) >= 0) return 8;
    if (fsync(fd) == 0) return 9;
    (void)close(fd);
    puts("UASWRITE STICKY PASS");
    return 0;
}

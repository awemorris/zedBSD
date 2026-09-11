/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdio.h>
int main(void)
{
 int fd=open("/dev/sda",O_RDONLY);
 if(fd<0){perror("async open");return 1;}
 if(ioctl(fd,0x57533019UL,0)<0){perror("async probe");close(fd);return 1;}
 if(close(fd)<0)return 1;
 puts("ASYNC NATIVE PASS");return 0;
}

/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <zedbsd/writeback.h>

static unsigned long long dirty_bytes(void)
{
    struct writeback_report report;
    size_t size = sizeof(report);
    unsigned i;
    memset(&report, 0, sizeof(report));
    if (sysctlbyname("vfs.writeback.stats", &report, &size, NULL, 0) != 0)
        return 0;
    if (size != sizeof(report) || report.header.version != WRITEBACK_REPORT_VERSION ||
        report.header.count > WRITEBACK_REPORT_MOUNTS) return 0;
    for (i = 0; i < report.header.count && i < WRITEBACK_REPORT_MOUNTS; i++)
        if (strcmp(report.mounts[i].path, "/run/uas") == 0)
            return report.mounts[i].device_dirty;
    return 0;
}

int main(void)
{
    unsigned char buffer[4096];
    unsigned long long dirty;
    int fd;
    fd = open("/run/uas/payload", O_RDWR);
    if (fd < 0) return 2;
    memset(buffer, 0x3c, sizeof(buffer));
    if (pwrite(fd, buffer, sizeof(buffer), 0) != sizeof(buffer)) return 3;
    dirty = dirty_bytes();
    if (dirty < sizeof(buffer)) return 4;
    printf("UASDIRTY READY %llu\n", dirty);
    fflush(stdout);
    if (getchar() == EOF) return 5;
    if (pread(fd, buffer, sizeof(buffer), 0) >= 0) return 6;
    if (fsync(fd) == 0) return 7;
    if (unmount("/run/uas", MNT_FORCE) == 0 || errno != EBUSY) return 8;
    puts("UASDIRTY BUSYREFUSED");
    if (close(fd) != 0) return 9;
    dirty = dirty_bytes();
    if (dirty < sizeof(buffer)) return 10;
    printf("UASDIRTY CLOSED %llu\n", dirty);
    return 0;
}

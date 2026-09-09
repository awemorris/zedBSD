/* Fail-fast unused read boundary, compiled separately from production. */
#include <kern/disk.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
int disk_read(struct disk *disk, uint64_t first, uint32_t count, void *data)
{ (void)disk; (void)first; memset(data, 0, (size_t)count * 512); abort(); return EIO; }
int disk_read_direct(struct disk *disk, uint64_t first, uint32_t count, void *data)
{ return disk_read(disk, first, count, data); }

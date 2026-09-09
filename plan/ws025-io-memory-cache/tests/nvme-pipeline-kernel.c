/* Test-only ioctl link wrapper: exercises real large BIOs, no installed ABI. */
#include <kern/disk.h>
#include <kern/atomic.h>
#include <hal/hal.h>
#include <errno.h>
#include <string.h>

#define NVME_TEST_WRITE 0x57532701UL
#define NVME_TEST_VERIFY 0x57532702UL
#define NVME_TEST_BYTES 65536U

static atomic_uint_t active;
static unsigned char expected[NVME_TEST_BYTES];
static unsigned char actual[NVME_TEST_BYTES];

int __real_disk_ioctl(struct disk *, unsigned long, void *);
int __wrap_disk_ioctl(struct disk *, unsigned long, void *);

int
__wrap_disk_ioctl(struct disk *disk, unsigned long request, void *argument)
{
	uint64_t block;
	unsigned iteration;
	unsigned was = 0;
	unsigned index;
	int error = 0;

	if (request != NVME_TEST_WRITE && request != NVME_TEST_VERIFY)
		return __real_disk_ioctl(disk, request, argument);
	if (disk == NULL || strcmp(disk->d_name, "nvme0n1") != 0 ||
	    disk->d_block_size != 512U)
		return EINVAL;
	if (!atomic_compare_exchange(&active, &was, 1))
		return EBUSY;

	for (iteration = 0; iteration < 32; iteration++) {
		/* Exercise both sides of 4 GiB, apart from the raw-helper regions. */
		block = UINT64_C(65536) + (uint64_t)iteration * 128U;
		if (iteration >= 16)
			block += UINT64_C(8388608);
		for (index = 0; index < NVME_TEST_BYTES; index++)
			expected[index] = (unsigned char)(index * 13 + iteration);
		if (request == NVME_TEST_WRITE) {
			error = disk_write_direct(disk, block, 128, expected);
			if (error != 0)
				break;
			error = disk_sync(disk);
			if (error != 0)
				break;
		}
		memset(actual, 0xa5, sizeof(actual));
		error = disk_read_direct(disk, block, 128, actual);
		if (error != 0)
			break;
		if (memcmp(expected, actual, sizeof(actual)) != 0) {
			error = EIO;
			break;
		}
	}
	atomic_store_release(&active, 0);
	if (error != 0) {
		hal_printf("NVME BIO FAIL iteration=%u error=%d\n", iteration, error);
		return error;
	}
	hal_printf("NVME BIO PASS mode=%s iterations=32 bytes=65536\n",
	    request == NVME_TEST_WRITE ? "write" : "verify");
	return 0;
}

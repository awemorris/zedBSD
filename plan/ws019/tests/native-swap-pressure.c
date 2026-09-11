/* Native UFS paging acceptance probe; never installed as an OS command.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#define main ws016_unused_main
#include "../../ws016/tests/runtime-swap-guest.c"
#undef main

static int
check_native_source(const char *path)
{
	struct system_swap_source_info source;
	unsigned index;

	for (index = 0; index < SOURCE_COUNT; index++) {
		if (get_source(index, &source) != 0)
			return failure("native-source-query");
		if (index == 0) {
			if (!source_active(&source) || strcmp(source.source, path) != 0)
				return failure("native-source-identity");
		} else if (source.state != ZEDBSD_SYSTEM_SWAP_STATE_INACTIVE) {
			return failure("unexpected-additional-swap");
		}
	}
	return 0;
}

static int
prepare_fragmented(const char *path, const char *gap)
{
	unsigned char bytes[65536];
	unsigned index;
	int file;
	int other;
	int status;

	memset(bytes, 0, sizeof(bytes));
	file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (file < 0)
		return failure("fragment-file-create");
	other = open(gap, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (other < 0) {
		(void)close(file);
		return failure("fragment-gap-create");
	}
	status = 0;
	/* Alternate allocation requests; the runtime extent count must prove gaps. */
	for (index = 0; index < 512; index++) {
		if (write(file, bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes) ||
		    write(other, bytes, sizeof(bytes)) != (ssize_t)sizeof(bytes)) {
			status = failure("fragment-write");
			break;
		}
	}
	if (fsync(file) != 0 || fsync(other) != 0)
		status = failure("fragment-sync");
	if (close(file) != 0)
		status = failure("fragment-close");
	if (close(other) != 0)
		status = failure("fragment-gap-close");
	if (status == 0)
		puts("NATIVE SWAP fragmented files prepared");
	return status;
}

int
main(int argc, char **argv)
{
	struct pressure_process process;
	struct vm_statistics before;
	struct vm_statistics after;
	struct system_swap_source_info source;
	unsigned generation;
	int status;

	if (argc == 4 && strcmp(argv[1], "prepare") == 0)
		return prepare_fragmented(argv[2], argv[3]);
	if (argc != 2 && argc != 3)
		return 2;
	system_descriptor = open("/dev/system", O_RDWR | O_CLOEXEC);
	if (system_descriptor < 0 || check_native_source(argv[1]) != 0)
		return failure("native-source-open");
	if (get_statistics(&before) != 0)
		return failure("native-initial-statistics");
	printf("NATIVE SWAP source=%s extents=%llu\n", argv[1],
	    (unsigned long long)before.swap_extents);
	if (argc == 3 && (strcmp(argv[2], "fragmented") != 0 ||
	    before.swap_extents < 32))
		return failure("fragmentation-not-established");
	fflush(stdout);

	for (generation = 1; generation <= 3; generation++) {
		if (pressure_start(&process, 2048, 0, 0, 128, generation) != 0)
			return failure("native-pressure-start");
		status = pressure_finish(&process);
		if (status != 0 || check_native_source(argv[1]) != 0)
			return failure("native-pressure-readback");
		/* Release/reactivate only after the worker has verified and unmapped data. */
		if (run_command("/sbin/swapoff", argv[1], 0) != 0)
			return failure("native-swapoff");
		if (get_source(0, &source) != 0 ||
		    source.state != ZEDBSD_SYSTEM_SWAP_STATE_INACTIVE)
			return failure("native-release-state");
		if (run_command("/sbin/swapon", argv[1], 0) != 0 ||
		    check_native_source(argv[1]) != 0)
			return failure("native-reactivation");
	}
	if (get_statistics(&after) != 0 ||
	    after.vm_page_out <= before.vm_page_out ||
	    after.vm_page_in <= before.vm_page_in ||
	    after.vm_io_errors != before.vm_io_errors)
		return failure("native-final-statistics");
	printf("NATIVE SWAP PRESSURE PASS source=%s page-in=%llu page-out=%llu\n",
	    argv[1], (unsigned long long)(after.vm_page_in - before.vm_page_in),
	    (unsigned long long)(after.vm_page_out - before.vm_page_out));
	return close(system_descriptor) != 0;
}

/* Short native q086 stories, synthetic data only. SPDX-License-Identifier: Zlib */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "userland/base/net/wifi-store.h"

#define REQUIRE(x) do { if (!(x)) { +printf("q086 FAIL line=%d errno=%d\n", __LINE__, errno); return 1; } } while (0)
static unsigned char data[65536], check[65536];
static unsigned char large[256 * 1024 + 1];
static struct wifi_conf_model profiles;

static int store_check(int directory, const char *key)
{
	char error[512];
	wifi_conf_model_init(&profiles);
	if (wifi_store_load_at(directory, "wifi.conf", 0, 0, &profiles,
	    error, sizeof(error)) != 0) return 0;
	return profiles.profile_count == 1 &&
	    profiles.profiles[0].passphrase_length == strlen(key) &&
	    memcmp(profiles.profiles[0].passphrase, key, strlen(key)) == 0;
}

int main(int argc, char **argv)
{
	int fd, directory;
	char error[512];
	REQUIRE(argc == 2);
	if (strcmp(argv[1], "bench") == 0) {
		struct timespec start, end;
		fd = open("/q086-bench", O_CREAT | O_TRUNC | O_RDWR, 0600);
		REQUIRE(fd >= 0);
		memset(data, 0x57, sizeof(data));
		REQUIRE(write(fd, data, sizeof(data)) == sizeof(data));
		REQUIRE(fsync(fd) == 0);
		REQUIRE(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
		for (unsigned i = 0; i < 4; i++)
			REQUIRE(pwrite(fd, data, sizeof(data), 0) == sizeof(data));
		REQUIRE(fsync(fd) == 0);
		REQUIRE(clock_gettime(CLOCK_MONOTONIC, &end) == 0);
		REQUIRE(pread(fd, check, sizeof(check), 0) == sizeof(check));
		REQUIRE(memcmp(data, check, sizeof(data)) == 0);
		REQUIRE(close(fd) == 0);
		long long elapsed = (long long)(end.tv_sec - start.tv_sec) * 1000000000LL +
		    end.tv_nsec - start.tv_nsec;
		printf("BENCH PASS bytes=262144 elapsed_ns=%lld includes_final_fsync=1\n", elapsed);
		return 0;
	}
	if (strcmp(argv[1], "verify") == 0) {
		directory = open("/q086", O_RDONLY | O_DIRECTORY);
		REQUIRE(directory >= 0 && store_check(directory, "public-key-three"));
		fd = open("/q086/data", O_RDONLY);
		REQUIRE(fd >= 0 && pread(fd, check, 512, 16384) == 512);
		for (unsigned i = 0; i < 512; i++) REQUIRE(check[i] == (unsigned char)(i * 13));
		REQUIRE(close(fd) == 0 && close(directory) == 0);
		puts("S49 PASS native grouped reboot persistence");
		return 0;
	}
	REQUIRE(mkdir("/q086", 0700) == 0 || errno == EEXIST);
	directory = open("/q086", O_RDONLY | O_DIRECTORY);
	REQUIRE(directory >= 0);
	/* Exercise the production store first, through actual overlay fsync. */
	REQUIRE(wifi_store_set_key_at(directory, "wifi.conf", 0, 0,
	    "q086-public", 11, "public-key-one", 14, 1, error, sizeof(error)) == 0);
	REQUIRE(store_check(directory, "public-key-one"));
	puts("S45 PASS native wifi-store fsync rename directory-fsync");
	wifi_store_test_fail_once(WIFI_STORE_TEST_TEMP_SYNC, EIO);
	REQUIRE(wifi_store_set_key_at(directory, "wifi.conf", 0, 0,
	    "q086-public", 11, "public-key-two", 14, 1, error, sizeof(error)) != 0);
	REQUIRE(store_check(directory, "public-key-one"));
	REQUIRE(wifi_store_set_key_at(directory, "wifi.conf", 0, 0,
	    "q086-public", 11, "public-key-two", 14, 1, error, sizeof(error)) == 0);
	REQUIRE(store_check(directory, "public-key-two"));
	puts("S46 PASS native failed save preserves generation and next save recovers");
	for (unsigned i = 0; i < 4; i++) {
		REQUIRE(wifi_store_set_key_at(directory, "wifi.conf", 0, 0,
		    "q086-public", 11, "public-key-three", 16, 1, error, sizeof(error)) == 0);
		REQUIRE(store_check(directory, "public-key-three"));
	}
	puts("S47 PASS native repeated replacement and production daemon store loader");
	fd = open("/q086/data", O_CREAT | O_TRUNC | O_RDWR, 0600);
	REQUIRE(fd >= 0);
	for (unsigned i = 0; i < sizeof(data); i++) data[i] = (unsigned char)(i * 13);
	REQUIRE(write(fd, data, sizeof(data)) == sizeof(data));
	REQUIRE(lseek(fd, 0, SEEK_SET) == 0);
	REQUIRE(read(fd, check, sizeof(check)) == sizeof(check));
	REQUIRE(memcmp(data, check, sizeof(data)) == 0);
	puts("S37 PASS native 64KiB regular write/read");
	/* Cross the current syscall cap on real page-backed kernel allocations. */
	int large_fd = open("/q087-large", O_CREAT | O_TRUNC | O_RDWR, 0600);
	REQUIRE(large_fd >= 0);
	for (unsigned i = 0; i < sizeof(large); i++) large[i] = (unsigned char)(i * 17);
	REQUIRE(write(large_fd, large, sizeof(large)) == sizeof(large));
	REQUIRE(lseek(large_fd, 0, SEEK_SET) == 0);
	memset(large, 0, sizeof(large));
	REQUIRE(read(large_fd, large, sizeof(large)) == sizeof(large));
	for (unsigned i = 0; i < sizeof(large); i++) REQUIRE(large[i] == (unsigned char)(i * 17));
	REQUIRE(fsync(large_fd) == 0 && close(large_fd) == 0);
	REQUIRE(unlink("/q087-large") == 0);
	puts("q087 PASS native 256KiB+1 write/read with real kernel allocation and cap crossing");
	memset(check, 0x65, 777);
	REQUIRE(pwrite(fd, check, 777, 101) == 777);
	REQUIRE(lseek(fd, 0, SEEK_CUR) == sizeof(data));
	memcpy(data + 101, check, 777);
	REQUIRE(pread(fd, check, sizeof(check), 0) == sizeof(check));
	REQUIRE(memcmp(data, check, sizeof(data)) == 0);
	puts("S38 PASS native unaligned positional I/O preserves offset and neighbors");
	struct iovec v[3] = {{data, 4095}, {data, 0}, {data + 4095, 8193}};
	REQUIRE(lseek(fd, 0, SEEK_SET) == 0);
	REQUIRE(writev(fd, v, 3) == 12288);
	v[0].iov_base = check; v[2].iov_base = check + 4095;
	REQUIRE(lseek(fd, 0, SEEK_SET) == 0);
	REQUIRE(readv(fd, v, 3) == 12288 && memcmp(data, check, 12288) == 0);
	puts("S39 PASS native vectors with empty element and crossed chunk boundary");
	unsigned char *mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	REQUIRE(mapping != MAP_FAILED);
	REQUIRE(mapping[101] == data[101]);
	unsigned char value = 0x19;
	REQUIRE(pwrite(fd, &value, 1, 101) == 1 && mapping[101] == value);
	mapping[102] = 0x28;
	REQUIRE(msync(mapping, 4096, MS_SYNC) == 0);
	REQUIRE(pread(fd, check, 2, 101) == 2 && check[0] == 0x19 && check[1] == 0x28);
	REQUIRE(munmap(mapping, 4096) == 0);
	puts("S43 PASS native MAP_SHARED and ordinary file coherence");
	REQUIRE(fsync(fd) == 0 && close(fd) == 0);
	int pipes[2], status;
	REQUIRE(pipe(pipes) == 0);
	pid_t child = fork();
	REQUIRE(child >= 0);
	if (child == 0) {
		close(pipes[0]); memset(data, 0xa1, 512);
		struct iovec pair[2] = {{data, 255}, {data + 255, 257}};
		_exit(writev(pipes[1], pair, 2) == 512 ? 0 : 1);
	}
	memset(data, 0xb2, 512);
	struct iovec pair[2] = {{data, 257}, {data + 257, 255}};
	REQUIRE(writev(pipes[1], pair, 2) == 512);
	REQUIRE(close(pipes[1]) == 0);
	size_t got = 0;
	while (got < 1024) { ssize_t n = read(pipes[0], check + got, 1024 - got); REQUIRE(n > 0); got += n; }
	for (unsigned i = 0; i < 512; i++) {
		REQUIRE(check[i] == check[0] && check[512 + i] == check[512]);
	}
	REQUIRE(check[0] != check[512]);
	REQUIRE(waitpid(child, &status, 0) == child && status == 0);
	REQUIRE(close(pipes[0]) == 0);
	puts("S40 PASS native competing PIPE_BUF vector writes remain indivisible");
	fd = open("/etc/zedbsd-root", O_RDWR);
	REQUIRE(fd >= 0 && pread(fd, check, 1, 0) == 1 && check[0] == 'z');
	value = 'Z'; REQUIRE(pwrite(fd, &value, 1, 0) == 1 && fsync(fd) == 0);
	REQUIRE(close(fd) == 0); fd = open("/etc/zedbsd-root", O_RDWR);
	REQUIRE(fd >= 0 && pread(fd, check, 1, 0) == 1 && check[0] == 'Z');
	value = 'z'; REQUIRE(pwrite(fd, &value, 1, 0) == 1 && fsync(fd) == 0);
	REQUIRE(close(fd) == 0);
	puts("S36 PASS native overlay copy-up small change and reopen");
	REQUIRE(fsync(directory) == 0 && close(directory) == 0);
	sync();
	puts("S48 PASS native FAT loop UFS overlay persistence path");
	return 0;
}

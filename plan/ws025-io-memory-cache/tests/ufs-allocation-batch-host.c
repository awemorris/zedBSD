/* Allocation/publication regression using the real unified UFS implementation. */
#define main retained_audit_main
#include "../../ws024-unified-ufs/tests/ufs-metadata-host.c"
#undef main

int mutex_init(struct mutex *mutex, enum lock_rank rank, const char *name)
{ (void)rank; (void)name; memset(mutex, 0, sizeof(*mutex)); return 0; }

static void prepare(AUDIT_STATE *fs, AUDIT_INODE *node, struct mount *mountp, struct disk *disk, unsigned depth);
static off_t logical_offset(AUDIT_STATE *fs, unsigned depth);
static void check_references(AUDIT_STATE *fs, unsigned depth);
static void success_cases(void);
static void failures(void);
static void quota_prefix(void);
static void split_cases(void);

int main(void)
{
	success_cases();
	failures();
	quota_prefix();
	split_cases();
	printf("UFS allocation batch PASS %u checks\n", functional_checks);
	return 0;
}

/* Create a free contiguous extent and an optional existing indirect leaf. */
static void
prepare(AUDIT_STATE *fs, AUDIT_INODE *node, struct mount *mountp,
    struct disk *disk, unsigned depth)
{
	unsigned n;

	storage_fixture(fs, node, mountp, disk, 0, 1);
	node->inode.i_size = logical_offset(fs, depth);
	node->direct[0] = 0;
	for (n = 160; n < 416; n++)
		bit_set(storage + 32 * 512 + 264, n);
	memset(storage + 160 * 512, 0xcc, 256 * 512);
	AUDIT_PUT32(storage + 32 * 512, AUDIT_CG_NBFREE, 32, 0);
	fs->super.cstotal_nbfree = 32;
	fs->cg_valid = 0;
	if (depth != 0) {
		node->indirect[depth - 1] = 440 + (depth - 1) * 8;
		node->blocks = depth * 8;
		for (n = 1; n < depth; n++)
			AUDIT_PUTPTR(storage + (440 + n * 8) * 512, 0, 440 + (n - 1) * 8, 0);
	}
	REQUIRE(persist_inode(&node->inode) == 0);
	memcpy(durable, storage, sizeof(storage));
	storage_writes = storage_reads = storage_syncs = 0;
	commit_error = 0;
}

/* Select the first block under each indirect root. */
static off_t
logical_offset(AUDIT_STATE *fs, unsigned depth)
{
	uint64_t logical;
	unsigned n;
	uint64_t span;

	if (depth == 0)
		return 0;
	logical = 12;
	span = fs->super.nindir;
	for (n = 1; n < depth; n++) {
		logical += span;
		span *= fs->super.nindir;
	}
	return (off_t)(logical * fs->super.bsize);
}

/* Verify that visible and durable pointers never name reusable blocks. */
static void
check_references(AUDIT_STATE *fs, unsigned depth)
{
	const uint8_t *raw;
	const uint8_t *bytes;
	uint64_t fragment;
	unsigned image;
	unsigned n;

	for (image = 0; image < 2; image++) {
		bytes = image ? durable : storage;
		raw = depth ? bytes + 440 * 512 : bytes + 8 * 512 + 2 * AUDIT_DINODE_SIZE + AUDIT_DB;
		for (n = 0; n < 8; n++) {
			fragment = AUDIT_GETPTR(raw, n * 8, 0);
			if (fragment == 0)
				continue;
			REQUIRE(fragment >= 160 && fragment < 416);
			REQUIRE(!bit_test(bytes + 32 * 512 + 264, (unsigned)fragment));
			REQUIRE(bytes[fragment * 512] == 0x5a);
			REQUIRE(bytes[fragment * 512 + fs->super.bsize - 1] == 0x5a);
		}
	}
}

/* Compare real create/append commands and contents across all pointer depths. */
static void
success_cases(void)
{
	AUDIT_STATE fs;
	AUDIT_INODE node;
	struct mount mountp;
	struct disk disk;
	struct io_stats before, after;
	unsigned char input[32768], output[32768];
	unsigned depth;
	off_t offset;

	memset(input, 0x5a, sizeof(input));
	for (depth = 0; depth <= 3; depth++) {
		prepare(&fs, &node, &mountp, &disk, depth);
		offset = logical_offset(&fs, depth);
		io_stats_snapshot(&before);
		REQUIRE(pwrite_inode(&node.inode, input, sizeof(input), offset) == sizeof(input));
		io_stats_snapshot(&after);
		REQUIRE(storage_writes == (depth ? 5U : 4U));
		REQUIRE(storage_syncs == 2);
		REQUIRE(after.events[IO_UFS_CONTENT_WRITE].calls - before.events[IO_UFS_CONTENT_WRITE].calls == 1);
		printf("depth=%u create bytes=32768 reads=%u writes=%u flush=%u content=1\n",
		    depth, storage_reads, storage_writes, storage_syncs);
		REQUIRE(pread_inode(&node.inode, output, sizeof(output), offset) == sizeof(output));
		REQUIRE(memcmp(input, output, sizeof(input)) == 0);
		check_references(&fs, depth);
		REQUIRE(!node.inode.i_lock.locked && !fs.lock.locked);
		free(fs.cg);
	}
}

/* Fail writes before and after storage mutation, and every flush boundary. */
static void
failures(void)
{
	AUDIT_STATE fs;
	AUDIT_INODE node;
	struct mount mountp;
	struct disk disk;
	unsigned char input[32768];
	unsigned depth;
	unsigned mode;
	unsigned boundary;
	ssize_t count;
	off_t offset;

	memset(input, 0x5a, sizeof(input));
	for (depth = 0; depth <= 3; depth++) {
		for (mode = 0; mode < 4; mode++) {
			for (boundary = 1; boundary <= 8; boundary++) {
				prepare(&fs, &node, &mountp, &disk, depth);
				offset = logical_offset(&fs, depth);
				if (mode == 2) {
					failure_sync = (int)boundary;
				} else {
					failure_write = (int)boundary;
					commit_error = mode != 0;
					if (mode == 3)
						failure_write_again = (int)boundary + 1;
				}
				count = pwrite_inode(&node.inode, input, sizeof(input), offset);
				REQUIRE(count == sizeof(input) || count == -EIO);
				check_references(&fs, depth);
				REQUIRE(!node.inode.i_lock.locked && !fs.lock.locked);
				if (count == -EIO)
					REQUIRE(node.inode.i_size == offset);
				free(fs.cg);
			}
		}
	}
}

/* Keep the accepted prefix and release unused reservations at a hard quota. */
static void
quota_prefix(void)
{
	AUDIT_STATE fs;
	AUDIT_INODE node;
	struct mount mountp;
	struct disk disk;
	struct quota_record quota;
	unsigned char input[32768];

	prepare(&fs, &node, &mountp, &disk, 0);
	quota_state_init(&fs.quota);
	memset(&quota, 0, sizeof(quota));
	quota.id = 0;
	quota.block_hard = 3;
	REQUIRE(quota_set(&fs.quota, QUOTA_USER, &quota) == 0);
	REQUIRE(quota_enable(&fs.quota, QUOTA_USER, 1) == 0);
	memset(input, 0x5a, sizeof(input));
	REQUIRE(pwrite_inode(&node.inode, input, sizeof(input), 0) == 3 * 4096);
	REQUIRE(pwrite_inode(&node.inode, input, 4096, 3 * 4096) == -EDQUOT);
	REQUIRE(quota_get(&fs.quota, QUOTA_USER, 0, &quota) == 0);
	REQUIRE(quota.blocks == 3);
	check_references(&fs, 0);
	free(fs.cg);
}


/* Exercise CG boundaries, ENOSPC prefixes and zero-filled partial blocks. */
static void
split_cases(void)
{
	AUDIT_STATE fs;
	AUDIT_INODE node;
	struct mount mountp;
	struct disk disk;
	unsigned char input[65536], output[65536];
	uint8_t *cg;
	uint64_t fragment;
	unsigned n;

	/* Reject active ABI overflow and negative offsets before any storage access. */
	prepare(&fs, &node, &mountp, &disk, 0);
	REQUIRE(pwrite_inode(&node.inode, input, 1, -1) == -EINVAL);
	fs.super.maxfilesize = UINT64_MAX;
	REQUIRE(pwrite_inode(&node.inode, input, 1, (off_t)INT64_MAX) == -EFBIG);
	REQUIRE(pwrite_inode(&node.inode, input, (size_t)-1, 1) == -EINVAL);
	REQUIRE(storage_reads == 0 && storage_writes == 0);
	free(fs.cg);

	/* Split a leaf over two allocation groups without sharing pending CG state. */
	prepare(&fs, &node, &mountp, &disk, 1);
	fs.super.ncg = 2;
	fs.super.fpg = 256;
	fs.super.cstotal_nbfree = 8;
	node.indirect[0] = 400;
	cg = storage + 32 * 512;
	memset(cg + 264, 0, 64);
	for (n = 160; n < 192; n++)
		bit_set(cg + 264, n);
	AUDIT_PUT32(cg, AUDIT_CG_NDBLK, 256, 0);
	AUDIT_PUT32(cg, AUDIT_CG_NBFREE, 4, 0);
	memcpy(storage + 288 * 512, cg, 4096);
	AUDIT_PUT32(storage + 288 * 512, AUDIT_CG_CGX, 1, 0);
	memset(storage + 400 * 512, 0, 4096);
	REQUIRE(persist_inode(&node.inode) == 0);
	memcpy(durable, storage, sizeof(storage));
	memset(input, 0x5a, sizeof(input));
	REQUIRE(pwrite_inode(&node.inode, input, 32768, 12 * 4096) == 32768);
	REQUIRE(pread_inode(&node.inode, output, 32768, 12 * 4096) == 32768);
	REQUIRE(memcmp(input, output, 32768) == 0);
	for (n = 0; n < 8; n++) {
		REQUIRE(bmap(&node.inode, 12 + n, &fragment) == 0);
		REQUIRE(fragment == (n < 4 ? 160 + n * 8 : 416 + (n - 4) * 8));
	}
	REQUIRE(fs.super.cstotal_nbfree == 0);
	free(fs.cg);

	/* Return only the completed prefix when the entire filesystem is full. */
	prepare(&fs, &node, &mountp, &disk, 0);
	memset(storage + 32 * 512 + 264, 0, 64);
	for (n = 160; n < 184; n++)
		bit_set(storage + 32 * 512 + 264, n);
	AUDIT_PUT32(storage + 32 * 512, AUDIT_CG_NBFREE, 3, 0);
	fs.super.cstotal_nbfree = 3;
	REQUIRE(pwrite_inode(&node.inode, input, sizeof(input), 0) == 3 * 4096);
	REQUIRE(pwrite_inode(&node.inode, input, 4096, 3 * 4096) == -ENOSPC);
	REQUIRE(node.inode.i_size == 3 * 4096);
	free(fs.cg);

	/* Keep the partial-block fallback's zero initialization outside caller bytes. */
	prepare(&fs, &node, &mountp, &disk, 0);
	REQUIRE(pwrite_inode(&node.inode, input, 32767, 0) == 32767);
	REQUIRE(bmap(&node.inode, 7, &fragment) == 0);
	REQUIRE(storage[fragment * 512 + 4095] == 0);
	REQUIRE(pread_inode(&node.inode, output, 32768, 0) == 32767);
	REQUIRE(memcmp(input, output, 32767) == 0);
	free(fs.cg);

	/* Fall back before ownership changes if the bounded working set is unavailable. */
	prepare(&fs, &node, &mountp, &disk, 0);
	allocation_failure_size = 3 * fs.super.bsize;
	REQUIRE(pwrite_inode(&node.inode, input, 32768, 0) == 32768);
	REQUIRE(allocation_failure_size == 0);
	REQUIRE(pread_inode(&node.inode, output, 32768, 0) == 32768);
	REQUIRE(memcmp(input, output, 32768) == 0);
	free(fs.cg);

	/* Cross the direct limit, creating an initialized indirect root as needed. */
	prepare(&fs, &node, &mountp, &disk, 0);
	REQUIRE(pwrite_inode(&node.inode, input, sizeof(input), 0) == sizeof(input));
	REQUIRE(pread_inode(&node.inode, output, sizeof(output), 0) == sizeof(output));
	REQUIRE(memcmp(input, output, sizeof(input)) == 0);
	REQUIRE(node.indirect[0] != 0);
	REQUIRE(node.blocks == 17 * 8);
	free(fs.cg);
}

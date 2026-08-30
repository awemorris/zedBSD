/* Shared native BIOS loader disk layout. */
#ifdef ZBL_STAGE2_LBA_OVERRIDE
.set ZBL_STAGE2_LBA,       ZBL_STAGE2_LBA_OVERRIDE
#else
.set ZBL_STAGE2_LBA,       1
#endif
.set ZBL_PARTITION_LBA,   2048
.set ZBL_STAGE2_SEG,      0x1000
.set ZBL_BOUNCE_SEG,      0x8000
.set ZBL_STAGE2_MAX_SECT, 127

/* Stable Stage 1 layout metadata.  The image backend and publication checker
 * consume this record instead of decoding movable instruction bytes.  Keep it
 * below the disk signature at 0x1b8 and outside the executable code/data. */
.set ZBL1_METADATA_OFFSET, 0x01a0
.set ZBL1_MAGIC,           0x314c425a
.set ZBL1_VERSION,         1
.set ZBL1_HEADER_SIZE,     16

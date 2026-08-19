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

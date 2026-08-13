/* Fixed disk layout for the dual PC/AT and PC-98 BIOS loader. */
.set ZBU_RELOC_ADDR,        0x0600
.set ZBU_PC98_STAGE1_LBA,   2
.set ZBU_PC98_STAGE2_LBA,   3
.set ZBU_PCAT_STAGE1_LBA,   66
.set ZBU_PCAT_STAGE2_LBA,   67
.set ZBU_SLOT_SECTORS,      64
.set ZBU_PARTITION_LBA,     2048
.set ZBU_PC98_STAGE1_SEG,   0x1200
.set ZBU_PCAT_STAGE1_SEG,   0x1400
.set ZBU_STAGE2_SEG,        0x1000
.set ZBU_STAGE2_MAX_SECT,   63
.set ZBU_PC98_HEADS,        8
.set ZBU_PC98_SECTORS,      17


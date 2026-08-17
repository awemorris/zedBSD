logerror "Z68:SCRIPT\n"
bp fc0000,1,{ logerror "Z68:SCSI-ROM pc=%08X\n",pc ; g }
bp fc004a,1,{ logerror "Z68:SCSI-ROM-COLD pc=%08X\n",pc ; g }
bp fc00a2,1,{ logerror "Z68:SCSI-ROM-ENTRY pc=%08X\n",pc ; g }
bp fc00c2,1,{ logerror "Z68:SCSI-READY-BEGIN id=%08X\n",d4 ; g }
bp fc00d0,1,{ logerror "Z68:SCSI-READY rc=%08X\n",d0 ; g }
bp fc0130,1,{ logerror "Z68:SCSI-READY-OK id=%08X\n",d4 ; g }
bp fc0144,1,{ logerror "Z68:SCSI-INQUIRY rc=%08X\n",d0 ; g }
bp fc0176,1,{ logerror "Z68:SCSI-START-STOP rc=%08X\n",d0 ; g }
bp fc0180,1,{ logerror "Z68:SCSI-CAPACITY rc=%08X\n",d0 ; g }
bp fc0192,1,{ logerror "Z68:SCSI-BLOCK-SHIFT shift=%08X\n",d5 ; g }
bp fc01aa,1,{ logerror "Z68:SCSI-MARK-READ rc=%08X shift=%08X\n",d0,d5 ; g }
bp fc01c4,1,{ logerror "Z68:SCSI-MARK-OK\n" ; g }
bp fc01fe,1,{ logerror "Z68:SCSI-IPL-READ rc=%08X\n",d0 ; g }
bp fc0216,1,{ logerror "Z68:SCSI-IPL-OK\n" ; g }
bp fc0224,1,{ logerror "Z68:SCSI-AUTO-SCAN id=%08X\n",d2 ; g }
bp fc0326,1,{ logerror "Z68:SCSI-AUTO-MARK-READ rc=%08X id=%08X\n",d0,d6 ; g }
bp fc0346,1,{ logerror "Z68:SCSI-AUTO-MARK-OK id=%08X\n",d6 ; g }
bp fc0356,1,{ logerror "Z68:SCSI-AUTO-IPL-READ rc=%08X id=%08X\n",d0,d6 ; g }
bp fe003a,1,{ logerror "Z68:IPL pc=%08X\n",pc ; g }
bp ff93e2,1,{ logerror "Z68:BOOT-DEVICE d4=%08X\n",d4 ; g }
bp ff944c,1,{ logerror "Z68:DISK-MARK-READ rc=%08X id=%08X shift=%08X\n",d0,d4,d5 ; g }
bp ff9466,1,{ logerror "Z68:DISK-MARK-OK\n" ; g }
bp ff9474,1,{ logerror "Z68:BOOT-BLOCK-READ rc=%08X\n",d0 ; g }
bp ff9482,1,{ logerror "Z68:BOOT-BLOCK-OK\n" ; g }
bp ff94be,1,{ logerror "Z68:SCSI-SCAN id=%08X\n",d2 ; g }
bp ff9634,1,{ logerror "Z68:SCSI-SCAN-END\n" ; g }
bp 2000,1,{ logerror "Z68:STAGE1 pc=%08X\n",pc ; g }
bp 20000,1,{ logerror "Z68:STAGE2 pc=%08X\n",pc ; g }
bp 10000,1,{ logerror "Z68:LOW pc=%08X\n",pc ; g }
bp 80102bd4,1,{ logerror "Z68:HIGH pc=%08X\n",pc ; g }
bp 80101cc8,1,{ logerror "Z68:CMAIN pc=%08X\n",pc ; g }
bp 8010c6f2,1,{ logerror "Z68:KERNEL-ENTRY pc=%08X\n",pc ; g }
g

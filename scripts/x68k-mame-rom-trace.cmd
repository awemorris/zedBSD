logerror "Z68:ROM-TRACE\n"
bp fc00a2,1,{ logerror "Z68:SCSI-ROM-ENTRY pc=%08X\n",pc ; g }
bp fc0eb4,1,{ logerror "Z68:SCSI-WAIT pc=%08X a2=%08X a6=%08X r9=%08X dreg=%08X\n",pc,a2,a6,b@(a6+9),b@a2 ; quit }
bp 2400,1,{ logerror "Z68:STAGE1 pc=%08X\n",pc ; trace off ; g }
g

logerror "Z68:DIRECT-STAGE2-SMOKE\n"
bp fc00a2,1,{ logerror "Z68:ROM-INITIALIZED pc=%08X\n",pc ; load build/x68k/stage2.bin,20000 ; load build/x68k/mame-manifest.bin,1f000 ; d4=0 ; a0=1f000 ; pc=20000 ; g }
bp 20056,1,{ logerror "Z68:STAGE2-FATAL pc=%08X caller=%08X message=%08X\n",pc,d@sp,d@(sp+4) ; g }
bp 2050a,1,{ logerror "Z68:CRC-FINISH raw=%08X expected=%08X\n",d2,d@(a6-8) ; g }
bp 10000,1,{ logerror "Z68:KERNEL-LOW pc=%08X sp=%08X\n",pc,sp ; g }
bp 801019e2,1,{ logerror "Z68:CMAIN pc=%08X sp=%08X\n",pc,sp ; g }
bp 8010c3d2,1,{ logerror "Z68:KERNEL-ENTRY pc=%08X sp=%08X\n",pc,sp ; g }
bp 80101124,1,{ logerror "Z68:TASK-INIT pc=%08X\n",pc ; g }
bp 8010b004,1,{ logerror "Z68:PLATFORM-INIT pc=%08X\n",pc ; g }
bp 8010c242,1,{ logerror "Z68:KERNEL-SPC-INIT pc=%08X\n",pc ; g }
bp 80102a32,1,{ logerror "Z68:KERNEL-MAIN pc=%08X\n",pc ; g }
bp 80115d36,1,{ logerror "Z68:INIT-START pc=%08X\n",pc ; g }
bp 801006ae,1,{ logerror "Z68:HAL-FATAL caller=%08X file=%08X line=%08X message=%08X\n",d@sp,d@(sp+4),d@(sp+8),d@(sp+12) ; g }
g

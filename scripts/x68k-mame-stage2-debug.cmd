logerror "Z68:DIRECT-STAGE2\n"
bp fc00a2,1,{ logerror "Z68:ROM-INITIALIZED pc=%08X\n",pc ; load build/x68k/stage2.bin,20000 ; load build/x68k/mame-manifest.bin,1f000 ; d4=0 ; a0=1f000 ; pc=20000 ; g }
bp 20006,1,{ logerror "Z68:STAGE2 pc=%08X w0=%08X w1=%08X w2=%08X\n",pc,w@20006,w@20008,w@2000a ; g }
bp 20444,1,{ logerror "Z68:STAGE2-MAIN sp=%08X arg0=%08X arg1=%08X\n",sp,d@(sp+4),d@(sp+8) ; g }
bp 20056,1,{ logerror "Z68:STAGE2-FATAL pc=%08X\n",pc ; g }
bp 20bb4,1,{ logerror "Z68:SPC-INIT pc=%08X\n",pc ; g }
bp 20746,1,{ logerror "Z68:BUS-FREE psns=%08X ints=%08X ssts=%08X\n",b@e9602b,b@e96029,b@e9602d ; g }
bp 2106c,1,{ logerror "Z68:SPC-READ10 pc=%08X\n",pc ; g }
bp 210dc,1,{ logerror "Z68:SPC-READ10-RETURN rc=%08X\n",d0 ; g }
bp 10000,1,{ logerror "Z68:KERNEL-LOW pc=%08X\n",pc ; g }
bp 80102bd4,1,{ logerror "Z68:KERNEL-HIGH pc=%08X\n",pc ; g }
bp 80101cc8,1,{ logerror "Z68:CMAIN pc=%08X\n",pc ; g }
bp 8010c6f2,1,{ logerror "Z68:KERNEL-ENTRY pc=%08X\n",pc ; g }
g

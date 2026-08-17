logerror "Z68:DIRECT-STAGE2-SMOKE\n"
bp fc00a2,1,{ logerror "Z68:ROM-INITIALIZED pc=%08X\n",pc ; load build/x68k/stage2.bin,20000 ; load build/x68k/mame-manifest.bin,1f000 ; d4=0 ; a0=1f000 ; pc=20000 ; g }
bp 20056,1,{ logerror "Z68:STAGE2-FATAL pc=%08X caller=%08X message=%08X\n",pc,d@sp,d@(sp+4) ; g }
bp 2050a,1,{ logerror "Z68:CRC-FINISH raw=%08X expected=%08X\n",d2,d@(a6-8) ; g }
bp 10000,1,{ logerror "Z68:KERNEL-LOW pc=%08X sp=%08X\n",pc,sp ; g }
bp 80101a14,1,{ logerror "Z68:CMAIN pc=%08X sp=%08X\n",pc,sp ; g }
bp 8010c80a,1,{ logerror "Z68:KERNEL-ENTRY pc=%08X sp=%08X\n",pc,sp ; g }
bp 80101156,1,{ logerror "Z68:TASK-INIT pc=%08X\n",pc ; g }
bp 8010b43c,1,{ logerror "Z68:PLATFORM-INIT pc=%08X\n",pc ; g }
bp 8010c67a,1,{ logerror "Z68:KERNEL-SPC-INIT pc=%08X\n",pc ; g }
bp 80102e6a,1,{ logerror "Z68:KERNEL-MAIN pc=%08X\n",pc ; g }
bp 8011619c,1,{ logerror "Z68:INIT-START pc=%08X\n",pc ; g }
bp 801116fa,1,{ logerror "Z68:SPAWN-ENTER pc=%08X path=%08X\n",pc,d@(sp+8) ; g }
bp 801117a6,1,{ logerror "Z68:SPAWN-OPENED pc=%08X\n",pc ; g }
bp 801117ce,1,{ logerror "Z68:SPAWN-ACCESS-OK pc=%08X\n",pc ; g }
bp 801117ee,1,{ logerror "Z68:SPAWN-PROCESS-CREATED pc=%08X\n",pc ; g }
bp 80111808,1,{ logerror "Z68:SPAWN-VMSPACE-CREATED pc=%08X\n",pc ; g }
bp 8011182a,1,{ logerror "Z68:SPAWN-ELF-LOADED pc=%08X\n",pc ; g }
bp 8011184c,1,{ logerror "Z68:SPAWN-BRK-SET pc=%08X\n",pc ; g }
bp 80111870,1,{ logerror "Z68:SPAWN-STACK-BUILT pc=%08X\n",pc ; g }
bp 801119c2,1,{ logerror "Z68:SPAWN-THREAD-CREATED pc=%08X\n",pc ; g }
bp 80111a12,1,{ logerror "Z68:SPAWN-RETURN pc=%08X rc=%08X\n",pc,d2 ; g }
bp 801161aa,1,{ logerror "Z68:INIT-RETURN pc=%08X rc=%08X\n",pc,d0 ; g }
bp 801013a2,1,{ logerror "Z68:TASK-EXEC pc=%08X entry=%08X user-sp=%08X\n",pc,d@(sp+8),d@(sp+12) ; g }
bp 80102e10,1,{ logerror "Z68:USER-TASK-START pc=%08X\n",pc ; g }
bp 400000,1,{ logerror "Z68:USER-ENTRY pc=%08X sp=%08X argc=%08X\n",pc,sp,d@sp ; g }
bp 80101fe8,1,{ logerror "Z68:KEYBOARD-IRQ pc=%08X\n",pc ; g }
bp 80112d76,1,{ logerror "Z68:SYSCALL-DISPATCH pc=%08X number=%08X\n",pc,d@(sp+4) ; g }
bp 80100400,1,{ logerror "Z68:MACHINE-RESET pc=%08X\n",pc ; g }
bp 801006f6,1,{ logerror "Z68:HAL-FATAL caller=%08X file=%08X line=%08X message=%08X\n",d@sp,d@(sp+4),d@(sp+8),d@(sp+12) ; g }
g

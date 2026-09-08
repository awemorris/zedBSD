# UFS formatter ownership

The disk definitions, endian codec and superblock decoder in this directory
were copied from the zedBSD UFS driver during WS025 p031 (2026-09-08).
They are now maintained as userland code under their original Zlib license.
No source or private header from src/drivers is required to build mkfs.

ufs-format.c and the codec use ordinary host C/POSIX interfaces and are checked
against the maintained image generator, including fault injection and byte equality.
Keep format changes compatible with the kernel decoder through those tests;
do not reintroduce a kernel-source build dependency.

The command frontend also uses userland/base/common/format-file.c/.h and the
public zedbsd/fcntl.h interface. The zedBSD frontend requires its exclusive
format-reservation ioctl before writing. A port to another OS must supply an
appropriate host reservation frontend; compiling the portable codec does not
provide that OS-specific reservation service. Kernel implementation sources
are unnecessary in either case.

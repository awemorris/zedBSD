/*
 * Kernel HAL basic types
 *
 * Both spellings are provided: the classic uint8/uint16/uint32 the HAL
 * sources use, and the _t forms newer code prefers.
 */

#ifndef SYS_TYPES_H
#define SYS_TYPES_H

#define NULL	((void *)0)

/* Basic types (i386: long is 32-bit). */
typedef unsigned long long	uint64;
typedef unsigned long		uint32;
typedef unsigned short		uint16;
typedef unsigned char		uint8;
typedef signed long long	int64;
typedef signed long		int32;
typedef signed short		int16;
typedef signed char		int8;

typedef uint64	uint64_t;
typedef uint32	uint32_t;
typedef uint16	uint16_t;
typedef uint8	uint8_t;
typedef int64	int64_t;
typedef int32	int32_t;
typedef int16	int16_t;
typedef int8	int8_t;

typedef unsigned long	size_t;
typedef unsigned long	clock_t;
typedef unsigned long	off_t;
typedef uint32		uintptr_t;
typedef int32		intptr_t;
typedef uint32		physaddr_t;	/* TODO: 64-bit targets */

#ifndef __cplusplus
typedef int	bool;
#define true	(1)
#define false	(0)
#endif

#endif

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * GPT partition type GUIDs that zedBSD's tools write or name.
 *
 * Each value is the canonical text form (upper case, as the UEFI
 * specification prints it).  The administration tools compare them
 * without regard to case.
 */
#ifndef KERN_UAPI_GPT_TYPE_H
#define KERN_UAPI_GPT_TYPE_H

/* The EFI System Partition, which the firmware reads loaders from. */
#define KERN_GPT_TYPE_EFI_SYSTEM	"C12A7328-F81F-11D2-BA4B-00A0C93EC93B"

/* A zedBSD UFS file system (the value FreeBSD also uses for UFS). */
#define KERN_GPT_TYPE_ZEDBSD_UFS	"516E7CB6-6ECF-11D6-8FF8-00022D09712B"

/* A zedBSD swap partition (the value FreeBSD also uses for swap). */
#define KERN_GPT_TYPE_ZEDBSD_SWAP	"516E7CB5-6ECF-11D6-8FF8-00022D09712B"

/*
 * A partition the installer made and has not given a use yet.
 *
 * The installer creates a partition with this type and no name, and
 * replaces the type with the UFS or swap value only when the partition is
 * chosen as a root or a swap area.  No other system claims the value, so
 * a partition of this type holds nothing another system would miss.
 */
#define KERN_GPT_TYPE_ZEDBSD_UNASSIGNED	"7EF6A3E2-F243-445D-B77A-C37CA9605418"

/* Microsoft Basic Data (FAT and NTFS volumes of Windows and others). */
#define KERN_GPT_TYPE_BASIC_DATA	"EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"

/* The Microsoft Reserved Partition that Windows places after the ESP. */
#define KERN_GPT_TYPE_MICROSOFT_RESERVED	"E3C9E316-0B5C-4DB8-817D-F92DF00215AE"

/* The Windows Recovery Environment. */
#define KERN_GPT_TYPE_WINDOWS_RECOVERY	"DE94BBA4-06D1-4D40-A16A-BFD50179D6AC"

/* A Linux file system. */
#define KERN_GPT_TYPE_LINUX_FILESYSTEM	"0FC63DAF-8483-4772-8E79-3D69D8477DE4"

/* A Linux swap partition. */
#define KERN_GPT_TYPE_LINUX_SWAP	"0657FD6D-A4AB-43C4-84E5-0933C84B4F4F"

/* A FreeBSD boot partition, which zedBSD does not use but names. */
#define KERN_GPT_TYPE_FREEBSD_BOOT	"83BD6B9D-7F41-11DC-BE0B-001560B84F0F"

/* An Apple APFS container. */
#define KERN_GPT_TYPE_APPLE_APFS	"7C3457EF-0000-11AA-AA11-00306543ECAC"

#endif

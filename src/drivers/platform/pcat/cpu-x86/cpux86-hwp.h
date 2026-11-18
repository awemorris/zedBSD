/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The decoder of Hardware-Controlled Performance States (HWP) and of the
 * hardware feedback table: the fields of the HWP registers, the request
 * the driver writes with its range checks, the status bits and their
 * acknowledgement, and the layout and cells of the table the processor
 * fills with each logical processor's performance and efficiency.
 *
 * The registers' meaning and the order the driver must touch them in
 * (enable first, then the others) are the processor's; this decoder only
 * builds and reads the values.  Which register a logical processor may
 * touch, and when, is the driver's to decide from the CPUID features.
 */

#ifndef DRIVERS_PLATFORM_PCAT_CPU_X86_CPUX86_HWP_H
#define DRIVERS_PLATFORM_PCAT_CPU_X86_CPUX86_HWP_H

#include "cpux86.h"
#include "cpux86-cpuid.h"

/*
 * The architectural registers of HWP, of the hardware feedback interface,
 * of the package thermal status that announces a table update, and of the
 * delivered-performance counters.
 *
 * Values referenced: Intel SDM Vol. 4 (335592-093), Table 2-2
 * (IA32_MPERF E7H, IA32_APERF E8H, IA32_PACKAGE_THERM_STATUS 1B1H,
 * IA32_PM_ENABLE 770H .. IA32_HWP_STATUS 777H, IA32_HW_FEEDBACK_PTR 17D0H,
 * IA32_HW_FEEDBACK_CONFIG 17D1H).
 */
#define CPUX86_MSR_MPERF		0x0E7U
#define CPUX86_MSR_APERF		0x0E8U
#define CPUX86_MSR_PACKAGE_THERM_STATUS	0x1B1U
#define CPUX86_MSR_PM_ENABLE		0x770U
#define CPUX86_MSR_HWP_CAPABILITIES	0x771U
#define CPUX86_MSR_HWP_REQUEST_PKG	0x772U
#define CPUX86_MSR_HWP_INTERRUPT	0x773U
#define CPUX86_MSR_HWP_REQUEST		0x774U
#define CPUX86_MSR_HWP_CTL		0x776U
#define CPUX86_MSR_HWP_STATUS		0x777U
#define CPUX86_MSR_HW_FEEDBACK_PTR	0x17D0U
#define CPUX86_MSR_HW_FEEDBACK_CONFIG	0x17D1U

/*
 * Reports whether HWP is enabled from IA32_PM_ENABLE.
 *
 * Input: raw, the register as read.
 * Output: *enabled is bit 0 (HWP_ENABLE), 0 or 1.
 * Errors: EINVAL for a NULL pointer or a set reserved bit (63:1).
 * Specification: Intel SDM Vol. 3B rev. 093, §17.4.2, Figure 17-4.
 */
int
cpux86_hwp_enabled(
	uint64_t raw,
	uint32_t *enabled);

/*
 * The value that enables HWP when written to IA32_PM_ENABLE.  The bit can
 * be set once; only a reset clears it.
 *
 * Values referenced: Intel SDM Vol. 3B rev. 093, §17.4.2, Figure 17-4.
 */
#define CPUX86_PM_ENABLE_HWP		0x1U

/*
 * The four performance levels IA32_HWP_CAPABILITIES reports, in the
 * processor's abstract units (0..255).  Guaranteed and most efficient can
 * change while the processor runs, and so can highest on a processor that
 * reports such changes (CPUID leaf 06H EAX[15], status bit 3).
 */
struct cpux86_hwp_capabilities {
	uint32_t highest;		/* bits 7:0 */
	uint32_t guaranteed;		/* bits 15:8 */
	uint32_t efficient;		/* bits 23:16, most efficient */
	uint32_t lowest;		/* bits 31:24 */
};

/*
 * Decodes IA32_HWP_CAPABILITIES.
 *
 * Input: raw, the register as read.
 * Output: *capabilities, the four fields as they are; no ordering between
 * them is checked here (cpux86_hwp_request_encode() checks what it needs).
 * Errors: EINVAL for a NULL pointer or a set reserved bit (63:32).
 * Specification: Intel SDM Vol. 3B rev. 093, §17.4.3, Figure 17-5.
 */
int
cpux86_hwp_capabilities_decode(
	uint64_t raw,
	struct cpux86_hwp_capabilities *capabilities);

/*
 * The valid bits of IA32_HWP_REQUEST (63:59), as bits of
 * struct cpux86_hwp_request.valid: bit 0 is bit 59 of the register.
 * They matter only when cpux86_hwp_source() says CPUX86_HWP_SOURCE_PACKAGE,
 * and exist only when CPUID leaf 06H EAX[17] (flexible HWP) is set.
 *
 * Values referenced: Intel SDM Vol. 3B rev. 093, §17.4.4.1, Figure 17-6.
 */
#define CPUX86_HWP_VALID_WINDOW		0x01U	/* bit 59 */
#define CPUX86_HWP_VALID_EPP		0x02U	/* bit 60 */
#define CPUX86_HWP_VALID_DESIRED	0x04U	/* bit 61 */
#define CPUX86_HWP_VALID_MAXIMUM	0x08U	/* bit 62 */
#define CPUX86_HWP_VALID_MINIMUM	0x10U	/* bit 63 */

/*
 * One IA32_HWP_REQUEST value, field by field.
 *
 * minimum, maximum and desired are performance levels in the units of
 * IA32_HWP_CAPABILITIES; desired 0 hands the choice to the hardware.
 * preference is the energy-performance preference, 0 (performance) ..
 * 255 (energy).  window is the raw 10-bit activity window field
 * (cpux86_hwp_window_encode() makes it); 0 lets the hardware choose.
 */
struct cpux86_hwp_request {
	uint32_t minimum;		/* bits 7:0 */
	uint32_t maximum;		/* bits 15:8 */
	uint32_t desired;		/* bits 23:16 */
	uint32_t preference;		/* bits 31:24 */
	uint32_t window;		/* bits 41:32 */
	uint32_t package_control;	/* bit 42, 0 or 1 */
	uint32_t valid;			/* bits 63:59, CPUX86_HWP_VALID_* */
};

/*
 * Builds an IA32_HWP_REQUEST value.
 *
 * Input: request, the fields; capabilities, the levels the logical
 * processor reported just before, or NULL to check only the field widths.
 * Output: *raw, the register value with the reserved bits 58:43 zero.
 * Checks, all before anything is written:
 * - every field fits its width: minimum, maximum, desired and preference
 *   0..255, window 0..1023, package_control 0..1, valid 0..31;
 * - with capabilities: lowest <= minimum <= maximum <= highest, and
 *   desired is 0 or lowest <= desired <= highest.
 * The processor itself clips fields it does not support; the decoder does
 * not know which (the CPUID features say) and does not clear them.
 * Errors: EINVAL for a NULL request or raw, or any failed check; *raw
 * unchanged.
 * Specification: Intel SDM Vol. 3B rev. 093, §17.4.4.1, Figure 17-6 and
 * the field descriptions after it.
 */
int
cpux86_hwp_request_encode(
	const struct cpux86_hwp_request *request,
	const struct cpux86_hwp_capabilities *capabilities,
	uint64_t *raw);

/*
 * Decodes an IA32_HWP_REQUEST or IA32_HWP_REQUEST_PKG value.
 *
 * Input: raw, the register as read (a read returns the last value written,
 * not the clipped one).
 * Output: *request, every field as it is.  For IA32_HWP_REQUEST_PKG bits
 * 63:42 are reserved (Figure 17-7): this decoder does not check them there,
 * and the caller ignores package_control and valid of a package value.
 * Errors: EINVAL for a NULL pointer or a set reserved bit (58:43).
 * Specification: Intel SDM Vol. 3B rev. 093, §17.4.4.1 and §17.4.4.2.
 */
int
cpux86_hwp_request_decode(
	uint64_t raw,
	struct cpux86_hwp_request *request);

/* The longest activity window the field can say, in microseconds (127 * 10^7). */
#define CPUX86_HWP_WINDOW_MAX_US	1270000000U

/*
 * Encodes an activity window for IA32_HWP_REQUEST[41:32].
 *
 * The field is a 7-bit mantissa (bits 38:32 of the register, 6:0 of the
 * field) and a 3-bit power-of-ten exponent (41:39, 9:7 of the field); the
 * window is mantissa * 10^exponent microseconds.  A window of 0 is the
 * field 0, which lets the hardware choose.
 * Input: microseconds, 0..CPUX86_HWP_WINDOW_MAX_US.
 * Output: *field, the encoding of the largest representable window that is
 * not above microseconds; when several encodings say that window, the
 * one with the smallest exponent (so 100 is mantissa 100 exponent 0, 128
 * becomes 127: mantissa 127 exponent 0, and 1275 becomes 1270: mantissa
 * 127 exponent 1).  For 1..CPUX86_HWP_WINDOW_MAX_US the result is never
 * 0.
 * Errors: EINVAL for a NULL pointer; ERANGE above the maximum; *field
 * unchanged.
 * Specification: Intel SDM Vol. 3B rev. 093, §17.4.4.1 (Activity_Window).
 */
int
cpux86_hwp_window_encode(
	uint32_t microseconds,
	uint32_t *field);

/*
 * Decodes an activity window field.
 *
 * Input: field, the 10-bit field (bits 41:32 of the register shifted down).
 * Output: *microseconds, mantissa * 10^exponent.
 * Errors: EINVAL for a NULL pointer or a field above 1023.
 * Specification: Intel SDM Vol. 3B rev. 093, §17.4.4.1 (Activity_Window).
 */
int
cpux86_hwp_window_decode(
	uint32_t field,
	uint32_t *microseconds);

/*
 * Which request register the processor takes a logical processor's HWP
 * values from, by IA32_HWP_CTL bit 0 and IA32_HWP_REQUEST bit 42.
 *
 * Values referenced: Intel SDM Vol. 3B rev. 093, §17.4.4.4, Table 17-2.
 */
#define CPUX86_HWP_SOURCE_THREAD	1U	/* IA32_HWP_REQUEST */
#define CPUX86_HWP_SOURCE_PACKAGE	2U	/* IA32_HWP_REQUEST_PKG, with the valid bits' overrides */

/*
 * Reports which request register is in force.
 *
 * Input: control_bit, IA32_HWP_CTL bit 0 (0 when the register does not
 * exist); package_control, IA32_HWP_REQUEST bit 42.
 * Output: *source, CPUX86_HWP_SOURCE_THREAD when the two bits are equal,
 * CPUX86_HWP_SOURCE_PACKAGE when they differ.
 * Errors: EINVAL for a NULL pointer or a bit above 1.
 * Specification: Intel SDM Vol. 3B rev. 093, §17.4.4.4, Table 17-2.
 */
int
cpux86_hwp_source(
	uint32_t control_bit,
	uint32_t package_control,
	uint32_t *source);

/*
 * The events of IA32_HWP_STATUS, as bits of the register itself.  Each is
 * sticky and cleared by writing 0 to it.
 *
 * Values referenced: Intel SDM Vol. 3B rev. 093, §17.4.5, Figure 17-9.
 */
#define CPUX86_HWP_STATUS_GUARANTEED	0x01U	/* bit 0, guaranteed performance changed */
#define CPUX86_HWP_STATUS_EXCURSION	0x04U	/* bit 2, excursion to minimum */
#define CPUX86_HWP_STATUS_HIGHEST	0x08U	/* bit 3, highest performance changed */
#define CPUX86_HWP_STATUS_PECI_ENTRY	0x10U	/* bit 4, PECI override started */
#define CPUX86_HWP_STATUS_PECI_EXIT	0x20U	/* bit 5, PECI override ended */

/* The bits of IA32_HWP_STATUS that the table above defines. */
#define CPUX86_HWP_STATUS_DEFINED	0x3DU

/*
 * Decodes IA32_HWP_STATUS.
 *
 * Input: raw, the register as read.
 * Output: *events, the CPUX86_HWP_STATUS_* bits that are set.
 * Errors: EINVAL for a NULL pointer or a set reserved bit (1 and 63:6).
 * Specification: Intel SDM Vol. 3B rev. 093, §17.4.5, Figure 17-9.
 */
int
cpux86_hwp_status_decode(
	uint64_t raw,
	uint32_t *events);

/*
 * Builds the IA32_HWP_STATUS value that acknowledges some events.
 *
 * Writing 0 to an event bit clears it and writing 1 leaves it, so the
 * value has 0 at the events to clear, 1 at the other defined events, and 0
 * at every reserved bit.
 * Input: events, CPUX86_HWP_STATUS_* bits to clear.
 * Output: *raw.
 * Errors: EINVAL for a NULL pointer or a bit outside
 * CPUX86_HWP_STATUS_DEFINED; *raw unchanged.
 * Specification: Intel SDM Vol. 3B rev. 093, §17.4.5 (RWC0 bits).
 */
int
cpux86_hwp_status_ack(
	uint32_t events,
	uint64_t *raw);

/*
 * The capabilities a feedback table cell can hold, by their index in a
 * class's cells: the index is the bit's position in CPUID leaf 06H
 * EDX[7:0] counted among the enumerated bits.  Bits 0 and 1 are always
 * enumerated together, so performance and efficiency are the first two.
 *
 * Values referenced: Intel SDM Vol. 1 rev. 093, Table 21-16 (EDX[7:0]);
 * Vol. 3B rev. 093, Table 17-7 note 4 and Table 17-9.
 */
#define CPUX86_FEEDBACK_CAP_PERFORMANCE	0U
#define CPUX86_FEEDBACK_CAP_EFFICIENCY	1U

/*
 * The size of one page of the feedback table.
 *
 * Value referenced: Intel SDM Vol. 1 rev. 093, Table 21-16 (EDX[11:8], 4 KB
 * pages).
 */
#define CPUX86_FEEDBACK_PAGE_SIZE	4096U

/*
 * The layout of one package's hardware feedback table.
 *
 * The table is a header followed by one entry (row) per logical processor
 * row index.  The header is an 8-byte timestamp and one change-flag byte
 * per class and capability, padded to a multiple of 8 bytes; a row is one
 * byte per class and capability, padded the same way.  The cell of class
 * c, capability p in row r is at header_size + r * row_size +
 * capabilities * c + p.  Without Thread Director the table has one class
 * and is the plain feedback table (16-byte header, 8-byte rows for two
 * capabilities).
 *
 * The number of classes is the processor's enumeration, not what the
 * driver enables: a processor with Thread Director lays the table out for
 * all its classes and fills only class 0 while Thread Director is off.
 */
struct cpux86_feedback_layout {
	uint32_t capabilities;		/* CP: enumerated bits of leaf 06H EDX[7:0] */
	uint32_t classes;		/* CL: leaf 06H ECX[15:8] with Thread Director, else 1 */
	uint32_t header_size;		/* 8 + CP * CL rounded up to a multiple of 8 */
	uint32_t row_size;		/* CP * CL rounded up to a multiple of 8 */
	uint32_t table_size;		/* feedback_pages * 4096, the memory to provide */
	uint32_t row_count;		/* rows that fit: (table_size - header_size) / row_size */
};

/*
 * Works out the layout of the feedback table from leaf 06H.
 *
 * Input: power, decoded from leaf 06H.  The layout has
 * power->feedback_classes classes when CPUX86_POWER_THREAD_DIRECTOR is
 * set, otherwise one.
 * The number of capabilities is the number of set bits of
 * feedback_capabilities (EDX[7:0]); bits other than 0 and 1 are accepted
 * and counted, so a capability added later only widens the cells.
 * Output: *layout.
 * Errors, *layout unchanged, checked in this order:
 * - EINVAL: a NULL pointer.
 * - ENOTSUP: CPUX86_POWER_HW_FEEDBACK is clear, or feedback_capabilities
 *   is 0 (nothing else is looked at then).
 * - EINVAL: bit 0 or bit 1 of feedback_capabilities clear (the
 *   specification says both are always set); Thread Director with 0
 *   classes; a header and one row that do not fit the table.
 * Specification: Intel SDM Vol. 3B rev. 093, §17.6.1 (Table 17-4 ..
 * Table 17-6) and §17.6.2 (Table 17-7 and its notes 1 to 6, Table 17-8,
 * Table 17-9); Vol. 1 Table 21-16 (leaf 06H).
 */
int
cpux86_feedback_layout(
	const struct cpux86_power *power,
	struct cpux86_feedback_layout *layout);

/*
 * Reads the timestamp of the last hardware update of a feedback table.
 *
 * Input: table, the table's memory, table_size bytes of it readable;
 * layout, from cpux86_feedback_layout().
 * Output: *timestamp, the 8 bytes at offset 0 as a little-endian count of
 * crystal clock ticks.
 * Errors: EINVAL for a NULL pointer, or table_size below 8 or below
 * layout->header_size.
 * Specification: Intel SDM Vol. 3B rev. 093, Table 17-5 and Table 17-8.
 */
int
cpux86_feedback_timestamp(
	const uint8_t *table,
	size_t table_size,
	const struct cpux86_feedback_layout *layout,
	uint64_t *timestamp);

/*
 * The layout given to the readers below is the caller's data and is not
 * trusted: every offset is computed with overflow checks, and an offset
 * that would overflow, or a byte at or beyond table_size, is EINVAL.  A
 * forged layout can make a reader return a wrong byte inside the table,
 * never a byte outside it.
 */

/*
 * The meaning of a change-flag byte of the feedback table header.
 *
 * Values referenced: Intel SDM Vol. 3B rev. 093, Table 17-5 and
 * Table 17-8.
 */
#define CPUX86_FEEDBACK_CHANGED		0x1U	/* bit 0, the capability changed for some row */
#define CPUX86_FEEDBACK_FORCE_IDLE	0x2U	/* bit 1, the hardware asks for idle on some rows */

/*
 * Reads one change-flag byte of the feedback table header.
 *
 * Input: table and table_size as above; layout; class_index, below
 * layout->classes; capability, below layout->capabilities.
 * Output: *flags, the byte at offset 8 + capabilities * class_index +
 * capability.
 * Errors: EINVAL for a NULL pointer, an index out of range, or a byte
 * beyond table_size; *flags unchanged.
 * Specification: Intel SDM Vol. 3B rev. 093, Table 17-8 and Table 17-7
 * note 1.
 */
int
cpux86_feedback_change(
	const uint8_t *table,
	size_t table_size,
	const struct cpux86_feedback_layout *layout,
	uint32_t class_index,
	uint32_t capability,
	uint32_t *flags);

/*
 * Reads one cell of the feedback table.
 *
 * Input: table and table_size as above; layout; row, a logical
 * processor's row index (leaf 06H EDX[31:16]), below layout->row_count;
 * class_index and capability as above.
 * Output: *value, 0..255; 0 means the hardware recommends not running
 * work of that class there for that capability's reason.
 * Errors: EINVAL for a NULL pointer, an index out of range, or a cell
 * beyond table_size; *value unchanged.
 * Specification: Intel SDM Vol. 3B rev. 093, Table 17-6, Table 17-9 and
 * Table 17-7 note 3.
 */
int
cpux86_feedback_cell(
	const uint8_t *table,
	size_t table_size,
	const struct cpux86_feedback_layout *layout,
	uint32_t row,
	uint32_t class_index,
	uint32_t capability,
	uint32_t *value);

/*
 * Builds the IA32_HW_FEEDBACK_PTR value for a table.
 *
 * Input: physical, the physical address of the table's first page;
 * physical_bits, MAXPHYADDR (leaf 80000008H EAX[7:0]), 12..52.
 * Output: *raw, the page frame address with bit 0 (valid) set and bits
 * 11:1 zero.
 * Errors: EINVAL for a NULL pointer, physical_bits outside 12..52, an
 * address that is not 4 KiB aligned, or one at or above 2^physical_bits;
 * *raw unchanged.
 * Specification: Intel SDM Vol. 3B rev. 093, §17.6.4.
 */
int
cpux86_feedback_pointer_encode(
	uint64_t physical,
	uint32_t physical_bits,
	uint64_t *raw);

/*
 * Builds the IA32_HW_FEEDBACK_CONFIG value.
 *
 * Input: enable, 1 to enable the feedback interface; thread_director, 1 to
 * enable Thread Director's classes as well; supported, 1 when CPUID leaf
 * 06H EAX[23] (Thread Director) is set.
 * Output: *raw, bit 0 = enable and bit 1 = thread_director.
 * Errors, *raw unchanged, checked in this order: EINVAL for a NULL
 * pointer, an argument above 1, or thread_director without enable (the
 * hardware ignores that combination); then ENOTSUP for thread_director
 * without support (the hardware faults).
 * Specification: Intel SDM Vol. 3B rev. 093, §17.6.5, Table 17-10.
 */
int
cpux86_feedback_config_encode(
	uint32_t enable,
	uint32_t thread_director,
	uint32_t supported,
	uint64_t *raw);

/*
 * IA32_PACKAGE_THERM_STATUS bit 26: the hardware updated the feedback table.
 *
 * Value referenced: Intel SDM Vol. 3B rev. 093, §17.6.6; Vol. 4 rev. 093,
 * Table 2-2 (IA32_PACKAGE_THERM_STATUS).
 */
#define CPUX86_PACKAGE_THERM_FEEDBACK	26U

/*
 * Reports whether the hardware has updated the feedback table.
 *
 * Input: raw, IA32_PACKAGE_THERM_STATUS as read.
 * Output: *updated, bit 26, 0 or 1.
 * Errors: EINVAL for a NULL pointer.
 * Specification: Intel SDM Vol. 3B rev. 093, §17.6.6; Vol. 4 Table 2-2
 * (IA32_PACKAGE_THERM_STATUS).
 */
int
cpux86_feedback_updated(
	uint64_t raw,
	uint32_t *updated);

/*
 * Builds the IA32_PACKAGE_THERM_STATUS value that hands the table back to
 * the hardware.
 *
 * The value has 0 at bit 26 (clears it), 1 at the log bits 3, 5, 7, 9 and
 * 11 (R/WC0 in both volumes: writing 1 keeps them, so an event logged
 * between the read and the write is not lost), bit 1 as it was read
 * (Vol. 4 calls it R/W and Vol. 3B R/WC0; writing back what was read is
 * neutral under both), and 0 everywhere else (status bits ignore writes,
 * reserved bits must be written as 0).  The driver copies the table
 * before it writes this; until bit 26 is cleared the hardware does not
 * update the table again.
 * Input: raw, the register as just read; only its bit 1 is used.
 * Output: *value, as above.
 * Errors: EINVAL for a NULL pointer.
 * Specification: Intel SDM Vol. 3B rev. 093, §17.6.6, §17.6.7 and the
 * IA32_PACKAGE_THERM_STATUS figure of §17.8 (Figure 17-33); Vol. 4
 * Table 2-2 (IA32_PACKAGE_THERM_STATUS, bits and their access).
 */
int
cpux86_feedback_ack(
	uint64_t raw,
	uint64_t *value);

#endif

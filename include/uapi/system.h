/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * /dev/system
 */

#ifndef KERN_UAPI_SYSTEM_H
#define KERN_UAPI_SYSTEM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <uapi/ioctl.h>

#define KERN_SYSTEM_IOC_GROUP 's'
#define KERN_SYSTEM_SWAP_PAGE_SIZE 4096U

struct system_info {
	uint32_t boot_bios_id;
	uint32_t device_count;
	uint32_t partition_count;
	uint32_t reserved;
};

struct system_device_info {
	uint32_t index;
	uint32_t device_class;
	uint32_t flags;
	uint32_t bios_id;
	uint32_t display_index;
	uint32_t heads;
	uint32_t sectors;
	uint32_t reserved;
};

struct vm_statistics {
	uint64_t physical_total, physical_reserved, physical_allocated,
	    physical_free;
	uint64_t image, heap_fixed, heap_current, heap_peak;
	uint64_t heap_largest_free, heap_largest_failed;
	uint64_t kern_tasks, kern_task_stack_bytes, kern_spaces, kern_page_tables;
	uint64_t vm_resident, vm_anonymous, vm_file, vm_wired, vm_busy,
	    vm_dirty;
	uint64_t vm_clean, vm_swapped, vm_faults, vm_page_in, vm_page_out;
	uint64_t vm_reclaims, vm_io_errors, swap_total, swap_free, swap_extents;
	uint64_t vm_commit_limit, vm_commit_used, vm_commit_available;
};

/*
 * Debug/validation snapshot.  Counts are live kernel objects, not capacity.
 */
struct system_resource_info {
	uint64_t process, thread, filedesc, file, pipe;
	uint64_t mount, inode, namecache;
	uint64_t vmspace, vm_object, vm_page, swap_slot;
	uint64_t disk, bio, socket, packet, net_device;
};

#define KERN_SYSTEM_PROCESS_COMMAND_MAX 64U
struct process_info {
	int32_t pid;
	int32_t ppid;
	uint32_t uid;
	uint32_t state;
	uint32_t threads;
	uint32_t gid;
	uint64_t virtual_bytes;
	char command[KERN_SYSTEM_PROCESS_COMMAND_MAX];
	uint32_t version;
	uint32_t struct_size;
	int32_t process_group;
	int32_t session;
	int32_t nice_value;
	uint32_t has_controlling_terminal;
	/* CPU times in 1/KERN_PROCESS_TIMES_HZ of a second (<uapi/process.h>). */
	uint64_t cpu_ticks;
	uint64_t user_ticks;
	uint64_t system_ticks;
	uint64_t reserved[4];
};

#define KERN_SYSTEM_PROCESS_INFO_VERSION 1U

/*
 * A process's command line (BUG-274): what exec gave it, the arguments
 * after argv[0] each behind one space, cut to fit with the terminating NUL
 * (KERN_SYSTEM_GET_PROCESS_ARGUMENTS); the title it set instead, when it set
 * one (setproctitle).  The caller names the pid; the record carries the
 * version and size of the answer.  Another identity's process answers an
 * empty line, as KERN_SYSTEM_GET_PROCESS hides its command.
 */
#define KERN_SYSTEM_PROCESS_ARGUMENTS_MAX 256U
#define KERN_SYSTEM_PROCESS_ARGUMENTS_VERSION 1U
struct system_process_arguments {
	int32_t pid;
	uint32_t version;
	uint32_t struct_size;
	uint32_t reserved;
	char arguments[KERN_SYSTEM_PROCESS_ARGUMENTS_MAX];
};

#define KERN_SYSTEM_FILE_USAGE_VERSION 1U
#define KERN_SYSTEM_FILE_USAGE_PATH_MAX 256U
#define KERN_SYSTEM_FILE_USAGE_QUERY_MOUNT 0x00000001U
#define KERN_SYSTEM_FILE_USAGE_CWD 0x00000001U
#define KERN_SYSTEM_FILE_USAGE_ROOT 0x00000002U
#define KERN_SYSTEM_FILE_USAGE_EXECUTABLE 0x00000004U
#define KERN_SYSTEM_FILE_USAGE_OPEN 0x00000008U
#define KERN_SYSTEM_FILE_USAGE_MAPPED 0x00000010U
#define KERN_SYSTEM_FILE_USAGE_SOCKET 0x00000020U

struct system_file_usage {
	uint32_t version;
	uint32_t struct_size;
	int32_t cursor_pid;
	int32_t pid;
	uint32_t uid;
	uint32_t usage_flags;
	uint32_t query_flags;
	uint32_t reserved0;
	char path[KERN_SYSTEM_FILE_USAGE_PATH_MAX];
	uint64_t reserved[4];
};

/*
 * Runtime swap control is a zedBSD extension.  The structures deliberately
 * contain no pointer-sized fields so one request number and one layout serve
 * both ILP32 and LP64 processes.
 */
#define KERN_SYSTEM_SWAP_VERSION 1U
#define KERN_SYSTEM_SWAP_SOURCE_COUNT 4U
#define KERN_SYSTEM_SWAP_SOURCE_MAX 256U
#define KERN_SYSTEM_SWAP_UUID_SIZE 8U
#define KERN_SYSTEM_SWAP_LABEL_SIZE 20U

#define KERN_SYSTEM_SWAP_STATE_INACTIVE 0U
#define KERN_SYSTEM_SWAP_STATE_ACTIVE 1U
#define KERN_SYSTEM_SWAP_STATE_DRAINING 2U

struct system_swap_control {
	uint32_t version;
	uint32_t struct_size;
	uint32_t flags;
	uint32_t reserved0;
	char source[KERN_SYSTEM_SWAP_SOURCE_MAX];
	uint32_t reserved[8];
} __attribute__((aligned(4)));

struct system_swap_source_info {
	uint32_t version;
	uint32_t struct_size;
	uint32_t flags;
	uint32_t source_id;
	uint32_t state;
	uint32_t header_version;
	uint32_t total_pages;
	uint32_t used_pages;
	uint8_t uuid[KERN_SYSTEM_SWAP_UUID_SIZE];
	char label[KERN_SYSTEM_SWAP_LABEL_SIZE];
	char source[KERN_SYSTEM_SWAP_SOURCE_MAX];
	uint32_t reserved[8];
} __attribute__((aligned(4)));

_Static_assert(sizeof(struct system_swap_control) == 304U,
    "runtime swap control ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct system_swap_control, source) == 16U,
    "runtime swap source selector offset is an ABI contract");
_Static_assert(sizeof(struct system_swap_source_info) == 348U,
    "runtime swap source ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct system_swap_source_info, source) == 60U,
    "runtime swap diagnostic source offset is an ABI contract");

/*
 * One PCI function, found by its position in the kernel's enumeration.
 *
 * The caller sets index and asks for 0, 1, 2 and so on until the request
 * fails with ENOENT.  A machine without PCI answers ENOENT at once.  Names
 * for the vendor, device and class numbers are not in the kernel; a program
 * that wants them brings its own table.  driver is the name of the driver
 * bound to the function, or empty when none is.  Every field has a fixed
 * width, so one layout serves ILP32 and LP64 processes.
 */
#define KERN_SYSTEM_PCI_DRIVER_NAME_MAX 32U

struct system_pci_device_info {
	uint32_t index;
	uint16_t segment;
	uint8_t bus;
	uint8_t device;
	uint8_t function;
	uint8_t revision;
	uint8_t base_class;
	uint8_t subclass;
	uint8_t programming_interface;
	uint8_t header_type;
	uint16_t reserved0;
	uint16_t vendor;
	uint16_t product;
	uint16_t subvendor;
	uint16_t subproduct;
	char driver[KERN_SYSTEM_PCI_DRIVER_NAME_MAX];
	uint32_t reserved[4];
};

_Static_assert(sizeof(struct system_pci_device_info) == 72U,
    "PCI device description ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct system_pci_device_info, vendor) == 16U,
    "PCI device identity offset is an ABI contract");
_Static_assert(offsetof(struct system_pci_device_info, driver) == 24U,
    "PCI device driver name offset is an ABI contract");

/*
 * One USB device, found by its position in the kernel's enumeration.
 *
 * Asked for the same way as a PCI function: index 0, 1, 2 and so on until
 * ENOENT.  Root hubs are listed too, flagged KERN_SYSTEM_USB_ROOT_HUB, with
 * address 0.  port_path holds the port on each hub from the root hub down,
 * port_depth of them; a device on a root port has depth 1.  The descriptor
 * fields are the device's own.  driver names the drivers bound to its
 * interfaces, separated by commas, or is empty when none is; a list too long
 * for the field is cut short.  Every field has a fixed width, so one layout
 * serves ILP32 and LP64 processes.
 */
#define KERN_SYSTEM_USB_DRIVER_NAME_MAX 32U
#define KERN_SYSTEM_USB_PORT_DEPTH_MAX 7U
#define KERN_SYSTEM_USB_ROOT_HUB 0x01U

#define KERN_SYSTEM_USB_SPEED_UNKNOWN 0U
#define KERN_SYSTEM_USB_SPEED_LOW 1U
#define KERN_SYSTEM_USB_SPEED_FULL 2U
#define KERN_SYSTEM_USB_SPEED_HIGH 3U
#define KERN_SYSTEM_USB_SPEED_SUPER 4U
#define KERN_SYSTEM_USB_SPEED_SUPER_PLUS 5U

struct system_usb_device_info {
	uint32_t index;
	uint16_t bus;
	uint8_t address;
	uint8_t speed;
	uint8_t port_depth;
	uint8_t port_path[KERN_SYSTEM_USB_PORT_DEPTH_MAX];
	uint16_t vendor;
	uint16_t product;
	uint16_t usb_version;
	uint16_t device_version;
	uint8_t device_class;
	uint8_t device_subclass;
	uint8_t device_protocol;
	uint8_t configuration_count;
	uint8_t interface_count;
	uint8_t flags;
	uint16_t reserved0;
	char driver[KERN_SYSTEM_USB_DRIVER_NAME_MAX];
	uint32_t reserved[4];
};

_Static_assert(sizeof(struct system_usb_device_info) == 80U,
    "USB device description ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct system_usb_device_info, vendor) == 16U,
    "USB device identity offset is an ABI contract");
_Static_assert(offsetof(struct system_usb_device_info, driver) == 32U,
    "USB device driver name offset is an ABI contract");

#define KERN_SYSTEM_GET_INFO                                                 \
	_IOR(KERN_SYSTEM_IOC_GROUP, 1, struct system_info)
#define KERN_SYSTEM_GET_DEVICE                                               \
	_IOWR(KERN_SYSTEM_IOC_GROUP, 2, struct system_device_info)
#define KERN_SYSTEM_GET_VMSTAT                                               \
	_IOR(KERN_SYSTEM_IOC_GROUP, 3, struct vm_statistics)
#define KERN_SYSTEM_HALT _IO(KERN_SYSTEM_IOC_GROUP, 4)
#define KERN_SYSTEM_REBOOT _IO(KERN_SYSTEM_IOC_GROUP, 5)
/* Discards every idle object the page cache keeps, so counts return to a baseline. */
#define KERN_SYSTEM_DROP_CACHES _IO(KERN_SYSTEM_IOC_GROUP, 15)
#define KERN_SYSTEM_GET_RESOURCES                                            \
	_IOR(KERN_SYSTEM_IOC_GROUP, 6, struct system_resource_info)
#define KERN_SYSTEM_GET_PROCESS                                              \
	_IOWR(KERN_SYSTEM_IOC_GROUP, 7, struct process_info)
#define KERN_SYSTEM_GET_FILE_USAGE                                           \
	_IOWR(KERN_SYSTEM_IOC_GROUP, 8, struct system_file_usage)
#define KERN_SYSTEM_SWAP_ADD                                                 \
	_IOW(KERN_SYSTEM_IOC_GROUP, 9, struct system_swap_control)
#define KERN_SYSTEM_SWAP_REMOVE                                              \
	_IOW(KERN_SYSTEM_IOC_GROUP, 10, struct system_swap_control)
#define KERN_SYSTEM_GET_SWAP_SOURCE                                          \
	_IOWR(KERN_SYSTEM_IOC_GROUP, 11, struct system_swap_source_info)
/* Number 12 is KERN_SYSTEM_GET_MOUNTS in <uapi/mountinfo.h>. */
#define KERN_SYSTEM_GET_PCI_DEVICE                                           \
	_IOWR(KERN_SYSTEM_IOC_GROUP, 13, struct system_pci_device_info)
#define KERN_SYSTEM_GET_USB_DEVICE                                           \
	_IOWR(KERN_SYSTEM_IOC_GROUP, 14, struct system_usb_device_info)
/*
 * Turns the machine's power off (ACPI S5 on a PC), after the same
 * preparation as KERN_SYSTEM_HALT.  Only init may ask.  A platform that
 * cannot cut its own power answers EOPNOTSUPP, and the caller halts instead.
 */
#define KERN_SYSTEM_POWEROFF _IO(KERN_SYSTEM_IOC_GROUP, 16)
#define KERN_SYSTEM_GET_PROCESS_ARGUMENTS                                    \
	_IOWR(KERN_SYSTEM_IOC_GROUP, 20, struct system_process_arguments)

/*
 * The system's events (ws132-p002).  An open of /dev/system names the
 * classes of events it wants with KERN_SYSTEM_EVENT_SUBSCRIBE before it
 * reads; read() then gives whole struct system_event records of those
 * classes (a buffer too small for one is refused with EINVAL; an open
 * that has not subscribed is refused with EINVAL), waits for one unless
 * the file is non-blocking (EAGAIN), and poll() is readable while one
 * waits.  Each open keeps up to KERN_SYSTEM_EVENT_QUEUE records: when they
 * overflow the oldest go, and the next read starts with an OVERFLOW event
 * whose value is how many went, after which the reader asks for the
 * present state again (KERN_SYSTEM_GET_POWER, the device lists).
 */
#define KERN_SYSTEM_EVENT_POWER		0x00000001U	/* the power and sleep buttons */
#define KERN_SYSTEM_EVENT_LID		0x00000002U	/* the lid opened or closed */
#define KERN_SYSTEM_EVENT_AC		0x00000004U	/* the AC adapter plugged or unplugged */
#define KERN_SYSTEM_EVENT_BATTERY	0x00000008U	/* a battery's state or information */
#define KERN_SYSTEM_EVENT_DISK		0x00000010U	/* a disk or partition added or removed */
#define KERN_SYSTEM_EVENT_INPUT		0x00000020U	/* an input device added or removed */
#define KERN_SYSTEM_EVENT_NETWORK	0x00000040U	/* a network interface added or removed */
#define KERN_SYSTEM_EVENT_USB		0x00000080U	/* a USB device attached or detached */
#define KERN_SYSTEM_EVENT_OVERFLOW	0x80000000U	/* events were lost (always delivered) */
#define KERN_SYSTEM_EVENT_CLASSES	0x000000ffU

/* What happened. */
#define KERN_SYSTEM_EVENT_ADD		1U
#define KERN_SYSTEM_EVENT_REMOVE	2U
#define KERN_SYSTEM_EVENT_CHANGE	3U
#define KERN_SYSTEM_EVENT_PRESS		4U

/* The records each open keeps, and the sizes of a record's texts. */
#define KERN_SYSTEM_EVENT_QUEUE		64U
#define KERN_SYSTEM_EVENT_SUBJECT_MAX	32U
#define KERN_SYSTEM_EVENT_DETAIL_MAX	64U

/* The classes an open subscribes to; the reserved words are zero. */
struct system_event_subscription {
	uint32_t classes;
	uint32_t reserved[3];
};

/*
 * One event: its size (sizeof the record), class and action, a value (the
 * lid 1 open, the AC 1 plugged, a battery's percent, the events lost), the
 * kernel's sequence number (one more for each event posted), the time it
 * was posted (CLOCK_MONOTONIC nanoseconds), what it is about ("lid",
 * "power-button", "da0s1", "event5", "ue0", "usb1.3"), and details as
 * space-separated key=value words ("removable=1", "name=...").
 */
struct system_event {
	uint32_t size;
	uint32_t class_bit;
	uint32_t action;
	int32_t value;
	uint64_t sequence;
	uint64_t time_ns;
	char subject[KERN_SYSTEM_EVENT_SUBJECT_MAX];
	char detail[KERN_SYSTEM_EVENT_DETAIL_MAX];
};

_Static_assert(sizeof(struct system_event) == 128U,
    "system event ABI must be identical on ILP32 and LP64");

/*
 * The power's present state: which parts are known (KERN_SYSTEM_POWER_HAS_*),
 * the lid (1 open), the AC adapter (1 plugged), the first battery
 * (percent, 1 charging), and what the machine can do
 * (KERN_SYSTEM_POWER_FLAG_*, ws052-p011: KERN_SYSTEM_POWER_FLAG_CAN_SLEEP
 * when KERN_SYSTEM_SLEEP in the mode KERN_SYSTEM_SLEEP_S0IDLE would not be
 * refused with EOPNOTSUPP).  An unknown part reads zero; a kernel before
 * the flags reads them zero (it cannot sleep).
 */
#define KERN_SYSTEM_POWER_HAS_LID	0x1U
#define KERN_SYSTEM_POWER_HAS_AC	0x2U
#define KERN_SYSTEM_POWER_HAS_BATTERY	0x4U

#define KERN_SYSTEM_POWER_FLAG_CAN_SLEEP	0x1U

struct system_power_info {
	uint32_t known;
	uint32_t lid_open;
	uint32_t ac_online;
	uint32_t battery_percent;
	uint32_t battery_charging;
	uint32_t flags;
	uint32_t reserved[2];
};

_Static_assert(sizeof(struct system_power_info) == 32U,
    "power info ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct system_power_info, flags) == 20U,
    "power info flags offset is an ABI contract");

#define KERN_SYSTEM_EVENT_SUBSCRIBE                                         \
	_IOW(KERN_SYSTEM_IOC_GROUP, 17, struct system_event_subscription)
#define KERN_SYSTEM_GET_POWER                                               \
	_IOR(KERN_SYSTEM_IOC_GROUP, 18, struct system_power_info)

/*
 * Sleep (S0 idle, ws052, docs/architecture/power-management.md).
 * KERN_SYSTEM_SLEEP with the mode KERN_SYSTEM_SLEEP_S0IDLE sleeps to idle
 * until a wake event; KERN_SYSTEM_SLEEP_DEVICES suspends every device and
 * resumes it again at once, without the processors' low-power idle (the
 * test of the devices' suspend and resume).  On input the mode is set and
 * every other field is zero.
 *
 * The ioctl fails only when no attempt was made: EPERM for a caller that
 * is not root, EBUSY while a sleep is under way, EINVAL for an unknown mode
 * or a field that should be zero, EOPNOTSUPP when the platform cannot
 * sleep in that mode (nothing was touched).  Otherwise it succeeds, and
 * result is zero when the machine slept (S0IDLE) or every device was
 * suspended (DEVICES), else the error that stopped it (EBUSY a busy
 * device, EOPNOTSUPP a driver that cannot suspend), with device naming the
 * device that refused ("pci 0000:00:1b.0 hda"); resume_result is zero when
 * every device came back, else the first error of their resume; wake says
 * why the machine woke (S0IDLE only; KERN_SYSTEM_WAKE_NONE in the DEVICES
 * mode and when the machine did not sleep).
 */
#define KERN_SYSTEM_SLEEP_DEVICES	1U	/* suspend and resume the devices only */
#define KERN_SYSTEM_SLEEP_S0IDLE	2U	/* suspend to idle until a wake event */
#define KERN_SYSTEM_SLEEP_DEVICE_MAX	48U

struct system_sleep_request {
	uint32_t mode;
	int32_t result;
	int32_t resume_result;
	uint32_t wake;
	char device[KERN_SYSTEM_SLEEP_DEVICE_MAX];
};

/* Why the machine woke (struct system_sleep_request's wake). */
#define KERN_SYSTEM_WAKE_NONE		0U
#define KERN_SYSTEM_WAKE_POWER_BUTTON	1U
#define KERN_SYSTEM_WAKE_LID		2U
#define KERN_SYSTEM_WAKE_KEYBOARD	3U	/* the embedded controller's keyboard GPE */
#define KERN_SYSTEM_WAKE_USB		4U	/* xHCI or PCIe PME */
#define KERN_SYSTEM_WAKE_AC		5U
#define KERN_SYSTEM_WAKE_TIMER		6U	/* RTC alarm */
#define KERN_SYSTEM_WAKE_SPURIOUS	7U	/* the limit of spurious wakes was reached */
#define KERN_SYSTEM_WAKE_OTHER		8U

_Static_assert(sizeof(struct system_sleep_request) == 64U,
    "sleep request ABI must be identical on ILP32 and LP64");
_Static_assert(offsetof(struct system_sleep_request, device) == 16U,
    "sleep request device offset is an ABI contract");

#define KERN_SYSTEM_SLEEP                                                   \
	_IOWR(KERN_SYSTEM_IOC_GROUP, 19, struct system_sleep_request)

#ifdef __cplusplus
}
#endif

#endif

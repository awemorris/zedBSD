# Power management

This document describes how zedBSD puts a machine to sleep and wakes it,
which part decides what, and the interface programs use. The reference
machine is the Dell Latitude 5330 (Intel Alder Lake-P).

## Scope

zedBSD supports one sleep state: **S0 low-power idle** (S0ix, known as
"modern standby" on Windows and "suspend to idle" on Linux), down to its
deepest level, S0i3. In this state the machine stays in S0. The operating
system quiets every device, parks every processor in its deepest idle state,
and the platform's power controller then lowers the package to SLP_S0.

Not supported:

- S3 (suspend to RAM) and S4 (hibernation);
- runtime power saving of idle devices, beyond what entry to and exit from
  sleep need;
- processor frequency control and thermal management.

## Who decides

| Part | Decides and does |
| --- | --- |
| Keiland (the desktop) | **When** to sleep. It sleeps when the lid closes, when the power button is pressed briefly (no dialog), and after the idle time set in Settings > Power. It tells the user why a sleep could not start. |
| `sessiond` | Asks the kernel to sleep on Keiland's behalf. It is the root process that holds the request. |
| Kernel | **How** to sleep. It checks support, stops user processes, suspends devices, informs the firmware (ACPI LPS0), arms the wake sources and coordinates the processors. After waking it works out why, corrects the clock and reverses every step. The kernel never decides on its own to sleep. |
| HAL | The processor and interrupt-controller mechanism. It picks the idle state, stops and restarts the local tick, quiets processor-local interrupt sources, masks system interrupt routing except the wake sources, and delivers cross-processor wake-ups. It holds no policy. |

The HAL interface for this lives in `include/hal/hal.h`:

- `hal_cpu_idle_suspend_supported()` and `hal_cpu_idle_suspend()`;
- `hal_irq_set_wake()`, `hal_irq_suspend()` and `hal_irq_resume()`;
- the contracts of `hal_cpu_notify()` and `hal_rtc_read_counter()`.

The comments in that header are the contract.

## Entering and leaving sleep

1. **Support check.** Sleep can start only if all three hold: the HAL
   reports a suspend-safe idle state, the firmware declares low-power S0
   idle (FADT), and the LPS0 device exists. Otherwise the request fails
   with `EOPNOTSUPP` before any device is touched. This is the answer on
   QEMU and on other architectures.
2. **Begin.** The event `sleep.begin` is sent. User processes are
   frozen, except the caller.
3. **Devices.** Devices are suspended children first. A device without
   suspend support is stopped and restarted after wake instead; Wi-Fi and
   audio take this path for now. If any device refuses, the devices
   already suspended are resumed, the sleep is abandoned, and the request
   reports the device that refused.
4. **Firmware.** The LPS0 device is told "display off", then "low-power
   entry". Only the wake GPEs stay enabled.
5. **Interrupts.** `hal_irq_suspend()` masks every system interrupt
   except those armed with `hal_irq_set_wake()`, such as the ACPI SCI.
6. **Processors.** Every processor enters `hal_cpu_idle_suspend()` from
   its idle loop. The platform reaches SLP_S0 when all of them are in.
7. **Wake.** A wake interrupt runs its handler as usual. The processor
   that wakes first wakes the requester, and the requester wakes the other
   processors.
8. **Reason.** The kernel decides why it woke from the ACPI events handled
   meanwhile: power button, lid, keyboard, USB, AC adapter or RTC alarm. If
   there is no real reason (for example a battery status query from the
   embedded controller), the wake was spurious and the processors go back
   to idle with everything else still suspended. After a limit of
   consecutive spurious wakes the sleep ends with the reason "spurious".
9. **Clock.** The kernel advances its tick count by the time measured with
   `hal_rtc_read_counter()`. The counter keeps running in every idle state
   the HAL enters. The kernel also compares it with the RTC as a sanity
   check.
10. **Leave.** The kernel reverses the steps: interrupts, firmware
    ("low-power exit", "display on"), devices (parents first) and user
    processes. Then the event `sleep.end` is sent with the reason,
    and the request returns.

If any step fails, the steps already done are undone in reverse order and
`sleep.failed` is sent.

## Devices

| Device | On sleep | On wake |
| --- | --- | --- |
| Display and GPU (i915) | Scanout stops, the display power wells go to their lowest state, the GPU goes to RC6, and the device goes to D3hot. | The display returns to the outputs that were in use before sleep, including Keiland's choices, not to the firmware's boot output. Keiland is told. |
| NVMe | Writes are flushed and the controller is shut down to its deepest power state. | Reinitialized. |
| xHCI (USB) | The controller is stopped, its state saved and PME enabled, so USB devices can wake the machine. | State restored, or the controller is attached again. |
| Wi-Fi, audio | Stopped before sleep (networkd disconnects and turns the radio off; audiod closes streams). | Started again. |
| Embedded controller | Keeps running: it is a wake source. | — |

Required for sleep: display, NVMe and xHCI. Wi-Fi and audio only need to be
stopped.

## Interface: `/dev/system`

Sleep is requested through the same control node as the system events, with
the `KERN_SYSTEM_SLEEP` operation in `include/uapi/system.h`. Only root may
ask (`EPERM`).

```c
#define KERN_SYSTEM_SLEEP_DEVICES	1U	/* suspend and resume the devices only */
#define KERN_SYSTEM_SLEEP_S0IDLE	2U	/* suspend to idle until a wake event */
#define KERN_SYSTEM_SLEEP_DEVICE_MAX	48U

struct system_sleep_request {		/* 64 bytes on ILP32 and LP64 */
	uint32_t mode;
	int32_t result;
	int32_t resume_result;
	uint32_t wake;
	char device[KERN_SYSTEM_SLEEP_DEVICE_MAX];
};

#define KERN_SYSTEM_WAKE_NONE		0U
#define KERN_SYSTEM_WAKE_POWER_BUTTON	1U
#define KERN_SYSTEM_WAKE_LID		2U
#define KERN_SYSTEM_WAKE_KEYBOARD	3U	/* the embedded controller's keyboard GPE */
#define KERN_SYSTEM_WAKE_USB		4U	/* xHCI or PCIe PME */
#define KERN_SYSTEM_WAKE_AC		5U
#define KERN_SYSTEM_WAKE_TIMER		6U	/* RTC alarm */
#define KERN_SYSTEM_WAKE_SPURIOUS	7U	/* the limit of spurious wakes was reached */
#define KERN_SYSTEM_WAKE_OTHER		8U

#define KERN_SYSTEM_SLEEP \
	_IOWR(KERN_SYSTEM_IOC_GROUP, 19, struct system_sleep_request)
```

On input, `mode` is set and every other field is zero.

The ioctl itself fails only when no attempt was made:

- `EPERM`: the caller is not root.
- `EBUSY`: a sleep is already under way.
- `EINVAL`: unknown mode, or a field that should be zero is not.
- `EOPNOTSUPP`: the platform cannot sleep in this mode. Nothing was
  touched.

When an attempt was made, the ioctl succeeds and the fields report what
happened:

| Field | Meaning |
| --- | --- |
| `result` | 0 when the machine slept (S0IDLE) or every device was suspended (DEVICES). Otherwise the error that stopped it, such as `EBUSY` for a busy device or `EOPNOTSUPP` for a driver that cannot suspend. |
| `resume_result` | 0 when every device came back, otherwise the first error of their resume. |
| `wake` | S0IDLE only: why the machine woke, one of `KERN_SYSTEM_WAKE_*`. Always `KERN_SYSTEM_WAKE_NONE` in the DEVICES mode and when the machine did not sleep. |
| `device` | When a device refused: its name, for example `pci 0000:00:1b.0 hda`. Otherwise empty. |

The DEVICES mode keeps its existing meaning: it suspends every device and
resumes it again at once, without the processors' idle. It is the test of
device suspend and resume.

The layout is unchanged from the DEVICES-only version: `wake` takes the
place of the former reserved word. A program built against the earlier
header sends zero there, which remains valid.

### Events

Subscribers of the `KERN_SYSTEM_EVENT_POWER` class on `/dev/system` receive
records with the action `KERN_SYSTEM_EVENT_CHANGE` and these subjects:

| Subject | Detail | When |
| --- | --- | --- |
| `sleep.begin` | empty | Before user processes are frozen. |
| `sleep.end` | `reason=power-button\|lid\|keyboard\|usb\|ac\|timer\|spurious\|other` | After everything is resumed. |
| `sleep.failed` | `device=<name>` | When a sleep was abandoned. |

## Verification

- On hardware, the platform's SLP_S0 residency counter (from the firmware's
  LPIT) increases across a sleep. The machine wakes with the power button
  and the lid, and the reason matches. The session and the network
  continue. The tick counter and the RTC agree across the sleep.
- On QEMU, which has no S0ix, `KERN_SYSTEM_SLEEP_S0IDLE` fails with
  `EOPNOTSUPP` without touching a device, and the DEVICES mode completes a
  suspend and resume of every device.

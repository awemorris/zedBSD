# Power management

Status: current; the Raspberry Pi 5 fan's receiver and boost are planned

This document describes how zedBSD saves power and keeps a machine within
its thermal limits: how it puts a machine to sleep and wakes it, how an
idle processor rests, and how one device, `/dev/thermal`, brings the
temperatures, the fans, the energy meters, the processors' performance and
the machine's strategy together. It says which part decides what and the
interfaces programs use. The reference machine is the Dell Latitude 5330
(Intel Alder Lake-P).

## Scope

Supported:

- one sleep state, **S0 low-power idle** (S0ix, known as "modern standby"
  on Windows and "suspend to idle" on Linux), down to its deepest level,
  S0i3. The machine stays in S0: the operating system quiets every device,
  parks every processor in its deepest idle state, and the platform's
  power controller then lowers the package to SLP_S0;
- processor idle states deeper than HLT (MWAIT C-states on x86);
- processor performance control: Intel HWP, the performance and efficiency
  cores' relative performance, and the processors' idle residency;
- the thermal framework: temperature sensors, fans with a raised lower
  bound ("boost"), energy meters, one strategy for the whole machine,
  passive cooling and an emergency stop at a critical temperature.

Not supported:

- S3 (suspend to RAM) and S4 (hibernation);
- runtime power saving of idle devices, beyond what entry to and exit from
  sleep need;
- writing power limits (the energy meters are read only);
- stopping the tick of an idle processor (the tick is periodic on every
  processor).

## Who decides

| Part | Decides and does |
| --- | --- |
| Keiland (the desktop) | **When** to sleep. It sleeps when the lid closes, when the power button is pressed briefly (no dialog), and after the idle time set in Settings > Power. It tells the user why a sleep could not start. It shows the processor's temperature in the System Monitor. |
| `sessiond` | Asks the kernel to sleep on Keiland's behalf. It is the root process that holds the request. It takes no part in the thermal framework (its role there is only to authenticate the user who logs in). |
| `/sbin/thermal` | Shows the thermal state and lets root and the members of the `desktop` group choose the strategy, raise a fan or set the processors directly. It keeps the chosen strategy and gives it back to the kernel at boot. |
| `init` | Brings the machine down when the kernel reports a thermal emergency. |
| Kernel | **How** to sleep, and the thermal framework: it keeps the copy of every sensor, fan and meter, hands the strategy to the parts that act on it, watches the temperatures, and stops the machine in an emergency. The kernel never decides on its own to sleep, and never picks a strategy other than the one it was given. |
| Drivers | The hardware: they read the sensors, fans and meters and translate a strategy into their own settings (processor performance registers, a firmware's thermal mode, a fan's trip points). |
| HAL | The processor and interrupt-controller mechanism. It picks the idle state, stops and restarts the local tick, quiets processor-local interrupt sources, masks system interrupt routing except the wake sources, and delivers cross-processor wake-ups. It holds no policy. |

The HAL interface for this lives in `include/hal/hal.h`:

- `hal_cpu_idle()`, `hal_cpu_idle_suspend_supported()` and
  `hal_cpu_idle_suspend()`;
- `hal_irq_set_wake()`, `hal_irq_suspend()` and `hal_irq_resume()`;
- the contracts of `hal_cpu_notify()` and `hal_rtc_read_counter()`.

The comments in that header are the contract. The thermal framework uses
only these existing calls; it adds nothing to the HAL.

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

A sleep is refused with `EBUSY` once a thermal emergency has begun, and a
sleep under way is abandoned if one begins before user processes are
frozen.

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

## Processor idle

The kernel's idle loop calls `hal_cpu_idle()`, and the HAL decides how the
processor waits. The kernel neither knows nor names the state it entered.

On x86 the HAL waits with MWAIT in a C-state deeper than C1 when all of
these hold, and with HLT (C1) otherwise:

- the time-stamp counter is invariant, so the HAL's counter keeps its
  contract in every state;
- the local APIC timer keeps running in deep states (ARAT), so a
  processor's timed sleep is not delayed;
- MWAIT ends on an interrupt even while interrupts are masked, so
  `hal_cpu_notify()` always wakes the processor;
- no hypervisor is underneath (a virtual processor's MWAIT exits to the
  hypervisor or is slow).

The HAL also aligns the processors' ticks: in the TSC-deadline mode of the
local APIC every processor's tick falls on a multiple of the tick period
of the same time-stamp counter, so the processors wake together and rest
together, and the package can stay in its deep states between ticks.

The idle residency counters (how long each core and each package spent in
each C-state) are read through `/dev/thermal` (below).

## The thermal framework

```
 user     /sbin/thermal            Keiland's System Monitor
               \                       /
 device         /dev/thermal (root:desktop 0664)   /dev/thermal-inject (tests only)
                      |
 kernel   thermal core: copies, strategy, receivers, meters, events, ioctls
          thermal guard: passive cooling, emergency stop
          CPU performance: topology, domains, requests, relative performance
                      ^ registration (kernel calls only)
 drivers  x86 processor (CPUID, MSRs: HWP, HFI, RAPL, C-state residency)
          ACPI thermal zones (sensors)
          ACPI WMI and the Dell provider (fans, the firmware's thermal mode, boost)
          Raspberry Pi 5 fan (sensor, fan, trip points, boost)
```

The core holds copies and hands out decisions; the drivers read and write
the hardware. The meaning of a strategy is the core's; how a part carries
it out is its driver's.

### Sensors

A driver registers a sensor (a firmware thermal zone, or a device's own
reading) and pushes each reading into the core. A reading outside −40 to
150 °C is no reading; a jump of 20 °C or more from the last value is
dropped once, then taken. A sensor may have a passive level and a critical
level; a level of 0 or of 127 °C and above means none. A sensor that gave
no reading for 30 seconds is marked stale. A sensor that follows the
processors is marked as the processors' (passive cooling acts on it).

### Fans and boost

A driver registers a fan. The core reads it every five seconds: its speed
in rpm where the fan reports one, its level where it has levels, and the
level the automatic control (the firmware's or the driver's) has it at.
A fan whose speed stays 0 for three readings while it should turn is
reported as stalled.

A **boost** sets a lower bound on a fan's level: the fan turns at the
higher of its automatic level and the boost. A boost never lowers a fan
below what the temperature needs.

### Energy meters

A driver registers an energy meter (package, cores, graphics, platform or
memory). The core reads each meter every second and keeps its energy in 64
bits across the hardware counter's wraps. The power over an interval is
the difference of two readings over the difference of their times.

### Strategy

There is one strategy for the whole machine:

| Strategy | Meaning |
| --- | --- |
| `firmware` | Nothing was chosen since boot; every part runs as the firmware left it. It cannot be chosen. |
| `performance` | Speed first. |
| `balanced` | The default after boot. |
| `powersave` | Power and quiet first. |
| `manual` | Three axes, each 0 to 100, where 100 always means more of the thing: `speed`, `noise` (100 allows the loudest fans) and `gating` (100 allows the deepest power gating). |

Each part that acts on the strategy is a **receiver**. Every receiver gets
the same strategy and translates it into its own settings:

| Receiver | What it sets |
| --- | --- |
| `cpu` | Each performance domain's lowest and highest level and its energy-performance preference (HWP). |
| `dell-wmi` | The firmware's thermal mode (UltraPerformance, Optimized, Cool or Quiet; Quiet only for `manual` with a low noise). |
| the Raspberry Pi 5 fan | The fan's trip points, never cooler than the device tree's. |

Choosing a strategy hands it to every receiver, one after another, from a
thread of the core, and waits for them up to five seconds. A choice made
while another is still being handed out supersedes it; every receiver
then gets the newer one. The strategy is kept whatever the receivers
answer, and the answer tells what happened:

- no receiver acts on it here (a virtual machine): `EOPNOTSUPP`;
- every receiver that acts on it failed: the first receiver's error;
- some failed: success, marked partial;
- a later choice replaced it: success, marked superseded;
- the receivers did not all answer in five seconds: success, marked
  pending (the outcome comes later as an event).

A direct setting of the processors (below) marks the strategy as custom
until the next choice. A receiver that registers later gets the strategy
in force.

At boot the kernel chooses nothing. The boot service runs
`/sbin/thermal restore` as root, which hands the kernel the strategy kept
in `/etc/thermal.conf`, or `balanced` when none is kept.

### Processor performance

The kernel's CPU performance part keeps the processors' topology (package,
core, performance or efficiency core), their performance domains, and each
domain's request. A processor driver registers as its provider. On x86 the
driver reads CPUID for the topology and, where the build includes the MSR
path, uses HWP: it turns HWP on at the first request (not at boot), writes
each domain's request on the domain's own processor, and reads the
hardware feedback interface (HFI) and the frequency counters for the
relative performance of the cores. Without the MSR path, or under a
hypervisor, the topology is still there and the controls answer
`EOPNOTSUPP`. On architectures without a provider every processor is its
own core of unknown kind.

The relative performance of the cores (for the scheduler) is measured from
the frequency counters by default, or taken from the HFI or from the
provider. A generation number changes whenever the topology, the
capabilities or the relative performance change.

The core idle residency can be read only on the core's own processor, so
it is read only when asked for (it wakes that processor). The package
residency is read from any processor of the package.

The boot parameters `cpuperf=off` (leave the processor driver out) and
`cpuperf.hwp=off` (never turn HWP on) are diagnostic switches.

### Passive cooling

When a sensor that follows the processors passes its passive level, the
core lowers the processors' highest level by 10 % every five seconds (never
below the guaranteed level) and raises it again once the sensor is 3 °C
below the passive level.

### Emergency stop

The guard thread of the core looks at the sensors every second. An
emergency begins when a firmware thermal zone gives two readings in a row
at its critical level, or goes 30 seconds without a reading after its last
one was at that level. A critical level below 50 °C, or less than 5 °C
above the zone's first reading, is not trusted and stops nothing.

Then:

1. The kernel sends the event and keeps the emergency in the status. From
   now on a reboot or halt request powers the machine off, and a sleep
   request is refused (`EBUSY`); a sleep under way is abandoned.
2. If `init` handles `SIGUSR2`, the kernel sends it. `init` brings the
   machine down at once with short grace periods, ends with a power-off,
   records the emergency in `/var/log/thermal-emergency`, and
   acknowledges it on `/dev/thermal`.
3. If `init` does not acknowledge within 10 seconds, or the power is still
   on after 120 seconds, the kernel cuts it itself: it asks for a sync,
   waits for it up to 10 seconds, and powers the machine off without the
   firmware's preparation steps (on ACPI machines, without `_PTS`). If
   even that fails, it halts.

On machines whose firmware protects itself (the Latitude 5330 has no
critical zone; the processor's own thermal throttling and the firmware
act), the emergency stop does not act. On the Raspberry Pi 5 it acts at
the device tree's critical level.

After a sleep the guard restarts its staleness count, so the time asleep
does not count as time without a reading.

## Interface: `/dev/thermal`

`/dev/thermal` is owned by root and the `desktop` group, mode 0664
(`KERN_THERMAL_NODE_GROUP` and `KERN_THERMAL_NODE_MODE` in
`include/uapi/thermal.h`). The node's mode decides who may change
anything:

- anyone may open it for reading and read every state;
- a change (the strategy, a boost, a direct setting of the processors, the
  choice of the relative performance) and a read that wakes a processor
  (a processor's sample, a core's idle residency) need a descriptor opened
  for writing; on a descriptor opened for reading they fail with `EPERM`;
- root and the `desktop` group's members can open it for writing. The
  default user and the users Settings creates are members.

The ioctls (group `'T'`, `include/uapi/thermal.h`):

| ioctl | What it does |
| --- | --- |
| `KERN_THERMAL_GET_STATUS` | The counts, the strategy in force and its outcome, the passive ceiling and the emergency. |
| `KERN_THERMAL_GET_SENSOR`, `_GET_FAN`, `_GET_METER` | One sensor, fan or meter by index; each answer also gives the count. |
| `KERN_THERMAL_GET_STRATEGY`, `_SET_STRATEGY` | The strategy and its outcome; choosing one. |
| `KERN_THERMAL_GET_RECEIVER` | One receiver and its last answer. |
| `KERN_THERMAL_SET_BOOST` | A fan's (or every fan's) boost level. |
| `KERN_THERMAL_SUBSCRIBE` | The kinds of event this descriptor reads. |
| `KERN_THERMAL_EMERGENCY_ACK` | `init`'s acknowledgement of an emergency (process 1 only). |
| `KERN_THERMAL_GET_CPU`, `_GET_CPU_DOMAIN`, `_SET_CPU_DOMAIN` | A processor, a performance domain, and a direct setting of a domain. |
| `KERN_THERMAL_GET_CPU_RELATIVE`, `_SET_CPU_RELATIVE` | How the cores' relative performance is measured. |
| `KERN_THERMAL_GET_CPU_IDLE`, `_GET_PACKAGE_IDLE` | The idle residency of a core or a package. |

A machine without the thermal core in its kernel has no `/dev/thermal`.

### Events

A descriptor subscribed to some kinds reads 128-byte `struct
thermal_event` records with `read()` and waits for them with `poll()`:

| Event | When |
| --- | --- |
| `STRATEGY` | The receivers answered a strategy. |
| `BOOST` | A boost took effect. |
| `PASSIVE`, `CRITICAL` | A sensor reached its passive or critical level, or left it 3 °C below. |
| `EMERGENCY` | An emergency began. |
| `FAN_STALLED` | A fan stopped while it should turn. |
| `CPU` | The processors' topology, capabilities or relative performance changed. |
| `OVERFLOW` | Records were lost because the reader fell behind (each descriptor keeps 64). |

### `/sbin/thermal`

```
thermal                                   the status
thermal sensors | fans | meters | receivers
thermal strategy [performance | balanced | powersave | manual <speed> <noise> <gating>]
thermal restore                           (the boot service)
thermal boost <fan|all> <level>
thermal cpu [<n> [sample] | domains | set <domain|all> [min=N] [max=N] [desired=N] [epp=N]]
thermal cpu relative [frequency | feedback | provider]
thermal idle [cpu <n> | package <n>] [-i <seconds>]
thermal power [-i <seconds>]
thermal watch [strategy | boost | passive | critical | emergency | fan | cpu]...
```

Each line it prints is one item with `key=value` fields. It exits 0 when it
did what it was asked, 1 when the kernel refused (with the reason), and 2
for a command it does not know. A strategy that no part acts on is kept
and reported, not treated as a failure.

A chosen strategy is kept in `/etc/thermal.conf` (one line, root and the
`desktop` group, mode 0664), which `/sbin/thermal` alone writes.

### Tests

A kernel built with `CONFIG_THERMAL_TEST_INJECT` also has
`/dev/thermal-inject` (root only), which adds made-up sensors, made-up
receivers with a fixed answer, and a choice of the emergency path, so that
the whole framework can be exercised on a machine without sensors.

## Build options

| Option | What it adds |
| --- | --- |
| `CONFIG_KERN_THERMAL` | The thermal core, the processor performance part, `/dev/thermal`, and the ACPI thermal zones. |
| `CONFIG_DRIVER_X86_POWER_CONTROL` | The x86 processor driver's MSR path (HWP, HFI, RAPL meters, idle residency). Needs the thermal core. |
| `CONFIG_DRIVER_DELL_SMM_BOOST` | The Dell provider (fans, the firmware's thermal mode, boost). Needs the thermal core. |
| `CONFIG_DRIVER_RPI5_FAN` | The Raspberry Pi 5's temperature and fan. Needs the thermal core. |
| `CONFIG_THERMAL_TEST_INJECT` | `/dev/thermal-inject`, for test builds only. |

## Verification

- On hardware, the platform's SLP_S0 residency counter (from the firmware's
  LPIT) increases across a sleep. The machine wakes with the power button
  and the lid, and the reason matches. The session and the network
  continue. The tick counter and the RTC agree across the sleep.
- On QEMU, which has no S0ix, `KERN_SYSTEM_SLEEP_S0IDLE` fails with
  `EOPNOTSUPP` without touching a device, and the DEVICES mode completes a
  suspend and resume of every device.
- The thermal core, the device and the processor performance part are
  tested on the host, with the kernel's threads run as host threads.
- On QEMU, which has no sensors or fans, the injection node supplies them:
  `/sbin/thermal` shows them and the receivers' answers, the boot restores
  the kept strategy, the `desktop` group's members may choose a strategy
  and others may not, the System Monitor's backend reads the processor's
  temperature, and a made-up zone at its critical level powers the machine
  off, both through `init` and directly.
- On the Latitude 5330, the idle package power falls with the deeper
  C-states and the aligned ticks, the strategies change the processors'
  requests and the firmware's thermal mode, and the meters and idle
  residency read plausible values.

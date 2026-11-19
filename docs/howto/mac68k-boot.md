# Booting a 68040 Macintosh from a BlueSCSI disk

Status: how-to; 68040 Macintoshes (`mac68k`, the Quadra 630 / LC 630
family first), booted from a SCSI disk emulated by a BlueSCSI v2. The port
is young: the loader and the kernel's console run, the SCSI driver and the
login are still to come (see "What you should see").

zedBSD boots these machines without Mac OS. The ROM starts the disk through
the disk's own driver, which is Apple's: zedBSD does not ship it. You install
it once with Apple's disk tool from Mac OS (漢字Talk 7 / System 7), and the
zedBSD disk then boots by itself.

## What you need

- A Macintosh with a 68040 processor (the full 68040 or the 68LC040). The
  loader stops on any other processor.
- At least 8 MB of memory. The loader reads the amount from the ROM.
- A monitor that can show 256 colours. The kernel draws its console at 8 bits
  a pixel.
- A BlueSCSI v2 with an SD card, set up so that Apple's disk tools accept its
  disks (the settings you use for a disk you initialize with Apple's tools).
- A second SCSI disk (or a second BlueSCSI image) with Mac OS, and Apple's disk
  tool for that system: **Apple HD SC Setup** or **Drive Setup**.

## Get the image

Build it yourself (see [Build zedBSD from source](build-from-source.md)).
The platform's compiler is GCC; `toolchain-cached` takes a prebuilt one for an
x86_64 Linux host:

```sh
make CONFIGMK=config/config.mac68k.mk toolchain-cached
make -j4 CONFIGMK=config/config.mac68k.mk image-platform
ls -l build/mac68k/hdd-image.img
```

The disk uses the Apple Partition Map:

| Part | Type | Contents |
| --- | --- | --- |
| driver area (first 4 MiB) | `Apple_Free` | empty: Apple's tool puts its driver here |
| `zedBSD` | `Apple_HFS` | the boot block, the loader `zedboot`, the kernel `vmunix`, `zedbsd.cfg` |
| `zedBSD-root` | `Apple_UNIX_SVR2` | the UFS root |
| `zedBSD-swap` | `Apple_UNIX_SVR2` | swap |

## Put the image on the SD card

Copy `hdd-image.img` to the SD card under a name that BlueSCSI v2 serves as a
hard disk at a free SCSI ID, for example `HD20_512.hda` for SCSI ID 2 with
512-byte blocks. Keep the Mac OS disk at its own ID.

## Install Apple's driver

Do this once for each new image. **Do not initialize the disk**: that erases
the zedBSD partitions. Only update its driver.

1. Start the Mac from the Mac OS disk.
2. Open Apple's disk tool:
   - **Apple HD SC Setup:** choose the zedBSD disk with **Drive** (its SCSI ID
     is shown), then click **Update**.
   - **Drive Setup:** select the zedBSD disk in the list, then choose
     **Functions > Update Driver**.
3. The tool reports that the driver was updated. The HFS volume **zedBSD**
   (with `vmunix`, `zedboot` and `zedbsd.cfg`) appears on the desktop.

While Mac OS runs, you can replace `vmunix` on that volume with a newer
kernel, or edit `zedbsd.cfg` (see below) with a text editor.

## Set the monitor and the memory

- **Monitors** control panel: choose **256** colours. The ROM keeps the
  setting, and the loader stops on any other depth.
- **Memory** control panel: if it shows **32-Bit Addressing**, turn it on.

## Boot zedBSD

The ROM boots the startup disk chosen in Mac OS, and otherwise looks for a
disk it can start. zedBSD has no menu for choosing Mac OS; to boot zedBSD:

- choose the **zedBSD** volume in the **Startup Disk** control panel if it is
  listed there, and restart; or
- shut down and make the zedBSD disk the only bootable disk on the SCSI bus
  (take the Mac OS image off the SD card, or give it to BlueSCSI under a name
  it does not serve), then turn the Mac on.

To go back to Mac OS, give the Mac OS disk back and choose it as the startup
disk again.

## What you should see

1. The grey screen of the ROM, then a black screen with the loader's lines in
   grey: `zedBSD loader for the 68040 Macintosh`, the machine (`machine`, the
   Gestalt model, 0x62 on a Quadra 630 / LC 630), the processor, the memory
   banks it found, the kernel's size and entry, and `entering the kernel`.
2. Three seconds later the kernel clears the screen and prints its messages,
   up to `zedBSD 1.0.0`.

For now the kernel stops after that with
`VFS initialization failed (12); entering idle`: it has no SCSI driver to
reach the root yet. Turn the Mac off with its power switch.

## zedbsd.cfg

One setting per line; Mac OS's line ends are fine:

```text
kernel=vmunix
```

`kernel=` names the kernel's file on the `zedBSD` volume. Every other line is
a kernel parameter; the loader joins them into the kernel's command line.

## When the boot stops

- **A system error with ID 101 to 105 (a bomb or a sad Mac) before the
  loader's lines:** the boot block stopped.
  - 101: the processor is not a 68040.
  - 102: no memory for the loader.
  - 103: the boot block did not find the disk it was read from.
  - 104: the loader `zedboot` could not be read.
  - 105: `zedboot` is damaged or missing; build the image again.
- **The ROM shows the disk with a blinking question mark:** the driver is
  missing or the disk is not the startup disk; install the driver again.
- **`set the monitor to 256 colours`:** see "Set the monitor and the memory".
- **`the kernel's file did not open`, then `the kernel's sectors do not match`:**
  the loader could not open `vmunix` and the copy recorded at build time was
  replaced; put back the image's `vmunix` or check the volume with Mac OS.

Report the machine's model, the memory, the loader's lines and the last lines
of the kernel (a photo of the screen is enough).

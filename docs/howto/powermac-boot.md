# Booting a PowerPC Mac from a USB stick

Status: how-to; 32-bit New World PowerPC Macs (`macppc32`), booted from a
USB stick through Open Firmware.

The `macppc32` image runs on New World PowerPC Macs with a G3 or G4 processor
(PowerBook G4, iBook G4, Power Mac G4, Mac mini G4 and similar). It has been
tried on a 12-inch PowerBook G4 (A1010, PowerBook6,1). Other machines are
expected to work only as far as they share that machine's parts.

> **Read the fan warning below before the first boot.** zedBSD does not
> control the fan on most of these machines.

## The fan: keep the machine cool

On most PowerPC Macs the fan and the temperature sensors are run by the
operating system. zedBSD drives the fan controller of only a few machines
(the ADM1030 of the 12-inch PowerBook G4; the ADT7460/ADT7467 family is
supported but untried); on every other machine **nothing starts the fan**, and the processor can
overheat.

- **Laptops (PowerBook, iBook):** listen for the fan and feel the palm rest
  shortly after the boot. If the fan does not turn, use zedBSD only for short
  sessions and shut it down before the machine gets hot.
- **Desktops (Power Mac, Mac mini):** check that the fans turn after the boot.
  If they do not, do not keep using the machine; shut it down.

When the fan controller is supported, the kernel message shows it:

```sh
dmesg | grep mac99-fan
thermal sensors
thermal fans
```

`mac99.fan=read` (read the controller, change nothing) and `mac99.fan=off`
(leave it alone) are boot options for a machine where the fan setup misbehaves.

## Write the image to a USB stick

Use the nightly `zedbsd-macppc32.img.gz`, or build the image yourself (see
[Build zedBSD from source](build-from-source.md)):

```sh
make -j4 CONFIGMK=config/config.macppc32.mk disk
ls -l build/macppc32/hdd-image.img
```

Write it to the whole stick. This erases the stick; check the device name
first (`lsblk`), because writing to the wrong disk destroys its data:

```sh
gunzip -c zedbsd-macppc32.img.gz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
# or, for a local build:
sudo dd if=build/macppc32/hdd-image.img of=/dev/sdX bs=4M conv=fsync status=progress
```

The image uses the Apple Partition Map. Partition 2 is a small HFS volume
that Open Firmware reads; it holds the kernel (`\vmunix`) and the boot script
(`\ppc\bootinfo.txt`, blessed as the system folder). Partition 3 is the UFS
root.

## Boot: machines that offer the USB stick at the Option key

Older Macs that can boot from USB show it in the graphical startup picker:

1. Plug in the stick and turn the Mac on while holding **Option**.
2. Choose the stick's icon and click the arrow.

The firmware runs the boot script, which boots `\vmunix` from the same
partition. If the stick does not appear in the picker, use the Open Firmware
prompt below.

## Boot: other machines, from the Open Firmware prompt

1. Plug in the stick and turn the Mac on while holding
   **Command-Option-O-F** until the `0 >` prompt appears. A USB keyboard (or
   the laptop's own keyboard) is needed to type.
2. List the device aliases and find the USB controllers (`usb0`, `usb1`, ...):

   ```
   devalias
   ```

3. Find the stick. Try each controller, and `disk@1` or `disk@2` for the
   port; the right one lists `vmunix` and `ppc`:

   ```
   dir usb0/disk@1:2,\
   dir usb1/disk@1:2,\
   ```

4. Boot the kernel from that path, with any boot options after it:

   ```
   boot usb1/disk@1:2,\vmunix
   ```

   On the 12-inch PowerBook G4 the stick was `usb1/disk@1`. `boot usb1/disk@1:2,\\:tbxi` runs the boot script instead,
   which does the same.

## What you should see

The screen switches to the zedBSD console (the whole screen, 8 bits a pixel
as Open Firmware left it), the kernel messages scroll, and `login:` appears.
Log in as `user` with the published default password `user` (change it).

## The graphical session

The image includes Xzed, a small X11 server, with a window manager and a
terminal. Start it from the console after logging in:

```sh
startx
```

Xzed is still a simple implementation that draws everything with the
processor, so it is slow; expect windows to redraw visibly.

## When the boot stops

- **The screen stops after the USB storage messages:** boot again with
  `ppc32.powersave=off` added after the kernel path. This turns off the
  processor's idle power saving.
- **No keyboard at the login:** some boot options turn off the power manager
  that runs the laptop keyboard (`mac99.pmu=off`); remove them.
- **Open Firmware says the file cannot be found:** check the path with `dir`
  again; the stick's controller and port change with the socket it is in.

Report the machine's model (`Apple System Profiler`, or `dev / .properties`
at the Open Firmware prompt shows `model`) with the last lines on the screen.

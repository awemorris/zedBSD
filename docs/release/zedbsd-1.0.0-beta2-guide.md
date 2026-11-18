# Kei/zedBSD 1.0.0 Beta 2: user guide

Status: howto; for the 1.0.0 Beta 2 release.

Kei/zedBSD 1.0.0 Beta 2 is a preview. It runs from a USB drive and does not
install itself on the computer's internal disk. Expect rough edges: read the
[known issues](zedbsd-1.0.0-beta2-known-issues.md) and the
[security notes](#security-notes) before you start.

## What you need

- A **Dell Latitude 5330**. This is the computer Beta 2 is tested on. Other
  UEFI PCs with an Intel CPU and Intel graphics may start, but they are not
  tested.
- A USB drive of **4 GB or more**. Writing the image erases everything on it.
- From the release page on GitHub:
  - `zedbsd-1.0.0-beta2-amd64.img.gz`: the USB image, compressed with gzip.
  - `SHA256SUMS`: the checksums of the files.
  - `LICENSES.md`: the components in the image and their licenses.

If the release also has `zedbsd-1.0.0-beta2-windows.zip`, that file runs Kei
in a virtual machine on Windows instead (see [Running on Windows](#running-on-windows)).

## 1. Check the download

The checksum tells you whether the file arrived intact. It does not prove who
made the file.

On Linux or macOS, in the folder that holds both files:

```sh
sha256sum -c SHA256SUMS --ignore-missing     # Linux
shasum -a 256 -c SHA256SUMS --ignore-missing # macOS
```

The image's line must say `OK`.

On Windows, in PowerShell:

```powershell
Get-FileHash .\zedbsd-1.0.0-beta2-amd64.img.gz -Algorithm SHA256
```

Compare the hash with the image's line in `SHA256SUMS`.

## 2. Write the image to the USB drive

Writing erases the whole USB drive. Check twice that you picked the USB drive
and not a disk you need.

### Linux

Find the USB drive's device (for example `/dev/sdb`) with `lsblk`, unmount
its partitions if your desktop mounted them, then write:

```sh
gunzip -c zedbsd-1.0.0-beta2-amd64.img.gz | sudo dd of=/dev/sdX bs=4M conv=fsync status=progress
```

Replace `/dev/sdX` with the USB drive's device, the whole drive and not a
partition (`/dev/sdb`, not `/dev/sdb1`).

### macOS

Find the USB drive (for example `/dev/disk4`) with `diskutil list`, then:

```sh
diskutil unmountDisk /dev/diskN
gunzip -c zedbsd-1.0.0-beta2-amd64.img.gz | sudo dd of=/dev/rdiskN bs=4m
diskutil eject /dev/diskN
```

Replace `N` with the USB drive's number. `rdiskN` writes faster than
`diskN`.

### Windows

Use a tool that writes disk images, such as
[balenaEtcher](https://etcher.balena.io/) or [Rufus](https://rufus.ie/).
balenaEtcher reads the `.img.gz` file directly. For Rufus, first extract the
`.img` file with a tool that opens gzip files (for example 7-Zip), then choose
the `.img` file in Rufus and write it in DD image mode.

## 3. Set up the firmware

Kei/zedBSD starts in UEFI mode and its boot loader is not signed for Secure
Boot. On the Latitude 5330, press **F2** while the Dell logo is shown to open
the BIOS setup, then:

- Under **Boot Configuration**, turn **Secure Boot** off.
- Keep the boot mode UEFI (Legacy/CSM boot is not used).
- Save and exit.

The other settings can stay as they are.

## 4. Start from the USB drive

Plug in the USB drive, turn the computer on and press **F12** while the Dell
logo is shown. Choose the USB drive under the UEFI boot entries.

The Kei logo appears, and after a short while the desktop. The image signs in
the user **kei** by itself at the first start. After you log out, the login
screen asks for a user and password:

| User | Password |
| --- | --- |
| `kei` | `kei` |

The `root` user cannot log in on this release.

To stop the computer, use **Shut Down** in the system menu. Then remove the
USB drive to start the computer's own system again.

### The ZEDBSD partition

The USB drive holds a small FAT32 partition named **ZEDBSD** with the kernel
(`vmunix`), the boot settings (`zedbsd.cfg`) and the boot logo. Windows and
macOS show it like any USB stick when you plug the drive in. Kei shows it at
`/boot/boot0`, and the firmware's own partition (the EFI system partition,
which holds only the boot loader) at `/boot/efi`.

To keep the kernel's messages for a report, for example when the network
does not work, copy them to the ZEDBSD partition in the Terminal (the
partition belongs to root, so the copy goes through `sudo`, which asks for
the password of `kei`):

```sh
dmesg | sudo tee /boot/boot0/dmesg.txt > /dev/null
sync
```

Then shut down, plug the USB drive into another computer and open
`dmesg.txt` on the ZEDBSD partition.

### Older Intel graphics

On computers with a 6th to 10th generation Core processor (Intel Gen9 and
Gen9.5 graphics), Kei drives the GPU's engines and Vulkan and shows the
desktop on the framebuffer the firmware set up, but does not drive the
display yet. Each frame is copied to that framebuffer, so moving pictures
may tear and be slower than on newer Intel graphics. The screen stays at
the resolution the firmware chose, its
brightness and external displays cannot be changed, and in sleep the GPU
stays powered, so the battery runs down faster while asleep (see the
[known issues](zedbsd-1.0.0-beta2-known-issues.md)). Gen9.5 (7th to 10th
generation) was checked on an 8th generation Core; the 6th generation
(Skylake) was not.

If the GPU driver gives trouble on such a computer, turn it off. Kei then
has no GPU: the desktop does not start, and only the text console is shown.
To turn it off:

1. Plug the USB drive into a computer and open its **ZEDBSD** partition
   (see [The ZEDBSD partition](#the-zedbsd-partition)), the one that holds
   `vmunix` and `zedbsd.cfg`. In Kei itself the file is
   `/boot/boot0/zedbsd.cfg`.
2. Open `zedbsd.cfg` in a text editor, add the line `i915.gen9=off` at the
   end, and save it.
3. Eject the USB drive and start Kei from it.

To turn the driver on again, delete the line.

<!-- review (2026-10-14): the code freeze of 2026-10-14 18:00 took the Gen9 driver (WS229), on by default. The steps
     follow how the image's zedbsd.cfg takes extra lines (one per line, CR LF is accepted); they were not tried by hand
     on Windows or macOS. WS238 (2026-10-14) moved zedbsd.cfg from the ESP to the ZEDBSD FAT32 partition (Basic Data
     type) so that Windows and macOS mount it by themselves; that mount is to be checked on the release image.
     Whether sleep works with the driver off was not checked on a Gen9 machine.
     2026-10-14 BUG-310: the desktop on the firmware's framebuffer (i915 bootfb) is checked only by host tests so far;
     keep this paragraph only if the SV7 check passes, otherwise say that Gen9 machines show the text console only. -->

## 5. Connect to a network

### Wi-Fi

The Latitude 5330's built-in Wi-Fi (Intel Wi-Fi 6E AX211) works. Open
**Settings** → **Wi-Fi**, turn Wi-Fi on, choose your network and type its
password. WPA2-Personal networks are supported. Some 5 GHz networks may not
give an address; if that happens, use the 2.4 GHz network of the same router
(see the [known issues](zedbsd-1.0.0-beta2-known-issues.md)).

USB Wi-Fi adapters with the Realtek RTL8822BU chip also work.

### Wired network

USB Ethernet adapters that follow the USB CDC-NCM or CDC-ECM standard work
(many USB-C adapters and docks do). An adapter can be plugged in before or
after you start the computer.

<!-- review: BUG-168 (an adapter plugged in later did not come up) was fixed and passed in QEMU; the 5330 check is in
     the UAT. If it fails there, restore "Plug the adapter in before you start the computer". -->

### Bluetooth

Open **Settings** → **Bluetooth** to pair a Bluetooth keyboard or mouse
with the Latitude 5330's built-in Bluetooth.

## Security notes

Read these before you connect Beta 2 to a network.

- **The password is public.** Everyone who reads this guide knows the
  password of `kei`. Change it first: open **Settings** → **Users** and use
  **Change Password**, or type `passwd` in Terminal.
- **The SSH server is on.** Beta 2 accepts SSH logins with that password
  from the network. To turn the server off, type in Terminal:

  ```sh
  sudo service stop sshd
  sudo service disable sshd
  ```

  The first command stops it now, the second keeps it off at the next start.
- Until you change the password, anyone on the same network can log in to
  the computer as `kei`, and `kei` can run commands as root with `sudo`. Use
  Beta 2 only on networks you trust, such as your home network, and do not
  keep anything private in it.
- The image keeps what you save on the USB drive. Anyone who has the drive
  can read it.

## Running on Windows

When the release has `zedbsd-1.0.0-beta2-windows.zip`, you can run Kei in a
virtual machine on a Windows PC instead of starting from USB. Extract the zip
to a folder and run `boot.bat` in it. It starts QEMU with the same image and
3D graphics passed to the PC's GPU (Venus). The same login, password and
security notes apply.

## Building it yourself

The image is built from the source with `make menuconfig` (choose the CPU,
the boot options and the programs, then **Build boot image**). The steps are
in the [build-from-source guide](../howto/build-from-source.md). The menu
names the Base programs and libraries by their command and library names,
and has a **Retro** menu, right after Base, for the X11 programs Xzed, zwm, zshell
and zterm. Choosing the i386 CPU (with the PC-9800 or PC/AT board) leaves
the Desktop out, as the desktop is not built for i386.

The Keiland desktop also builds natively on Linux and FreeBSD with
`make keiland-linux` and `make keiland-freebsd`, which check the host's
packages first and offer to install the missing ones (see the
[README](../../README.md)). Besides the desktop, they install zedBSD's own
commands that the host does not have, such as `passkey` in
`/opt/keiland/sbin`; commands the host already has, such as `ls`, are left
to the host.

### Firmware and its licenses

The firmware of devices (Wi-Fi, Bluetooth, the graphics' display and the
Raspberry Pi 4's start) is not in the source tree. When the configuration
selects a firmware, the build downloads it from the GitHub repository
[awemorris/zedBSD-firmware](https://github.com/awemorris/zedBSD-firmware),
which keeps each firmware in a directory of its own with its license, and
from the firmware's original source only when that download fails. Either
way the build checks the file's size and SHA-256.

The first time the build fetches a firmware, it shows what the firmware is,
its copyright notice and its whole license, and asks whether you accept the
license:

- `make` asks `yes/no` in the terminal. Any answer but yes stops the build
  without downloading. An accepted license is not asked about again unless
  its text changes.
- For a build without a person at the terminal, such as CI, accept them all
  first with `yes | make firmware-licenses`.
- **Build boot image** in `make menuconfig` shows each license in a screen of
  its own. Scroll to the end of the license to enable **Accept**; **Decline**
  stops before the build. To build without a declined firmware, turn it off
  in the **Firmware** menu.

### PC-98

The i386 CPU with the PC-9800 board builds `build/pc98/hdd-image.img` for
NEC PC-9821 computers (for example with
`make ZEDBSD_CONFIG=config/ci/config-pc98.mk disk-image`). The image's
partitions start and end on cylinder boundaries of a disk with 8 heads and 17
sectors a track, which models such as the PC-9821 Ra43 need to start from it.

On an IDE drive emulator such as ZuluIDE, present the image as a hard disk
with that geometry, for example:

```ini
[IDE]
device = hdd
cylinders = 2667
heads = 8
sectors = 17
```

The number of cylinders is the image's size in bytes divided by 69,632
(8 heads × 17 sectors × 512 bytes); 2667 is that of the default image.

<!-- review (2026-10-14): this is the zuluide.ini the user's PC-9821 Ra43 check used (ZuluIDE firmware 2026.03.07,
     whose source reads `device = hdd` in the [IDE] section). Images of 20 MiB or less use 4 heads instead of 8. -->

## Licenses

`LICENSES.md` on the release page lists every component in the image and its
license. Inside the running system, the license texts are under
`/usr/share/licenses/`, and `/usr/share/licenses/INDEX` lists them.

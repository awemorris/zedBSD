<!-- Draft of 2026-10-09 (ws129-p005, P1), for the user's review. This file is the GitHub release's text
     (.github/workflows/release.yml takes docs/release/zedbsd-<VERSION>.md). The "review:" comments are for the
     review only; settle and delete them at the release candidate (10/13). -->

Kei/zedBSD 1.0.0 Beta 2 is the first public preview of Kei, an operating
system for laptops and tablets built on the zedBSD kernel, with its own
desktop, Keiland. It runs from a USB drive and does not touch the computer's
internal disk.

This is a beta. It is tested on the **Dell Latitude 5330** only, and it has
[known issues](https://github.com/awemorris/zedBSD/blob/zedbsd-1.0.0-beta2/docs/release/zedbsd-1.0.0-beta2-known-issues.md).
Read the
[user guide](https://github.com/awemorris/zedBSD/blob/zedbsd-1.0.0-beta2/docs/release/zedbsd-1.0.0-beta2-guide.md),
and its security notes in particular, before you start.

<!-- review: the links point at the final tag zedbsd-1.0.0-beta2 of github.com/awemorris/zedBSD (the origin), so they
     work only after the tag exists; the RC's Prerelease shows them broken. -->

## Downloads

| File | What it is |
| --- | --- |
| `zedbsd-1.0.0-beta2-amd64.img.gz` | The USB image (gzip). Write it to a USB drive of 4 GB or more; the guide shows how on Linux, macOS and Windows. |
| `SHA256SUMS` | The SHA-256 checksums of the files. They show that a download is intact, not who made it. |
| `LICENSES.md` | Every component in the image and its license. |

<!-- review: the Windows zip (zedbsd-1.0.0-beta2-windows.zip) is not carried while ZEDBSD_RELEASE_ZIP is n (U6). Add
     a row if that changes. -->

## Signing in

The image signs in the user `kei` by itself at the first start. The password
of `kei` is `kei`, and the SSH server is on: anyone on the same network can
log in as `kei`. Use Beta 2 only on networks you trust. `root` cannot log in.

## What is in Beta 2

### The desktop

- **Keiland**, the desktop: floating, frosted-glass title bars and menus, a
  system bar with the network, sound, battery and input method, and a
  frosted-glass control panel that opens from the status area.
- **App Home**, the app launcher, with search; **Wiseview**, the overview of
  the open windows; the top bar's list of the running apps with previews,
  and the Windows key to switch apps.
- Windows can be docked, floating or minimized, and arranged side by side
  from the arrangement menu.
- Touch: the built-in touch screen of the Latitude 5330 and USB touch
  screens, with multi-touch, inertial scrolling and edge swipes; touchpad
  gestures with two fingers.
- An on-screen keyboard (flick and QWERTY layouts) and handwriting input.
- Japanese input: an input method with a conversion dictionary, candidate
  window and prediction, and an SKK input method. Choose it in Settings →
  Languages.
- A lock screen with a large clock; sign in with a password, a 6-digit PIN
  or a FIDO2 security key (such as a YubiKey).
- Settings → Security Keys: the 6-digit PIN (Software Security Key) and the
  FIDO2 keys, added with a step-by-step window. A key works plugged in or held
  to an NFC reader; a key left lying on the reader counts as touched, so do
  not leave it there if anyone else can reach the computer.
- Icons of the files in `~/Desktop`, and drag and drop between apps.
- External displays over HDMI and USB-C (DisplayPort alternate mode),
  extended or mirrored, arranged in Settings → Display.
- Screen rotation in Settings → Display: four pictures of how the screen
  stands turn it by 90°, 180° or 270°, for example to hold the computer
  upright for Notes. The turn takes effect at once and is kept for the next
  sign-in. Touch, the pen and the pointer follow the turn. The angle can be
  changed while the display is the only one shown.

<!-- review (2026-10-14): rotation from Settings → Display passed in QEMU (T1-611, T1-616); the turn on the 5330's
     Intel graphics waits for the UAT. Drop the line if that check fails. -->

<!-- review: the control panel (WS192) passed in QEMU (T1-496) apart from the look, which a person judges; it and the
     lock screen (WS187) wait for the 5330 UAT (items 1 and 6). FIDO2 sign-in (WS172 p003, WS161 p006) waits for the UAT
     (item 7). Drop a line whose check fails. -->

### Applications

Files, Settings, Terminal, Text Editor, Notes (handwritten notes, with a pen
tablet, saved as PDF; editing PDF text and images), PDF Viewer, Image Viewer,
Photos, Music (m4a), Video Player, Calendar, Mail (IMAP and SMTP), Phone
(contacts and a message timeline; no phone line yet), System Monitor and the
Browser.

Notes draws with a brush, a pencil and watercolor as well as the pen and the
eraser, and fills an enclosed area with a color. The pen's pressure shapes
the strokes, and the saved PDF keeps them.

The Browser is an early web browser with an engine of its own. It scores
100 in the Acid3 test, and its picture of the test matches the reference
picture with no pixel different. It shows google.com, including the inline
SVG pictures the page draws its logo and icons with. The address is a box in
the window's title bar, as in other browsers: one click selects the whole
address, and held keys repeat while you edit it. Images are shown from PNG,
JPEG and GIF files.

<!-- review (2026-10-16): browser の最終の状態で更新 -->

<!-- review (2026-10-14): the Notes tools passed in QEMU (T1-580); how the pen feels with them on the 5330 waits for
     the UAT. The Browser's title bar (T1-618) and google.com were checked in QEMU only. -->

X11 programs run on the desktop through a rootless X server, with GLX.

<!-- review: Phone and Mail are early versions; say "preview" if the user prefers. -->

### Hardware

- Intel graphics with zedBSD's own i915 driver: Vulkan, OpenGL ES and
  OpenGL through Vulkan, used by the desktop.
- Vulkan Video: the i915 driver can decode H.264 in the GPU
  (`VK_KHR_video_decode_h264`) for programs that use it. It is on by default.
- Wi-Fi: the built-in Intel Wi-Fi 6E AX211 (WPA2-Personal), and USB Wi-Fi
  adapters with the Realtek RTL8822BU chip.
- Wired network: USB Ethernet adapters that follow USB CDC-NCM or CDC-ECM.
  IPv4 and IPv6.
- Bluetooth keyboards and mice, paired in Settings → Bluetooth.
- Sound through the built-in Intel HD Audio, with a volume control in the
  system bar.
- USB keyboards, mice, touch screens, pen tablets, USB drives (mounted and
  ejected from Files) and FIDO2 security keys.
- Printing to network printers over IPP and LPD.
- Computers whose firmware stops the old PIT timer, such as a desktop PC
  with a Core i5-13400F, no longer stop at the start with "Local APIC
  timer initialization failed": when the CPU reports the frequency of its
  time-stamp counter (TSC), the kernel measures its timer with the TSC
  instead.
- The older Intel graphics, Gen9 and Gen9.5 (the integrated GPUs of the
  6th to 10th generation Core processors), on by default. The driver runs
  the GPU's engines and Vulkan only: the picture stays on the framebuffer
  the firmware set up, there is no Vulkan Video, and in sleep the GPU stays
  powered, so the battery runs down faster while asleep. Gen9.5 (7th to 10th generation) was checked on the GPU
  of an 8th generation Core; the 6th generation (Skylake), and starting
  such a computer from the USB drive by itself, were not. The
  [user guide](https://github.com/awemorris/zedBSD/blob/zedbsd-1.0.0-beta2/docs/release/zedbsd-1.0.0-beta2-guide.md#older-intel-graphics)
  shows how to turn it off with `i915.gen9=off`.
- SATA drives on AHCI controllers, on by default, including Intel
  controllers set to RAID mode in the firmware, with basic support for
  sleep and resume. Kei still starts from USB; a SATA drive inside the
  computer can be read and written. NCQ, TRIM, hot plugging and optical
  drives are not supported yet.

<!-- review (2026-10-14): the Core i5-13400F desktop started in the user's check on real hardware; the same change on
     the 5330 (whose timer may now be measured with the TSC too) has not been checked there yet.
     The code freeze of 2026-10-14 18:00 took Gen9 (WS229) and AHCI (WS231); both are on by default. Gen9's GPU tests
     passed on a Let's Note SV7 (Core i5 8th generation, UHD 620) with its GPU passed through to a QEMU guest on that
     machine, so the machine's own firmware start from the USB drive is untested, as is any 6th generation part. -->

<!-- review (2026-10-16): WS236 が入れば Hardware に 1 行 -->

### For developers

- A POSIX system: `/bin/sh` with line editing, history and completion, and
  the POSIX utilities, with the GNU extensions of `sed`, `awk`, `grep` and
  `ls` that scripts commonly use, and a GNU-compatible `make`.
- Clang, LLD and LLDB, libc++ and the system headers, to build programs on
  the computer itself.
- Emacs (`/bin/emacs`), the OpenSSH client and server, curl, OpenSSL, and the
  Noct scripting language.
- Japanese input in Emacs with SKK (`C-x C-j`), with two dictionaries in the
  image: a compact one and SKK-JISYO.X. A single `n` before a space or the
  end of a reading becomes ん, so words such as にほん convert.
- `vi` (`/bin/vi`), a small vi of zedBSD's own: UTF-8 text with Japanese
  characters, the usual moves, edits and searches, undo and redo of many
  steps (`u` and `Ctrl-R`), `.` to repeat the last change, and `:w`, `:q`,
  `:wq` and `:e`. If it is cut off with unsaved changes, for example when
  its terminal closes, it saves them in `~/.vi-recover.<name>.<pid>`. It has
  no ex mode, `:s`, marks or visual mode.
- The kernel's text console understands cursor positioning and colors
  (the `cup` and SGR sequences), so full-screen programs such as `vi` draw
  correctly on it.
- `useradd`, `usermod` and `userdel` for root, in the usual Linux form
  (`usermod` does not change a user's name, ID or home, so it has no `-d`
  or `-m`). The account checks behind them and Settings → Users were fixed:
  the last administrator is counted correctly, a removed user's ID is never
  given again, and a user whose password is locked is not signed in
  automatically.
- `make menuconfig` shows the Base programs and libraries by their names
  (the image and PDF libraries moved to Desktop), offers the i386 CPU with
  the PC-9800 and PC/AT boards, and has a Retro menu for Xzed, zwm, zshell
  and zterm.
- The PC-98 disk image puts its partitions on cylinder boundaries, so
  PC-9821 models such as the Ra43 start from it (see the user guide).
- On the UFS file system, `fsync` writes the file's own data and the
  journal, instead of writing out everything waiting on the volume several
  times over, so saving a file (as `vi` and Settings do) takes less time.
- Device firmware (for Wi-Fi, Bluetooth, the graphics' display and the
  Raspberry Pi 4's start) comes from the GitHub repository
  [awemorris/zedBSD-firmware](https://github.com/awemorris/zedBSD-firmware),
  which keeps each firmware with its license, and from the firmware's
  original source only when that fails; its size and SHA-256 are checked
  either way. The first time the build fetches a firmware, it shows what the
  firmware is, its copyright and its whole license, and asks whether you
  accept it: `make` asks yes or no, `yes | make firmware-licenses` accepts
  them all for an unattended build, and **Build boot image** in
  `make menuconfig` shows each license in a screen you scroll to the end
  before you choose Accept or Decline.

<!-- review (2026-10-14): the PC-98 line rests on the user's check on a PC-9821 Ra43 and the QEMU start (T1-617). The
     fsync line passed in QEMU only (T1-615). The license screen of Build boot image passed the host test and a
     pseudo-terminal run; nobody has looked at it in a real terminal or built an image through it yet. -->

### Keiland on Linux and FreeBSD

The Keiland desktop also builds natively on Linux and FreeBSD (see the user
guide). With these builds:

- On Linux, Settings → Bluetooth works through BlueZ: turning Bluetooth on
  and off, finding, pairing (with the number check), connecting and
  forgetting devices, with clear messages when a connection fails. Cancel in
  the pairing window ends the pairing at once. On FreeBSD the page says that
  Bluetooth is not supported.
- zedBSD's own `passkey` command, which checks the password or PIN when you
  sign in, is built and installed in `/opt/keiland/sbin`. Programs the
  host already has, such as `ls`, are not.

<!-- review (2026-10-14): the Linux Bluetooth page passed in a Debian 13 QEMU+KVM guest with a virtual controller
     (btvirt), not on real hardware; the FreeBSD page and the FreeBSD build of passkey were not run (passkey only
     to the link). -->

## Licenses and source

zedBSD itself is under the zlib license. `LICENSES.md` lists every component
of the image and its license, and the license texts are in the image under
`/usr/share/licenses/`.

The image contains the FFmpeg libraries (libavutil, libswresample,
libavcodec, libavformat and libswscale) version 9.0.2, under the GNU Lesser
General Public License version 2.1 or later, as shared libraries in
`/usr/lib` that can be replaced. Their source is FFmpeg 9.0.2 from
[ffmpeg.org](https://ffmpeg.org/download.html)
([ffmpeg-9.0.2.tar.xz](https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz),
SHA-256
`8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e`).
zedBSD applies no patches to it; the configure arguments it is built with
are in the zedBSD source tree on GitHub, in
[userland/packages/multimedia/libavcodec/Makefile](https://github.com/awemorris/zedBSD/blob/zedbsd-1.0.0-beta2/userland/packages/multimedia/libavcodec/Makefile).

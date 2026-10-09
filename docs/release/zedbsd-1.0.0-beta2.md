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

<!-- review: the control panel (WS192) passed in QEMU (T1-496) apart from the look, which a person judges; it and the
     lock screen (WS187) wait for the 5330 UAT (items 1 and 6). FIDO2 sign-in (WS172 p003, WS161 p006) waits for the UAT
     (item 7). Drop a line whose check fails. -->

### Applications

Files, Settings, Terminal, Text Editor, Notes (handwritten notes, with a pen
tablet, saved as PDF; editing PDF text and images), PDF Viewer, Image Viewer,
Photos, Music (m4a), Video Player, Calendar, Mail (IMAP and SMTP), Phone
(contacts and a message timeline; no phone line yet), System Monitor and the
Browser (an early web browser of its own).

X11 programs run on the desktop through a rootless X server, with GLX.

<!-- review: Phone and Mail are early versions; say "preview" if the user prefers. -->

### Hardware

- Intel graphics with zedBSD's own i915 driver: Vulkan, OpenGL ES and
  OpenGL through Vulkan, used by the desktop.
- Vulkan Video: the i915 driver decodes H.264 in the GPU
  (`VK_KHR_video_decode_h264`) for programs that use it. The Video Player
  decodes with FFmpeg on the CPU.
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

<!-- review (2026-10-09 user): Bluetooth (WS143) and Vulkan Video (WS083) are in, and are turned off at the last
     moment (10/16) if they are not ready. If one is turned off, replace its line:
     - Bluetooth off: delete the "Bluetooth keyboards and mice" line here, and add the known-issue row from the
       known-issues draft.
     - Vulkan Video off: replace its line with
       "- Vulkan Video decoding (H.264) is not in this release; videos are decoded by FFmpeg on the CPU."
       or delete it.
     Vulkan Video is behind the boot option i915.debug=video until ws083-p008 makes it the default; if it is still
     behind the option at the RC, say so in its line ("start with i915.debug=video on the boot line to turn it on").
     IPv6 waits for the networkd retest (T1-500); drop "and IPv6" if it fails. -->

### For developers

- A POSIX system: `/bin/sh` with line editing, history and completion, and
  the POSIX utilities, with the GNU extensions of `sed`, `awk`, `grep` and
  `ls` that scripts commonly use, and a GNU-compatible `make`.
- Clang, LLD and LLDB, libc++ and the system headers, to build programs on
  the computer itself.
- Emacs (`/bin/emacs`), the OpenSSH client and server, curl, OpenSSL, and the
  Noct scripting language.

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

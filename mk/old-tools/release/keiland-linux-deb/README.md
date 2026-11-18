# Keiland Debian packages

```sh
make keiland-linux-debian
make keiland-linux-ubuntu2604
```

Both commands boot the pinned amd64 distribution in QEMU, build with its native
compiler and dpkg, then install the deb in a second fresh guest. The checks cover
runtime dependencies, the public Vulkan client, an actual KMS desktop and Terminal
keyboard input, reinstall, upgrade, removal and preservation of user data.

## The three debs without a VM

```sh
make keiland-deb-amd64   # Debian 13 and Ubuntu 26.04, amd64
make keiland-deb-arm64   # Debian 13 and Ubuntu 26.04, arm64
make keiland-deb-rpi     # Raspberry Pi OS (trixie), arm64
```

[rootfs.py](rootfs.py) makes a throwaway rootfs of the target with mmdebstrap (unshare mode, no
root; a subordinate uid range in /etc/subuid) from the archives in [rootfs.json](rootfs.json), and
[build.py](build.py) builds the deb inside it with the target's own compiler, headers and C library.
arm64 runs through the host's qemu-aarch64 binfmt (user-mode emulation, not a VM). One deb serves
Debian 13 and Ubuntu 26.04: it is built against Debian 13's C library, the older of the two. The
Raspberry Pi OS rootfs adds archive.raspberrypi.com, whose key is checked against the fingerprint
in rootfs.json; that key verifies the archive's InRelease, which leads by SHA256 to raspberrypi-archive-keyring, whose keyring apt (sqv) takes (kept in `KEILAND_DEB_TMPDIR/keiland-deb-keys/`, where the namespace can read it).

Each deb is then checked, not run: its fields, the machine of every ELF in it, no test program, and
`apt-get install --simulate` of it in a fresh rootfs of each distribution it is for (Debian 13 and
Ubuntu 26.04, or Raspberry Pi OS). Each run writes a new directory
`KEILAND_DEB_BUILD/rootfs/TARGET/STAMP/` (default `build/keiland-deb`): `out/` with the deb, its
manifest, buildinfo and checksums, `build.log`, `check-*/simulate.txt` and `check.json`. The
rootfs is made under `KEILAND_DEB_TMPDIR` (default `/var/tmp`; the namespace's root cannot enter a
private home) and mmdebstrap removes it. The host needs mmdebstrap, the qemu-user binfmt for arm64,
gpg, curl and the Ubuntu archive keyring (`ubuntu-keyring`).

## The QEMU smoke

The host needs Python 3.12+, Git, curl, OpenSSH, QEMU (`qemu-system-x86_64` and
`qemu-img`), and xorriso. KVM is used when accessible; otherwise QEMU uses TCG.
`KEILAND_DEB_ACCEL=auto|kvm|tcg` explicitly selects emulator acceleration;
`auto` is the default. CI uses TCG so it also runs without nested virtualization.
The amd64 guests use 8 GiB of RAM. The host's desktop, input devices and `/opt/keiland` are untouched. Connections
use a temporary key and a randomly assigned **127.0.0.1** SSH forwarding port.
The base images are verified against [inputs.json](inputs.json) on every run.
Each emulator writes into its own overlay. SSH and QMP screenshots verify
readiness and behavior; serial output is disabled.

`KEILAND_DEB_BUILD` selects the workspace (default `build/keiland-deb`).
Successful outputs are in `artifacts/debian13/` and `artifacts/ubuntu2604/`:

- `keiland_<version>_amd64.deb`: production desktop, libraries, fonts, dictionaries
  and session registration; test apps and development headers are excluded.
- `.manifest.json`: exact paths, modes and payload hashes.
- `.buildinfo.json`: native OS, compiler, dependencies and installed build packages,
  source archive and guest image hashes. This is a Keiland JSON record, not the
  Debian standard `.buildinfo` format.
- `.sha256`: checksums for the deb and its build/manifest records.
- `.smoke.json`, PNGs and the compositor's application journal: install/runtime
  evidence. No guest console or serial logs are used.

The runtime layout is `/opt/keiland`; system Vulkan/Mesa, libdrm, glibc and seat
services remain distribution packages. Select **Keiland** in a display manager,
or run `/opt/keiland/bin/keiland-desktop` as root from a text console.
The Apps configuration is a dpkg conffile. Installation starts no session, and
removal/purge do not erase files in users' homes.

The original `make keiland-linux` and DESTDIR installation keep their development
and demo contents. Packaging filters only its private staging tree.

Limits: amd64 only; QEMU software Vulkan evidence does not certify physical GPUs,
WiFi hardware, or every display manager. Build and test failures stop the target.
Downloads/SSH/apt/build/boot are bounded, and emulator processes are retired even
when a command fails. Artifacts are published only after runtime checks succeed.

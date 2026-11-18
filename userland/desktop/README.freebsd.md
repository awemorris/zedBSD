# Keiland on FreeBSD 15

The native port uses FreeBSD libc, system Vulkan/Mesa, and seatd. The compositor,
renderers, Wayland client and UI libraries use the same shared implementations as
Linux. No Linuxulator is required. The initial build targets FreeBSD 15 amd64.

## Build and install

The verified environment is FreeBSD 15.1-RELEASE-p4 amd64, base Clang 19.1.7,
gmake 4.4.1, Python 3.11.16, Meson 1.10.2, Ninja 1.13.2, Vulkan loader/headers
1.4.356, libdrm 2.4.133, Mesa 26.1.3 and seatd 0.9.3. Install the native build
and runtime prerequisites on the FreeBSD machine:

```sh
sudo pkg install gmake python3 meson ninja vulkan-headers vulkan-loader libdrm mesa-dri seatd
```

From the repository root, build with the host compiler and install:

```sh
make -j8 keiland-freebsd
sudo make keiland-freebsd-install
```

`make keiland-freebsd` first checks with `pkg info` that these packages are
installed; on a terminal it lists the missing ones and asks before installing
them with `sudo pkg install` (y/N), and after a build that succeeded it asks
before running the install (y/N).  Without a terminal, or with `KEILAND_ASK=n`,
it asks nothing: it prints the missing packages and stops, or prints the
install command.

Neither `make toolchain` nor a zedBSD `config.mk` is required. FreeBSD's base
`make` forwards these native targets to the installed `gmake`; GNU make can
also run the same targets directly. A staged installation is optional:

```sh
make keiland-freebsd-install DESTDIR=/tmp/keiland-stage
```

The default prefix is `/opt/keiland`; its private RUNPATH supplies the libraries.
No global `LD_LIBRARY_PATH`, standard library replacement or Python alias is
needed. `KEILAND_PREFIX`, `KEILAND_FREEBSD_BUILD`, `KEILAND_FREEBSD_LOCALBASE` and
`KEILAND_FREEBSD_PYTHON` can select another prefix/build/localbase/interpreter.
Use the same selections for build and install. The default interpreter is
`python3`; this must match the installed native Meson/Python tools. The `python3`
package supplies that command. Set `KEILAND_FREEBSD_PYTHON=python3.11` for the
earlier verified environment if it has only the versioned interpreter.

Linux likewise uses the host compiler: `make keiland-linux`, then
`sudo make keiland-linux-install`. Linux GDM registration is a separate step:
`sudo make keiland-linux-install-session`. FreeBSD GDM launch is outside the
accepted scope; use the local VT procedure below.

The build independently fetches and verifies the pinned MIT seatd client source,
Noto emoji font and Japanese dictionary. The private seat client enables only
the seatd backend. The system seatd daemon supplies device authority; Keiland
does not use the system libseat client, whose installed dependencies may differ.
Private compatibility headers remain in the build directory. Public Keiland,
Wayland-client, UI, PDF and seat client headers are installed under the prefix.
The model/textures, fonts/licenses, dictionary, App Home and generated wallpapers
are installed with the applications. The default wallpaper is generated Aurora;
`KEILAND_FREEBSD_WALLPAPER` selects a different existing picture at build time.

## Seat and graphics

Install and load the FreeBSD drm-kmod driver appropriate to the actual GPU using
that machine's normal FreeBSD driver configuration. Keiland uses standard Vulkan
and its display extensions; a usable DRM primary node and supported Vulkan
physical device are needed for a real screen. Software Vulkan can verify command
execution but does not supply the physical display, input or dma-buf fence gates.

The installed seatd service defaults to the `video` group. Configure the intended
session user for that group, then enable/start the native authority as root:

```sh
pw groupmod video -m SESSION_USER
sysrc seatd_enable=YES
service seatd start
```

Start Keiland from a local VT after the user has a new login with this membership.
The daemon's normal VT-bound configuration supplies native VT activation and
withdrawal. Each device has a separate kernel fd and daemon lease; pause closes
input/output and primary ownership before ACK, resume acquires fresh leases.
Daemon loss ends the compositor. No direct-root device-open fallback exists.
Do not change evdev permissions to make the compositor run.

## Launch

As the session user, launch from the local VT:

```sh
/opt/keiland/bin/keiland-desktop
```

The shared Linux/FreeBSD launcher supplies `--session`, the glass appearance,
installed wallpaper and matching server/client socket. It preserves a supplied
`XDG_RUNTIME_DIR`; otherwise it creates `$HOME/.cache/keiland-runtime` with mode
0700. Additional compositor options are passed through, for example
`keiland-desktop --wallpaper=/path/picture.png` (a PNG or a JPEG). Seat/device selections remain
under the native backend and caller's environment. Linux GDM continues to run
`wayland` directly, without this launcher.

`--session` keeps the desktop running until Log Out or normal termination.
Direct `wayland` without it still stops after 150 seconds. Run as the
ordinary session user from a local console, after logging in again following
the `video` group change. An SSH shell is for building/installing, and does not
supply the local active VT needed by seatd.

The shared compositor has a graphical login screen (`--greeter`). On zedBSD,
`sessiond` supplies its authentication channel and starts the selected user's
session. The native FreeBSD build does not include that session manager:
authenticate at the FreeBSD console first, then start the desktop with
`--session`. That option does not display a login screen. The shared password
lock also needs the session-manager channel and is inactive in this direct
native session.

Select the actual DRM primary node when it differs from `/dev/dri/card0`.
A standard Vulkan loader's normal device/ICD configuration selects the GPU;
`VK_DRIVER_FILES` may explicitly select an installed ICD when needed. The primary
must correspond to that physical device. App Home uses the installed native
`/opt/keiland/etc/keiland/apps.conf`. Terminal uses native `forkpty`/libutil;
Files obtains a kernel mount snapshot and translates user/system extended
attributes through native extattr. Unknown Linux attribute namespaces and
unsupported atomic flags are refused rather than emulated. Audio controls use
OSS `/dev/mixer*`; networking uses actual FreeBSD interfaces/net80211 and the
existing WPA control protocol. A real WPA service and supported radio are needed
for scan/connect/disconnect, and user access must follow that service's policy.

## Verified QEMU i915 configuration

The user selected Intel i915 PCI passthrough after the native Venus prerequisite
check. The tested guest is FreeBSD 15.1-RELEASE-p4 amd64, with the host's isolated
Intel Alder Lake-UP3 Iris Xe (`8086:46a8`) already bound to `vfio-pci`. The test
uses QEMU 10.0.11, Q35/KVM, four CPUs and 4 GiB RAM. Host devices and driver
bindings were preserved; this is a dedicated guest, not a host desktop session.

The GPU is assigned at guest PCI `00:02.0`, with `rombar=0` and
`x-igd-opregion=on`. The CPU setting `host-phys-bits-limit=39` matches this host's
IOMMU address width; use the actual width of the intended host. An emulated VGA
console supplies QMP boot observations separately from the passed-through GPU.
A QMP console screenshot does not capture the Intel display's scanout.

The guest uses `drm-66-kmod-6.6.25.1501000_8`, the matching FreeBSD 15.1 package,
and `gpu-firmware-intel-kmod-alderlake-20260519.1500068`. Load `i915kms` using the
normal native module configuration. The resulting `/dev/dri/card0` and
`renderD128` belong to the Intel GPU. Mesa 26.1.3's
`/usr/local/share/vulkan/icd.d/intel_icd.x86_64.json` selects actual Intel Vulkan;
it is not the software `lvp` ICD. The normal seatd service and the session's
`video` group membership supply device authority.

Native drm-kmod currently exports DMA-BUF files with zero internal access
flags. Their reservation ioctl returns `EBADF` even though the descriptor is
live. The FreeBSD adapters recognize this precise file-query result and report
unsupported transport to Keiland's existing CPU completion/release fallback.
Closed descriptors and invalid completion descriptors keep their original
errors; no fake fence is published. Actual mapped GPU windows and descriptor
ownership were tested. The upstream driver defect remains tracked as BUG-130;
this workaround does not claim to repair its sync-file implementation.

## Verification and limits

Native full build/DESTDIR, installed ELF/public ABI, Vulkan fill/copy, GPU
readback and actual unprivileged compositor/Vulkan windows were verified.
Native seatd VT withdrawal retires input and primary leases asynchronously;
restored VT activation acquires fresh leases and resumes actual GPU frames.
QEMU USB input was observed through the native kernel and Wayland path. CPU
UI/PDF, UFS attributes/mounts, PTY, OSS controls and wired state have separate
native evidence. See WS109's final acceptance record for the application results.

The original Venus configuration booted, but the native guest lacked the
required DRM/Venus ICD path. The current upstream virtio driver also lacks the
required host-visible memory feature. Venus remains unverified; the later user
instruction selected the tested i915 passthrough route. Neither host Venus
support nor lavapipe is reported as successful native Venus use.

The earlier QEMU acceptance waived physical-machine and physical WiFi tests.
The later physical-machine acceptance uses FreeBSD 15.1-RELEASE amd64 on Intel
Tiger Lake Iris Xe (`8086:9a49`), with the same base Clang 19.1.7, native Python
3.12 and installed Mesa 26.1.3/drm-66/seatd stack. Native build and installation
under `/opt/keiland` were verified there. The user subsequently accepted the
local-console session as working on 2026-10-02; this is user acceptance rather
than individual device benchmarks. GDM launch was withdrawn.

Native WiFi
ABI/refusal and WPA wire tests use an independent datagram peer; these do not
prove actual radio scan/association or a system supplicant's persistent storage.
Only FreeBSD 15.1 amd64 and the named Intel GPU stack were exercised. Other
FreeBSD versions, GPU drivers and physical radios remain untested. Linux and
zedBSD regression evidence covers the shared changes made by this port.

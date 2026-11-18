# Architecture

Status: current navigation index

Architecture documents give the external design of each subsystem: its
purpose, its parts and their boundaries, and the reasons for them. They state
the target design; the implementation follows ([rules](../style.md)).

---

## 1. Kernel, Hardware & Platform Layer

- [Kernel, HAL and driver boundaries](kernel-and-hal.md):
  Ownership, storage paths, display paths, public interfaces and implementation limits.
- [HAL and BSP architecture](hal-and-bsp.md):
  Hardware Abstraction Layer (HAL) subsystems, contract with the kernel,
  step-by-step procedures for adding a new CPU architecture and adding a new
  Board Support Package (BSP).
- [Power management](power-management.md):
  S0 low-power idle (S0ix), who decides what, entry and exit sequences,
  device suspension, and the `/dev/system` sleep interface; processor idle
  states; the thermal framework of `/dev/thermal` (sensors, fans, energy
  meters, the strategy, processor performance, passive cooling and the
  emergency stop) and `/sbin/thermal`.

---

## 2. Graphics, Display & Desktop System

- [Graphics and Wayland architecture](graphics-and-wayland.md):
  zedBSD's GPU capability handle architecture (memory sharing without Linux
  `dma-buf`), the native `kl_gpu_buffer_v1` protocol, Vulkan Window System
  Integration (WSI), and `/bin/wayland` compositor modes (`--session`,
  `--greeter`, `--testing`).
- [Session management and graphical login](session-management.md):
  The `sessiond` architecture, privilege separation (`_greeter`), desktop
  lifecycle, automatic login (`autologin`), and automatic text console fallback.
- [Keiland desktop environment](keiland.md):
  Purpose, layers, unified touch and desktop UI/UX, the stable `libkeiland` API,
  internal Wayland extensions, OS backends, and the titlebar.

---

## 3. System Services & Media Subsystems

- [Networking architecture](networking.md):
  The three-tier network architecture: kernel network stack, the `networkd`
  automation daemon (managed LAN, managed WLAN, DHCPv4/v6, SLAAC, DNS), and
  the relationships between administrative tools (`/sbin/net`, `/sbin/ifconfig`,
  and `/sbin/wifi`).
- [Audio architecture and sound subsystem](audio.md):
  The `/sbin/audiod` sound server, high-performance lock-free shared memory
  ring buffers (without socket PCM streaming), cache-line isolation, and the
  clean-room `libpulse.so` compatibility shim.
- [Security design](security.md):
  Privilege boundaries, account administration through the set-user-ID
  `account-admin`, and the shared account core.

---

## 4. Planned Architecture

- [μITRON-compatible real-time domain](muitron-rt-domain.md):
  Confirmed user-mode resident-ELF and explicit-MMIO direction, compatibility
  rationale, POSIX service bridge, and unresolved decisions.

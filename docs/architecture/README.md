# Architecture

Status: current navigation index

Architecture documents give the external design of each subsystem: its
purpose, its parts and their boundaries, and the reasons for them. They state
the target design; the implementation follows ([rules](../style.md)).

## Current architecture

- [Kernel, HAL and driver boundaries](kernel-and-hal.md): ownership, storage,
  public interfaces and implementation limits.
- [Keiland desktop environment](keiland.md) (target design): purpose, layers,
  the stable libkeiland API, internal Wayland extensions, the
  operating-system backend, touch input and the titlebar.
- [Power management](power-management.md) (target design): S0 low-power
  idle, who decides what, the entry and exit sequence, devices, and the
  `/dev/system` sleep interface.
- [Security design](security.md) (target design): privilege boundaries,
  account administration through the set-user-ID `account-admin`, and the
  shared account core.

## Planned architecture

- [μITRON-compatible real-time domain](muitron-rt-domain.md): confirmed
  user-mode resident-ELF and explicit-MMIO direction, compatibility rationale,
  POSIX service bridge, and the unresolved decisions that currently block
  WS015.

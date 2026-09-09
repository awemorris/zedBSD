# Architecture

Status: current navigation index

Architecture documents explain component boundaries, ownership, lifecycle,
and design rationale. They must label current implementation, compatibility
intent, and future design separately. Detailed implementation schedules remain
in the [plan](../../plan/master.md).

## Current architecture

- [Kernel, HAL and driver boundaries](kernel-and-hal.md): ownership, storage,
  public interfaces and implementation limits.

## Planned architecture

- [μITRON-compatible real-time domain](muitron-rt-domain.md): confirmed
  user-mode resident-ELF and explicit-MMIO direction, compatibility rationale,
  POSIX service bridge, and the unresolved decisions that currently block
  WS015.

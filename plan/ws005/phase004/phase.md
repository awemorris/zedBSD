# WS005 Phase 004: minimum primitive WLAN command

Last updated: 2026-09-01

WSID: `ws005`

Phase ID: `p004`

Combined ID: `ws005-p004`

Status: completed (`q059`)

Parent: [WS005 networking and WLAN](../ws.md)

Tests: [WS005 test index](../tests/README.md)

## Objective

Add the smallest useful `/sbin/wifi` command which lets root drive the
completed WLAN ioctls directly. The first implementation is deliberately a
normal-path vertical slice: scan, inspect results, connect to one
WPA2-Personal/CCMP network, inspect status, and disconnect.

`/sbin/wifi` remains a finite, non-resident L2 utility. It does not read a
profile database, run DHCP, configure addresses/routes/DNS, select an automatic
profile, or remain alive to own WLAN state.

## Normal-path-first boundary

This Phase implements only the human, direct-root forms required by q059:

```text
wifi INTERFACE search start
wifi INTERFACE search stop
wifi INTERFACE list
wifi INTERFACE status
wifi INTERFACE connect SSID PASSPHRASE
wifi INTERFACE disconnect
```

The following are intentionally not p004 completion gates:

- `--machine`, `WIFI1`, secret fd 4, ZNV2, and `networkd` child integration;
- profile lookup, persistence, automatic selection, `net wifi`, and DHCP;
- exhaustive malformed-input, output-boundary, signal, cancellation, detach,
  generation-race, and fault-injection matrices; and
- rekey, automatic reconnect, firmware recovery, repeated hotplug, or
  repeatability campaigns.

Those items remain visible in p006/p007, the new p010 primitive-hardening
Phase, WS004 p030 lifecycle hardening, and final p008 acceptance. They are not
silently discarded.

## Dependencies and baseline

- `ws005-p002` freezes the primitive command names and L2 ownership boundary.
- `ws005-p003` makes direct mutating WLAN ioctls root-only.
- WS004 p027 supplies the versioned pointer-free WLAN UAPI and dispatcher.
- WS004 p028 and p029 supply scan plus WPA2-Personal/CCMP L2 behavior.
- The selected adapter and firmware package are available for the following
  p009 physical development check, but physical radio evidence is not required
  to compile p004.

The implementation imports the public WLAN UAPI. It must not duplicate private
kernel or driver structures in userland.

## Minimal operation semantics

- `search start` submits the existing scan-start ioctl and reports whether the
  scan was admitted.
- `search stop` submits scan-stop and is successful when no scan remains.
- `list` prints one bounded snapshot from the existing scan-cache ioctl. It
  distinguishes scanning, completed-empty, completed-with-results, and failed.
- `status` prints the current public WLAN state and, when available, the
  associated public SSID/BSSID/channel facts.
- `connect` submits exactly one bounded SSID/passphrase request and waits, for
  the existing finite kernel deadline, until authorized L2 carrier or a
  terminal error. Success does not imply DHCP.
- `disconnect` submits the existing disconnect operation and returns after
  carrier is down. It does not edit L3 state or a profile file.

Human-readable errors may identify the interface, operation, public state, and
error number. They must never print the passphrase, PMK, PTK, GTK, nonce, or
key material.

## Essential safety retained in the first implementation

Normal-path-first does not waive these inexpensive safety properties:

- exact command arity and `IFNAMSIZ`, 1--32-byte SSID, and 8--63-byte WPA2
  passphrase limits are checked before copying or issuing an ioctl;
- fixed-size buffers and checked return values are used throughout;
- waits are finite and use the existing kernel operation deadline;
- the mutable userspace passphrase/request storage is explicitly erased on
  every ordinary exit path;
- output is bounded by the fixed scan-result maximum already published by the
  UAPI; and
- `/sbin/wifi` is installed mode `0755`, never setuid or setgid. Kernel
  authorization remains authoritative.

The public argv form has the accepted shell-history/process-list exposure of a
direct administrative command. q059 does not claim that argv is a secret
transport. The later `networkd` child path avoids argv through p006.

## Ordered implementation

1. Inventory the completed p027-p029 public request constants and current
   `/sbin` package/install convention.
2. Add one small parser and ioctl dispatcher for the six public forms.
3. Add bounded human rendering for list and status.
4. Add connect waiting, terminal-state reporting, passphrase erasure, and
   disconnect.
5. Install `/sbin/wifi` in the selected amd64 image.
6. Run the focused normal-path fixture, affected builds, and bounded boot
   regression, then hand the same candidate to p009.

## Verification and completion

Automatic evidence for p004 is intentionally small:

- a production-ioctl fake WLAN executes one sequence of search start,
  completed list, status, connect through authorized carrier, disconnect, and
  search stop;
- empty and oversized interface/SSID/passphrase values fail before an ioctl;
- captured normal output and errors contain no supplied passphrase or derived
  key material, and mutable userspace secret storage is cleared;
- the command builds, is installed in the amd64 image, and affected ordinary
  build/QEMU boot regressions pass; and
- no operation invokes `dhcpc`, edits a profile/config file, or stays resident.

p004 completes at this minimum direct-command boundary. Physical IP
communication is then checked once by p009. Broader primitive abnormal and
semi-normal coverage is p010 and is intentionally performed after the first
successful vertical path.

## Reconsideration boundary

Return to planning only if the existing public UAPI cannot express one of the
six operations, if successful WPA2 association requires a resident userspace
engine, or if the command would have to expose kernel-private structures.
Ordinary implementation bugs on the direct happy path are fixed inside p004;
they are not reasons to import p006/p007/p010/p030 scope.

## Execution result

Completed in q059 on 2026-09-01. `/sbin/wifi` implements the six declared
human direct-root forms and is installed in the amd64 image. The focused
production-ioctl fixture passes search start/stop, completed list, status,
connect through authorized carrier, and disconnect; it also covers the basic
pre-ioctl length rejections and mutable userspace secret clearing required by
this Phase.

The final q059 candidate is `build/amd64/hdd-image.img`, 203423744 bytes,
SHA-256
`6d0ec924f0d063b663c6da8ab1a7b8b39bcef8b2b2849e4fb0d8308340f9ed75`.
Its one physical-equivalent USB-passthrough run completed p009's normal path.
No profile persistence, DHCP ownership, `networkd` protocol, or `net wifi`
orchestration was added to this primitive. Those remain p006/p007 work, and
the intentionally deferred abnormal/semi-normal matrix remains p010.

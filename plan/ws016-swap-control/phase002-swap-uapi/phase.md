# WS016 Phase 002: `/dev/system` runtime-swap UAPI

Last updated: 2026-08-27

WSID: `ws016`

Phase ID: `p002`

Combined ID: `ws016-p002`

Status: Planned; Queue-ready after `ws016-p001`

Parent: [WS016](../ws.md)

Tests: [WS016 test index](../tests/README.md)

## Objective

Expose the runtime manager through a bounded, versioned zedBSD UAPI without
claiming a POSIX `swapon(2)` or `swapoff(2)` interface.

## Fixed UAPI contract

Extend `<zedbsd/system.h>` with version-1 control and source-information
structures and new `/dev/system` ioctl numbers after the existing file-usage
request:

- `ZEDBSD_SYSTEM_SWAP_ADD` takes a bounded NUL-terminated source selector;
- `ZEDBSD_SYSTEM_SWAP_REMOVE` takes the same selector and completes only after
  drain/removal or returns an error with a usable source retained;
- `ZEDBSD_SYSTEM_GET_SWAP_SOURCE` enumerates source IDs 0--3 and reports
  inactive/active/draining state, header version, total/used pages, source
  UUID/label when present, and its diagnostic source spelling.

Every structure begins with `version` and `struct_size`. Flags and reserved
members are zero-only in version 1. Source text is at most 255 bytes plus NUL.
The kernel validates the complete input before mutation and copies out only a
fully initialized structure. Control requests require effective UID 0;
enumeration does not. No kernel pointer, physical disk address, inode pointer,
or implementation lock state is exposed.

`bootN:PATH`, absolute regular-file paths, `/dev/NAME`, `UUID=...`, and
`PARTUUID=...` resolve through the manager's canonical identity rules. The
diagnostic spelling returned by enumeration does not become object identity.

## Work packages

1. Add the UAPI constants/structures with 32/64-bit layout assertions and
   reserved expansion space.
2. Add system-device copyin/copyout, credential, string, and operation dispatch
   with no lock held across user memory access.
3. Connect add/remove/enumeration to p001 manager transactions.
4. Produce exact errno behavior for bad version/size/flags/string, privilege,
   unsupported file backend, duplicate identity, full registry, unsafe commit
   reduction, page-in/I/O failure, and unknown source.
5. Update UAPI and extension documentation without adding libc functions.

## Verification and completion conditions

SWAP-T007 and SWAP-T008 must prove native and compat structure layout, invalid
pointer/string rejection, non-root `EPERM`, canonical alias matching, source
state/stats enumeration, interrupted calls, and failure atomicity. The Phase is
complete when those tests, p001 regressions, `make -j16`, and
`git diff --check` pass and no failed ioctl changes source ownership or VM
commit capacity.

## Reconsideration boundary

Stop if `/dev/system` cannot identify the calling credentials or keep the
operation's source/VM lifetime stable. Do not substitute an undocumented
numeric syscall or expose kernel-private manager structures.

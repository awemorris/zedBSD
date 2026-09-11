# WS001 Phase 016: direct PDF printing over LPD

Last updated: 2026-08-31

WSID: `ws001`

Phase ID: `p016`

Combined ID: `ws001-p016`

Status: Complete implementation milestone (`agent2-q001`, 2026-08-31)

Parent: [WS001](../ws.md)

Tests: [WS001 test index](../tests/README.md)

## Objective

Replace the deliberate `lp` failure command with native `lp` and `lpr`
frontends that submit PDF data directly to an LPD printer.  There is no local
persistent spool queue, scheduler, retry daemon, conversion pipeline, or print
service.

## Fixed product contract

- A destination is written `host[:port]/queue`.  `lp -d` and `LPDEST` select
  it for `lp`; `lpr -P` and `PRINTER` select it for `lpr`.  The command line
  wins over the environment.  Absence or malformed syntax fails before a
  network connection.
- TCP port 515 is the default.  The client performs the LPD receive-job,
  control-file, and data-file exchange with bounded field sizes and checks
  every one-byte acknowledgement.
- Input must be PDF. Input is copied to a securely created, immediately
  unlinked, bounded-lifetime staging file because LPD requires an immutable
  byte count; this is not a durable spool queue.
- The raw-data control-file form is used.  Printer-side PDF support is
  required; zedBSD does not translate PDF, PostScript, or text.
- `lp` implements `-c`, `-d`, `-m`, `-n`, `-o raw`, `-s`, and `-t`, plus the
  Issue 8 `LPDEST` then `PRINTER` precedence. `-w` fails explicitly because a
  bare LPD exchange cannot prove physical print completion. `lpr` is a
  BSD-compatible convenience frontend over the same client.
- Multiple operands are separate jobs.  A failure is reported per job and the
  final status is nonzero if any submission fails; successful earlier jobs are
  not rolled back.

## Work packages

1. Replace `userland/base/lp`'s deferred-stub package and add the `lpr` package
   registration without changing the generic package interface.
2. Implement one private LPD client module and thin `lp`/`lpr` option
   frontends; do not duplicate protocol state machines.
3. Validate destination, queue, host, user, title, filename, size, PDF magic,
   and control-file fields before publication; reject newline/control
   injection and overflow.
4. Handle DNS/connect/write/read/timeout/negative-acknowledgement failures with
   truthful diagnostics, complete descriptor cleanup, and no leftover staging
   file.
5. Add a deterministic fake LPD server that records byte-for-byte request
   order and injects refusal at every acknowledgement boundary, plus local
   invalid-input and malformed-destination cases.
6. Build standalone and through the top-level image; run a disposable amd64
   guest binary/error-path smoke and retain guest network submission as a
   separate integration handoff when a QEMU-reachable NIC is configured.

## Completion conditions

- `/bin/lp` and `/bin/lpr` are native local implementations and `lp` no longer
  points to `deferred-stub/main.c`;
- valid file and stdin PDF jobs reach a fake LPD receiver with the declared
  control/data sizes and safe names;
- tested protocol refusals and local validation failures leave no persistent
  job or staging artifact;
- no local daemon or durable spool directory is introduced;
- all changed C/header files pass p015's style contract; and
- focused host/protocol tests, standalone builds, `make -j16`, the bounded
  amd64 binary/error-path runtime cell, and `git diff --check` pass.

## Execution result and retained boundaries

`userland/base/lp` now owns a native frontend and one private LPD client;
`userland/base/lpr` registers the alternate frontend over the same sources.
The old deferred-command table no longer contains `lp`. The fake receiver
proves file and stdin PDF payloads, sizes, safe control fields, copy count,
mail/title fields, request IDs, environment precedence, and refusal at the
receive-job, data-header, data-body, control-header, and control-body ACKs.

```text
LPD-T001 direct lp/lpr protocol and refusal matrix: PASS
native amd64 lp/lpr ELF checks: PASS
make -j16: PASS
AGENT2-Q001 amd64 utility smoke: PASS
```

This is not a POSIX `lp` review. Issue 8 specifies text input, permits an
unspecified default destination, and requires `-w`; this product contract is
PDF-only, requires an explicit direct destination, and rejects `-w`. Multiple
operands are independent LPD jobs. Timeout/early-close fault injection and a
guest-to-fake-server transaction remain unproved. The selected amd64 config
has NE2000 disabled and no maintained QEMU-reachable Ethernet driver, so the
guest cell proves installation and truthful local rejection only.

## Reconsideration boundary

Stop for review if the target printer requires IPP, TLS/authentication,
discovery, format conversion, a persistent retry queue, or a destination
namespace other than the approved `host[:port]/queue` spelling.  Do not grow a
print service implicitly inside these commands.

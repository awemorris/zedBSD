# WS020 Phase 004: Intel Mac physical UEFI bring-up

Last updated: 2026-09-05

WSID: `ws020`

Phase ID: `p004`

Combined ID: `ws020-p004`

Status: complete; the user confirmed successful Intel Mac real-hardware
operation on 2026-09-05 and accepted it without the older five-run campaign

Parent: [WS020](../ws.md)

## Objective

Boot the frozen fixed UEFI-only artifact on the declared Intel Mac and reach
the ordinary zedBSD login without adding a BIOS fallback to the image.

## Procedure

1. Record exact Mac model/year, firmware version, CPU, installed RAM, Secure
   Boot setting if present, boot picker path, target medium model, and its
   512-byte sector count. The physical medium may be larger than the fixed GPT
   extent; no capacity selection or exact-size match is required.
2. Use only the p005 production-checked `build/amd64/hdd-image.img`; do not use
   a `.pre-*`, p002/p003 evidence copy, or an artifact produced before the
   fixed UEFI-only contract. Identify it by repository revision,
   byte length, and SHA-256. Hash
   `f811a0f5eff70f8081b6725f417355afa9ef1bf14e0c6d24fd1900823ad09c96`
   is the already tested and failed observation, not the next handoff. P006
   must refresh p005 and record the replacement frozen identity first. State
   the purpose and provide that exact file link for the next single human
   check.
3. Write and verify the image, then perform one physical boot. This single
   provisional observation is sufficient to return logs and continue fixes;
   do not require repeated human boots between fixes.
4. Capture the last successful marker or full failure screen. A failure after
   GPT scan must include either every `sda partition N` publication or the new
   exact `create failed (error N)` / zero-block diagnostic. Resolve only
   defects inside the declared Variant/GPT/amd64 boot contract; extract new
   hardware-driver work into WS004 if necessary.
5. Once provisional acceptance passes and all corrections are frozen, perform
   the final five consecutive cold-boot campaign with the same artifact.

## Latest provisional result

The refreshed p005 artifact reached zedBSD on the Intel Mac far enough to
attach the expected 60,549,120-sector USB Mass Storage device as `sda`. Its
boot parameters named payload UUID `FDC1-A4EF`, matching the current p005
source. GPT discovery then failed before any partition publication:

```text
gpt: sda rejected: bounded extent requires both copies primary=3 backup=3
vfs: scan sda H/S=255/63 blocks=60549120: -3 entries
vfs: boot0 selector resolution failed (error 6)
```

Errno 3 is `EINVAL`. The pristine same-size QEMU copy passes, so this is not a
payload UUID or selector defect. The leading diagnosis is that host software
relocated GPT metadata to the USB medium's physical end while retaining the
compact PMBR advertisement. P006 owns the GPT-over-PMBR precedence rule
and automatic regression. After those gates pass, p004 needs one provisional
boot of the newly frozen p005 artifact; the five-run campaign remains last.

## Completion conditions

The exact frozen image boots through Apple's UEFI path, reaches init/login with
the expected root/data/swap configuration, contains no BIOS boot path, and
passes the final five-run campaign. The p006 repair must first pass its one
provisional physical boot. Physical repetition otherwise occurs only at the
end unless a genuinely probabilistic defect requires a separately justified
campaign.

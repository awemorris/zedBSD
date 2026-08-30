# WS020 Phase 004: Intel Mac physical UEFI bring-up

Last updated: 2026-08-30

WSID: `ws020`

Phase ID: `p004`

Combined ID: `ws020-p004`

Status: Planned physical checkpoint after `ws020-p003`

Parent: [WS020](../ws.md)

## Objective

Boot the frozen UEFI-only artifact on the declared Intel Mac and reach the
ordinary zedBSD login without adding a BIOS fallback to the image.

## Procedure

1. Record exact Mac model/year, firmware version, CPU, installed RAM, Secure
   Boot setting if present, boot picker path, target medium model, and exact
   512-byte sector count.
2. Select the matching configured capacity; if none matches exactly, stop at
   the WS reconsideration boundary rather than knowingly writing a mismatched
   GPT.
3. Write and verify one frozen compact image, then perform one physical boot.
   This single provisional observation is sufficient to return logs and
   continue fixes; do not require repeated human boots between fixes.
4. Capture the last successful marker or full failure screen.  Resolve only
   defects inside the declared variant/GPT/amd64 boot contract; extract new
   hardware-driver work into WS004 if necessary.
5. Once provisional acceptance passes and all corrections are frozen, perform
   the final five consecutive cold-boot campaign with the same artifact.

## Completion conditions

The exact frozen image boots through Apple's UEFI path, reaches init/login with
the expected root/data/swap configuration, contains no BIOS boot path, and
passes the final five-run campaign.  Physical repetition occurs only at the
end unless a genuinely probabilistic defect requires a separately justified
campaign.

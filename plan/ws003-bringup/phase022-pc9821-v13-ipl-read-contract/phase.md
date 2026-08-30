# WS003 Phase 022: PC-9821V13 IPL stack and disk-read contract

Last updated: 2026-08-31

Phase ID: `ws003-p022`

Status: Uncleared (`q037`); automatic checkpoint passed, awaiting the already
requested one PC-9821V13 observation

Parent: [WS003](../ws.md)

Tests: [WS003 test index](../tests/README.md), especially `BR-T54`

## Objective

Restore physical boot of the ordinary i386 PC-98 disk image on the user's
NEC PC-9821V13.  Preserve the native PC-98 disk layout while making the first
two IPL stages obey a self-owned stack and the PC-98 INT 1Bh fixed-disk
contract before they chain-load the BOOT partition.

The automatic result is one frozen `build/pc98/hdd-image.img` whose structure
and QEMU boot pass.  Completion additionally needs one user-operated V13 boot
that advances beyond the current beep-and-stop boundary.  Repeated physical
boots remain a later final-acceptance activity.

## Captured boundary

The V13 executes the image and then emits a beep before any zedBSD loader or
kernel text appears.  All early loader failures currently converge on an
indistinguishable beep, so this observation does not identify whether LBA 2,
the native partition table, the BOOT entry, or the partition PBR failed.

Inspection of the exact generated image disproves the PC/AT-MBR hypothesis:

- bytes 446--507 of LBA 0 contain no PC/AT partition entries;
- bytes 508--509 retain the PC-98 IPL metadata word `9`;
- bytes 510--511 contain `55 aa`;
- LBA 1 is the sixteen-entry native PC-98 partition table;
- LBA 2 begins the native BOOT-partition selector.

The PC-9821V13 does not require the trailing signature, but it also does not
turn this otherwise native layout into a PC/AT MBR.  Retain `55 aa` for later
PC-9821 compatibility; removing it is not a fix for the observed read failure.

The pre-refactor Stage 1 established a private `SS:SP` and performed INT 1Bh
AH=`84h` SENSE before AH=`06h` reads.  The current LBA-0 IPL performs stack
operations and an INT 1Bh read while relying on firmware stack state and
without SENSE.  The current LBA-2 selector also uses the inherited stack
before installing its own.  QEMU accepts that sequence, but the physical V13
does not establish that it is portable.

## Fixed decisions

1. Keep the PC-98-native LBA 0/LBA 1/LBA 2 layout.  Do not add a PC/AT MBR,
   GPT, active flag, or compatibility partition entry.
2. Keep `IPL1`, the word-9 PC-98 metadata, and the trailing `55 aa` signature.
3. Do not use the inherited firmware stack.  Before the first `push`, `call`,
   or software interrupt that can depend on it, establish a bounded private
   stack which cannot overlap the relocated IPL or its load buffers.
4. Preserve the firmware-supplied `SI`, `DI`, and boot-device identity and
   restore the expected values before every INT 1Bh call and chain-load.
5. Issue AH=`84h` SENSE and validate carry/status, 512-byte sector size, and
   nonzero geometry before the first disk read.  Re-establish `ES:BP` after
   SENSE rather than assuming the firmware preserved it.
6. Continue to address the raw LBA-2 selector as physical CHS 0/0/2.  The
   LBA-2 selector consumes the native absolute CHS stored in the PC-98
   partition entry when loading its PBR; do not reinterpret it with a
   V13-only geometry.
7. Normal boot remains silent.  Failures must emit a two-character code to
   QEMU debug port E9 and the physical PC-98 text VRAM, followed by one audible
   alarm: `1S` for SENSE, `1R` for LBA 2, `2T` for the partition table, `2N`
   for BOOT-name lookup, and `2P` for the PBR.  Write complete character and
   attribute cells so stale VRAM high bytes cannot hide the code.
8. A reset plus bounded retry may be added only around an otherwise valid BIOS
   disk read.  Invalid SENSE data, missing BOOT entry, or invalid loader
   signature fail immediately; no unbounded retry is permitted.

## Implementation plan

1. Refactor `bootloader/pc98/disk-ipl.S` so relocation itself needs no stack,
   then install the private stack before all stack-using code.
2. Restore the validated SENSE/read sequence, preserve the ROM entry
   registers, and explicitly set the complete INT 1Bh register contract.
3. Apply the same no-inherited-stack rule to `bootloader/pc98/lba2.S`, then
   consume the native partition table's stored CHS values without changing
   the on-disk partition format.
4. Add bounded stage-specific diagnostics which cannot appear on a successful
   boot.  Keep every fixed IPL binary within its existing sector budget.
5. Add a Phase-owned source/binary contract fixture.  It must reject a PC/AT
   partition entry, missing PC-98 metadata/signature, stack use before stack
   ownership, missing SENSE validation, a changed selector LBA, or a Stage-2
   overflow.
6. Rebuild from the ordinary PC-98 configuration with `make -j16`, run the
   existing image checker, and boot the production image under qemu-pc98 to
   init/login.  Do not use `make check` or `.internal/`.
7. Freeze and hash the exact image for one V13 boot.  If it still stops, use
   the diagnostic code from that one observation to define a follow-up Phase;
   do not repeatedly ask for speculative physical boots.

## Completion conditions

- neither LBA-0 nor LBA-2 IPL uses the inherited firmware stack;
- SENSE and fixed-disk read inputs are validated and load-buffer registers are
  re-established explicitly;
- the image remains a native PC-98 layout with word `9`, `55 aa`, LBA-1 table,
  and LBA-2 selector, and contains no PC/AT partition entry;
- focused source/binary/layout fixtures and the existing PC-98 image checker
  pass;
- `make -j16` succeeds and the production image reaches init/login in
  qemu-pc98 without a loader diagnostic;
- one identified image advances beyond the beep boundary on the PC-9821V13;
- `git diff --check` passes and the exact physical artifact hash is recorded.

## Automatic checkpoint (2026-08-31)

The implementation and every automatic completion gate pass.

- LBA 0 and LBA 2 establish `SS:SP=9000:fffe` with interrupts disabled before
  their first stack-dependent operation. Relocation occupies only
  `9000:0000`--`9000:01ff`.
- LBA 0 performs AH=`84h` SENSE, validates BIOS status, 512-byte sectors, and
  nonzero heads/sectors, then re-establishes `ES:BP` immediately before its
  AH=`06h` LBA-2 read.
- Both stages preserve firmware `SI`, `DI`, and boot-device identity.
- Failure-only `1S`, `1R`, `2T`, `2N`, and `2P` codes write complete character
  and attribute cells to PC-98 text VRAM and also reach debug port E9 before
  one audible alarm.
- `BR-T54` proves stack ordering, SENSE/read markers, 512-byte Stage 1,
  14-sector Stage 2, word `9`, `55 aa`, native LBA-1 partition table, exact
  installed loader bytes, and zero PC/AT partition entries.

Verification evidence:

| Gate | Result |
| --- | --- |
| `make -j16` | PASS |
| `BR-T54` Noct source/binary/layout fixture | PASS |
| production `make check-disk-image` | PASS |
| maintained BR-T46 `pc98/default` cell | PASS 1/1 to `login:` |
| independent final qemu-pc98 VRAM capture | PASS to `init: system running` and `login:` with no IPL diagnostic |
| BOOT-name corruption | PASS: E9 and full-cell VRAM both contain `2N` |
| `git diff --check` | PASS |

An invalid stored CHS and an exact sector-2048 blkdebug EIO were also tried as
bounded `2P` injections. In both cases qemu-pc98 remained inside its ROM/INT1B
path rather than returning carry to LBA 2, so no production branch could emit
`2P`. This is recorded as an emulator fault-injection limitation, not hidden
as a passing live cell; the source fixture still proves the branch and the
same full-cell diagnostic implementation already executes in the `2N` cell.

The frozen physical handoff artifact is:

| Property | Value |
| --- | --- |
| Image | `/home/awe/zedBSD/build/pc98/hdd-image.img` |
| Size | 135,266,304 bytes |
| SHA-256 | `d2bfc9c45077434670f4dd0578b26d295653eef98413ab31b667ca8d3368ed4d` |

## Physical handoff (one observation)

Boot the exact frozen image above once on the NEC PC-9821V13. The purpose is
only to determine whether the private stack plus validated SENSE/read sequence
advances beyond the original beep boundary. A successful loader/kernel screen
is sufficient for this Phase; repeated boots are deferred to final acceptance.

If it still stops, report the two characters at the upper-left corner:

- `1S`: the V13 rejected or returned invalid SENSE data;
- `1R`: the first raw LBA-2 selector read failed;
- `2T`: the native LBA-1 partition-table read failed;
- `2N`: the BOOT partition name was not found;
- `2P`: the native partition PBR read failed.

The original undifferentiated beep without a visible code is also meaningful
and must be photographed rather than retried.

## Reconsideration boundary

Stop and request human input if the V13 observation identifies a disk-interface
or firmware geometry incompatible with the fixed 512-byte INT 1Bh contract, if
the boot medium is not exposed through the expected fixed-disk BIOS service,
or if compatibility requires replacing the native on-disk layout.  Those are
not permission to remove `55 aa`, introduce a PC/AT MBR, or weaken other PC-98
targets silently.

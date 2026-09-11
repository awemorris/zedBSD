# WS003 Phase 025: HAL clock-source split and amd64 SMP monotonic counter

Last updated: 2026-09-03

Phase ID: `ws003-p025`

Status: Automatic milestone complete (`q066`); one physical observation is
shared with `ws004-p038` and remains pending

Parent: [WS003 bring-up](../ws.md)

## Objective

Replace the ambiguous single RTC read with the approved wall-clock/counter
pair, and make the amd64 implementation of `hal_rtc_read_counter()` a proven
system-wide fixed-frequency monotonic counter when a caller migrates between
CPUs. Invariant-TSC capability establishes a constant rate but does not by
itself establish equal offsets between logical CPUs. The q065 AX211 path needs
a real microsecond deadline source and correctly fails when it is unavailable;
this Phase supplies that platform service rather than adding a device-local
PIT, TSC calibration, or time clamp.

## Public contract

The only public time-source operations are:

```c
bool hal_rtc_read_epoch_time(uint64_t *unix_seconds);
bool hal_rtc_read_counter(uint64_t *counter, uint64_t *freq_hz);
```

- `hal_rtc_read_epoch_time()` returns whole UTC seconds since the Unix epoch.
  It may be unavailable on a platform without a usable wall clock.
- `hal_rtc_read_counter()` returns a boot-local raw counter and its nonzero
  fixed frequency. Its epoch is unspecified and only sample differences have
  meaning; it has no implied relationship to epoch time.
- On `false`, output arguments remain unchanged. On `true`, both outputs refer
  to the same source, the frequency remains stable for that boot, and no value
  successfully returned after an earlier successful value is smaller, even
  across CPU migration or concurrent readers.
- Reading the counter provides no general memory-ordering guarantee. Callers
  use ordinary HAL synchronization separately when ordering shared state.

## Scope and design boundary

- Rename all existing wall-clock definitions/callers to
  `hal_rtc_read_epoch_time()` and provide a truthful counter implementation on
  each maintained HAL: use an existing conforming architectural counter where
  one exists, otherwise return `false` without changing outputs.
- Keep calibration and cross-CPU validation in the amd64 PC/AT HAL clock and
  SMP bring-up path. Do not add another public HAL operation or make the
  generic kernel or a device driver responsible for TSC policy.
- Separate TSC/timecounter ownership from the LAPIC timer implementation.
  LAPIC/PIT initialization may provide a bounded calibration window, but the
  private amd64 timecounter component owns source selection, calibrated
  frequency, validation, publication, and fail-closed state.
- Prefer valid CPUID leaf `0x15` ratio/crystal data and use the already bounded
  PIT channel-2 window as fallback. Missing TSC or invariant-TSC support leaves
  the counter unavailable without preventing the OS from booting.
- A PIT-calibrated BSP candidate may be admitted on SMP only when every AP has
  matching TSC, invariant-TSC, CPUID, and optional `IA32_TSC_ADJUST` metadata,
  and all eight real TSC bracket rounds pass within the uncertainty bound.
  PIT calibration alone is not evidence of a system-wide counter.
- Do not publish a calibrated BSP TSC immediately. Extend the existing private
  INIT-SIPI per-CPU startup state with a bounded generation handshake: bracket
  serialized AP samples with BSP samples, retain the narrowest valid round,
  and validate every admitted CPU before publication. This is not a new public
  rendezvous API.
- Require compatible TSC/invariant-TSC and frequency metadata on every CPU.
  A sample outside its BSP bracket, an incomplete/ambiguous handshake, overflow,
  or incompatible source metadata fails the complete counter publication.
- Inspect `IA32_TSC_ADJUST` only when the CPU advertises that contract. The
  first implementation may reject inconsistent offsets and leave the counter
  unavailable; it must not write TSC/TSC_ADJUST or silently normalize CPUs
  without a separately reviewed policy.
- Always release an AP from the private probe even when its validation fails;
  counter unavailability must not strand CPU startup. Publish only after the
  complete admitted CPU set passes. A single-CPU system may publish after its
  BSP source and frequency checks pass.
- After AP timer initialization and ready publication, hold every AP at a
  second private boot gate.  Once the full set is waiting, provisionally
  publish the candidate and release all CPUs into a bounded, concurrent check
  through the real `hal_rtc_read_counter()` entry point.  Publish the final
  READY result only when the BSP and every AP complete with one stable nonzero
  frequency and no failed/backward read.  Disable the counter on any failure,
  and release every AP to generic boot on every exit path.
- Protect the successful-read boundary with a HAL-global atomic last raw sample.
  If a raw sample is below a value already returned successfully, atomically
  make the counter unavailable and return `false` without exposing that sample.
  This is a fail-closed safety gate, not clamping or offset compensation.
- AX211 and other consumers continue to treat unavailable, changed-frequency,
  or backward observations as checked errors. They do not read TSC directly.

## Verification

- Header/caller fixtures on amd64/i386 prove the exact two-function API, unchanged outputs on
  failure, nonzero/stable successful frequency, and truthful false
  implementations on unsupported HALs.
- Private policy fixtures cover CPUID.15 and PIT fallback, equal/bracketed
  samples, best-round selection, bounded sampling uncertainty, positive and
  negative inconsistent offsets, arithmetic limits, missing invariant TSC,
  mismatched CPU metadata, and inconsistent optional TSC_ADJUST reports.
- An amd64 UEFI SMP QEMU fixture executes concurrent readers on every admitted
  CPU and observes one stable frequency and no successful backward sample.
- A negative build/runtime cell injects inconsistent per-CPU data and proves
  that the counter remains unavailable while the OS continues booting. A
  separate backward-sample fixture proves that the offending raw value is never
  returned successfully and publication becomes unavailable.
- Existing amd64 early-init and AX211 PCI-MMIO deadline tests remain green; the
  latter must reject counter absence, frequency change, overflow, and backward
  samples without a private timing fallback.
- i386 receives the API rename and truthful unavailable-counter behavior and
  passes its affected focused/build gates. arm64, sparcv9, and X68000 receive
  source-contract review and configured cross-build gates only; no non-x86
  runtime or focused execution test is required in this Phase.
- One supported multi-core physical machine passes the same bounded reader
  stress after all CPUs are online. In q066, this observation is shared with
  p038's one final direct AX211 boot instead of requesting an intermediate
  human boot.
- `make -j16` and the existing amd64 early-init, timer, and AX211 MMIO tests
  remain green.

## Completion conditions

- Counter availability is a checked property of the complete online CPU set,
  not an inference from the invariant-rate bit alone.
- Cross-CPU migration cannot expose a backward value to a successful caller.
- The approved two-function public HAL API is the only public change;
  hardware-specific calibration and validation stay out of AX211 and other
  consumers.
- Automatic QEMU/build gates pass, and the final shared physical observation
  either passes or is recorded as the sole explicit human resume condition.

## Q066 automatic evidence

- The host policy fixture covers CPUID.15/PIT arithmetic, CPU metadata and
  `IA32_TSC_ADJUST` disagreement, eight-round brackets, uncertainty bounds,
  backward-sample fail-closed behavior, and concurrent guarded reads.
- Production amd64 UEFI KVM boots with one and four CPUs reached `login:`.  The
  four-CPU run admitted a PIT-calibrated invariant TSC only after every AP
  passed all eight brackets, then every CPU completed 64 calls through the
  public counter entry point before READY publication.
- A separate four-CPU build injected incompatible CPU metadata.  It rejected
  the complete counter set, verified 64 unavailable calls with unchanged
  outputs on every CPU, released all four CPUs, and still reached `login:`.
- The configuration-stamp regression changes fault-injection flags in one
  existing build directory and proves that the timecounter object is actually
  rebuilt; a repeated identical invocation remains incremental.
- Fresh i386 PC/AT and PC-98 configured kernel builds pass.  Fresh configured
  arm64/Raspberry Pi 4, sparcv9/sun4u, and m68030/X68000 kernel builds also
  pass; no runtime test is required for those three non-x86 targets.
- `make -j16`, the focused early-init fixture, and `git diff --check` pass.
  The only remaining evidence is the one shared direct-boot observation
  described above.

## Execution intake

The q065 carry-over already contains an uncommitted draft of the API rename,
architectural implementations, CPUID.15/PIT frequency policy, and the AX211
checked consumer. It is not completion evidence: amd64 currently exposes the
BSP result before AP validation and keeps timecounter state in the LAPIC module.
Q066 must correct those boundaries rather than merely committing the draft.

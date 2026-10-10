# RPi4 PCIe boot exception / 2026-10-11

Base main `df1d2be263514710d645da115959609fc1a6ec77`.
Owned branch/worktree `codex/rpi4-pcie-boot` / `.claude/worktrees/rpi4-pcie-boot`.
[Config](rpi4-pcie-boot-config-20261011.mk), [user photo](rpi4-pcie-boot-20261011.png).

## Diagnosis

Photo ESR0x96000210/EC0x25/DFSC0x10: synchronous external data abort at current EL1.
ELR0xffff0000000cb44c matches the current main kernel instruction:

```text
ffff0000000cb440: add x0,x0,#0x509
ffff0000000cb448: str x8,[x19,#0x50]
ffff0000000cb44c: ldr w2,[x8,x9]     (x9=0x406c)
ffff0000000cb450: dmb oshld
```

Function drv_pci_brcmstb_start reads PCIe revision offset0x406c before bring_up/reset_controller.
FAR0xffff0000fd50406c is the direct-map alias of controller0xfd500000+revision0x406c.
The last URB log is from driver registration before PCIe start, not evidence of an URB wait fault.

Independent concrete source defect: range_is_ram checks `ram_end - physical < size` without proving
physical is below ram_end. In an 8GiB split RAM map, 0xfd500000 lies beyond the first bank; unsigned
subtraction wraps and falsely accepts it as RAM. Device mapping then returns an existing cacheable
alias instead of making the PCIe register region Device-nGnRE. The same bug affects other MMIO gaps
and PCIe BAR space above RAM. The exact controller power state cannot be proven from the photo.
Fix mapping classification and delay revision read until the existing bridge/SerDes initialization
settles, with a safe progress log before accessing the controller.

Hardware behavior comparison: [Linux brcmstb source](https://github.com/torvalds/linux/blob/master/drivers/pci/controller/pcie-brcmstb.c)
and [Linux PCI maintainer patch discussion](https://lkml.iu.edu/hypermail/linux/kernel/2207.2/00363.html)
use setup before revision read. Consulted only as behavior evidence; no Linux source/name/table copied.
Existing zedBSD WS048 design/reset mechanism reused, Zlib unchanged.

## Verification

Commands executed in owned worktree:

```sh
make -j16 ZEDBSD_CONFIG=plan/ws048/tests/rpi4-pcie-boot-config-20261011.mk \
  -o build/arm64/sysroot/.zedbsd-sysroot-complete vmunix
make -f plan/ws048/tests/host-test.mk OUT=build/boot-fix/host \
  build/boot-fix/host/brcmstb-host-test
make -f plan/ws048/tests/host-test.mk OUT=build/boot-fix/host \
  DTB=build/boot-fix/bcm2711-rpi-4-b.dtb build/boot-fix/host/disabled.dtb
ASAN_OPTIONS=detect_leaks=0 build/boot-fix/host/brcmstb-host-test \
  build/boot-fix/bcm2711-rpi-4-b.dtb build/boot-fix/host/disabled.dtb
```

- Full current config kernel build: PASS, warnings/errors0. Existing arm64 Image/ELF checker PASS.
  Logs `build/boot-fix/kernel-build.log`, `kernel-build-final.log`.
- brcmstb existing host register model plus powered-down revision rejection: 1295 checks PASS
  under ASan/UBSan, warnings0; `host-build.log`, `host-check-2.log`.
  Initial run omitted required disabled DTB argument (usage failure); corrected argument run's
  LeakSanitizer cannot run under environment ptrace. Disable leak checking only for host model;
  ASan bounds and UBSan remain enabled. No production environment switches introduced.
- Same augmented model with baseline production driver: FAIL at HARD_DEBUG_IDDQ revision guard,
  demonstrating it catches the original premature read. Compiled baseline copy resides in build/boot-fix only.
- Exact production range_is_ram extracted unchanged into a tiny host harness with two split banks:
  RAM starts/ends, crossing bank end, peripheral hole0xfd500000/mailbox, high RAM, after last bank,
  PCIe BAR0x600000000: PASS, UBSan and warnings0. Baseline function fails PCIe hole assertion.
  Harness is disposable `build/boot-fix/ram-boundaries.c`; no extra maintained test/tool introduced.
- clang-format19 changed ranges produce identical code; final manual C standard review covers
  changed functions/paragraphs, ordering, arithmetic, ownership and no API changes.
- style-check before/after counts: driver0→0, HAL29→29, existing host test33→33. These are pre-existing
  whole-file findings; no unrelated mass style conversion. `git diff --check` PASS.

Tools: shared read-only Clang23.1.0/LLVM, host cc, Python3.13.5, GNU Make4.4.1, clang-format19.
Current main sysroot is copied, completed stamp held; no toolchain build/source edits.
No aggregate make check, no QEMU or physical boot. Fresh kernel artifact: `build/arm64/vmunix`.
No full image build is claimed in this Phase. Main merge approval/push/remote projections separate:
source commit approval pending, push unauthorized, shared records/GitHub owned by Q1.
Phase uncleared until user physical confirmation; no claim of verified hardware crash resolution.

Config SHA256: `ffa5d13528a22fce859f4052c56ab27462e8f1901322cdbcda06a1c3e37d5f66`.

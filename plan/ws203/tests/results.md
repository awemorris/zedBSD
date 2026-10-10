# WS203 source verification — 2026-10-11

Worktree: `.claude/worktrees/rpi4-genet`, branch `codex/rpi4-genet`, base main `a094b953c`.
Scope: RPi4 GENET v5 / BCM54213PE, MTU1500, one RX/TX ring, existing kernel/network APIs.
Final source hashes: [source.sha256](source.sha256). Build settings: [config-rpi4.mk](config-rpi4.mk), copied from the current RPi4 config; absent `CONFIG_DRIVER_BCM2711_GENET` inherits `y` on RPi4. Root/shared config was not edited.

## Implementation and review

- `src/drivers/platform/rpi4/rpi4-ethernet.c`: firmware resources, GENET MAC reset/filter/speed, MDIO transport, uncached DMA, RX/TX rings, IRQ, periodic service, net_device `en0`, open/close.
- `src/drivers/ethernet/bcm54213pe.c`: external PHY identity/reset, RGMII clock skew, full-duplex autonegotiation and link status. Caller-owned MDIO callbacks; no GENET MMIO dependency.
- Platform starts GENET before PCIe discovery; periodic service starts before USB root probing. Neither PCIe nor USB is a prerequisite.
- Existing `networkd/main.c:lan_raise()` opens wired interfaces before carrier; its default wired policy obtains DHCP. No networkd changes were needed. This is source-path review, not a DHCP/SSH runtime result.
- Resources unwind before interface publication; permanently attached DMA storage remains owned across close/reopen. Network calls and packet frees are outside the higher-ranked device lock. IRQ uses the existing opaque EOI token. MDIO/reset deadlines use the monotonic counter and independent iteration bounds, without sleeping under the spinlock.
- Full `plan/coding-style.md` review covers all new C/headers and changed platform paragraphs; Guardrail/HAL boundaries and build wiring were reviewed. Clang-format 19.1.7 previews and formatting were followed by manual restoration of required one-argument definitions, single-line static declarations, semantic paragraphs and controlled-body braces. No unrelated formatting.
- `style-check.py` reports only 9 `goto` review candidates: all are single forward jumps to the same attachment cleanup label, allowed by the full standard. No remaining mandatory violations identified. This partial checker is not full semantic certification.

## Commands and outcomes

Run in the owned worktree:

```sh
make -j16 build/arm64/vmunix
make -j16 build/arm64/vmunix CONFIG_DRIVER_BCM2711_GENET=n
make -j16 build/arm64/vmunix
make -f plan/ws203/tests/host-test.mk run DTB=/home/awe/zedBSD-claude1/vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb
python3 plan/tools/style-check.py src/drivers/ethernet/bcm54213pe.c src/drivers/platform/rpi4/rpi4-ethernet.c include/drivers/ethernet/bcm54213pe.h src/drivers/platform/rpi4/rpi4-ethernet.h plan/ws203/tests/genet-host-test.c plan/ws203/tests/include/kern/thread.h --summary
git diff --check
sha256sum -c plan/ws203/tests/source.sha256
```

- ON and OFF kernel builds exit 0, warnings 0. Both `check-arm64-vmunix.py` passes (entry `0xffff000000080000`). OFF `llvm-nm kernel.elf` contains no `drv_rpi4_ethernet` / `drv_bcm54213pe` symbols. Final state is ON. The final source received an additional incremental ON build after manual review.
- Target compiler: zedBSD Clang 23.1.0 (`d7f1bbaca898fb5f4cc373b082e915ec1a07310f`); read-only shared LLVM, AArch64 target, existing full LTO and `-Wall -Wextra -Werror`.
- Host: Debian GCC 14.2.0, `-Wall -Wextra -Werror -fsanitize=address,undefined`. Final host test exit 0, ASan/UBSan/LeakSanitizer clean. An earlier sandbox execution could not run LeakSanitizer under ptrace; the final test ran with approved sandbox escalation and leak detection enabled.
- Firmware DTB SHA256: `75761b73c284e26623e4d1624bff13e67bce2ae620880efd81d6571a3739fcfb`. A prepared firmware tree is required; the above command used the main tree's read-only vendor DTB.
- Model PASS: actual FDT MMIO/DMA/IRQ/PHY binding, disabled node and invalid IRQ trigger, DMA allocation failures, wrong PHY ID, stuck MDIO and frozen/missing counter, IRQ setup failure unwind, cable-absent open, delayed carrier, advertisement/skew and gigabit MAC speed, TX padding/CRC/ownership/backpressure/16-bit sequence wrap, RX alignment/errors/malformed lengths/pool exhaustion/budget, IRQ EOI/rearm, MDIO fault and recovery, close/reopen without freed DMA.
- Menu Python API checks: `driver_groups()` includes GENET in common Ethernet; `driver_defaults()` is `y` only for RPi4; `save()` → `load()` preserves explicit `y` and `n` on i386/amd64/pc98/rpi4/sun4u/x68k. Twelve save/load cases pass.
- `git diff --check`: clean. Source hash validation: pass.

## Hardware facts and provenance

Independent Zlib implementation; references supplied numeric register/bit definitions and hardware behavior, with no copied source or comments:

- [FreeBSD GENET controller](https://github.com/freebsd/freebsd-src/blob/main/sys/arm64/broadcom/genet/if_genet.c)
- [FreeBSD GENET registers](https://github.com/freebsd/freebsd-src/blob/main/sys/arm64/broadcom/genet/if_genetreg.h)
- [FreeBSD Broadcom PHY](https://github.com/freebsd/freebsd-src/blob/main/sys/dev/mii/brgphy.c), [PHY registers](https://github.com/freebsd/freebsd-src/blob/main/sys/dev/mii/brgphyreg.h)
- [NetBSD GENET registers](https://github.com/NetBSD/src/blob/trunk/sys/dev/ic/bcmgenetreg.h)

No WS141 GPL exception was used. No HAL API or responsibility changed.

## Limits and handoff

No RPi4 execution, electrical timing, actual DMA/IRQ delivery, DHCP or SSH verified. Host model is single-threaded; it checks callback lock boundaries and lifecycle paths, not arbitrary concurrent scheduling. Actual `rgmii-rxid` binding only; 10/100/1000 full-duplex advertisement, no half-duplex or forced parallel-detect link. No offload/jumbo/WOL/suspend/multiqueue support. Main integration, full SD image and physical acceptance remain for handoff/p003. No QEMU, toolchain build, full-image build, push or shared-plan writes.

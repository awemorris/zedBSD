# RPi4 command completion repair / 2026-10-11

Parent: [ws048-p011](../phase011/phase.md)
Worktree: `/home/awe/zedBSD-claude1/.claude/worktrees/rpi4-sshd` / `codex/rpi4-sshd`, base main `402598d27`.
Final source hashes: [SHA256](command-repair-20261011.sha256).

## Read-only physical evidence

User authorized `ssh kei@10.0.30.2`. SSH succeeds; running kernel g402598d, arm64 RPi4, 8192MiB. GENET is UP/RUNNING at 1000Mbps full-duplex with assigned IPv4, traffic both directions and no RX/TX errors or drops. No remote writes/update/reboot were performed. Credentials, MAC and unrelated user data are omitted.

VL805 1106:3483 rev01 uses INTx175; root PCIe gen2 x1. Controller RUN and port reset succeed. Enable Slot(type9) times out; subsequent EIO is command_failed admission, not evidence of another independent error. The command path polls event DMA with interrupts disabled, so INTx alone does not explain absent command completion.

## Final changes

- `drv_dma_device_create_window` creates an unpublished owner with immutable bus offset and inclusive CPU backing limit. Existing `drv_dma_constraints` layout and identity creation API remain unchanged, avoiding changes to amd64/i915 positional initializers. HAL API/UAPI unchanged.
- Allocation limits are calculated in physical space, with checked alias arithmetic/alignment/boundary. Allocation, streaming mapping and vector segments expose PCI addresses; freeing and CPU mappings retain CPU paddr. brcmstb supplies the inbound PCI base from firmware dma-ranges, keeps backing below 2GiB and the usable range, and records actual base/size on next boot. Identity windows retain the 31-bit bus mask.
- Every command/endpoint/recovery/resume doorbell write is followed by a readback, with ordered MMIO reads. Timeout snapshots USBSTS/CRCR/IMAN/PCI command and command/event/ERST addresses plus cursor/cycle, without acknowledging events or attempting an unsafe reset.

## Verification and limitations

Commands in the owned worktree:

```
make -f plan/ws048/tests/host-test.mk OUT=build/ws048-command-host build/ws048-command-host/dma-host-test build/ws048-command-host/dma-uncached-host-test build/ws048-command-host/brcmstb-host-test build/ws048-command-host/disabled.dtb
build/ws048-command-host/dma-host-test
build/ws048-command-host/dma-uncached-host-test
build/ws048-command-host/brcmstb-host-test /home/awe/zedBSD-claude1/vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb build/ws048-command-host/disabled.dtb
make -j16 build/arm64/vmunix
git diff --check
```

DMA no-uncached fixture: 99 checks PASS; uncached fixture: 190 checks PASS; brcmstb: 1790 checks PASS. Includes identity and 4/8GiB aliases, CPU backing limit, map/vector, mask/overflow and alias alignment/boundary refusal. Production DMA/PCIe source is compiled in these models. ASan/UBSan/LSan run with normal settings outside the ptrace-constrained sandbox. A fixture lifetime leak found when replacing its published host was fixed; final LSan passes. A formatting helper briefly replaced attributed fixture arrays; final source was restored, successfully rebuilt and tested. These transient failures do not count as successful verification.

Final kernel build: exit0, warnings0/errors0, ARM64 image header/entry check PASS at 0xffff000000080000. Logs are owned `build/rpi4-command-final-build.log`, `build/ws048-command-host/build-final.log`, `run-final.log`; current config.mk matches main byte-for-byte. Read-only shared Clang23.1.0 / GCC14.2.0 / clang-format19.1.7; no toolchain build or shared artifact write.

Final changed C/header scope was reviewed against the complete coding-style.md, including publication ordering, arithmetic short circuit, ownership/lifetime, declarations/forward prototypes, paragraphs, split calls and braces. clang-format was applied only to changed ranges; definition/forward declaration shape and comment form follow the authoritative full standard where formatter defaults differ. Existing unrelated legacy style was preserved. No aggregate make check, QEMU, full userland rebuild, physical new-kernel USB acceptance or GPU pixel testing was performed.

The physical runtime FDT dma-ranges is not exposed by the current kernel and has not been read back. Nonzero PCI alias handling is a verified source contract fix, not a proven physical root cause. Posted-write completion follows the primary Linux xHCI hardware procedure. Next boot's added DMA/timeout logs distinguish remaining failures. USB input remains uncleared in p009/p006; main integration requires approval for the concrete commit; shared projections/GitHub remain Q1 pending.

## GPU logs supplied during USB investigation

GPU source is unchanged. User/SSH logs show P0/V0 and native V1/V2 succeeded; N0 stops at firmware HDMI-clock query reporting 0Hz with error0, despite framebuffer handoff 1920x1080/pitch7680. Console output itself does not prove the firmware's clock13 query represents the live native HDMI clock. Investigate the firmware clock query/provider and guarded native readout before relaxing the access prerequisites.

V3 reports version42, one core, PA35/VA35, then ENOTSUP21. `v3d-hardware.c identify()` currently refuses virtual_bits !=32; the synthetic fixture instead supplies MMU_DEBUG_INFO0x620. This directly explains this software refusal. Check raw MMU width encoding and the low-32-bit page-table/index contract before changing admission; do not blindly enlarge/enable the hardware on log evidence alone. WS141 physical display/Keiland acceptance remains unmet; software checks are not physical success.

## Primary hardware references

- [Linux xHCI doorbell publication](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/host/xhci-ring.c): posted command and endpoint writes are flushed by readback.
- [RPi Linux PCIe inbound windows](https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.12.y/drivers/pci/controller/pcie-brcmstb.c): PCI inbound offset follows firmware DMA ranges and CPU-side base is zero; firmware may edit ranges for board memory/revision.
- [Linux V3D MMU registers](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/v3d/v3d_regs.h) and [MMU procedure](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/v3d/v3d_mmu.c): used for read-only comparison, no external implementation code copied.

## Main integration read-back / 2026-10-11

Exact user approval: `16c139e9f` →「mainへ統合する」. Main was clean at `402598d27`; `git merge --ff-only 16c139e9f` succeeded without conflict. `git status --short` is empty; all six entries in command-repair-20261011.sha256 match. Source diff against the verified commit is empty and main config.mk equals the owned worktree config.mk. No source changes after verification, so prior final host/build results apply without repeating tests. No push, remote deployment or reboot. Actual new-kernel USB recovery is unverified. Earlier integration-pending statements are historical; shared/remote projections remain Q1 pending.

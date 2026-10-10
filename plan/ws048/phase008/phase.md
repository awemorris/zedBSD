<!-- awesome-plan project=zedbsd record=ws048-p008 -->
# WS048 p008: RPi4起動時PCIe register readの同期例外

Status: uncleared（source/host/build完了、実機の再起動結果待ち）
Disposition: normal
Parent: [WS048](../ws.md)
Queue: [Codex RPi4 boot](../codex-pcie-boot-queue.md)

## Authorization and scope / 2026-10-11

Current user:「Arm64カーネルのデバッグをお願いします。URB completion contractの表示でずっと固まっていて、そのあと落ちました。…修正してください。」
This reactivates only this reported boot crash under the otherwise blocked WS048; no full USB acceptance/feature expansion is authorized.
Finite scope: diagnose photo against current kernel/config, fix first PCIe MMIO access and related arm64 mapping defect, focused host checks and kernel build, final changed-source full-standard review. Physical boot is user verified; no QEMU, push, HAL API or toolchain source/build changes.
Base main df1d2be26; owned worktree/branch `.claude/worktrees/rpi4-pcie-boot` / `codex/rpi4-pcie-boot`.

## Evidence and procedure

Photo: ESR 0x96000210, EC0x25, ELR0xffff0000000cb44c, FAR0xffff0000fd50406c.
Current main kernel disassembly at ELR is `ldr w2,[x8,x9]` with x9=0x406c in drv_pci_brcmstb_start.
This is controller revision read, before reset_controller, not an URB completion execution failure. DFSC0x10 reports synchronous external abort.
Additionally range_is_ram subtracts physical from a prior RAM bank end without first ordering them;
addresses beyond that bank underflow and are returned as RAM, bypassing Device-nGnRE mapping.
Fix the bank exclusion and read revision after bridge/SerDes reset sequence. Maintain HAL APIs,
existing fault semantics, driver configuration and PCIe windows. Add safe progress log before reset.

Applicable: AGENTS.md, plan/guardrail.md, full plan/coding-style.md and .clang-format/standards automation.
Shared LLVM is read only; current main arm64 sysroot copied with completed stamp held via -o.
Use existing brcmstb host model with a powered-down revision read rejection, plus short numeric RAM-boundary check of actual production function.
Physical confirmation remains unverified; describe source fix/build result separately from crash resolution.
Main integration requires approval of specific source commit; shared Master/Queue/history/GitHub projection pending Q1.

## Final source result / 2026-10-11

Fixed source: `src/hal/arm64/space.c` checks physical against each RAM bank end before subtraction,
so PCIe/MMIO gaps and BARs beyond RAM are not silently returned as cacheable RAM.
`src/drivers/pci/pci-brcmstb.c` logs mapping progress without an MMIO read and reads revision after
reset_controller releases the bridge and powers/settles the SerDes. No HAL API change.
The old first read is proven by photo/ELF address match; the precise hardware state at the abort
cannot be proven from one photo. Both erroneous RAM mapping and early initialization order are repaired.

Evidence: [result](../tests/rpi4-pcie-boot-20261011.md), [photo](../tests/rpi4-pcie-boot-20261011.png).
Host brcmstb model PASS 1295 checks with reset/power/delay guards; old production order fails those guards.
Actual RAM-classifier function runs host boundary cases PASS; old function fails at 0xfd500000.
Full current-config `make -j16 ... vmunix` and existing AArch64 Image/ELF checker PASS, compiler warnings/errors0.
Final changed-range clang-format and full C standard/manual review PASS; no new style-check findings
(pre-existing space.c29/test33 retained; pci-brcmstb.c0). No mass formatting.

Attempt rpi4-pcie-boot-20261011-i01 ends uncleared pending physical result, not falsely resolved.
Specific source commit main integration also awaits user approval. Resume: integrate approved commit,
build user's image, user boots RPi4 and confirms boot progresses beyond PCIe/USB; if it fails, retain new
photo/ELF/config and diagnose that evidence. No further source scope/Queue auto-started.
Shared Master/Queue/history/bug projection and GitHub publication remain Q1 pending; no push.

<!-- awesome-plan project=zedbsd record=ws048-p009 -->
# WS048 p009: RPi4 USB input activation and selected class drivers

Status: uncleared（source/build済み、USB実機受入待ち）
Disposition: normal
Parent: [WS048](../ws.md)
Queue: [focused queue](../codex-usb-session-queue.md)

## Authority / 2026-10-11

Current user:「今は、ひとまずUSBが動くようにして、sessiondが起動するオプションも使えるようにしたい。ブートローダのロゴと、カーネルがメッセージの代わりにアニメ、はまた今度。」
Follow-up:「停止していないです。USBのカーネルメッセージは出ず、ログインまで行きました。」
Prior VC4 initialization and reaching login confirm that p008's former synchronous exception no longer stops this user's boot. USB physical behavior remains unverified.

Scope: inspect PCIe/xHCI and USB input path; enable missing input-ready handoff and selected portable USB class drivers in RPi4 manifest/registration. No x86 HCD migration, HAL API, LLVM rebuild, QEMU, push, splash/quiet animation. Independent worktree codex/rpi4-usb-session, base main fbcb2b543.
Criteria: current config warning0 kernel build; selected classes linked and registered before xHCI; HID pending activation after console input registration; final changed-source full-standard review; physical keyboard/mouse verification by user. Build success alone cannot clear physical USB acceptance.
Rules: AGENTS.md, plan/guardrail.md, full plan/coding-style.md/.clang-format and standards/automation.md. Current existing compiler used read-only. Investigation bounded to confirmed glue omissions and new physical evidence; no speculative xHCI hardware rewrite.
Sessiond boot option belongs to ws193-p009; no shared Master/Queue/cache edits, remote projection pending Q1.

## Build environment follow-up

Current-config full image validation uses a fresh owned build, current main sysroot copied and its completed stamp held with -o; shared LLVM/source and host Noct are read-only links with source/build verification stamps held. Target Noct is built in this dedicated worktree under the earlier user's express approval「専用worktreeでビルドしてよい」; no Noct/toolchain source or rules changes. Current validated archive/firmware cache copied from main, existing package hash/license validation retained. The target Noct CMake output copied with the source is moved aside in owned build before configuration, preventing writes to its old main path. No prior test image/application artifacts are inputs.

## Terminal source/build outcome / 2026-10-11

rpi4-usb-session-20261011-i01 ends uncleared only because current USB physical acceptance remains absent. Source glue and selected portable class integration complete, current config kernel build warning0 and full selected image build/image checks pass; final changed-source full-standard review complete. [Evidence](../tests/rpi4-usb-session-20261011.md). Do not claim controller/HID physically working from build success. Resume after approved specific commit integration: user boots rebuilt image, verifies USB keyboard/mouse and provides dmesg/physical evidence if it still fails. Previous p008 boot-crash clearance is separate. No next Queue, shared projections/publication Q1 pending.

## Main integration follow-up / 2026-10-11

User explicitly instructed「mainにマージしてください。」for source commit `8c93ba8e026c3d3e8e2bfe3f22c5ffe430e9dd62`. Main was clean at `fbcb2b543` and fast-forwarded to that exact commit without conflict. Read-back confirmed all 15 integrated files byte-identical to the verified worktree. Owned-worktree `make -j16 build/arm64/vmunix` succeeds with no further source changes; prior full image/option checks remain applicable. Integration is complete; earlier integration-pending text is historical. USB input and greeter physical checks remain pending, with no acceptance-state promotion from merge alone. Shared Master/Queue/history/cache/GitHub reconciliation remains Q1 pending; no push.

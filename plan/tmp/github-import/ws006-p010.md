<!-- awesome-plan project=zedbsd record=ws006-p010 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws006/phase010/phase.md`

親: [ws006](https://github.com/awemorris/zedBSD/issues/7)

# WS006-p010: EHCI/UHCI USB-root recovery

Date: 2026-09-09
Status: completed (q147; ordinary paired campaign passes after q141 UHCI repair)
Parent: [WS006](https://github.com/awemorris/zedBSD/issues/7)
Timebox: 120 active minutes

Closure: [q147 results](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/phase011-terminal-identity/results.md), with q141
checked halt and q142 heap/lifecycle evidence. The ordinary replay passes
USB-root I/O, input, hotplug and Xzed without heap tracing.

## Outcome and authorization

Repair the QEMU paired EHCI/UHCI root-enumeration failure preventing p009
acceptance. User explicitly requested that legacy USB defects be investigated
and corrected within the full Priority goal after ready implementation work.
Prior q126 evidence is port 1 enumeration ENODEV (13), before login; physical
hardware is not a prerequisite. Preserve the existing request retirement and
checked HCD DMA-stop contracts.

## Procedure

q146 resume: q141 identified and fixed the UHCI stale schedule link; q142 closed
its heap/lifecycle gate. Rerun the ordinary (no heap-trace macro) paired campaign
with the Xzed option. The q141 checked terminal-halt evidence remains applicable
because q145 adds only an unregistered UAS parser, without changing USB runtime.

1. Reproduce using a fresh disposable ordinary amd64 USB-root image with the
   paired topology from the maintained USB HID campaign. Record exact args,
   stage, deadline and log. Do not reuse a historical kernel as current evidence.
2. Trace the first ENODEV to its actual controller/core state transition and
   check descriptor/control transfer completion, port enable/reset/handoff and
   root-worker admission. Add bounded diagnostics or fake-HCD tests as needed;
   do not restore retired console APIs as a speculative workaround.
3. Repair the proven state/ownership/transfer defect. If UHCI short-transfer or
   EHCI token decoding causes it, include the exact necessary correction and
   add status/short-transfer regressions; record unrelated residuals separately.
4. Run focused ordinary/sanitizer host cases with current complete sources,
   make -j16 with explicit amd64/pcat/pc98 CI configs, then paired USB HID
   acceptance including USB-root I/O and hotplug. Regress xHCI and USB halt.
5. If paired boot is recovered, p009's remaining Xzed GUI acceptance is the
   next bounded item. Do not count host tests as that GUI runtime.

## Completion and uncleared boundary

The original paired topology must reach login and pass the actual HID/input,
root-I/O and hotplug checks without weakening retirement or success/error
semantics. Post-boot halt must retain the q135 all-CPU-stop/quiet USB behavior.
If incomplete, report the first established cause, changes, evidence and exact
resume step, keeping p009 and WS006 open.

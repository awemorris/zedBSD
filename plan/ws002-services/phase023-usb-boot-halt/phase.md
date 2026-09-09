# WS002-p023: USB boot halt failure

Date: 2026-09-09
Status: completed (q135 QEMU reproduction and correction)
Parent: [WS002](../ws.md)
Bug: [BUG-010](../../known-bugs.md)

## Scope and authorization

USB起動後にhaltするとUSBエラーで終了できないとのユーザー報告。
QEMUで再現できた場合、今回のPriority全件ゴール内で修正するとの明示指示。
実機を必須条件にしない。実装可能な独立作業を先行し、USB解析を後段へ回す。
WS006 EHCI/UHCI起動障害とは症状と判定を分け、共通原因が実証された場合のみ統合する。

## Procedure

1. 通常amd64画像の使い捨てコピーをUSB storageへ接続し、xHCIでlogin→書込み→haltを試す。QEMU controller/port/媒体、停止判定、期限とログを記録する。
2. 成功済みbootが条件。EHCI/UHCIはWS006のroot enumeration障害解消後に同じ停止試験を追加する。起動失敗をhalt再現扱いしない。
3. clean、dirty overlay、active swap、停止直前I/Oの条件を絞って確認。サービス終了、mount sync、writeback終了、USB class disconnect/cancel/drain、PCI停止の境界を追跡する。
4. 再現時は最初のエラーと保持中request/worker/media所有権を特定し、停止順序または寿命管理を修正。エラーを隠すだけの対策はしない。
5. 同じ画像条件でhalt完了と再起動後のデータ保持、通常I/O、rebootを回帰確認する。

## Acceptance and uncleared boundary

再現できた条件で修正前失敗・修正後完了の証拠を残す。haltはQEMU processが終了しない場合もあるため、CPU停止/最終状態を確認しログの静止だけで成功にしない。
再現しない場合は確認したQEMU条件と未確認の実機差を記録し、BUG-010と本Phaseをunclearedのまま残す。実機バグを解決済みと推測しない。

Timebox: 120 active minutes per finite queue. Queue化してから実行する。

## Existing QEMU evidence (2026-09-09)

Read-only inspection of WS019 q130 publication acceptance found a related
shutdown message in `plan/ws019-installation/temp/q130-publication-final/guest.log`
(lines 190–194; repository-relative path): USB-root q35/xHCI, after real writes,
sync and unmount of non-root test filesystems, `reboot` prints:

```text
usb0: device 1 driver shutdown failed (17); class resources retained
usb0: host controller quiesced; resources retained
```

The guest then enters UEFI and boots successfully. Thus this is evidence of a
USB class shutdown error, **not** evidence that halt cannot complete. errno 17
is EBUSY in zedBSD. Static inspection shows shutdown calls ordinary interface
detach; USB storage detach calls disk_gone_if_idle/disk_destroy while its root
media may remain referenced. This is a candidate cause requiring explicit halt
reproduction and ownership checks, not yet a confirmed diagnosis. Preserve the
difference between legitimate retained root-media objects and running I/O or
workers that must be stopped before HCD teardown.

The same reboot-time EBUSY messages recur in
`plan/ws019-installation/temp/q133-staging-reboot/guest.log` (154–155), followed
by successful reboot and persistent-data verification. This strengthens the
shared shutdown-path evidence without claiming halt reproduction or a fix.

## q135 reproduced design correction

`temp/q135-halt-before3` reproduces post-halt repeated BOT CBW EBUSY. QMP
shows one CPU in terminal CLI/HLT and three CPUs still in interruptible idle.
Fix both demonstrated lifetimes: USB storage implements a checked terminal
quiesce callback which closes media/control activity and joins its worker,
retaining root/swap-referenced disk and URB objects. USB core closes binding
admission, checks callbacks drained, retains those owners and completes HCD
DMA quiescence. Runtime detach keeps its existing busy/refusal semantics.
PC/AT halt uses the existing HAL terminal all-CPU broadcast after shutdown
barriers, instead of halting only the calling CPU. The HAL name contains
`panic` but the broadcast itself does not report a panic or assert.

Initial probe attempts used nonexistent swapctl/sysctl names and failed before
halt; those are fixture errors, not OS failures. The third cell checks mounts
and records QMP CPU registers, where the actual shutdown reproduction occurred.

## Result

[Reproduction, design correction, verification and remaining boundaries](results.md).
The related physical report and later EHCI/UHCI regression remain explicitly
unverified; they do not erase the demonstrated xHCI/SMP correction.

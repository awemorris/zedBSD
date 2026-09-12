<!-- awesome-plan project=zedbsd record=ws027 -->

# WS027: PowerPC移植

<!-- awesome-plan-current:start -->

Status: planned
Primary Milestone: MG008
Related Milestones: MG003
Objectives: O2, O4
Focused Goal: fg009
Parent: [Master](https://github.com/awemorris/zedBSD/issues/1)
Queue: none

<!-- awesome-plan-current:end -->

## 単一の到達目標

PowerBook G4 A1010 / 867MHzへ向けたPowerPC移植を行い、QEMU mac99でOpen Firmware起動のzedBSDがUSB上のrootfs.img/data.imgを用いてinit・シェルへ到達する。最初の段階はAPM+FAT上の独自ローダ→zedboot.cfg→同じFATのvmunixからPPCカーネル初期化まででよく、root/image mountは後続。

OHCIのamd64検証はこの移植のUSBストレージ経路を先行検証する依存作業であり、別の製品目標ではない。インストーラ実機対応・他CPU移植・FireWireは含めない。実機受け入れをQEMU成功と同一視しない。

## 移管

誤ってWS003に追加したp033-p039の計画を本WSのp001-p007へ移す。旧Phaseは取消・移管として閉じ、IDと履歴を残す。新規Phaseは未実行。WS003や他の終了済みWSは再利用しない。

## Phase

- [ws027-p001](https://github.com/awemorris/zedBSD/issues/375) ← [ws003-p033](https://github.com/awemorris/zedBSD/issues/367)
- [ws027-p002](https://github.com/awemorris/zedBSD/issues/376) ← [ws003-p034](https://github.com/awemorris/zedBSD/issues/368)
- [ws027-p003](https://github.com/awemorris/zedBSD/issues/377) ← [ws003-p035](https://github.com/awemorris/zedBSD/issues/369)
- [ws027-p004](https://github.com/awemorris/zedBSD/issues/378) ← [ws003-p036](https://github.com/awemorris/zedBSD/issues/370)
- [ws027-p005](https://github.com/awemorris/zedBSD/issues/379) ← [ws003-p037](https://github.com/awemorris/zedBSD/issues/371)
- [ws027-p006](https://github.com/awemorris/zedBSD/issues/380) ← [ws003-p038](https://github.com/awemorris/zedBSD/issues/372)
- [ws027-p007](https://github.com/awemorris/zedBSD/issues/381) ← [ws003-p039](https://github.com/awemorris/zedBSD/issues/373)

## 依存順と最初の受け入れ

p001→p002→p003。p004は独立して検証可能。p003→p005、p003+p004+p005→p006→p007。p003までが最初の到達点。詳細な起動契約・設定形式・根拠・検証条件は各Phaseに保持する。

## WSの単一目標と終了後の扱い（2026-09-12ユーザー指示）

WSは一つの具体的な到達目標を持つ。目標を達成したWS、またはユーザーが終了したWSは再利用・再開して別の目標を追加しない。似た領域だからという理由で一つのWSへまとめない。機種対応などの上位分類・到達点はMGが担い、インストーラ実機動作、PowerPC移植などは別のWSを作る。
一つの目標に必要な依存作業をPhaseへ分解することは可能だが、独立した別目標をPhaseとして混ぜない。WS終了時は子Phaseを全件照合し、未完了は完了に改変せず、ユーザー指定の保留先または別WSへ引き継いで元Phaseを終了する。旧ID、結果、転送先を残す。今回WS003は終了・再利用禁止、PPC移植はWS027へ、その他の未完了はFuture Workへ移す。

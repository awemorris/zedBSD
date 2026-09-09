# ws025-p029: 条件付き UAS driver

日付: 2026-09-07

Phase ID: `ws025-p029`

## 最新の受け入れ方針（2026-09-09、ユーザー指定）

実機UAS機器がないため、QEMU `usb-uas` での動作確認をもってclearedにする。
実機試験は完了の必須条件ではない。下記q122/q125などの実機先行条件は
履歴であり、実装や完了を妨げる条件にしない。

現在はQEMUの実descriptor取得とparser検証まで完了している。command/data/
status transportによるread/write/fsync、timeout/reset/cancelと再接続・再試行の
該当受け入れが残っており、descriptor取得だけではclearedにしない。
対応する速度・stream条件をQEMU構成とともに明記して検証する。
再開したp032の実機固有問題は、既に通ったPC98 QEMU gateを取り消すものでは
なく、独立したUAS実装を実機待ちで停止させない。

## q144 descriptor and transport contract

q145 implementation: introduce a pure `drv_usb_uas_decode_configuration` in
`src/drivers/usb/usb-uas.c` and a small capability header. Parse only the requested
interface/alternate, preserve endpoint/pipe association, require four unique
bulk endpoints with correct direction and high/super-speed packet/companion
rules. Reject truncated lengths, duplicate selected alternates, missing/duplicate
pipes and inconsistent stream capabilities. Publish output only after complete
validation. No allocations, transport registration or endpoint activation.
Host tests compile production source against captured descriptors and malformed
variants under ordinary and ASan/UBSan builds; run explicit three-platform builds.

Result and next implementation: [captured capabilities and transport design](transport-design.md).

Boot the current amd64 image on disposable USB BOT media, and attach a separate
QEMU `usb-uas` with an explicit blank `scsi-hd` LUN 0. Capture control transfers
using QEMU's per-device pcap facility. Repeat with UAS on EHCI (high speed) and
xHCI (super speed). Decode complete device/configuration replies, preserving
endpoint/pipe-usage association and SuperSpeed companion MaxStreams. Require
zedBSD's actual enumeration log, not merely firmware enumeration or QEMU help.

Record unsupported-driver status truthfully. Design the initial depth-one
command/status/data state machine, READY-vs-stream behavior, tag retirement,
timeout/reset, and descriptor rejection before production implementation. This
bounded queue establishes the backend and design; it does not complete p029.

Status: uncleared (q145); 実測descriptorに基づくproduction parserと異常系検証完了。transport実装が残る。

Parent: [WS025](../ws.md)

依存: ws025-p019、ws025-p024、QEMU UAS backendと実取得descriptor

追加の先行条件: [ws025-p031](../phase031-driver-layout-style/phase.md) のドライバ整理を完了してから実装する。下記の旧ソースパスは p031 の移行表で解決する。既存の採用条件は維持する。

追加の回帰 gate: [ws025-p032](../phase032-pc98-boot-regression/phase.md) の PC-98 QEMU 起動回復を完了してから実装へ進む。

## 現在の選択

ユーザーがp027〜p030を次に実施する最優先項目として選択した。
旧見送りを継続する扱いではなく、p031残件・現ソース・下記の測定/実機条件を確認してQueue化する。
今回の計画整理で実装や測定を実施済みとは扱わない。

## 目的と境界

BOT と別 owner の UAS driver を実装する。

## 変更対象

- `src/drivers (新 UAS owner)`
- `include/drivers/usb.h`
- `src/drivers/pci-xhci.c`
- `src/kern/disk.c`

実装時点の source owner と file 名を再確認し、WS024 の移行済みコードを二重実装しない。

## 設計と実装手順

1. QEMU UASの protocol/interface/pipe/stream capability を記録し、対応対象を固定する。印字された製品名だけで決めない。
2. command/status/data の owner、tag/stream、task management、queue 上限を設計し、descriptor parser と同期 depth 1 から実装する。
3. USB/disk の既存 generation/async/SG 契約に接続し、reset/cancel/timeout/out-of-order を受け入れてから多重化する。
4. 選択失敗時の BOT fallback が device の interface 切替え契約内で可能か確認し、live DMA を残して切替えない。

[共通 I/O 契約](../io-design.md) と [memory 設計](../memory-design.md) を維持する。

## 受け入れ

- 専用 descriptor/protocol fixture、ASYNC/SG/REC/FLUSH の該当セル、QEMU UAS read/write/fsync/再接続。
- 該当 ID の詳細は [受け入れ行列](../acceptance.md)。変更範囲の focused fixture と supported build を実行し、source/config・counter・結果を記録する。

## 停止・再開と成果物

QEMUでtransportを実装・検証する。実機未入手を停止条件にせず、BOT並列化で代用しない。

成果物は production patch、必要な fixture、実行後の `results.md`。途中状態は同ファイルに事実・失敗地点・残条件を記し、未実施を PASS にしない。Queue 選択前には実装せず、build/runtime は共有環境で直列に行う。

## q122 adoption decision

Not adopted in the mandatory WS025 implementation. Obtain target UAS descriptors and stream/pipe capabilities with a concrete hardware acceptance path.
See [effective policy and evidence basis](../phase026-integration-defaults/effective-policy.md).
This records the conditional decision, not completion of this optional phase.

## q125 現ソースに合わせた詳細化

ユーザーのPriority全件自走指示により着手条件を確認。完了できないPhaseはunclearedとし他WSへ進む。

対象: src/drivers/usb/（新UAS owner）、include/drivers/usb.h、src/drivers/pci/pci-xhci.c、src/kern/disk.c。

現状: 直近実機はWLANとホストUSB Ethernetであり、UAS storageのdescriptor/stream能力は取得していない。現在テスト機SSHがtimeout。

設計手順: (1) interface protocol=UASとcommand/status/data pipe usage、MaxStreamsをdescriptorから固定。(2) parser→depth1 command/status/data owner→cancel/reset/task-managementの順で実装。(3) tag/streamはdrain確認まで再利用せず、old generationのstatusを拒否。(4) 実機read/write/fsync/切断再接続が通ってから並行化し、BOTへのlive切替えは行わない。

今回の未クリア理由: 対象UAS機器とdescriptorが未確認。現Phaseの実機先行条件を満たさず、架空descriptorに合わせたdriverは実装しない。

再開条件: UAS対象のdescriptorと利用可能な実機試験経路。

旧停止節のplanned維持はq122時点の判断。今回の実行結果はunclearedとして扱う。

## 現在の実行条件（q140中の読み取り確認、2026-09-09）

上記q122/q125の実機先行条件は履歴であり、後続のユーザー指示により
実装の停止条件にはしない。ローカルQEMUの公式同梱文書
`/usr/share/doc/qemu-system-common/system/devices/usb.html` は `usb-uas` と
明示的な `scsi-hd,bus=uas.0,scsi-id=0,lun=0,drive=...` の構成を説明している。
usb-uas単体ではディスクを作らない。使い捨てbackingとSCSI LUNを明示して
descriptor/pipe/stream能力を取得するところから次の有限Queueを始める。

現ソースにはSuperSpeed companionのdecodeはあるが、USB API/HCDにUASの
stream/tag所有者は未実装。descriptorを取得せずストリーム不要と決めない。
QEMUが検証に使えるかを実構成で確認してから、depth 1の別transport owner、
command/status/dataとcancel/quiesceの順に設計する。実機がないだけでFutureに
移さず、QEMUでの検証が利用不能と確認された場合のみユーザー指定どおり移す。

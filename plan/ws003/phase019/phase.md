<!-- awesome-plan project=zedbsd record=ws003-p019 -->

<!-- awesome-plan-current:start -->

Status: planning
Phase disposition: canceled
Lifecycle: closed
Current Focus: No

2026-09-12ユーザー指示で本Phaseを終了。未完了事項は[Future Work F-004](https://github.com/awemorris/zedBSD/issues/364)へ保留として移管。試験PASSやclearedへ変更しない。再選択時は新WSを作る。

<!-- awesome-plan-current:end -->

<details>
<summary>終了前の計画・試行履歴（現在の再実行指示ではない）</summary>

# WS003 Phase 019: Latitude NVMe native installation and boot

<!-- installer-bringup-current:start -->

## 2026-09-11 LatitudeインストールGoalへの対応

Status: planning。Phase disposition: normal。Queue: none。
Latitudeでnativeインストール方式を選んだ場合の既存候補Phaseとして保持する。[ws003-p018](../phase018/phase.md)との方式選択は未決であり、両方を今回の必須条件にしない。
[ws019](../../ws019/ws.md)のnative/破壊的操作に関する実装と受け入れは完了・閉鎖済み。その成果を現行source/artifactで確認し、古い「WS019の設計・実装を待つ」という前提を更新する。具体的な対象disk・保持範囲・text/graphic・既存領域使用か作成かを選ぶまで本Phaseの実行範囲は未確定。
選択後は通常インストールとインストール先からinit/login・基本操作までを具体化する。NVRAM変更・全disk初期化・両方式の実施を今回の計画だけで許可しない。最終変更の規約確認は[ws003-p032](../phase032/phase.md)。開発・実機操作は未実施。

<!-- installer-bringup-current:end -->

## 過去の設計待ち記録


Last updated: 2026-08-29

Phase ID: `ws003-p019`

Status: future; not designed; not ready for a Queue

Parent: [WS003 real-hardware bring-up](../ws.md)

Tests: [WS003 test index](../tests/README.md)

## Objective

Accept the later native-root installer path on the Latitude without reopening
or weakening the completed existing-FAT overlay milestone.

## Deferred decisions

- select-existing versus create/format native filesystem;
- whole-disk GPT layout and destructive confirmation, if offered;
- UFS provisioning/growth and recovery;
- native `zedbsd.cfg` with `kernel=` and exact
  `rootpart=PARTUUID=...` selection;
- firmware Boot-entry policy after experience with p018 fallback boot.

## Entry condition

Do not detail or Queue this Phase until WS019 p006/p007 have separately fixed
and accepted the destructive/native product and safety contracts in QEMU.

## Completion direction

An explicitly selected native root boots through the installed UEFI loader to
init/login/root shell, with the final physical repeatability gate stated by the
later accepted WS019 contract.

</details>

<!-- awesome-plan project=zedbsd record=ws011 -->

## 登録済みの子Issue

- [ws011-p001](https://github.com/awemorris/zedBSD/issues/192)
- [ws011-p002](https://github.com/awemorris/zedBSD/issues/193)
- [ws011-p003](https://github.com/awemorris/zedBSD/issues/194)
- [ws011-p004](https://github.com/awemorris/zedBSD/issues/195)
- [ws011-p005](https://github.com/awemorris/zedBSD/issues/196)
- [ws011-p006](https://github.com/awemorris/zedBSD/issues/197)
- [ws011-p007](https://github.com/awemorris/zedBSD/issues/198)
- [ws011-p008](https://github.com/awemorris/zedBSD/issues/199)
- [ws011-p009](https://github.com/awemorris/zedBSD/issues/200)


既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws011/ws.md`

親: [master](https://github.com/awemorris/zedBSD/issues/1)

# WS011: network configuration console

Last updated: 2026-09-09
WSID: `ws011`
Status: completed — commit confirmedはユーザー確認により完了。
Parent: [master](https://github.com/awemorris/zedBSD/issues/1)

## 完了と範囲変更

- p001〜p003、p005〜p007、p009の実装・自動受け入れは既存証拠を保持する。
- p008は今回のユーザーの完了判定を受け入れ、追加の実機試験待ちを終了する。
  新しい試験ログや未観測の細部を合格として捏造しない。
- VLANはキャンセル。bridgeは [master Future List](https://github.com/awemorris/zedBSD/issues/1) F-001へ移管。
- p004の旧VLAN/bridge一括計画とMB-010は現行の残作業ではない。
  bridgeを選び直した場合、VLANを含めず独立したPhaseとして設計する。

## 現行契約

`/sbin/net`の対話設定、`/etc/net.conf`の永続化、`commit`、
`commit confirmed MINUTES`、`rollback`を保持する。
直接の`/sbin/ifconfig`は復旧経路として維持する。旧`apply`/`save`/`discard`は撤去済み。

## Phase registry

| Combined ID | Phase | Status | Completion result |
| --- | --- | --- | --- |
| `ws011-p001` | [`net.conf` v1 format and parser](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws011-net-config/phase001-netconf/phase.md) | Complete | Strict native parser/model/writer and host/native build gates pass |
| `ws011-p002` | [Interactive `net` console](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws011-net-config/phase002-console/phase.md) | Complete | Three modes, candidate safety, argv sharing, help/history, and native image gates pass |
| `ws011-p003` | [Persistence and boot migration](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws011-net-config/phase003-persistence/phase.md) | Complete software milestone | Atomic authoritative configuration and boot/request evidence retained; current WS completion accepted by the user |
| `ws011-p004` | [VLAN and bridge interfaces](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws011-net-config/phase004-vlan-bridge/phase.md) | Cancelled / transferred | VLANキャンセル。bridgeのみmaster Future List F-001へ移管。 |
| `ws011-p005` | [Confirmed-commit design](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws011-net-config/phase005-confirmed-commit-design/phase.md) | Complete design (2026-09-05) | Session-only candidate/token, networkd rollback timer, delayed config publication, and implementation bounds are frozen |
| `ws011-p006` | [Confirmed-commit implementation](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws011-net-config/phase006-confirmed-commit-implementation/phase.md) | Complete (`q073`, 2026-09-05) | Complete reconcile, interactive confirmed commit, volatile networkd rollback, serialized delayed publication, focused regressions, and amd64/i386 builds pass |
| `ws011-p007` | [Confirmed-commit automatic acceptance](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws011-net-config/phase007-confirmed-commit-acceptance/phase.md) | Complete (`q075`, 2026-09-05) | Q074 T020 plus all four post-fix q075 T021 cells prove timeout recovery, confirmed persistence, no late rollback, reboot and connectivity |
| `ws011-p008` | [Confirmed-commit physical acceptance](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws011-net-config/phase008-confirmed-commit-physical-acceptance/phase.md) | Complete (user-accepted, 2026-09-09) | ユーザーがcommit confirmed完了と判定。新たな実機試験を実行したという主張ではない。 |
| `ws011-p009` | [Confirmed-commit overlay publication correction](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws011-net-config/phase009-confirmed-commit-overlay-publication/phase.md) | Complete (`q075`) | FAT validation/seek/write fusion removes reproduced traversal amplification; old/new cost, corruption/growth faults, supported builds and four post-fix T021 cells pass |

## 再開と参照

現行WSに未完了の実行Phaseはない。新しい要求を選ぶ場合にPhaseを追加する。
既存の回帰試験は [tests](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws011-net-config/tests/README.md)、詳細設計・受け入れは各Phaseを参照。
[整理前のWS全文](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws011-net-config/ws-history-2026-09-09.md)は旧VLAN/bridgeモデル等の履歴として保持する。

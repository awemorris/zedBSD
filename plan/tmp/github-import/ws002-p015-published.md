<!-- awesome-plan project=zedbsd record=ws002-p015 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws002/phase015/phase.md`

親: [ws002](https://github.com/awemorris/zedBSD/issues/3)

# ws002-p015: initial networkd and net

WSID: `ws002`

Phase ID: `p015`

Status: complete baseline; superseded in part by `ws002-p020`

Parent WS: [WS002](https://github.com/awemorris/zedBSD/issues/3)

## Objective and design

Introduce foreground `networkd` and `/sbin/net` for loopback, interface
up/down, static IPv4, default route, initial DHCP, DNS output, and status while
retaining direct `/sbin/ifconfig` recovery.

## Acceptance and result

The first baseline completed. Synchronization, one-shot `dhcpc`, fd 3 readiness,
and command orchestration were redesigned and verified in
[`ws002-p020`](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws002-services/phase020-network-service/phase.md). Further network lifecycle
and WLAN work belongs to [WS005](https://github.com/awemorris/zedBSD/issues/6).

## Interruption record

No active work remains in this Phase. Resume only through WS005 or an explicit
WS002 maintenance Phase.

## Completion conditions

- `networkd` and `net` perform the declared loopback, static IPv4, route, DNS,
  initial DHCP, and status operations.
- Managed operation and direct `/sbin/ifconfig` recovery do not conflict.
- Focused protocol/configuration and installed QEMU network cases pass.

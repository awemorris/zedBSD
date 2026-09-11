<!-- awesome-plan project=zedbsd record=ws011-p004 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws011/phase004/phase.md`

親: [ws011](https://github.com/awemorris/zedBSD/issues/12)

# ws011-p004: VLAN and bridge interfaces

WSID: `ws011`  
Phase ID: `p004`  
Combined ID: `ws011-p004`  
Status: cancelled for VLAN; bridge transferred to Future List F-001 (2026-09-09)
Parent WS: [WS011](https://github.com/awemorris/zedBSD/issues/12)

## 現在の扱い

VLANはユーザー指示でキャンセル。bridgeのみ [master Future List](https://github.com/awemorris/zedBSD/issues/1)へ移管。
以下は旧一括設計の履歴であり、そのまま実行しない。MB-010による旧一括保留は終了。

## Historical objective

Implement kernel, networkd, persistence, and console support for independent
802.1Q VLAN interfaces and bridge interfaces.

## Fixed model

- A VLAN has an interface identity, one parent, and VLAN ID 1–4094; it inserts
  and removes tags and may carry L3 addresses.
- A bridge has an interface identity and members. Host L3 addresses belong to
  the bridge, not duplicate member identities.
- VLAN is not a bridge, but a VLAN interface may be a bridge member.

## Work packages

- [ ] Freeze virtual-interface creation/destruction and link-layer UAPI.
- [ ] Implement VLAN ingress/egress and parent lifecycle.
- [ ] Implement bridge learning, forwarding, aging, and member lifecycle.
- [ ] Add networkd and console operations.
- [ ] Apply topology in dependency order and reject invalid cycles.
- [ ] Add tagged-packet and bridge-forwarding QEMU tests.

## Completion conditions

- VLAN tagging, untagging, and isolation pass packet tests.
- Bridge learning and forwarding pass without incorrect member L3 ownership.
- Creation, deletion, boot restore, reconfiguration, and rollback pass.
- Physical-interface direct recovery remains available.

## Acceptance

Run `NVIR-T001`–`NVIR-T008` from the [shared test index](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws011-net-config/tests/README.md).

## Resume point

The feature is wanted later but its detailed design is intentionally on manual
hold. Resume only after the user explicitly selects VLAN/bridge discussion;
then jointly review the virtual-interface UAPI and packet ownership with WS005
before implementation.

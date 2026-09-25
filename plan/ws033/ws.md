<!-- awesome-plan project=zedbsd record=ws033 -->

# WS033: networking サービスと有線インタフェースの管理

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG005
Related Milestones: MG003
Objectives: O1, O2, O3
Parent: [Master](../master.md)
Queue: なし
Resume point: ケーブルの抜き差しの実機での確認（CDC-ECM の試験装置で carrier を動かす試験）
<!-- awesome-plan-current:end -->

## 目標

有線インタフェースを networkd が常駐して管理し（ケーブルの抜き差しに追従して DHCP・static を構成する）、
起動時の `networking` サービスが「どれか一つのインタフェースが到達可能なアドレスを持つ」まで待てるようにする。

## 結果

この WS は計画の記録（ws.md）を持たないまま実行された。経緯と決定は [notes.md](notes.md) にある。

- 判断と実行を分けた `managed-lan`（IDLE・PENDING・CONFIGURED・UNCONFIGURED の状態）。装置を開かず、次にすべきことを答えるだけなので host で試せる。
- carrier の変化は既存の route socket（`RTM_IFINFO`）で受ける。新しい kernel interface は要らなかった。
- `net lan enable`・`net lan disable`・`net startup`、`/etc/service.d/networking`（oneshot）。待つのは `net startup` で、`rc.conf` の `networking.wait` を読む。
- DHCP が取れなければ MAC から 169.254.x.y を導く（有効なアドレスとは扱わない）。
- 試験: `make managed-lan-host-test`（31 項目、[tests/managed-lan-host-test.c](tests/managed-lan-host-test.c)）。

## 残り

- QEMU の試験機に有線 NIC が無く、ケーブルの抜き差しは実機で試せていない（判断の側は host で試した）。
- dp8390 はリンクを検出しないので、抜線を検出できない。
- 旧 `networking-target.sh` は console log を読む harness（`run-target-console.py`）に頼っていたため、2026-09-24 の plan 整理で削除した。

## Phase 一覧

Phase は定義されていない。残りを進めるときは Phase を立てる。

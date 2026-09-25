<!-- awesome-plan project=zedbsd record=ws060p001 -->

# ws060-p001: journal の commit の費用の実測と group commit の設計

Phase ID: `ws060-p001`
Parent: [WS060](../ws.md)
Status: uncleared
Queue: q431-i01（撤回）

## 目的

名前の操作 1 回で journal が何回 `disk_sync()`（flush）し、QEMU の NVMe で 1 回の flush が何 ms かを測る。group commit の設計（commit の単位、timer と閾値、`fsync`/`sync`/unmount の即時 commit、crash の安全性 = journal の順序と checkpoint）を書く。code は変えない。

## 受け入れ

- 操作あたりの flush の数と flush 1 回の時間の表（guest、NVMe の journal の volume）。
- 設計: どの経路で commit を遅らせ、何が即時か、crash のときに何が保証されるか、試験の方法。

## 結果（q431-i01、撤回）

ユーザーの再優先付け（expat の configure と compile を Linux 同等に）で撤回。分かったこと: `journal_flush()` は `disk_sync()`（device 全体の flush）で、commit の経路は `drv_ufs_journal_commitv()` → `drv_ufs_journal_checkpoint()` → `journal_replay()`。journal の commit の call site は `ufs.c` の 3228 行（名前の操作）と 5974 行（複数 extent）。再開の条件: fg011 の後、または journal の volume を実用で使うとき。

### 撤回前の実測（guest 8 GiB、NVMe の 256 MiB の volume、200 の空 file の作成）

    p: 200 creates 919 ms
    j: 200 creates 20579 ms

<!-- awesome-plan project=zedbsd record=ws336-p026 -->

# ws336-p026: 試験の C の規約の全文の見直し

Status: planned（2026-11-14 Q1。p025 の残りを分けた）
Parent: [WS336](../ws.md)
担当（推し）: conformance-reviewer
依存: p025

## 目的

`plan/ws336/tests/` の C（unit・fuzz・driver・shim）を `plan/coding-style.md` の全文と §14 の checklist に合わせる。書き方だけを直し、試験の中身（期待の値・場面）は変えない。

## 範囲

p025 の機械の検査で残った物（[p025 の phase.md](../phase025/phase.md) の「残り」に file ごとの件数と再開の手順）: paragraph comment 464、条件の中の呼び出し 141、forward declaration 61、conditional 51、入れ子の宣言 13、comment の形 1。purpose comment は 1 つずつ中身を読んで書く。

## 確かめ

- 見直しの前後で `sh plan/ws336/tests/run-unit.sh all`・`sshd-pair-host.sh`・`interop-host.sh`・`BASE=4ef0ba4b28 SSH_ZEDBSD_SYSROOT=<自分の build の sysroot> sh plan/ws336/tests/check-skeleton.sh`・`crypto-contract.sh` が PASS、`git diff --check` が空。
- 機械の検査の残りが 0、または規約が許す形（前向きの 1 つの cleanup の goto など）だけである事を数で示す。

## 結果

（未着手）

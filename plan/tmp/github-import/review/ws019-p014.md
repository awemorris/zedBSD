# WS019-p014: current UFS formatter integration

Date: 2026-09-09
Status: completed (q129); see [results](results.md)
Parent: [WS019](../ws.md)

## Objective

p008の旧UFS1受け入れを現行形式の根拠として流用せず、単一64bit UFSのmkfsをinstallerの前提として検証・整合させる。WS024で本体は移行済みだが、p008資料と旧fixtureには旧契約が残る。

## Procedure

1. userland/base/mkfsの独立codec・formatterを現行kernelと照合。カーネル私有ソースへの依存を復活させない。
2. 現行C/Python/Noct producerの普通・journal-snapshotプロファイル、32MiB installer画像、上下限、短いI/O・書込み失敗・破損拒否を検証する。リファクタリング前のfragment依存をfixtureから除く。
3. 旧IN-T010を現行`mkfs -t ufs`へ更新し、UFS1/UFS2という旧指定を拒否することも検証する。
4. targetの予約・固定サイズ・flush/readback、生成画像のmount/read/write/remountを使い捨てQEMU媒体で検証。不足があれば本体を修正する。
5. p004/p005はこの結果を前提とする。q079の履歴は書き換えない。

## Acceptance

独立userland build、ordinaryとASan/UBSan formatter検証、生成物比較、target実行と現行kernelでのmount/read/writeの記録を残す。host codec成功をtarget予約成功とは扱わない。

Timebox: 120 active minutes。残件は事実と再開条件を添えてunclearedとする。

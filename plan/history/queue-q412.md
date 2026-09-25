<!-- awesome-plan project=zedbsd record=queue-q412 -->

# Queue q412: 実 package を guest で最後まで（ws046-p008）

<!-- awesome-plan-current:start -->
Status: finished（2026-09-24）
Active Queue: なし
Executor: メインセッション（サブエージェントは使わない）
<!-- awesome-plan-current:end -->

Approval: 2026-09-24 ユーザー指示（WS046 の作成と p004 の引き継ぎ）「現在のphaseはclearedにしますが、課題が残ったので引き継ぐ形にしましょう。」「引き続き自走してください。」範囲は [ws046-p008](ws046/phase008/phase.md)。
Timebox: このセッション。

| Order | Attempt | Phase | Status |
| --- | --- | --- | --- |
| 1 | q412-i01 | [ws046-p008](ws046/phase008/phase.md) | uncleared（expat・zlib は guest で configure・build・check・install まで。coreutils は UFS の directory の 1 block の限界 BUG-038 で展開できず、WS054 を立てた） |

依存: ws046-p007 の修正 1・2（guest の build が実用の速さ。commit 1d74c966）。人間の判断は要らない。

Upcoming Work Outlook: ws054-p001（UFS の複数 block の directory の設計）、ws046-p009（BUG-033 の残り）、ws046-p005（規約と回帰）。

## 結果の要約（ws046-p008、q412-i01）

- guest の `data.img` は 32 MiB なので、1 GiB の UFS の作業の disk（`make-data-image.noct --size-mib 1024`、後に `zedimage-host ufs --inodes=16384`）を xHCI の port 4 につなぎ `/work` に mount した。
- expat: configure・make・install は status 0。check は automake の `test-driver` を直に使い（包みが bash）、512 MiB で 1 GiB の確保の試験が 12 落ち、2 GiB で 4932/4932。
  install の差は共有 library だけ（libtool が zedbsd を知らない、F-008）。
- zlib: configure・make・check・install が status 0。共有 library は ld.lld の `--no-undefined-version` の既定で作られず（F-009）、`-Wl,--undefined-version` で作ると試験も通り install の一覧が host と一致。
- coreutils: guest の pax が pax 形式の拡張 header を読めない（BUG-037、ustar で回避）。展開が `m4/` の 370 番目で ENOSPC: **UFS の directory が 1 block を超えない**（BUG-038）。WS054 を立てた。
- 起動の 1 回で CDC-ECM の attach が error 3 で失敗（BUG-036、間欠）。

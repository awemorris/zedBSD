<!-- awesome-plan project=zedbsd record=ws125 -->

# WS125: vim の package

<!-- awesome-plan-current:start -->
Status: planning
Primary Milestone: MG002
Objectives: O2
Parent: [Master](../master.md)
Focused goal: fg019（ベータ1）
Queue: none
Resume point: 2026-10-09 P1 q916: p002 は test-wait（Q1: ws126-p005 の image の T1 で boot を確かめてから cleared。`--subtree DEST=DIR`、[phase002](phase002/phase.md)）。p001（vim の取得・検証・監査・cross build）は planned。旧: p001 と p002 が planned、互いに独立。
2026-10-02 user: packages（Emacs・vim・Python）は「リリースのイメージに入れます」→ vim も release の image に既定で入れる。
Target: **ベータ4 以降**（2026-10-05 user「WS037, WS044,WS048,WS141, WS112, WS118, WS124, WS125, WS126, WS119, WS096, WS097, WS039, WS038, WS144, WS143, WS146,WS147, WS152,  WS119, WS080, は、ベータ4以降としてください。…WS027, WS015, WS047, WS028, WS017,  WS077, はキャンセルします。」）
<!-- awesome-plan-current:end -->

## 目標（2026-10-02 ユーザー（ベータ1、リリース目標 10/17））

「userland/packages/vim を追加」

- 外部 package として vim を build/install する。置き場所は `userland/packages/editors/vim`（2026-10-02 user 承認）。
- 版の提案: **9.2 系の最新の patch の tag**（[WS034 の台帳](../ws034/package-inventory.md) §1.2 では 9.2.1125 を取得・照合済み）。vim は公開 digest が無いので、GitHub の tag archive の `git get-tar-commit-id` と `git ls-remote` の tag の commit の一致で照合する。実装 Phase で最新の tag を取り直して実測する。
- license: Vim license（GPL 互換、package の境界の中）。

## ベータ1 の到達目標と受け入れ条件（案）

端末の vim（GUI 無し、`--with-features=huge` を既定案）と runtime（syntax・help 等）を image に入れ、guest で編集できる。

| # | 基準 | 確かめ方 |
| --- | --- | --- |
| V1 | `vim --version` が 9.2.x、`+multi_byte +syntax -X11 -gui` を示す | SSH |
| V2 | guest で `vim`（TERM=xterm）が起動し、挿入・保存（`:wq`）・終了ができる。内容を byte で照合し、終了後に端末の状態が戻る | SSH の pty の自動試験（`plan/ws125/tests/`）、Keiland の terminal の画面 |
| V3 | C の file で `:syntax on` の色が付き、`:help` が開く（runtime が見つかる） | 自動試験（`:redir` 等で確かめる）と画面 |
| V4 | UTF-8 の日本語を表示・編集・保存し、編集していない部分が byte で変わらない | 自動試験 |
| V5 | `/usr/share/licenses/vim/LICENSE`、provenance、license の機械監査の結果がある | 文書と image |
| V6 | vim を選んだ image でも `plan/tools/boot-test.sh` の login prompt が出る | boot-test の PNG |

## 依存（既存と不足）

| 依存 | 状態 | 扱い |
| --- | --- | --- |
| termcap API（`tgetent` 必須） | base の curses にある（ws034-p041 cleared） | `--with-tlib=curses` |
| cross の実行時判定 | `vim_cv_toupper_broken`・`vim_cv_terminfo`・`vim_cv_tgetent`・`vim_cv_getcwd_broken`・`vim_cv_timer_create_works`・`vim_cv_stat_ignores_slash`・`vim_cv_memmove_handles_overlap`・`vim_cv_uname_*` を cache で与える（台帳 §2.1） | p001 で根拠つきの値 |
| config.sub の zedbsd | 未対応 | package ごとの patch |
| 多数の file の image への導入 | runtime は約 2000 file。今の file ごとの `--file` は 1 つの shell の引数の上限（128 KiB）を超える | p002 で共通の仕組みを作る |
| Python・Lua・Perl・Ruby の interface、X11、GUI | 無い／不要 | 無効。Python の interface は WS126 の後の別件 |

## ユーザーの判断が要る点

- **D1（`vi` の名前）**: base に `vi` が無い。`/usr/bin/vi` を vim への link にするか。既定案は link しない（base の方針と衝突させない）。
- **D2（image の既定）**: release image に既定で入れるか（WS124 の D2 と同じ。既定案は menuconfig の既定 n、release は WS129 で決める）。

## Phase

| Phase | 目的 | Status | 依存 | 目安 |
| --- | --- | --- | --- | --- |
| [ws125-p001](phase001/phase.md) | 取得・検証・license 監査、cross build と stage（binary と runtime）、ELF の確認 | planned | — | 2〜3h |
| [ws125-p002](phase002/phase.md) | 共通: package の staged tree を image に入れる仕組み（`--subtree DEST=DIR`、file 数の上限を外す）。WS124・WS126 も使う | cleared 候補（2026-10-09 P1 q916、host 試験 PASS、boot-test は ws126-p005 の image で T1） | — | 2〜3h |
| [ws125-p003](phase003/phase.md) | menuconfig への登録、image への導入、guest の受け入れ V1〜V6 | planning（p001・p002 の成果待ち） | p001、p002 | 2〜3h |
| [ws125-p004](phase004/phase.md) | 全文規約と回帰、制限の整理（必須の最終確認） | planning | p003 | 1〜2h |

Graph: p001 → p003 → p004、p002 → p003。p002 → ws124-p004、ws126-p005。

WS034 の旧 Phase ws034-p008（vim、planning）とこの WS が重なる。所有の移管は main に依頼する。

## Event history

2026-10-02 / ws125-plan-detail-20261002: 計画担当が受け入れ条件と Phase を詳細化。image の file の導入が 1 つの shell の引数に入る制限を見つけ、共通の仕組みを p002 に置いた（WS124・WS126 の依存）。p001・p002 planned、Queue none。

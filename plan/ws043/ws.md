<!-- awesome-plan project=zedbsd record=ws043 -->

# WS043: base の utility を POSIX に（sed・grep・awk ほか）

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-24（q375〜q391）
Primary Milestone: MG002
Related Milestones: MG006
Objectives: O2
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

`userland/base` の text と file の utility を POSIX（XCU）に合わせ、autoconf の configure・libtool・package の build script が使う使い方が通るようにする。

## 結果

- 作り直した: `sed`（約 2700 行）、`awk`（POSIX awk の全体、約 5000 行）、`grep`、`cut`・`wc`・`head`・`tail`、`sort`・`uniq`・`tr`、`od`・`paste`・`join`、`rm`・`ln`・`touch`。
  直した: `expr`（0 での除算）、`cp`（`-f`）。足した: `printf`・`echo`・`test`・`true`・`false` の command（sh の builtin の source）。
- libc: `printf` の `%f`・`%e`・`%g` を double の正確な十進展開と最近接偶数の丸めに作り直した（glibc と 107 万件で一致）。stdout を端末でないとき完全 buffer にした。
- 差分試験（GNU の POSIX mode と比較）: 開始時 88/329 → **484/484**（host）。amd64 guest（zedBSD の libc・TRE の regex・/bin/sh）でも全件。
- 実際の configure: expat 2.8.5 と coreutils 9.12 を、我々の sed・grep・awk と他の utility で走らせ、GNU の道具のときと生成物が同一。
- 規約: WS の全 source を `plan/coding-style.md` と照らし、道具 `plan/tools/style-check.py` の違反 0（ws043-p008、後の変更も同じ道具で確かめた）。

## 制限・移管

- guest の上で configure を丸ごと走らせる確かめ（C compiler の入った image が要る）は ws042-p005 が担う（WS043 に依存していた）。
- GNU 拡張（`sed -i`・`\+`、gawk の拡張、複数文字の RS ほか）は [WS045](../ws045/ws.md)。`[` の実行 file は Future Work F-005。
- `expr/main.c` の既存の規約違反（WS043 では 2 行だけ変えた）は範囲外。
- 見つけた不具合: tmpfs に約 230 個の file で system 全体が file を開けなくなる（[BUG-029](../bugs/BUG-029.md)）。guest の試験は 40 件ずつに分けて回避した。
- 試験と道具は `plan/tools/utils/`（Tools 節）に移した: `util-diff.py`・`build-host-utils.sh`・`cases/`・`configure-diff.sh`・`float-format.c`。guest では `plan/tools/sh/guest-diff.sh` で走らせる。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws043-p001 | 調査と差分試験の土台 | cleared（q375） |
| ws043-p002 | `sed` | cleared（q379。q376 は中断で uncleared） |
| ws043-p003 | `awk` の言語 | cleared（q385） |
| ws043-p004 | `grep` | cleared（q380） |
| ws043-p005 | `cut`・`wc`・`head`・`tail` | cleared（q381） |
| ws043-p006 | `sort`・`uniq`・`tr` | cleared（q382） |
| ws043-p007 | `od`・`expr`・`paste`・`join` ほか | cleared（q383） |
| ws043-p008 | 全文の規約確認 | cleared（q387） |
| ws043-p009 | `printf`・`echo`・`test`・`true`・`false` の command | cleared（q384） |
| ws043-p010 | `awk` の入出力と実際の configure | cleared（q386） |
| ws043-p011 | guest の回帰 | cleared（q390。q388 は libc の欠陥で uncleared） |
| ws043-p012 | libc の浮動小数の十進変換と stdout の buffer | cleared（q389） |
| ws043-p013 | `rm`・`ln`・`touch`・`cp -f` | cleared（q391） |

## 記録の所在

各 Phase の計画・結果・証拠は、WS の完了のときに削除した。git の commit `0c181cc4` 以前の `plan/ws043/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。

<!-- awesome-plan project=zedbsd record=ws338-p03 -->

# ws338-p03 Pi 5・5330 の基準の実測

- 親: [WS338](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-11-15 p001 で作成）
- 依存: p002、design §1 の build の flag が main に入った事、実機の割当（Q1）
- 担当の想定: Opus 5.5（phase-runner）

## 目的と範囲（design §3.4・§9）

- p002 の `measure-hw.sh` を Pi 5（zedBSD、arm64）と 5330（zedBSD、x86_64）で流す。実機は Q1 が割り当てる（電源は plan/tools.md の「実機」）。
- 取る数: grain・実 B の 1 thread の best・median・60 秒の連続の平均、4 本の 1 thread の process の同時の各 fps（scheduler・memory・熱の上限の目安）、mutex・cond の往復の費用、出力の MD5 が host と同じか（arm64 の正しさの初めての確かめ）、温度・clock（取れれば）。
- design §3.4 の換算（Pi 5 の 1 core = host の 0.6〜0.75 倍）を実測で置き換え、§7.4・§9 の見込みを計算し直して結果に書く。見込みで Pi 5 の 40 fps に届かないと分かったら Q1 へ（p011・p012 の判断の材料）。

## 受け入れ

- 両機の要約が `plan/ws338/measure/` にあり、MD5 が host と同じ（違えば止めて Q1 へ。bug の解析は別）。

## 変えてよい物

- `plan/ws338/tests/`（道具の直し）、`plan/ws338/measure/`、この phase.md と ws.md の行。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C、宣言は関数の先頭、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、§14 の checklist で自分の diff を見直す）。拡張は `__inline`・`__attribute__` の綴りで。
3. [design.md](../design.md) の §1（決まった事・出典）と、この Phase の節。[../../ws326/design.md](../../ws326/design.md) の §1（出典の規則: FFmpeg・openh264・JM・x264 などの code・comment・構造・名前を写さない。仕様と自分の profile から導く）・§4（所有と寿命）
4. 仕様: ITU-T H.264 (08/2021)。PDF は ITU の公開の URL（`T-REC-H.264-202108-S!!PDF-E`）から自分の worktree の `build/sources/specs/` へ（git に入れない）、SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`。印刷の頁 N = PDF の頁 N + 22。

## 変えてはいけない物

- この Phase の「変えてよい物」の外の source。HAL・toolchain・Makefile・`mk/`・`sources.mk`・共有の build/（AGENTS.md §7）。新しい `.c` を足さない。
- 他の WS の記録、`plan/tools/h264sw/`（不備は Q1 へ。この WS の試験は `plan/ws338/tests/`）。
- rm などの削除をしない（消す物は path と理由を Q1 へ）。
- header の不備・足りない口を見つけたら（header を変えない Phase）、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。

## 終わりの条件

- 上の確かめが全部 PASS（出力を自分で読む）。自分の diff を coding-style §14 で見直した。source の comment・log に WS・Phase の番号や plan/ への参照が無い。表の block に `Values referenced: ITU-T H.264 (08/2021) ...`。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・command と結果・未実施・残りを Q1 へ。
- この phase.md に「結果」の節（日付は host の `date`、command、数、未実施）を足し、ws.md の表の行を「実装済み・試験 PASS」に（cleared は Q1）。

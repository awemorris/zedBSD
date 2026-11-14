<!-- awesome-plan project=zedbsd record=ws338-p10 -->

# ws338-p10 Pi 5・5330 の実測と目標の判定

- 親: [WS338](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-11-15 p001 で作成）
- 依存: p009 が main に、p003、実機の割当（Q1）
- 担当の想定: Opus 5.5（phase-runner）

## 目的と範囲（design §9）

- p002 の `measure-hw.sh`（thread の数 1・2・4）を Pi 5・5330 で。VF2 は 1 thread を p003 の頃の tree と今の tree で（scalar での後退の記録、ゲートにしない）。
- ゲート: Pi 5 の zedBSD で Q338-3 のゲートの stream が 4 thread で 40 fps 以上（60 秒以上の連続の decode の平均、3 回の median）。出力の MD5 が host と同じ。温度・clock を記録。
- 出荷の libmedia.so を `media_decoder_*` で通す計測を 1 本（speed-bench と差が大きければ理由を調べて記録）。
- Pi 5 の Video Player で実 B を再生（boot-test の image で。app に落とした frame の数の口があればその数、無ければ「未実施」と書き Q1 へ）。
- 5330: Q338-2 の答えのとおり。
- 届かなければ: p008 の P の割合と Pi 5 の数から、p011（picture の重なり）と p012（short vector 型、Q338-1）のどちらが効くかを見積もって Q1 へ。

## 変えてよい物

- `plan/ws338/tests/`、`plan/ws338/measure/`、この phase.md と ws.md の行。

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

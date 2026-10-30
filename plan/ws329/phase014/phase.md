<!-- awesome-plan project=zedbsd record=ws329-p014 -->

# ws329-p014 動きの探索: h264enc-me

- 親: [WS329](../ws.md)、設計 [design.md](../design.md)。
- 状態: planned（2026-10-31 p001 で作成）。
- 依存: p012、p013（依存の Phase の実装が main に入ってから始める）。
- 担当の想定: Sonnet 5.5 high（phase-runner-low）を 1 回起動（標準廉価実装手順）。header は p001 の設計（p003 で source に置いた物）で、変えない。

## 読む物

1. AGENTS.md の §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文（§14 の checklist）、[design.md](../design.md) の §1・§3・§4・§7・§10.1・§13.1、`userland/desktop/libmedia/h264sw.h` と `h264enc.h`
2. 実装する header: `h264enc-me.h`（各関数の comment が仕様）。それが include する header（decoder の `h264sw-*.h` を含む）も読む。
3. 仕様: ITU-T H.264 (08/2021)、PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`）。`pdftotext -layout` は列が崩れることがある。 読む節: （仕様に無い。header の手順と文献）。MV の範囲は A.3.1・表 A-1（MaxVmvR）。

## 作る物

- `userland/desktop/libmedia/h264enc-me.c`。
- 単体試験 `plan/ws329/tests/unit/test-me.c`。
- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- 合成の画（雑音の texture を平行移動、量は full・1/2・1/4 画素の組、大きな移動）で、範囲の中なら求めた vector が既知の移動と一致（雑音の無い時）。
- 手順の再現: 候補の順と同点の扱い、large diamond の有無、refine、subpel 0・1・2、points 4・8 で、試験の中に書いた header の手順の素朴な実装（同じ cost の関数）と結果が一致（乱数の画で 1000 例）。
- 窓と範囲と mv_min/max の端で外に出ない。`h264enc_me_cost` が predict + SATD + λ·bits と一致。
- 共通: 誤りの引数で落ちず header の誤りを返す。ASan・UBSan の報告 0。期待の値は仕様の表・手の計算・decoder の module の読み戻し（design §10.1）から作り、他の encoder（x264・openh264・JM など）の出力や source を使わない。

## 注意

- 探索の点の順と strict な改善は header のとおり（再現のため）。
- 他の実装の code・comment・構造・名前を写さない。表は block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment。source の comment に WS・Phase の番号や plan/ への参照を書かない。

## 変えてはいけない物

- 全部の `userland/desktop/libmedia/h264sw*.h`・`h264enc*.h`・`vcodec-*.h`。header の不備・矛盾・足りない口を見つけたら直さずに止めて Q1 に返す（どの header のどの関数か、何が足りないか、案の差分）。header が黙っている所は自分で決め、「結果」に列挙する。
- 担当の外の `.c`（他の Phase と decoder の物）、Makefile、`plan/ws329/tests/run-unit.sh`・`check-skeleton.sh`、`plan/ws326/tests/` の全部。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。

## 確かめの command（worktree の root で）

- `timeout 900 sh plan/ws329/tests/run-unit.sh me` と `timeout 1800 sh plan/ws329/tests/run-unit.sh all` → 最後の行 `run-unit: PASS`。
- `timeout 1800 sh plan/ws326/tests/run-unit.sh all` → `run-unit: PASS`（decoder が変わっていない）。
- `BASE=<p003 の SHA（ws.md に記録）> timeout 300 sh plan/ws329/tests/check-skeleton.sh` → `check-skeleton: PASS`。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、warning 0
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 試験が通った後、実装をわざと数通りに壊して（git の外で）試験が FAIL することを確かめ、元に戻す（`cmp` などで戻ったことを確かめる）。見逃しがあれば試験を足す。
- 自分の diff を coding-style の §14 の checklist で見直した。
- 新しい file を `git add` してから `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と最後の行・自分で決めた点・未実施を Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、command、PASS の数、自分で決めた点、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1）。

<!-- awesome-plan project=zedbsd record=ws344-p011 -->

# ws344-p011 VP8 の frame header（vp8-header.c）

- Status: 実装済み・試験 PASS（Sonnet q1951-i01、2026-11-02。cleared は Q1）
- 親: [ws.md](../ws.md)、設計: [design.md](../design.md)。担当の model: Sonnet 5.5 high（phase-runner-low）。依存: p009, p010。
- 目的: frame tag、key frame の header（segment・filter・partition・量子化・確率の更新・skip）、逆量子化の係数。
- 基点の SHA（header と道具の凍結）: `aa2426a5763975c358cf667aff3e17bfb841b7e2`。

## 1. 読む物

- AGENTS.md §4（担当の共通の規則）・§7（禁止）・§9（検証）・§10（コード）、plan/agents/protocol.md、docs/agent/spec-module-delegation.md §5.3・§7（出典と license）。
- plan/coding-style.md の全文（ANSI C、宣言は関数の先頭、段落ごとの comment、if・loop・return の comment、Review checklist）。
- plan/guardrail.md の「MIT・GPL の出典のデータの引用」（block ごとの複数行の comment「Values referenced: <仕様と版> <節・表>」「Procedure referenced: ...」、file の先頭に書かない）。
- plan/ws344/design.md の §1（出典と RFC の既知の誤り）・§4（決定）・§7（上限と信用できない入力）・§8（試験の道具）と、§5 のこの module の行。
- `userland/desktop/libwebp-compat/common.h`（誤りの値・算術の規則。全 module の約束）。
- 仕様（git の外、著作物）: RFC 9649 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc9649.txt`（SHA-256 `f4d7cd92b015c0b340f10a932bb03e564a758a17df50cbb992c16d8459d60e58`）、RFC 6386 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc6386.txt`（SHA-256 `c044997ae5e26f74dc28569f8866937412b2783e887c96e0c09372af78be411c`）、RFC 1951 `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc1951.txt`（SHA-256 `5ebf4b5b7fe1c3a0c0ab9aa3ac8c0f3853a7dc484905e76e03b0b0f301350009`）。text なので節は `sed -n '/^13\.3\./,/^13\.4\./p' <file>` のように読む（頁の区切りの行 `Bankoski, et al.`・`RFC 6386 ...` は無視）。
- `vp8-header.h`（仕様）、`bool.h`、`vp8-tables.h`。
- RFC 6386 §9.1〜§9.6、§9.9、§9.11、§19.1、§19.2（key frame の順の通り）、§13.4（token_prob_update）、§14.1（dc_q・ac_q と y2・uv の規則）、§20.4（dequant_init）。

## 2. 作る物

- `vp8-header.c`、`unit/test-vp8_header.c`。
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 試験の項目

- read_tag: 正しい 10 byte、各誤り（10 byte 未満 ENOBUFS、interframe・version 4・show_frame 0・start code 違い・幅 0・高さ 0 は EINVAL）、scale の bit が大きさに入らない。
- read_header: boolwriter で first partition を組む（§19.2 の順）: segmentation（無効、有効で map だけ（update_data 0: absolute 0・値 0 のまま、dequant は base_q）、data だけ、両方、絶対値と差分、quantizer と filter_level の正負、tree_probs の有無（無ければ 255））、filter（simple・normal、level 0・63、sharpness 0・7）、delta（無効、有効で更新無し、ref・mode の各 4 つの有無と正負）、partition 1・2・4・8（size の表、大きすぎる size は残りに切る、最後の partition が空は ENOBUFS、size の表が足りないのは ENOBUFS）、量子化の index と 5 つの delta の有無と正負、refresh_entropy_probs、確率の更新（いくつかの entry を更新、他は既定）、skip の有無と確率。first partition の size が data に入らないのは ENOBUFS。header の途中で first partition が尽きるのは ENOBUFS。
- dequant_build: q = 0（y1 4・4、y2 8・8、uv 4・4）、q = 127（y1 157・284、y2 314・440、uv 132・284）、q + delta の clamp（q = 120 で delta 15、q = 3 で delta −15）、segment の絶対値と差分（q 自体は clamp しない、§20.4）。期待の値は §14.1 の表と規則から手で。
- 全ての単体試験は ASan・UBSan で落ちず（run-unit.sh）、途中で切れた入力・範囲の外の値・乱数の入力で決められた誤りを返す。
- 期待の値は仕様の表・式と手の計算、test の側の逆の処理（lbitwriter・boolwriter で組んだ入力）だけから作る。他の実装の出力を期待の値にしない（fixture の全体の試験は道具が libwebp の出力と比べる）。

## 4. 注意

- key frame の header は §19.2 の順の通り: color_space、clamping_type、segmentation_enabled（と update_segmentation）、filter_type、loop_filter_level（6）、sharpness_level（3）、mb_lf_adjustments、log2_nbr_of_dct_partitions（2）、quant_indices、refresh_entropy_probs、token_prob_update、mb_no_coeff_skip（と prob_skip_false）。interframe の field（refresh_golden など）は無い。
- segment の quantizer は L(7) + 符号、filter_level は L(6) + 符号。delta は L(6) + 符号。量子化の delta は L(4) + 符号。
- partition の size は 3 byte の little endian。

## 5. 変えてはいけない物

- 全ての header: `userland/desktop/libwebp-compat/*.h`、`include/libc/compat/webp/*.h`。header の comment が誤り・不足と思えば直さずに止め、どの header のどの関数で何が足りないかを Q1 に返す。header が黙っている所は自分で決め、結果の節に書く。
- この Phase が名指す以外の .c、`common.c`、`Makefile`・`sources.mk`・`exports.map`、`platform/`・`apps/`・`config/`。
- 試験の道具: `plan/ws344/tests/` の `*.sh`・`*.py`・`*.c`・`fixtures.list`・`fixtures.sha256`、`unit/check.h`・`unit/lbitwriter.h`・`unit/boolwriter.h`・`unit/test-helpers.c`・`unit/test-common.c`。道具の不具合（補助の header の未定義の動作など）は Q1 に返す。
- plan/ws344/design.md（判断や食い違いは結果の節に書く）、HAL・toolchain・共有の `build/`、main の checkout。
- 他の実装（libwebp・libvpx・FFmpeg など）の source を読まない・写さない。RFC 6386 §20 の参照 decoder は仕様として読んでよいが code・comment・構造・名前は写さない（自分の名前と構造で書く）。

## 6. 確かめの command

```sh
cd <worktree>
sh plan/ws344/tests/run-unit.sh vp8_header
sh plan/ws344/tests/run-unit.sh all
BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2 sh plan/ws344/tests/check-skeleton.sh
sh plan/ws344/tests/target-build.sh
git diff --check
```

## 7. 終わりの条件

- 上の command が全部 PASS（最後の行を自分で読んで確かめる）。`check-skeleton` は `BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2` で header と道具が変わっていないこと。
- 試験が通った後、実装をわざと数通りに壊し（git の外の複写で。例: 境界の比較を 1 つずらす、符号を 1 つ逆にする、表の値を 1 つ変える）、単体試験が FAIL することを確かめ、元に戻す（`cmp` で戻ったことを確かめる）。見逃しがあれば試験を足す。
- 規約の checklist（coding-style.md §14）で自分の diff を見直した。表・定数の block ごとに出典の comment がある。source の comment に開発の管理情報（WS・Phase・Queue・Bug の番号、plan/ への参照、日付）が無い。
- commit: 新しい file は先に `git add`、`git commit -m WIP -- <作った path>...`。push しない。rm しない。
- 結果の節（下）: 日付（host の `date`）、実行した command と最後の行、件数、未実施、自分で決めた点、仕様と phase.md・header の食い違い。ws.md のこの行を「実装済み・試験 PASS（担当、日付）」に（cleared は Q1 が決める）。

## 結果

- 日付: 2026-11-02（host の `date`）。担当: Sonnet 5.5 high（q1951-i01）。worktree `s-ws344-p011`、実装の commit e12d4f0492（`vp8-header.c`・`unit/test-vp8_header.c`）。
- 実行した command と最後の行:
  - `sh plan/ws344/tests/run-unit.sh vp8_header` → `test-vp8_header: PASS (45344 checks)`、`run-unit: PASS`。
  - `sh plan/ws344/tests/run-unit.sh all` → test-bool 8376、test-common 32、test-helpers 26、test-lbits 205384、test-prefix 1430293、test-vp8_header 45344、test-vp8_tables 37674 の PASS、`run-unit: PASS`。
  - `BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2 sh plan/ws344/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - `sh plan/ws344/tests/target-build.sh` → `target-build: PASS (build/amd64/dynamic/libwebp-compat.so, 0 warnings)`。
  - `git diff --check` → 出力無し。
- 変異試験（git の外の複写 `build/tmp/p011-mut-1/m<n>/` に 44 通り: 境界の比較・符号・bit 数・field の順・表の添字・定数・segment の絶対値と差分・partition の切り方・clamp など）: 39 通りが FAIL（検出）、5 通りが生存。生存は全て等価な変異: (8) `first_size >= after_tag`（first が data の終わりちょうどの時、後続が空で最後の partition の ENOBUFS になり同じ誤り）、(23) `rest_size <= table_size`（等しい時 remaining 0 で ENOBUFS、同じ）、(24) `part_size >= remaining`（等しければ同じ値）、(30) `uv[0] > 133`（表に 133 が無い）、(34) `q < 1`（q 0 は元から 0）。元の file は変えていない（git status clean）。
- 試験の中身: boolwriter で §19.2 の順に first partition を組む spec（全 field に presence flag と値）を作り、読み戻して全 field・dequant（試験の側で long の算術と表の lookup から別に計算）・coeff_probs・partition の位置と大きさ・marker（header の直後に L(8) で 0x5a を書き、`first` が最初の macroblock header の位置にあることを確かめる）を比べる。固定の場合: tag の正誤、segmentation の各形、filter の delta、partition 1・2・4・8 と大きすぎる size・空の最後・足りない table、量子化の delta、確率の更新、手で計算した dequant（q 0・127・1・2・116〜118・60 の delta 付き）、q の clamp と範囲外の q（±2^31 を含む）。4 partition の frame を全長で切り、§9.5 の模型と照合。乱数の spec 300 個、乱数の byte 2000 個と乱数で書き換えた frame 2000 個（誤りは 0 か ENOBUFS だけ、partition は buffer の中）。全て ASan・UBSan で、exact-size の複写から読む。
- 未実施: fixture の全体の試験（libwebp の出力との比較）は統合の Phase（p017）。QEMU・実機は対象外。
- 自分で決めた点（header が黙っていた所）:
  - `read_tag` は最初に header を 0 で消し、10 byte 未満でも消した状態で ENOBUFS を返す。width・height の 0 は field を埋めた後で EINVAL（header は部分的に埋まったまま、失敗の時は不定の約束の内）。
  - `read_header` は `size < 10` も ENOBUFS。first partition の終わりの検査（`webpc_bool_ended`）は skip の読みの後、partition table の前。`read_header` は segmentation・filter・skip_prob を自分で 0 から作り直す（read_tag を通らず再利用されても古い値が残らない）。
  - segmentation の update_data が 1 の時、flag 0 の quantizer・filter_level は 0（§20.4 の bool_maybe_get_int と同じ）。無効の時は dequant は base_q から作る（4 segment とも同じ）。
  - `webpc_vp8_dequant_build` は q を ±256 に先に絞ってから delta（±15）を足す（q が int32 の端でも符号付き溢れが起きず、143 以上は 127、−15 未満は 0 になる結果は変わらない）。
  - partition の size の読みは 3 byte little endian（common の `webpc_le24`）。size が残りを越える時は残りまでに切り、残りは以降の partition に回らない（以降は空、最後が空になれば ENOBUFS）。
- 仕様と phase.md・header の食い違い: 無し。RFC 6386 §9.3 の本文は segment_feature_mode を「absolute 0・delta 1」と書くが、§19.2 の欄と §20.4（`abs = bool_get_bit`）は 1 が absolute で、header.h も後者（absolute 1）。後者に従った。token_prob_update の flag は L(1)（確率 128）ではなく `coeff_update_probs` の確率の bool（§13.4、§20.4 の decode_entropy_header）で読む。
- 削除の依頼: `build/tmp/p011-mut-1/`（変異試験の複写、自分の worktree の scratch）。

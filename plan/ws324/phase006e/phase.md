<!-- awesome-plan project=zedbsd record=ws324-p006e -->

# ws324-p006e H.265: h265-dpb の実装（Vulkan の DPB、8.3.2 の marking と C.5.2 の出力の順。標準廉価実装手順、Sonnet 5.5 high）

Status: planned（2026-11-01 P1 が p006a で作成。p006a 段 2 の基点の後に起動。p006b〜d と並べてよい）

- WS: [WS324](../ws.md)。設計: [h265-parse-design.md](../h265-parse-design.md)（§1・§4 D2・D10・D11・D13・§5.3・§6・§7・§8）。
- 基点 BASE: `1f16db0e6`（2026-11-02 p006a 段 2 の commit 1f16db0e619a9cd23c74f7f36a8729efdba4ecb3）。この module は他の module の実装を呼ばない（型だけを使う）。試験の入力は手で組む。

## 1. 読む物

- AGENTS.md の §4・§7・§9・§10。`plan/coding-style.md` の全文。`plan/guardrail.md` の「出典」「開発の管理情報」。`docs/agent/spec-module-delegation.md` の §5・§7。
- 設計の §1・§4（D2・D10・D11・D13）・§5.3（使い手の流れ）・§6・§7・§8。
- 実装する header: `userland/desktop/libmedia/h265-dpb.h`。使う型: `h265sw-poc.h` の `struct h265sw_rps_pocs`、`h265sw-ps.h` の `struct h265sw_sps`（size・reorder・latency・log2_max_poc_lsb を読むだけ）。
- 仕様: `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`（ITU-T H.265 (V11) (01/2026)、SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`、印刷の頁 N = PDF の頁 N + 18）。
  - 8.1.3（NoRaslOutputFlag、step 4 の marking、PDF 135〜137）、8.3.2（8-6・8-7、step 1〜4、NOTE 4・5、PDF 138〜141）、8.3.3.1 の NOTE（PDF 141）、7-9（SpsMaxLatencyPictures、7.4.3.2.1、PDF 95 前後）、C.5.2.1〜C.5.2.4（PDF 306〜307）。

## 2. 作る物

- `userland/desktop/libmedia/h265-dpb.c`。
- `plan/ws324/tests/unit/test-dpb.c`。
- この phase.md の「結果」、ws.md の表のこの行。

## 3. 試験の項目

試験は、絵の列（各絵の POC、NoRaslOutputFlag、NoOutputOfPriorPicsFlag、PicOutputFlag、RPS の 5 つの POC の list）を手で書き、C.5.2 の手順を紙の上でたどった表（各段の outputs の順、discarded、各 entry の marking と needed、使った slot）と照合する。SPS は試験の中で `struct h265sw_sps` の該当の field だけを埋める。

- IDR だけの列（reorder 0、size 1・2）: 毎回すぐ出力。
- I P P P（reorder 0、short-term 1 枚）: marking と出力の順、slot の再利用（release の後）。
- 階層の B（GOP 8、reorder 2・3・4、size 5・6）: 出力の順、bumping の 3 つの条件のそれぞれが効く場面（needed の数、latency、size の満ち）。latency_limited 0 と 1（max_latency の境）。
- long-term: LSB だけの照合（msb 0）と全 POC の照合（msb 1）、short-term から long-term への移り、Foll の残り、一致が 2 つある時に id の大きい方。
- IRAP の NoRaslOutputFlag 1: 最初の絵（picture 0 で何もしない）、2 つ目以降で NoOutputOfPriorPicsFlag 0（全部を順に出す）と 1（discarded、出さない）。
- Curr の欠け: missing の数と slot の H265_DPB_NO_SLOT。Foll の欠けは数えない。
- 壊れた stream: 全部が参照で満ちた buffer で evicted（Curr に無い最小の POC）、全部が Curr の時の EINVAL（in_picture 0、outputs は plan にある）。
- slots 16（nvrtx と Wave511 の maxDpbSlots と同じ）で、size 16 の SPS の列を出力が busy のまま流し、EBUSY → release → 再試行で進むこと、各段で reference_count ≤ 15。
- slot: take_slot の最小の空き、全部 busy の EBUSY（何も変わらない、release の後に成功）、2 回目の take_slot が同じ slot、出力した slot の busy、参照のままの出力済みの slot が release の後も保たれる。
- abort（current を入れない、slot が空く）、end の誤り（setup_slot の不一致、in_picture 0）、flush の順と started 0、clear の後の busy の保持。
- 乱数の列（seed 固定、10 万絵、ランダムな RPS と型）で落ちず、不変条件（used の entry の slot が重ならない、setup_slot が reference_slot に無い、busy は slots の中、出力の POC が CVS の中で増える、used の数 ≤ size）を毎段で確かめる。
- わざと壊す確かめ（例: bumping の > と >=、latency の +1 の条件、LT の照合の mask、evict の選び）。

## 4. 注意

- 試験の道具の口は設計の §8.1.1（p006a 段 2 の結果）。試験は `plan/ws324/tests/unit/test-<name>.c` に置き、include は `"check.h"`・`"h265write.h"`・`"userland/desktop/libmedia/<module>.h"`（include の道は tree の root）、`main` の最後は `check_finish("<name>")` の値を返す。大きな構造体（`h265sw_ps_store`・`h265_stream`・`h265_picture`・`h265w_sps` など）は static か calloc に置く。
- 骨格の関数は本体ごと置き換え、「Not implemented yet」の comment も実装の comment に書き換える。module の中の補助は static 関数（一行の前方宣言を付ける）。外へ見える名前は `h265sw_`・`h265_` で始まる物だけ（check-skeleton の nm の照合）。
- libmedia の .c は check-skeleton で `-std=c89 -Wpedantic` と zedBSD の clang（x86_64・aarch64・riscv64、tree の libc の header だけ）にも通す: `LL`・`ULL` の接尾辞（`-Wlong-long`）、`//` の comment、文の後の宣言、host にしか無い関数を使わない。64 bit の定数は `(uint64_t)1U << n` か乗算で作る。
- `run-unit.sh` の `H265_UNIT_DIR` は道具の自己確かめ用。使わない。
- 8.3.2 の照合の順: long-term（step 1・2）を先に、short-term（step 3）は short-term の参照の中から。1 つの絵は 2 つの set に入らない（NOTE 5）ので、選んだ絵を印して再び選ばない。
- `PicOrderCntVal & ( MaxPicOrderCntLsb − 1 )` は負の POC で floor の算術（`h265sw.h` の約束）。
- C.5.2.2 の IRAP の枝は picture 0（started 0）では何もしない。
- bumping は、出力の要る絵が 1 つも無ければ止める（条件が真でも、空の bumping で無限に回らない）。
- 出力した絵の slot は busy にする。entry が空になっても busy の間は take_slot が選ばない。
- `latency` は uint64_t、比べは uint64_t で。

## 5. 変えてはいけない物

全ての header、他の module の .c、Makefile・`sources.mk`、試験の道具、HAL・toolchain・共有の build。header の不備は止めて報告。黙っている所は決めて結果に書く。仕様が正。

## 6. 確かめの command

```
sh plan/ws324/tests/run-unit.sh dpb
sh plan/ws324/tests/run-unit.sh all
BASE=1f16db0e6 sh plan/ws324/tests/check-skeleton.sh
git diff --check
```

## 7. 終わりの条件

p006b の §7 と同じ。

## 結果

（実装の担当が書く）

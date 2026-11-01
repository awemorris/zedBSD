<!-- awesome-plan project=zedbsd record=ws324-p006e -->

# ws324-p006e H.265: h265-dpb の実装（Vulkan の DPB、8.3.2 の marking と C.5.2 の出力の順。標準廉価実装手順、Sonnet 5.5 high）

Status: cleared（2026-11-02 03:43 Q1、main で test-dpb PASS）

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

実装の担当（Sonnet 5.5、q1900-i01、worktree `s-ws324-p006e`）、2026-11-02（host の `date` で 03:2x JST）。cleared は Q1 が決める。

### 作った物

- `userland/desktop/libmedia/h265-dpb.c`（骨格を実装で置き換え）。header・道具・他の module は変えていない（check-skeleton の BASE 照合 PASS）。
- `plan/ws324/tests/unit/test-dpb.c`。
- commit: 実装と最初の試験 `c14ff1c812`、試験の追加（壊す確かめで見逃した所を塞いだ物）は`b0b3acd002`。

### 確かめた command と最後の行

| command | 最後の行 |
| --- | --- |
| `sh plan/ws324/tests/run-unit.sh dpb` | `test-dpb: PASS (40617273 checks)`、`run-unit: PASS`（約 6 秒） |
| `sh plan/ws324/tests/run-unit.sh all` | dpb 40617273・h265write 51・nal 816099・ps 1447981・rps 1891521・scaling 5499・vui 2432070 checks、全部 PASS、`run-unit: PASS` |
| `BASE=1f16db0e6 sh plan/ws324/tests/check-skeleton.sh` | `check-skeleton: PASS`（h265-std は Vulkan の header が tree に無いため飛ばし、p006b・p006c と同じ） |
| `git diff --check` | 出力なし（終了 0） |

### 試験の中身（要点）

- 手の scenario（絵を文字列 `"poc I ! n b.. a.. f.. L.. F.."` で書き、各段の「begin で出る絵」「end で出る絵」「参照の絵（POC 昇順、long は L）」「slot」「missing・evicted・discarded」を C.5.2.2〜C.5.2.4 と 8.3.2 から手で解いた表と照合。各 scenario の comment に解き方）: IDR だけ（size 1・2、release あり・なし）、IPPP（slot の再利用）、階層 B の GOP 8（reorder 3・4・2、size 5・6・7、release あり・なし）、latency（plus1 1・2・0、limit の境、pic_output 0 で数えない）、size の満ちによる bumping（begin で 2 回）、evict（Foll、2 つの Foll の小さい方）、long-term（lsb だけ・msb 付き・一致 2 つで id の大きい方と NOTE 5 の missing・負の POC の floor・unused だが待つ絵は見つけない・short→long・Foll の LT が残る）、IRAP（picture 0、NoOutputOfPriorPicsFlag 0 で全部を順に出す・1 で discarded、Foll だけの CRA 風、Curr の missing、最初の絵が IRAP でない時）、missing の数え方。
- 個別: 全部が Curr で EINVAL（outputs は plan に、in_picture 0、後の take_slot・end は EINVAL）、slots 16 で size 16 の列 300 絵（EBUSY → release → 再試行、reference_count ≤ 15）、slot の規則（init の clamp、EBUSY で不変、release で再試行、2 回目の take_slot が同じ slot、別の plan でも埋める、32 slot 全部 busy、参照のままの出力済み slot が保たれる、範囲外の release、busy の保持）、begin の誤り（二重 begin・size 17・minus1 の最大・log2 3 と 17・sub-layer 7・各 list 17 個・NULL）で不変、各 list 16 個は受理、HighestTid の値を使う（sub-layer 2 と 6 で値を変えて確認、plus1 0xFFFFFFFF）、end の誤り（slot 無し・不一致・entry 満杯・NULL）で不変、abort（begin の marking と出力が残る、slot が戻る）、flush（出力順・id・slot busy・空に・started 0・in_picture で EINVAL）、clear（busy 保持）、plan.reset（最初・flush の後・clear の後）、壊れた状態（範囲外の slot を持つ entry）で UB なし。
- 乱数: 10 本 × 1 万絵（seed 固定、slots 16〜32、SPS の値と sub-layer、IRAP と NoOutputOfPriorPicsFlag、壊れた RPS（無い絵・同じ絵を 2 回）、PicOutputFlag、release の遅れ、EBUSY、abort、flush、clear）。各段で、test の中に別に書いた model（plain な配列、選択は単純な走査、LT・ST の照合・bumping・evict・slot を spec から独立に書いた）と、begin の戻り・plan の全 field・出力・slot・buffer の全 entry・busy・started を比べ、invariants（使用中の entry の slot が重ならない・busy が slots の中・空の entry は全部 0・marking の範囲・reference_slot が entry と一致し setup_slot を含まない・reference_count ≤ 15）を確かめる。
- 入力は手で組んだ POC と RPS（`h265write.h` は使っていない。この module は ps・poc の実装を呼ばない）。期待の値は C.5.2・8.3.2 と手の計算で、他の実装の出力は使っていない。

### わざと壊す確かめ（git の外、元の .c は変えず mutant を別の場所で build、`git diff` で元の無変更を確認）

- 418 か所（script `scratchpad/p006e-mut/mutate.py`。比較・論理演算の反転、`return EINVAL/EBUSY` を 0 に、`continue`・`break` の削除、定数 0/1 の反転、`+= 1U` の無効化、単純な文の削除）。各回を ASan・UBSan 付きで build して流し、file に書き、RLIMIT_FSIZE 64 MB・RLIMIT_CPU 400 秒・RSS 6 GB 超か wall 150 秒（build は 120 秒）で kill（4 並列。ASan のため RLIMIT_AS は使えず RSS 監視で代えた）。
- 結果: 378 か所が試験の失敗（`FAIL`・ASan・UBSan の停止・上限の kill 10 件を含む）、生き残り 40 か所。build の誤りは 102 か所出た（未使用変数・空の if の body の警告）。それらは `-Werror` を外して再 build（意味は同じ mutant）し、全部 build でき、試験の失敗で落ちた物 88 + 2、生き残り 12 に入る（`R` の印）。再 build できなかった物は 0。
- 最初の実行（試験を足す前）の生き残りのうち、実際に見逃していた物を試験で塞いだ（再実行で全部 KILLED）: 5 つの list がちょうど 16 個の受理（`>`→`>=` の 7 か所）、別の plan での take_slot の setup_slot の書き込み、32 slot 全部 busy（slot の loop の `<=`）、範囲外の slot を持つ entry（`&&`→`||`・`<=`、shift の UB）。
- 生き残り 40 か所は全部、観測できる振る舞いが変わらない壊し（等価）と判断した: (1) 防御の重複の guard（slots が init で 16〜32 に clamp 済みの所の再 clamp・`slot >= MAX_SLOTS`・output の上限 17 の guard・reference_count の上限の guard）、(2) 空の entry は全部 0 という不変から効かない `if (used == 0) continue;` の削除（11 か所）と `needed = 0`・`latency = 0U` の削除（空の entry に書く初期化）と memset 済みの plan field の再初期化、(3) 自分自身との比較で効かない tie 判定の `continue` の削除と `<=`（id は呼び手が一意にする）と、一致が 2 つで id が等しい時の `>=`、(4) latency_limited 0 の時の max_latency（読まれない）、(5) 最初の `break`（どの空の entry でもよい）、(6) 読み手が見ない Curr list の -1 の初期化（`i < num` の範囲しか読まない）、(7) IRAP の flush の `dpb_remove_unneeded` と最後の空にする loop（全部 unused なので bump が空にし、結果が同じ）、(8) begin の `current = H265_DPB_NO_SLOT`（in_picture 0 なら元から NO_SLOT）。

### 自分で決めた点（header が黙っている所、または解釈）

- begin: 引数が NULL、sub-layer が 7 以上（HighestTid が配列の外）、log2_max_poc_lsb が 4〜16 の外、いずれかの list の個数が H265SW_MAX_RPS（16）超は EINVAL（何も変えず、plan にも触れない）。plan に触れるのは、検べを通った後（memset で 0 にしてから）。
- begin の二重呼び・size 超の EINVAL では plan に触れない（二重呼びで進行中の plan を壊さないため）。ステップ 3 の失敗では plan を全部（Curr の slot・missing・reference・outputs）書き、`started` は変えない（header は「成功の時に 1」）。plan.reset はその時の `started == 0`。
- discarded は、NoOutputOfPriorPicsFlag 1 で捨てた絵のうち「出力待ちだった絵」の数（出力済みで参照だけの絵は数えない）。header は「出力無しで空にした絵」と書いて、どちらとも取れる。
- reference_slot などの並びは slot の昇順。
- LT・ST の照合で、同じ POC の絵が複数ある時（壊れた stream）は id の大きい物を選び、すでに選んだ絵は選ばない（ST も同じ。ST の候補は short-term の参照だけ）。bumping と evict の同じ POC は id の小さい方。
- take_slot: dpb->slots が 32 を超える状態でも 32 で止める（防御）。end・flush は `count` が NULL なら EINVAL（`output` が NULL も EINVAL）。flush の `*count` は EINVAL でも 0。
- bump の出力の slot が範囲外（手で壊した状態）の時は busy を触らず、そのまま報告する。
- 8.3.2 の「全ての参照を unused」は irap_no_rasl 1 の時、picture 0 でも行う（空なので無害）。C.5.2.2 の IRAP の枝は started 1 の時だけ。picture 0 は「それ以外」の枝（空の buffer なので何も起きない）。

### 未実施

- QEMU・実機（この Phase の範囲外）。規約の全文の見直し（p015）。coding-style の checklist で自分の diff は見直した（関数を条件に使わない、関数の先頭の宣言、段落の comment、forward 宣言、成功の return の comment、for の本体が if の時の brace）。試験の .c は host の C89 の試験で、一部に条件演算子と compound の if 条件（model の中）を使っている。
- 表の値（Table）を持つ module ではないので、値の照合の節は無い。

### 消す物（Q1 へ）

- `/tmp/claude-1000/-home-awe-zedBSD-claude1/b57eaf0e-d1bd-5084-9fc6-6409b527934c/scratchpad/p006e-mut/`（mutant の作業 directory。obj・w0〜w3・out.txt・results.json）。旧 `scratchpad/mut/` は Q1 が消した物（途中の run はそれで止まり、`p006e-mut` で全部やり直した）。
- worktree の `build/tmp/h265-unit/`・`build/tmp/h265-skeleton/`（run-unit・check-skeleton の出力）。

### header について（変えていない、規約の見直しの Phase へ）

- `h265-dpb.h` の `discarded` の定義（上の決め）、ステップ 3 失敗時の `started`、reference_* の並び（slot の昇順）を header の comment に書くと明確になる。

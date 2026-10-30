<!-- awesome-plan project=zedbsd record=ws329-design -->

# WS329 libmedia の H.264 の software の encoder の設計（p001）

- 親: [ws.md](ws.md)。作成: ws329-p001（P5、q1758-i01、2026-10-31）。調べた tree: main 85fbeb091（worktree `p5-ws329`）。
- 正本: 各 module の仕様は [proposed/](proposed/) の header の comment（p003 で source に置き、以後の実装の Phase の基点にする）。この文書は全体の地図・決定・共通化・試験の道・Phase の割り振り。
- ユーザーの判断を待つ点は [user-questions.md](user-questions.md)（Q329-1〜5）。敵対的レビューと反映は [design-review.md](design-review.md)。
- HAL（`include/hal/hal.h`）は関係しない（§12）。toolchain も変えない。

## 1. 出典と引用

- 仕様: **ITU-T H.264 (08/2021)**（WS326 と同じ版）。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22（例: 9.3.4.5 は印刷 282 頁 = PDF 304 頁）。
  encoder が読む節: 7.3〜7.4（構文と意味）、7.4.2.10（cabac_zero_word の制約）、9.1・9.2（Exp-Golomb、CAVLC）、9.3.1〜9.3.3（CABAC の初期化・2 値化・context）、**9.3.4（CABAC の encoder、informative）**、8.2（POC・参照）、8.3〜8.5・8.7（予測・変換・deblocking、再構成は decoder の物を使う）、Annex A（profile・level、表 A-1・A-4、A.3.1 n)・A.3.2 j) の 1 macroblock の bit の上限、A.3.1 c)・d) の access unit の大きさ、A.2.1・A.2.2 の PCM の標本の制約）、Annex B、Annex E（VUI）。
- 仕様に無い encoder の判断（mode の選び、動きの探索、量子化の丸め、rate control）は、出典を header の該当の block に書いた公開の文献の考えを、数値まで header で固定した（WS318 D20 の AAC と同じ扱い）:
  - Lagrange 乗数 λ_mode = 0.85·2^((QP−12)/3)、λ_motion = √λ_mode: T. Wiegand ほか, IEEE TCSVT 13(7), 2003。
  - diamond の探索の点の形: S. Zhu, K.-K. Ma, IEEE TIP 9(2), 2000。
  - picture 単位の目標の配分: MPEG-2 Test Model 5（MPEG93/457）の step 1。
  - 前向きの変換・量子化の係数 MF は、decoder の normAdjust（式 8-315・8-318）と逆変換の基底から式で導いた（§5.4）。他の実装の表は写さない。
- 他の実装（x264・openh264・JM・FFmpeg など）の code・comment・構造・名前を写さない（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md) §7、[guardrail](../guardrail.md) の出典）。試験で host の ffmpeg（libavcodec の H.264 decoder、libx264）を**副の oracle と比べる物差し**として使うだけ（§10）。
- 表の値は block ごとに「Values referenced: ITU-T H.264 (08/2021) Table …」、文献の手順は該当の struct・関数の直前の block に「Procedure referenced: …」（guardrail の書式、複数行の comment）。file の先頭の comment には出典を書かない。λ のように式で計算する値は、式を comment に書き、計算は実行の時に行う（表を写さない）。source の comment に WS・Phase の番号・plan/ への参照・日付を書かない（proposed の header もそう書いてある）。

## 2. 範囲

| 項目 | 範囲 | 範囲の外（理由） |
| --- | --- | --- |
| 形式 | progressive の frame、8 bit、4:2:0、幅と高さは偶数で 16〜16880（H264SW の上限と同じ）、MB の倍数でない大きさは SPS の cropping | interlace・4:2:2 以上・10 bit（WS326 の decoder と同じ範囲） |
| profile | Constrained Baseline（66、constraint_set0/1）、Main（77）、High（100）。既定は High | Baseline の FMO・ASO・冗長、Extended、High 10 以上 |
| slice | I・P・B（B は Main と High、参照にしない B、最大 3 枚続き）、1 picture に 1〜16 slice（MB の行の単位） | B を参照にする pyramid、SP・SI |
| entropy | CAVLC（全 profile）、CABAC（Main・High） | — |
| 予測 | Intra_4x4・Intra_8x8（High）・Intra_16x16・chroma、inter 16x16・16x8・8x16・8x8、P_Skip・B_Skip・spatial direct、1/4 画素、P の参照 1〜4 枚 | 8x8 より小さい分割（A.3.3 の MaxMvsPer2Mb・MinLumaBiPredSize を構造で満たす）、temporal direct、weighted prediction、long-term・MMCO |
| 変換 | 4x4、8x8（High）、平らな scaling list | scaling matrix の送出、trellis の量子化 |
| その他 | I_PCM は 1 MB の bit の上限（3200）を超える時だけ、VUI（SAR・色・timing・bitstream_restriction）、CBR の filler | HRD の parameter と buffering period の SEI、SEI 全般 |
| rate control | CQP・CBR・VBR、picture 単位の QP | MB 単位の適応の量子化（AQ）、2 pass、lookahead |

範囲の外の config は `h264enc_create` が ENOTSUP（libmedia では MEDIA_PROBLEM_CONFIG、次の実装へ回すのは WS318 の encoder の層の規則 D15 のとおり open の時だけ）。

## 3. 決定（根拠と、選ばなかった案）

| # | 決定 | 根拠 | 選ばなかった案と理由 |
| --- | --- | --- | --- |
| E1 | **encoder は decoder の状態を鏡のように持つ**: picture・slice の context、`struct h264sw_mb` の配列、DPB、参照の list、frame の pool を decoder の構造体のまま持ち、予測の値（MV の予測、intra の予測 mode、CAVLC の nC、CABAC の context）を decoder の関数で導く | ユーザーの決定（decoder の実装を共通の関数にして再利用）。同じ関数なので encoder と decoder の導出が食い違えない。単体試験が「書いて decoder で読む」往復で書ける | encoder 専用の状態と導出（同じ規則を 2 度書き、ずれると壊れた stream になる） |
| E2 | **参照画は decoder の再構成だけで作る**: MB ごとに `h264sw_recon_mb` を 2 回（予測を得る 1 回目、levels を入れた 2 回目）、picture の後に `h264sw_deblock_picture` | 適合の試験を通った decoder と同じ画素になり、drift が構造的に起きない。再構成の code を 2 つ持たない | encoder の独自の再構成（速いが、一致を試験で追い続ける必要）。速さは p025 で、一致の試験を残したまま近道を足す |
| E3 | SPS・PPS は書いた後に decoder の reader（`h264sw_ps_read_sps/pps`）で読み戻し、その構造体を使う | encoder の思い込みと decoder が読む値が別にならない | encoder が構造体を直に埋める（真実が 2 つ） |
| E4 | 書き側の新しい module のうち codec に依らない物は `vcodec-` の名前（bit の書き、NAL と emulation prevention、CABAC の算術の encoder） | H.265 の encoder（WS330）が同じ物を使う。WS326 の決定 3（共通部分は汎用の名前） | `h264enc-` に置いて WS330 で移す（移しの手間と試験の 2 度手間） |
| E5 | H.264 に固有で encoder と decoder が共に使う物は `h264sw_` の名前のまま共通にする（CAVLC の表、CABAC の context の選び、表 A-1、context の初期化）。encoder の module は `h264enc-` | decoder の名前と試験を壊さない最小の移し。`h264sw` は「software の H.264」で decoder 専用の意味ではない（header の文の "decoder" は p002 で "codec" に） | 共通部分を `h264-` に全部改名（decoder の 25 file の改名、WS326 の試験の差分が大きい） |
| E6 | 前向きの変換は整数の厳密な core 変換、量子化の係数 MF は decoder の normAdjust から式で計算（§5.4）、丸めは intra 1/3・inter 1/6 | decoder の逆変換と組で誤差が量子化だけになる（§5.4 で数値で確かめた）。表を写さず式なので照合が簡単 | 文献の MF の表を写す（出典の扱いと照合の手間）、RDOQ・trellis（遅い、最初の範囲の外） |
| E7 | scaling matrix を送らない（全部 Flat_16） | MF が式 1 つで済む。画質への効きは小さい | 送る（量子化が list ごとに変わり試験が増える） |
| E8 | 動きの探索は参照ごとに 1/2 画素の 3 面（b・h・j）を前もって作り（decoder の補間で）、1/4 画素は Table 8-12 の平均で作る | 探索の中で 6 tap の補間を候補ごとにすると遅すぎる（§9 の見積もり）。平均は仕様の式そのもので、decoder と同じ値になる | 候補ごとに `h264sw_interp_luma`（1 MB あたり数十 k 回の演算増） |
| E9 | mode の選びは SATD + λ_motion·bits（bits は CAVLC の長さで近似、CABAC でも同じ）。inter の 4x4・8x8 の変換の選びだけ SSD + λ_mode·bits | 速さと画質の釣り合い。決まった式で再現でき、Sonnet が header から書ける | 全 mode の RDO（遅い。QUALITY でも入れない。画質の目標に届かなければ p023 で足す） |
| E10 | 速さの preset は FAST・BALANCED・QUALITY の 3 つで、各 parameter を表で固定（h264enc-mode.h） | 実時間（camera の録画）と画質の両方の使い手。WS318 の config に速さの欄が無いので Q329-5 で問う | 1 つだけ（camera の実時間と file の画質の両立ができない） |
| E11 | B は参照にしない（nal_ref_idc 0）、direct は spatial、L1 は次の anchor の 1 枚、max_num_reorder_frames 1 | 構造が単純で DPB と遅れが小さい。spatial は co-located の POC の縮尺が要らず速い | B-pyramid（参照の管理と MMCO が要る）、temporal direct |
| E12 | IDR だけを key に使う（非 IDR の I は使わない）。B の群が IDR で切れる時は最後の待ちを P にする | open GOP の leading picture を作らない。seek と回復が単純 | open GOP（圧縮は少し良いが decoder の leading の扱いに頼る） |
| E13 | POC は type 0（log2_max_poc_lsb 8）、frame_num は log2 8、gaps 無し、sliding window | B の並べ替えに type 0 が要る。1 つの道で済む | B が無い時 type 2（数 bit 少ないが道が 2 つ） |
| E14 | rate control は picture 単位の QP: CQP、TM5 の step 1 の目標の配分＋ Qstep の model ＋ 1 秒の buffer の確かめ、CBR は filler data | 決定的で試験できる（合成の model で収束を確かめる）。MB 単位の適応は PSNR を下げ、試験も重い | MB 単位の TM5 step 2・3（mb_qp_delta の bit と複雑さ）、x264 風の ABR（他の実装の構造に寄る） |
| E15 | 1 MB の bit が A.3.1 n)・A.3.2 j) の 3200 を超えたら書き直して I_PCM（Constrained Baseline と Main では PCM の標本 0 を 1 に、A.2.1・A.2.2）、CAVLC で High 以外は level を ±2063 に clamp（level_prefix ≤ 15、A.2.1〜A.2.3）、CABAC は 9.3.4.6 の cabac_zero_word、access unit が A.3.1 c)・d) の上限を超えたら QP + 6 で符号化し直す（3 回まで、超えた数を数える） | どれも適合の要件。低い QP（0 付近）と CQP で実際に起きる。自前の decoder も ffmpeg も調べないので、encoder が自分で数え試験が確かめる（stats） | QP の下限で避ける（ユーザーの CQP 0 を断ることになる）、CQP で高い level を選ぶ（level の意味が崩れる） |
| E19 | skip の kind（P_Skip・B_Skip）は残差を作らない。P 16x16（ref 0、vector が pskip と同じ）と B_Direct_16x16 は cbp 0 なら skip に変える | 残差を持つ skip は書けない（syntax が無い）。逆の変換を決めないと実装で分かれる（review 3） | skip の決定に残差があれば P 16x16 に変える（mvd の導出が pskip と mvpred で違い、規則が増える） |
| E20 | 試験の口: `h264enc_set_recon_hook`（再構成の frame を符号化の順に）と `h264enc_get_stats`（PCM・MB の最大の bit・zero word・AU の符号化し直し・MV の最大・clamp・underflow）を `h264enc-encoder.h` に | header は p003 で凍結するので今決める。decoder が調べない制約（PCM の 0、MB の bit、AU の大きさ、MV の範囲）を試験が数で確かめる（review 6・7） | encoder の内部を試験が直に読む（構造に依存） |
| E16 | 出力は Annex B の access unit（4 byte の start code、IDR の前に SPS・PPS）。長さの前置（avcC）への変換は WS318 の `packet-format.c` | WS318 §5.1 の分担。encoder は形式を 1 つだけ持つ | encoder が両方を出す（WS318 と重複） |
| E17 | 入力は I420・NV12 を CPU で受け、MB の倍数へ端を繰り返して詰める（copy 1 回）。WS318 の `input` には I420 と答える | B の待ちのために保持が要り copy は避けられない。端の繰り返しは padding の bit が少ない | caller の frame の参照を持つ（B の待ちの間 camera の slot を塞ぐ） |
| E18 | 最初は 1 thread。slice ごとの状態（slice context・writer・MB context）を分けて持ち、p025 で WS318 の `thread.c` の pool で slice を並べる | 正しさが先（WS326 と同じ順）。構造は並列の形にしておく | 最初から並列（試験と debug が難しい） |

## 4. decoder の module の洗い出し（encoder でも使える物）

WS326 の各 module（main 85fbeb091 の header を読んだ事実）と、encoder での使い方・必要な変更。「変更」は p002（共通化の refactor）で行い、decoder の単体試験と JVT の適合の試験が前後で同じであることで確かめる。

| decoder の module | encoder での使い方 | 変更 |
| --- | --- | --- |
| h264sw.h（上限・誤りの約束・算術の規則） | そのまま使う（h264enc.h が参照） | 先頭の文の "decoder" を "codec" に（p002） |
| h264sw-bits（RBSP の reader） | 単体試験で書いた物を読み戻す | 無し（WS326 p026 が汎用の名前に移すなら、その名前を使う） |
| h264sw-nal（Annex B の分割・emulation の除去） | 試験で読み戻す | 無し（同上） |
| h264sw-tables（zig-zag・既定の list・QPC・normAdjust） | 量子化の scan、MF の計算、QP'C | 無し |
| h264sw-ps（SPS・PPS の reader と store、表 A-1 の MaxDpbMbs） | 書いた SPS・PPS の読み戻し（E3）、`struct h264sw_sps/pps` | 表 A-1 を `h264sw-levels`（全列）へ出し ps.c はそれを使う（p002、[proposed/shared/h264sw-levels.h](proposed/shared/h264sw-levels.h)） |
| h264sw-slice（slice header の構造体と reader、新しい picture の判定） | encoder が埋める header の型、試験の読み戻し | 無し |
| h264sw-frame（frame と pool） | 再構成の frame と参照 | 無し |
| h264sw-poc（POC） | DPB の状態の更新（`h264sw_poc_finish` は使わず、gop が値を決める） | 無し |
| h264sw-dpb（marking・格納・bumping） | 参照の管理（sliding window）をそのまま | 無し |
| h264sw-reflist（PicNum・初期の list） | slice の参照の list | 無し |
| h264sw-mb（MB の entry・mb_type の表・neighbour） | entry を decoder と同じに保つ、neighbour、mb_type の意味 | 無し |
| h264sw-context（picture・slice の context、MB の syntax） | encoder の MB の syntax も同じ構造体 | 無し |
| h264sw-cavlc（読み、nC、cbp の表） | nC（`h264sw_cavlc_nc`）、符号の表 | 表（9-5・9-7〜9-10・9-4）を `h264sw-cavlc-tables` に出す（p002、[proposed/shared/h264sw-cavlc-tables.h](proposed/shared/h264sw-cavlc-tables.h)） |
| h264sw-cabac-init（(m, n) の表） | context の初期化 | 無し |
| h264sw-cabac（算術の decoder、表 9-44・9-45） | 表を encoder の engine が使う、試験で読み戻す | context の初期化を `h264sw_cabac_contexts_init` に分ける、表の宣言を header に（既に非 static）（p002） |
| h264sw-cabac-se（2 値化と ctxIdxInc） | ctxIdxInc の導出 | neighbour に依る ctxIdxInc と residual の ctxIdx の導出を `h264sw-cabac-ctx` に出し、cabac-se.c はそれを呼ぶ（p002、[proposed/shared/h264sw-cabac-ctx.h](proposed/shared/h264sw-cabac-ctx.h)） |
| h264sw-macroblock（slice_data・MB の解析） | 試験の oracle（MB を書いて読み戻す） | 無し |
| h264sw-transform（逆量子化・逆変換） | 再構成（intra の block の逐次と recon_mb の中）、MF の照合 | 無し |
| h264sw-intra（予測・端・予測 mode） | intra の mode の選び | 無し |
| h264sw-interp・h264sw-weight | 1/2 画素の面の作成、再構成 | 無し |
| h264sw-mvpred・h264sw-direct | mvd の計算、P_Skip、B の direct | 無し |
| h264sw-recon | 再構成の全部（E2） | 無し |
| h264sw-deblock | picture の deblocking | 無し |
| h264sw-decoder・h264sw-backend | 全体の試験の oracle（encoder の出力を decode） | 無し |
| bits.h（`media_bits`、`media_rbsp_unescape`） | 使わない | 無し |
| 試験の道具（`plan/ws326/tests/unit/check.h`・`bitwriter.h`・`cabacwriter.h`） | `check.h` は WS329 の試験も使う。`bitwriter.h`・`cabacwriter.h` は decoder の試験の独立の逆実装なので、製品の writer にはしない（同じ誤りの共有を避ける） | 無し |

decoder の module で encoder に要らない物は無い（全部が「使う」か「oracle」）。

## 5. 共通化の設計（WS326 p026 との順と名前）

### 5.1 順

1. WS326 の p022（適合）→ **p026（codec に依らない名前への移し）** → p023〜p025 → WS326 completed（header の凍結 `BASE=fd1d6f88c` が外れる）。
2. **WS329 p002**: H.264 の encoder と decoder の共通化（§4 の「変更」の 4 つ）。WS326 p026 の名前の上に足す。
3. WS329 p003: encoder の header（proposed/）と骨格を source に置き、WS329 の基点 BASE を決める。

p026 と p002 を 1 つにしないのは、WS326 の範囲（H.265 と共通の名前）に encoder の目的を足さないため（AGENTS §2）。p002 の差分は p026 の後の tree に当てる。

### 5.2 名前（WS326 p026 への提案）

- codec に依らない物は `vcodec-<名>.h`、関数 `vcodec_`、定数 `VCODEC_` を推す（Q1 経由で WS326 p026 の設計に渡す）。WS329 が新しく作る `vcodec-bitwriter`・`vcodec-nalwriter`・`vcodec-cabac-enc` はこの形。p026 が decoder の reader 側を `vcodec-bits`・`vcodec-nal`・`vcodec-cabac`（算術の engine と表 9-44・9-45）に移せば、読み書きが揃う。
- p026 が別の名前を選んだら、WS329 p003 が proposed/ の header の名前（`h264sw_cabac_range_lps` などの参照）を機械的に置き換える。proposed の内容（関数の仕様）は変わらない。
- H.264 に固有の共通部分は `h264sw_` のまま（E5）。

### 5.3 p002 の変更（decoder の振る舞いを変えない refactor）

| 変更 | 内容 | 確かめ |
| --- | --- | --- |
| R1 CAVLC の表 | `h264sw-cavlc.c` の static の表を `h264sw-cavlc-tables.c` へ移し、名前と形を proposed のとおりに（coeff_token の 4 列を [4][62] に、cbp を [2][48] に）。値と順は変えない | test-cavlc の全項目、適合 |
| R2 CABAC の context の選び | `h264sw-cabac-se.c` の `se_mb_type_i_cond`・`se_mb_type_b_cond`・`se_ref_idx_cond`・`se_mvd_abs`・`se_cbp_chroma_cond`・`se_coded_block_flag_inc`・`se_significance_inc` と mb_skip・transform_8x8・chroma・qp_delta・cbp luma の inc の計算を `h264sw-cabac-ctx.c` の公開の関数へ。cabac-se.c はそれを呼ぶ | test-cabac_se、適合、新しい test-cabac_ctx（各関数を手の例で） |
| R3 context の初期化 | `h264sw_cabac_start` の context の部分を `h264sw_cabac_contexts_init` に。表 9-44・9-45（既に非 static の `h264sw_cabac_range_lps`・`_trans_lps`・`_trans_mps`）を header で宣言（p026 が vcodec に移していればその名前） | test-cabac、適合 |
| R4 表 A-1 | `h264sw-ps.c` の `ps_level_limits` を `h264sw-levels.c` の全列の表へ、`h264sw_level_find` | test-ps、新しい test-levels（全行を PDF の画で照合） |
| R5 文 | 共通の header の先頭の "of the software H.264 decoder" を "codec" に（意味は変えない） | check-skeleton |

R1〜R5 は decoder の header を変えるので、WS326 の `check-skeleton.sh` の BASE はこの Phase の commit に移る（Q1 が WS326 の記録に書く）。

### 5.4 前向きの変換と量子化の導出（header の式の根拠）

- 4x4: 前向き Cf と decoder の逆 Ci（8.5.12.2）の積 Cf·Ci = diag(4, 5, 4, 5)。平らな list（16）で LevelScale4x4 = 16·v、逆量子化は c·v·2^(qP/6)。よって MF4(m, i, j) = Round(2^21 / (v(m, k)·s_i·s_j))、qbits = 15 + qP/6。MF4(0, ·) = 13107・5243・8066（文献の値と一致、写していない）。
- 8x8: 前向き Tf を逆の基底の 8 倍の転置にすると Tf·Ti = diag(E)/4、E = (256, 289, 160, 289, …)。MF8 = Round(2^32 / (v8·E_i·E_j))、qbits = 20 + qP/6。
- DC: Intra16x16 は 16 個の W00 に H を両側から掛け 1/2、2x2 の chroma DC は H2 を両側から。どちらも MF4(m, 0, 0) と qbits + 1（decoder の 8.5.10・8.5.11 と組）。
- 確かめ（p001、python の scratch、repo に入れない）: 乱数の残差 200 個で、qP 0 の再構成の誤差が 4x4・8x8 とも最大 1、luma DC・chroma DC は 0（量子化の他に誤差が無い）。qP が上がると誤差は量子化の幅で増える（qP 40 で約 80）。p008 の単体試験が同じ性質を decoder の関数で確かめる。

## 6. encoder の module

全部 `userland/desktop/libmedia/`。各 header の comment が仕様（入力・出力・前提・誤り・節）。案は [proposed/](proposed/)。

| module（.h/.c） | 内容 | 使う物 |
| --- | --- | --- |
| vcodec-bitwriter | u・ue・se・te・整列・trailing・mark/rewind・長さ | — |
| vcodec-nalwriter | emulation prevention の挿入、Annex B の access unit、cabac_zero_word | — |
| vcodec-cabac-enc | 9.3.4 の算術の encoder、bin の数 | bitwriter、h264sw-cabac の表 |
| h264enc.h | 上限・約束・公開の核の API（create・send・receive・parameter_sets・destroy） | — |
| h264enc-ps | sequence の選び、level の選び（表 A-1）、SPS・PPS・VUI の書き | bitwriter、h264sw-levels |
| h264enc-slice | NAL の header、slice header の書き、cabac_zero_word の数、filler | bitwriter、h264sw-slice・ps |
| h264enc-quant | 前向きの変換、量子化、DC、clamp | h264sw-tables |
| h264enc-cavlc | residual_block_cavlc の書き、bit 数、cbp の codeNum | bitwriter、h264sw-cavlc-tables |
| h264enc-cabac-se | MB の構文要素の CABAC の書き、PCM | cabac-enc、h264sw-cabac-ctx |
| h264enc-cabac-res | residual_block_cabac の書き | cabac-se、h264sw-cabac-ctx |
| h264enc-cost | SAD・SATD・SSD・λ・mvd の bit | bitwriter（長さ） |
| h264enc-source | 入力の copy と端の詰め | — |
| h264enc-refplanes | 参照の 1/2 画素の 3 面と 1/4 画素の予測 | h264sw-interp、h264sw-frame |
| h264enc-me | 動きの探索（候補・大小の diamond・1/2・1/4） | refplanes、cost |
| h264enc-mbctx.h | preset・MB の context・MB の決定（構造体だけ） | — |
| h264enc-intra | intra の mode の選び（16x16・4x4・8x8・chroma） | h264sw-intra・transform、quant、cost |
| h264enc-mb | MB の符号化（entry の kind・qp を先に、2 回の再構成、残差、cbp、skip、entropy ごとの entry の field、PCM） | h264sw-recon・mvpred・direct・mb、quant、cavlc |
| h264enc-mbwrite | slice_data と macroblock_layer の書き（CAVLC・CABAC）、mark/rewind | cavlc、cabac-se・res |
| h264enc-mode | preset の表、I と P の決定 | me、intra、mb、cost |
| h264enc-mode-b | B の決定 | 同上、h264sw-direct |
| h264enc-ratecontrol | CQP・CBR・VBR | — |
| h264enc-gop | picture の種類と順、frame_num・POC・時刻 | — |
| h264enc-encoder | 全体の流れ（p022、Opus） | 全部 |
| h264enc-backend.c | libmedia の encoder の ops（p024、Opus、WS318 p012a の後） | encoder、WS318 の encoder の層 |

依存の順（実装の Phase の順）: bitwriter・nalwriter → cabac-enc → ps → slice、quant、cavlc、cabac-se → cabac-res、cost・source、refplanes → me、intra、mbwrite → mb → mode → mode-b、ratecontrol、gop → encoder → 品質と適合 → backend → 速さ → 規約。

### 6.1 1 つの picture の流れ

`h264enc-encoder.h` の comment が正。要点: gop が種類と順を決め → rate control の QP → slice header → frame と picture の context（decoder の手順 4 と同じ）→ slice ごとに list（`h264sw_reflist_build`）・direct の準備・CABAC の開始 → MB ごとに決定 → mark → 符号化（2 回の再構成）→ 書き（ERANGE なら clamp して書き直し、3200 bit を超えたら rewind して I_PCM）→ slice の終わり・NAL → deblocking・motion の写し・DPB の marking と格納 → 参照の面の作成 → rate control の更新・filler → packet。

## 7. 所有と寿命

- `struct h264enc` が全部を持つ: config、sequence と SPS・PPS の NAL、読み戻しの store、frame の pool、DPB と出力の queue、MB の entry・slice の parameter（sequence の大きさで確保）、slice の状態（slice 数）、入力の slot（b_frames + 2）、参照の面（max_num_ref_frames + 1）、表（quant・λ・cavlc）、rate control、gop、packet の ring。
- frame は参照の数で生きる（今の picture・DPB）。DPB の bumping が出した frame は encoder が即座に unref（encoder は frame を外に出さない）。
- 参照の面は frame の id で引き、DPB から外れた frame の面を次の参照の作成に使い回す。
- packet の bytes は次の `h264enc_receive` か `h264enc_destroy` まで encoder の物。
- 入力の picture は `h264enc_send` の中で copy され、caller は直後に再利用してよい（WS318 の「encoder が自分の参照を持ち、CPU が読んだら落とす」を満たす）。

## 8. 並行性・誤り

- global の状態を持たない（表は const の data か encoder の中）。1 つの encoder は 1 thread から呼ぶ。2 つの encoder は別の thread で lock 無しに動く。
- slice の状態を分けて持つので、p025 で slice を WS318 の `thread.c` の pool（名前の付いた thread、本数を決める、WS309 の方針）に並べられる。deblocking と DPB は全 slice の後に 1 thread で。
- 誤り: 範囲外の config は create で EINVAL・ENOTSUP。send・receive の中の失敗（ENOMEM、内部の矛盾 = bug の EINVAL）は encoder を「失敗」の状態にし、以後の呼び出しは同じ値を返す（壊れた stream を出し続けない）。libmedia の層の写しは WS318 §9（EIO で stream を終える）。
- 信用できない入力は無い（入力は画素だけ）。ただし画素の値が何でも、levels・mvd・MB の bit が仕様の範囲を出ないことを各 module が保証する（clamp、I_PCM、cabac_zero_word）。

## 9. 速さと画質の目標（Q329-1・Q329-2 で確かめる）

見積もり（scalar C、SIMD 無し、1 thread、5330 の Alder Lake の P core を 3 G 演算/秒として）:

| preset | 1 MB の演算の目安 | 720p（3600 MB） | 目標（5330） |
| --- | --- | --- | --- |
| FAST | 30〜50 k（16x16 だけ、参照 1、小さな diamond、1/4 画素は面から） | 約 20 fps（1 thread） | 720p30 を slice の 2〜4 thread で実時間、640x480 30 fps を 1 thread |
| BALANCED | 150〜200 k（分割・参照 2・intra 全部） | 約 5 fps | 720p で 1 thread 5 fps 以上（file の書き出し向け） |
| QUALITY | 400 k 以上 | 約 2 fps | 測るだけ |

- 1080p30 の実時間の software の encode は SIMD 無しでは目標にしない（p025 で測る。Pi 5 は FAST で 480p30 を目安）。
- 画質（PSNR-Y、同じ bitrate、host の試料 4 本）: BALANCED が libx264 の `-preset veryfast -tune psnr`（同じ profile と B の数）の BD-rate +40% 以内、FAST が +80% 以内。x264 は物差しで、code・設定の中身は参考にしない。
- 正しさの門（全 preset・全 profile・全 entropy）: 出力が host の ffmpeg（厳しい誤りの検出の設定）と自前の decoder の両方で誤り無く decode でき、復号の frame が encoder の再構成と全 frame で MD5 一致。

## 10. 試験の道

### 10.1 単体試験（host、各実装の Phase）

- 置き場 `plan/ws329/tests/unit/test-<name>.c`、流し方 `sh plan/ws329/tests/run-unit.sh <name>...|all`（WS326 の形: 全部の h264enc-*.c・vcodec-*.c と全部の h264sw-*.c（backend を除く）と bits.c を ASan・UBSan で link、最後の行 `run-unit: PASS`）。`check.h` は WS326 の物を include path で使う。p003 で作る。
- **主の oracle は decoder の module**（書いて読む往復）: bitwriter → `h264sw_bits`、SPS・PPS → `h264sw_ps_read_*`、slice header → `h264sw_slice_read`、CAVLC → `h264sw_cavlc_residual_block`、CABAC の engine → `h264sw_cabac_*`、CABAC の構文要素と residual → `h264sw_cabac_*` と `h264sw_cabac_residual_block`、MB の書き → `h264sw_macroblock_parse` で syntax と entry が一致、再構成 → `h264sw_recon_mb`、1/2 画素の面 → `h264sw_interp_luma` と全 16 の分数で一致。decoder は JVT の適合を通った物なので oracle にしてよい。共有の表（R1）は decoder の適合で確かめ済み、encoder 側だけの表（mb_type の bin 列）は往復で確かめる。
- 仕様に無い module（cost・me・intra・mode・ratecontrol・gop）は性質の試験: 合成の画（平行移動・勾配・平ら・雑音）で既知の答え（ずらした量の MV、明らかな intra mode、skip）、決定の再現（同じ入力で同じ結果）、範囲（MV の窓・level の上限）、rate control は合成の model（bits = X/Qstep）で目標の ±5% に 300 frame で収束し buffer が空にならない。
- 共通: 誤りの入力で落ちない、ASan・UBSan 0、わざと壊す確かめ（標準廉価実装手順 §5.3）。

### 10.2 全体の試験（p022・p023、host）

- host の道具 `plan/ws329/tests/tool/h264enc-tool.c`（p022 で作る）: raw の I420 を stdin から読み、`.264` を書き、encoder の再構成の各 frame の MD5（`h264enc_set_recon_hook`、crop した可視の範囲を表示の順に）と、source との PSNR を出す。`h264enc_get_stats` で decoder が調べない制約（MB の bit ≤ 3200、AU の大きさ、MV の範囲、PCM の 0）を確かめる。
- 往復の試験の限り: encoder の再構成 = decoder の再構成なので、意図と違う syntax（予測 mode・MV の誤り）は MD5 では見えない。p017〜p019 の単体試験で「decode した mode・MV・ref が決定と一致」を必ず比べる。
- `plan/ws329/tests/roundtrip.sh`: host の ffmpeg の lavfi（`testsrc2`・`mandelbrot`・`smptehdbars`・既存の試料 `plan/ws083/tests/streams/*.h264` を decode した自然画）を pipe で渡し（大きな YUV の file を作らない）、出力を (a) `ffmpeg -err_detect +crccheck+bitstream+buffer+explode -f framemd5`、(b) WS326 p022 の host の decoder の道具（`conform` と同じ核）で decode、(c) encoder の再構成の MD5 と 3 者が全 frame で一致することを確かめる。最後の行 `roundtrip: PASS`。
- 組み合わせ: profile 3 × entropy 2 × B 0/2 × slice 1/3 × preset 3 × 大きさ（176x144、360x240（crop）、1280x720 の短い物）× rate control 3、forced key、drain、QP 0・51。
- 画質: 同じ試料を libx264（host の ffmpeg）で同じ bitrate の 4 点に encode して BD-rate を出す（§9 の目標）。
- 容量: 出力は `build/tmp/ws329-*` の新しい directory（`fresh_out`）、1 回の全体で数十 MB 以内。消すのは Q1。
- JM の参照 decoder（HHI の配布）を厳しい oracle に足すかは Q1 の判断（§14）。

### 10.3 libmedia・QEMU・実機

- p024: WS318 の `media_encoder_*` の host の試験（WS318 p012a の試験の道具）で、codec の問い合わせの行・CONFIG の断り・往復。
- QEMU（Venus の amd64）: software の encoder は GPU を使わないので、image の中の短い encode の program（WS318 の camera の後の録画の道、または `media-probe` の encode）を p024 で決める。console の log で判定しない（AGENTS §9）。
- 実機: 速さ（p025）は Q1 が 5330・Pi 5 を割り当てる。

## 11. WS318 の encoder の口への載せ方

- WS318 §5.3 の encoder の backend の ops（kind・query・open・input・send_frame・send_sound・receive・codec_data・close）に `h264enc-backend.c` で載せる。表（`encoder.c`）の位置は `{ vkvideo_encode_h264, vkvideo_encode_h265, h264-software, aac_lc, avcodec }`（hardware の後、libavcodec の前。ユーザーの backend の順の決定に合わせる）。
- query の行: kind SOFTWARE、名前 "h264-software"、profiles CB・Main・High、max_level 62、最大の大きさ 16880、rate_controls CQP・CBR・VBR、max_b_frames 3、max_references 4、memory CPU、formats I420・NV12。
- open: `media_encoder_config` → `h264enc_config`（profile・level・rate・bitrate・qp・key_interval・b_frames・colour の写し、speed は Q329-5）。範囲の外は MEDIA_PROBLEM_CONFIG。
- send_frame: encoder の層が CPU の I420/NV12 に揃えた frame の plane を `h264enc_send` へ（copy は encoder の中、frame の参照はすぐ落とせる）。FORCE_KEY を写す。EAGAIN はそのまま。
- receive: `h264enc_receive` の packet（Annex B）を、config の output が RAW なら WS318 の `packet-format.c` で長さの前置に。KEY・時刻を写す。
- codec_data: PARAMETER_SETS は `h264enc_parameter_sets`、AVCC は `packet-format.c` で組む。
- 依存: WS318 p012a（encoder の層と ops）。p024 だけがこれを待つ。他の Phase は WS318 に依らない。

## 12. HAL・toolchain・他の WS

- HAL: 変えない。encoder は libmedia の userland の C だけで、kernel・driver・HAL の契約に触れない。
- toolchain・共有の build/: 変えない。host の試験は host の cc・clang・ffmpeg（既にある）。
- WS326: p026 の名前の提案（§5.2）、p002 の refactor で decoder の header の基点が移る（§5.3）。
- WS318: p012a の ops の形、`packet-format.c`、`thread.c`（p025）。Q329-5 は WS318 の公開の config に欄を足すかの問い。
- WS330: `vcodec-*` の 3 module を H.265 の encoder がそのまま使う。CABAC の engine は context の byte を渡す形で codec を知らない。動きの探索・cost・rate control・gop の枠は WS330 p001 が共通化を判断する。

## 13. Phase の表

型: O = Opus（phase-runner）、S = Sonnet 5.5 high（phase-runner-low、標準廉価実装手順）、D = conformance-reviewer。S の Phase は header 1〜2 個、1 回の起動。各 phase.md は `phaseNNN/phase.md`（共通の節は §13.1）。

| Phase | 目的（module） | 型 | 依存 | 受け入れ |
| --- | --- | --- | --- | --- |
| p001 | 設計（この文書、proposed、問い、review） | O | — | Q1 の受け入れ、Q329 の回答 |
| p002 | 共通化の refactor R1〜R5（decoder の振る舞いは変えない） | O | WS326 completed（p026 を含む） | WS326 の run-unit all と適合が前後で同じ、新しい test-cabac_ctx・test-levels、amd64 build warning 0 |
| p003 | encoder の header（proposed → source、p026 の名前に合わせる）・.c の骨格（ENOSYS）・Makefile・`run-unit.sh`・`check-skeleton.sh`・試験の土台 `fixture.h`・BASE | O | p002 | check-skeleton PASS、amd64 build warning 0、decoder の試験が変わらず PASS |
| p004 | vcodec-bitwriter、vcodec-nalwriter | S | p003 | run-unit（bitwriter・nalwriter）、突然変異 |
| p005 | vcodec-cabac-enc | S | p004 | 往復（h264sw_cabac）、見積もりの表の式の照合 |
| p006 | h264enc-ps | S | p004 | 往復（h264sw_ps_read_*）、表 A-1 の level の選び |
| p007 | h264enc-slice | S | p004、p006 | 往復（h264sw_slice_read）、9.3.4.6 の手計算 |
| p008 | h264enc-quant | S | p003 | MF の式の値、decoder の逆との誤差の性質 |
| p009 | h264enc-cavlc | S | p004 | 往復（h264sw_cavlc_residual_block）、level_prefix の上限 |
| p010 | h264enc-cabac-se | S | p005 | 往復（h264sw_cabac_* の各要素） |
| p011 | h264enc-cabac-res | S | p010 | 往復（h264sw_cabac_residual_block）、見積もり |
| p012 | h264enc-cost、h264enc-source | S | p003 | 手計算の SAD・SATD・SSD・λ、copy と端 |
| p013 | h264enc-refplanes | S | p003 | 全分数で h264sw_interp_luma と一致（端の外を含む） |
| p014 | h264enc-me | S | p012、p013 | 合成の平行移動で既知の MV、窓と範囲、再現 |
| p015 | h264enc-intra | S | p008、p012 | 明らかな mode、block の再構成が recon_mb と一致 |
| p016 | h264enc-mbwrite | S | p007、p009、p011（試験の土台 fixture.h は p003） | MB を書いて h264sw_macroblock_parse で syntax と entry が一致（CAVLC・CABAC、全 kind、PCM、skip の run） |
| p017 | h264enc-mb | O | p008、p015、p016 | 符号化 → 書き → decoder の slice_data で再構成が一致、decode した mode・MV が決定と一致、skip の変換、cbp、PCM の 0 |
| p018 | h264enc-mode（preset、I・P） | S | p014、p017 | 合成の画で skip・16x16・分割・intra の選び、encoder の再構成と decoder の一致（短い P の slice） |
| p019 | h264enc-mode-b | S | p018 | B の direct・skip・Bi・8x8 の選び、一致 |
| p020 | h264enc-ratecontrol | S | p003 | 合成の model で収束・buffer・CQP・filler |
| p021 | h264enc-gop | S | p003 | 種類・順・frame_num・POC・idr_pic_id・時刻の手の表 |
| p022 | h264enc-encoder の全体、host の道具と roundtrip.sh、最初の全体の試験 | O | p004〜p021 | roundtrip の基本の組（3 profile × 2 entropy × B 0/2）で 3 者の MD5 一致 |
| p023 | 品質と適合の行列（§10.2 の全組、BD-rate、極端の QP・大きさ・forced key・drain） | O | p022 | §9 の正しさの門、画質の目標（Q329-1 の答え） |
| p024 | libmedia の backend（WS318 の encoder の ops）、問い合わせの行、media_encoder の host 試験 | O | p022、WS318 p012a | WS318 の encoder の試験、amd64 build warning 0 |
| p025 | 速さ（測り、近道、slice の thread、実機の計測） | O | p023、WS318 p008 | §9 の速さの目標（Q329-2 の答え）、近道の前後で MD5 が同じ |
| p026 | 規約の全文の見直し、header の持ち越しの直し | D | p023〜p025 | conformance-reviewer の checklist、全試験 PASS |

- 並べられる組（p003 の後）: {p004 → (p005 → p010 → p011)、(p006 → p007)、p009}、p008、p012、p013、p020、p021。
- 推しの起動の順: p004・p008・p012・p013・p020・p021 を並べ（最大 5）、次に p005・p006・p009・p014・p015、…。

### 13.1 実装の Phase（S）の共通の節

各 phase.md は次を共通に指す（標準廉価実装手順 §4.6）:

- **読む物（共通）**: AGENTS.md §4・§7・§9・§10、plan/agents/protocol.md、plan/coding-style.md の全文（§14 の checklist）、この文書の §1・§3・§4・§7・§10.1、`h264sw.h`・`h264enc.h`、実装する header と、その header が include する header、仕様の PDF（§1 の path と SHA-256、頁の換算）。
- **変えてはいけない物**: 全部の `h264sw*.h`・`h264enc*.h`・`vcodec-*.h`、担当の外の `.c`、Makefile、`plan/ws329/tests/run-unit.sh`・`check-skeleton.sh`・`unit/check.h` 以外の試験の道具（自分の test を除く）、HAL・toolchain・共有の build/。header の不備は直さず止めて Q1 へ（どの header のどの関数で何が足りないか）。
- **確かめの command**: `timeout 900 sh plan/ws329/tests/run-unit.sh <name>` と `all`、`BASE=<p003 の SHA> timeout 300 sh plan/ws329/tests/check-skeleton.sh`、WS326 の `sh plan/ws326/tests/run-unit.sh all`（decoder が変わっていない）、zedBSD amd64 の libmedia.so の build（WS326 design §6 の command）で warning 0、`git diff --check`。
- **終わりの条件**: 上が全部 PASS（出力を自分で読む）、わざと壊す確かめ、規約の checklist、`git add` の後 `git commit -m WIP -- <path>`、phase.md の「結果」（日付は host の `date`、command と件数、自分で決めた点、未実施）、ws.md の行を「実装済み・試験 PASS」（cleared は Q1）。

## 14. Q1 への注意（ユーザーの判断ではない物）

1. WS326 p026 の設計に §5.2 の `vcodec-` の名前を渡してほしい（p026 の担当が別の名前を選ぶなら WS329 p003 が合わせる）。
2. p002 は WS326 の header の凍結が外れた後（WS326 completed の後）。WS326 の `check-skeleton.sh` の BASE が p002 の commit に移ることを WS326 の記録に。
3. WS318 の記録: WS318 §11 が書くとおり、WS329 の p024 の依存に WS318 p012a、p025 に WS318 p008（thread.c）。Q329-5 の答えによっては WS318 の proposed/media.h に `speed` の欄を足す（WS318 の p002 の前なら header の凍結に当たらない）。
4. JM の参照 decoder（ITU-T/ISO の配布の C、host でだけ build して実行）を p023 の厳しい oracle に足すか: 取得はパラメータの無い GET 1 回（約 5 MB の zip、SHA-256 を記録、cache は main の `build/sources/`）。足さなくても ffmpeg（厳しい設定）と自前の decoder の 2 つで門は作れる。推しは「足す」（ffmpeg は寛容な decoder で、適合の誤りを見逃すことがある）。master.md の web の取得の条件に照らして Q1 が決める。
5. host の disk が少ない（2026-10-31 に約 11 GB 空き）。§10.2 の試験は YUV を pipe で渡し、file を作らない形にした。
7. 大きな picture の memory: 参照の面は 4 面 × (refs + 2) 枠。4K で約 200 MB、8192x4320 で約 850 MB。Pi で 4K を encode する時の目安として WS318 の問い合わせの max_width に反映するかは p024 で決める。
8. WS326 p026 が frame・DPB の共通の部分まで移すと、`h264enc-encoder.h` の `h264sw_frame_pool`・`h264sw_dpb` の名前も変わる。p003 は名前の置き換え以上の差分を review する。
9. ws.md の「ユーザーの決定（2026-10-30、原文）」は Q1 が書いた原文の記録。AGENTS §8（2026-10-30 以降は原文を載せない）との整理は Q1（この Phase では変えていない）。
10. WS318 の `media_codec_info` は max_references を出すが `media_encoder_config` に refs・entropy・slices の欄が無い。今は encoder の preset の既定で決める（Q329-5 と一緒に判断）。
6. この Phase の proposed の header は source に入っていない（この Queue は plan/ws329/ だけ）。標準廉価実装手順の「p001 が header と骨格を source に置く」は p003 に移した。

## 15. design-reviewer の結果と反映

2026-10-31 に design-reviewer を 1 回（29 件: blocker 0・major 10・minor 19）。全件と反映は [design-review.md](design-review.md)。主な変更: MB の符号化で entry の kind・qp を最初に置く（intra の予測 mode の誤り）、entry の total_coeff と coded_flags を entropy ごとに（decoder の parser のとおり）、skip は残差を作らない（E19）、PCM の 0 の標本（E15）、access unit の大きさの確かめと符号化し直し（E15）、試験の口（E20）、出典を block の `Procedure referenced:` に、p003 に試験の土台、p017 を Opus に、使われない CABAC の bit の見積もりの API を外した、問いを 5 つに分けた。

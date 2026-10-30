<!-- awesome-plan project=zedbsd record=ws326-p026 -->

# ws326-p026 H.265 でも使える部分を codec に依らない名前の module へ移す

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。Queue: q1769-i01（P4、Opus）。
- 状態: 実装済み・試験 PASS（2026-10-31 P4、92ca2d0e8。cleared は Q1）。
- 依存: p022（JVT の適合。`h264sw-*.c` が p022 で直る可能性があるので、移しは p022 の merge の後の tree に当てる）。
- 調べた tree: main a08e7f129（worktree `p4-ws326p026`）。仕様: ITU-T H.264 (08/2021)（design.md §1）と ITU-T H.265 (V11) (01/2026)（`/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h265-202601.pdf`、SHA-256 `78b093480e178a2b99d8cb7499934240f359c50c6d09568f8be651bc1151d4d2`、git に入れない。H.265 の節の番号は pdftotext の見出しで照合した）。
- 2026-10-31 ユーザーから、libmedia の実装の WS を昼までに終えるよう優先度を上げる、という趣旨の指示を受けた（Q1 経由）。この Phase の移しは小さく安全な範囲に絞る（§1 の「先送り」）。

## 0. 目的と範囲

ws.md のユーザーの決定 3（共通部分は `h264sw_` でなく汎用の名前に）を、今の decoder の振る舞いを変えずに実行する。
範囲は「どれを codec に依らない名前にするか」の決定と、その移し（file の移し・名前の置き換え・試験の道具の追従）。
新しい機能・H.265 の module・WS329 の共通化（R1〜R5）は範囲の外。

## 1. 決定の要約

| 候補（ws.md の例） | 決定 | 理由 |
| --- | --- | --- |
| bit の読み・Exp-Golomb（h264sw-bits） | **移す** → `vcodec-bitreader` | u(n)・ue(v)・se(v)・more_rbsp_data()・rbsp_trailing_bits() は H.265 も同じ（H.265 7.2、9.2、7.3.2.11）。module は codec の状態を持たない。te(v) は H.264 だけの記述子だが、読みの形は ue(v) の変形で害が無いので同じ module に残す |
| NAL・RBSP（h264sw-nal） | **一部を移す** → `vcodec-nal`（Annex B の分割と emulation prevention の除去）。NAL unit header の読みと `H264SW_NAL_*` は H.264 に残す | start code の分割は H.264 B.1・B.2 と H.265 B.2 で同じ。emulation prevention と末尾の 0x03（cabac_zero_word の後）も H.264 7.4.1・H.265 7.4.2 で同じ。header は H.264 が 1 byte、H.265 が 2 byte で意味も違う |
| CABAC の engine と状態の遷移の表（h264sw-cabac） | **engine を移す** → `vcodec-cabac`（DecodeDecision・Bypass・Terminate・renormalization・engine の初期化・I_PCM の後の再開、表 9-44・9-45、式 9-5 の 1 つの context の初期値）。context の配列（460）・ctxIdx の検査・slice type ごとの初期化の一覧と (m, n) の表は H.264 に残す | H.265 9.3.4.3.2〜9.3.4.3.5 と 9.3.2.6 は H.264 9.3.3.2・9.3.1.2 と同じ算術、表 9-52・9-53 は表 9-44・9-45 と同じ値、式 9-6 は式 9-5 と同じ（m, n の出し方だけが違う）。context の数と番号は codec ごと |
| 共通の約束（h264sw.h の誤りの値・算術・global の状態の規則） | **移す** → `vcodec.h`。H.264 の範囲の文と上限（`H264SW_MAX_*`・`H264SW_MB_*`）は h264sw.h に残す | 誤りの値と「bitstream を信じない」「未定義の動作をしない」は H.265 の module も同じ約束にする。`UNUSED_PARAMETER` も vcodec.h へ（h264sw.h が include する） |
| frame・plane の確保（h264sw-frame） | **先送り**（WS330 p001） | `struct h264sw_frame` は 3 plane の他に H.264 の motion（macroblock ごと・8x8 の ref_idx）・frame_num・top/bottom の POC・non_existing を持ち、pool の大きさは macroblock の単位。H.265 は CTB・最小 CB（8）の単位の大きさ、4x4（参照は 16x16 に圧縮）の motion、frame_num が無い。plane と参照の数と pool だけを抜く形は H.265 の設計と一緒に決めないと推測の API になり、今の移しの差分（全 module の `frame->plane` の参照）が大きい |
| DPB・出力の順（h264sw-dpb） | **先送り**（WS330 p001） | H.264 の DPB は marking（sliding window・MMCO）・frame_num の gap・C.4 の bumping。H.265 は RPS と C.5.2 の bumping（sps_max_num_reorder_pics・latency）。共通は「出力待ちの中で POC 最小を出す」と出力の queue くらいで、型が `h264sw_frame` に結び付いている。frame と一緒に WS330 p001 で決める |
| 試験の道具（check.h・bitwriter.h・cabacwriter.h） | **名前は変えない、置き場の移しは WS326 の完了の時**（Q1） | check.h は codec に依らない。bitwriter.h は `bitwriter_nal` の header の 1 byte だけが H.264。cabacwriter.h は 460 の context と式 9-5 の写しで H.264 に結び付く（独立の oracle なので decoder の code を使わせない）。どれも plan/ws326/tests/unit にあり、WS329 が include path で使う。WS326 の完了で tests/ が消える前に、Q1 が `plan/tools/` へ移して tools.md に登録する（AGENTS.md §5） |
| 表（zig-zag・既定の scaling list・QPC・normAdjust）・変換・intra・補間・重み・deblocking | H.264 に残す | H.265 は対角の scan、別の既定の list、表 8-10 の QPC、4〜32 の DCT と DST、35 の intra mode、8・4 tap の補間、別の deblocking。共通の部分が無いか、数行（clip の足し算）で module にする値が無い |

## 2. 名前

- codec に依らない物は `vcodec-<名>.h/.c`、関数・型 `vcodec_`、定数 `VCODEC_`、include guard `LIBMEDIA_VCODEC_<名>_H`（WS329 design.md §5.2 の提案のとおり）。
- reader 側の名前: **`vcodec-bitreader`**（`struct vcodec_bitreader`、`vcodec_bitreader_*`）・**`vcodec-nal`**（`vcodec_nal_find`・`vcodec_nal_unescape`）・**`vcodec-cabac`**（`struct vcodec_cabac`、`vcodec_cabac_*`）。
  WS329 の書き側 `vcodec-bitwriter`・`vcodec-nalwriter`（`vcodec_nal_escaped_size`・`vcodec_nal_append`）・`vcodec-cabac-enc`（`vcodec_cabac_enc_*`）と対になる。
  WS329 は reader を `vcodec-bits` と仮に呼んだが、`vcodec_bitwriter` との対称と、WS329 の `vcodec_ue_bits`・`vcodec_se_bits`（符号の長さ）・libmedia の既存の `media_bits`（bits.h）との取り違えを避けるため `bitreader` にする。関数名の衝突は無い（`vcodec_nal_find`・`_unescape` と `vcodec_nal_escaped_size`・`_append`、`vcodec_cabac_start` と `vcodec_cabac_enc_start`）。
- H.264 に固有の物は `h264sw_` のまま（WS329 E5 と同じ）。

### 2.1 名前の対応（旧 → 新）

| 旧 | 新 | 移し方 |
| --- | --- | --- |
| `h264sw-bits.h`・`h264sw-bits.c` | `vcodec-bitreader.h`・`vcodec-bitreader.c` | `git mv`（Q1 の OK の後）、中身は名前の置き換えと説明の文 |
| `struct h264sw_bits` | `struct vcodec_bitreader` | 機械の置き換え（全 file） |
| `h264sw_bits_{init,u,u1,peek,skip,ue,se,te,aligned,left,more_rbsp_data,trailing}` | `vcodec_bitreader_{同じ}` | 同上 |
| `LIBMEDIA_H264SW_BITS_H`・`#include "h264sw-bits.h"` | `LIBMEDIA_VCODEC_BITREADER_H`・`#include "vcodec-bitreader.h"` | 同上 |
| static の `bits_fetch`・`bits_stop_find`・`BITS_*` | 変えない（file の中だけの名前） | — |
| `h264sw_nal_find`（と static の `nal_start_code_find`、`NAL_START_CODE_LENGTH`） | `vcodec_nal_find`（`vcodec-nal.c` へ移す） | 新しい file へ関数を移す |
| `h264sw_nal_parse` の中の「末尾の 0x03 を落として `media_rbsp_unescape`」 | `vcodec_nal_unescape`（新しい公開の関数）。`h264sw_nal_parse` はそれを呼ぶ | 同上 |
| `h264sw_nal_parse`・`struct h264sw_nal`・`H264SW_NAL_*` | 変えない（h264sw-nal に残る） | — |
| `struct h264sw_cabac` の engine の field（data・size・position・range・offset・bits_left・error） | `struct vcodec_cabac`。`struct h264sw_cabac` は `struct vcodec_cabac engine;` と `uint8_t state[H264SW_CABAC_CONTEXTS];` を持つ | 構造の変更（§3.4） |
| `cabac.error`・`cabac->error`・`cabac.range`（macroblock.c・cabac-se.c・cabac-se.h の comment・試験） | `cabac.engine.error` など | 機械の置き換え、compile で確かめる |
| `h264sw_cabac_bypass(X)`・`h264sw_cabac_terminate(X)`・`h264sw_cabac_pcm(X, …)` | `vcodec_cabac_bypass(&(X)->engine)` などの形（`&ctx->cabac` → `&ctx->cabac.engine`、`&cabac` → `&cabac.engine`） | 呼び出しの形ごとに置き換え。型が合わないと `-Werror` の compile が止まるので漏れは残らない |
| `h264sw_cabac_decision(cabac, ctx_idx)` | 変えない（ctxIdx の検査をして `vcodec_cabac_decision(&cabac->engine, &cabac->state[ctx_idx])`） | h264sw-cabac.c の中 |
| `h264sw_cabac_start` | 変えない（alignment の bit・slice type の context の初期化の後に `vcodec_cabac_start`） | 同上 |
| static の `cabac_engine_start`・`cabac_ahead`・`cabac_renorm`・`cabac_overrun_check` と engine の `CABAC_*` の定数 | `vcodec-cabac.c` の static | 移す |
| static の `cabac_state_derive(m, n, qp)` | `vcodec_cabac_context_init(m, n, qp)`（公開、式 9-5・H.265 の式 9-6） | 移す |
| `h264sw_cabac_range_lps`・`_trans_lps`・`_trans_mps`（.c の中の `extern`） | `vcodec_cabac_range_lps`・`_trans_lps`・`_trans_mps`、`vcodec-cabac.h` で宣言 | 移す（WS329 R3 の「表を header で宣言」はこれで済む） |
| `h264sw.h` の共通の約束の文と `UNUSED_PARAMETER` | `vcodec.h`（h264sw.h が include する） | 移す |

## 3. module の interface（header の comment が正本、ここは要点）

### 3.1 vcodec.h

libmedia の software の video codec（今は H.264 の decoder、後に H.264 の encoder と H.265）の共通の約束: bitstream を信じない読み（buffer の外を読まない、検査していない値で添字しない、上限の無い loop をしない、失敗は返す）、算術（符号付きの溢れ・負の値の左 shift をしない、狭い型へは明示の clamp）、誤りの値（0・EINVAL・ENOTSUP・ENOMEM・ENOBUFS・EBUSY、今の h264sw.h の表の文を codec に依らない言い方に）、global の状態を持たない。`<stddef.h>`・`<stdint.h>`、`UNUSED_PARAMETER`。
h264sw.h は `#include "vcodec.h"` し、先頭の文は H.264 の範囲（profile・断る道具）と「約束は vcodec.h」に、上限の定数と節の番号の読み方の block は残す。

### 3.2 vcodec-bitreader.h

今の h264sw-bits.h と同じ宣言（名前だけ替わる）。説明の文: 「libmedia の software の video codec の bit reader: H.264 7.2・9.1 と H.265 7.2・9.2 の記述子 u(n)・ue(v)・se(v)、H.264 の te(v)、RBSP の問い（more_rbsp_data()・byte_aligned()・rbsp_trailing_bits()）」。
「CAVLC の table の引き（h264sw-cavlc.h）」「CABAC の engine（h264sw-cabac.h）」の参照は「可変長の符号の表引き」「CABAC の engine（vcodec-cabac.h）」に。te(v) の comment の ref_idx の例は「H.264 の」と明記する。関数の契約・誤り（sticky）は変えない。

### 3.3 vcodec-nal.h

```c
int vcodec_nal_find(const uint8_t *data, size_t size, size_t *cursor, const uint8_t **nal, size_t *nal_size);
int vcodec_nal_unescape(const uint8_t *escaped, size_t size, uint8_t *buffer, size_t capacity, size_t *rbsp_size);
```

- `vcodec_nal_find`: 今の `h264sw_nal_find` の契約のまま（H.264 B.1.1・B.2、H.265 B.2）。
- `vcodec_nal_unescape`: NAL unit の header の後の byte（escaped、size）から RBSP を buffer へ。末尾が 0x000003 なら最後の 0x03 を先に落とす（H.264 7.4.1、H.265 7.4.2: 最後の cabac_zero_word の後の 0x03）。残りを `media_rbsp_unescape`（bits.h）で除く。
  返り値: 0 と `*rbsp_size`、EINVAL（NAL unit に起こりえない 3 byte の並び、rbsp_size が NULL、size が 0 でないのに escaped が NULL、capacity が 0 でないのに buffer が NULL）、ENOBUFS（capacity が足りない）。size 0 は 0 と `*rbsp_size` 0（今の `h264sw_nal_parse` の 1 byte だけの NAL unit と同じ結果）。
  `h264sw_nal_parse` は長さを local に受け、成功の時だけ out に書く（今のまま）。
- `h264sw_nal_parse` は header の読み（forbidden_zero_bit・nal_ref_idc・nal_unit_type、14・20・21 は header だけ）を残し、RBSP は `vcodec_nal_unescape(nal + 1, size - 1, …)`。誤りの順と値は今と同じ（size 0・forbidden で EINVAL が先）。

### 3.4 vcodec-cabac.h と h264sw-cabac.h

```c
#define VCODEC_CABAC_START_BYTES 2U   /* engine の初期化に要る最少の byte（ivlOffset の 9 bit） */
extern const uint8_t vcodec_cabac_range_lps[64][4];   /* H.264 表 9-44、H.265 表 9-52 */
extern const uint8_t vcodec_cabac_trans_lps[64];      /* H.264 表 9-45、H.265 表 9-53 */
extern const uint8_t vcodec_cabac_trans_mps[64];
struct vcodec_cabac { const uint8_t *data; size_t size; size_t position; uint32_t range; uint32_t offset; int bits_left; int error; };
uint8_t vcodec_cabac_context_init(int m, int n, int qp);
int vcodec_cabac_start(struct vcodec_cabac *engine, const uint8_t *data, size_t size);
unsigned vcodec_cabac_decision(struct vcodec_cabac *engine, uint8_t *context);
unsigned vcodec_cabac_bypass(struct vcodec_cabac *engine);
unsigned vcodec_cabac_terminate(struct vcodec_cabac *engine);
int vcodec_cabac_pcm(struct vcodec_cabac *engine, uint8_t *samples, size_t count);
```

- context の 1 byte は pStateIdx（bit 0..5）と valMPS（bit 6）。WS329 の `vcodec_cabac_enc_decision(engine, uint8_t *context, bin)` と同じ形（engine は codec の context の番号を知らない）。
- `vcodec_cabac_context_init`: 式 9-5（H.265 9.3.2.2 の式 9-6 も同じ）の preCtxState から packed の byte。今の static `cabac_state_derive` そのまま。
- `vcodec_cabac_start`: data・size を取り、9 bit の codIOffset を読む（H.264 9.3.1.2、H.265 9.3.2.6）。data は byte の境から。size が `VCODEC_CABAC_START_BYTES` 未満・codIOffset 510・511 は EINVAL（error も EINVAL）。今の static `cabac_engine_start(cabac, 0)`。
- `vcodec_cabac_decision`・`_bypass`・`_terminate`: 今の `h264sw_cabac_decision` の算術の部分・`h264sw_cabac_bypass`・`h264sw_cabac_terminate`（H.264 9.3.3.2.1〜9.3.3.2.4、H.265 9.3.4.3.2〜9.3.4.3.5）。読み過ぎの error の規則は今の struct の comment のまま。
- `vcodec_cabac_pcm`: 今の `h264sw_cabac_pcm`（8 bit の標本を count byte 写して engine を再開、context は engine の外なので触らない）。
- `struct h264sw_cabac { struct vcodec_cabac engine; uint8_t state[H264SW_CABAC_CONTEXTS]; }`。`H264SW_CABAC_CONTEXTS`・`H264SW_CABAC_TERMINATE` は残す。
  `h264sw_cabac_start` の契約は変えない（全体を 0 にして engine.error を EINVAL にしてから検査、alignment の bit、`VCODEC_CABAC_START_BYTES` の前の検査、表 9-11 の一覧の context を `vcodec_cabac_context_init` で、最後に `vcodec_cabac_start`）。
  `h264sw_cabac_decision` は ctxIdx 460 以上か 276 で `engine.error = EINVAL` と 0、他は `vcodec_cabac_decision(&cabac->engine, &cabac->state[ctx_idx])`。
  `h264sw_cabac_bypass`・`_terminate`・`_pcm` は消し、呼び手（cabac-se.c・macroblock.c・試験）が vcodec の関数を engine で呼ぶ（中継だけの関数を残さない）。
- `vcodec_cabac_start` は最初に engine の全部の field を 0 にし error を EINVAL にしてから検査する（data は size が 0 の時だけ NULL）。`vcodec_cabac_context_init` の m・n は -128..127（H.264 の表、H.265 の式 9-5 の値）。
- vcodec の file は vcodec*.h・bits.h・libc だけを include し、comment にも h264sw の名前を書かない（check-skeleton が grep で確かめる）。節の番号の版は vcodec.h の block（H.264 (08/2021)、H.265 (V11) (01/2026)）。
- 速さ: decision は h264sw の関数から vcodec の関数への 1 段の呼び出しが増える（別の翻訳単位なので inline にならない）。bypass・terminate は増えない。試験の build は ASan・-O1 なので時間の比べは意味が無く、測らない。詰めは p024（速さ、p026 の後）で。

## 4. H.264 に残す物

h264sw.h（範囲の文と上限）・h264sw-nal（header と `H264SW_NAL_*`、`h264sw_nal_parse`）・h264sw-cabac（context の配列・`h264sw_cabac_start`・`h264sw_cabac_decision`）・h264sw-cabac-init（(m, n) の表）と、他の全 module（tables・ps・slice・frame・poc・dpb・reflist・mb・context・cavlc・cabac-se・macroblock・transform・intra・mvpred・direct・interp・weight・recon・deblock・decoder・backend）。名前・header は変えない（include の行と、bit reader・CABAC の名前の参照の置き換えを除く）。

## 5. 移し方（p022 の merge の後）

0. Q1 の知らせの後、worktree に main を取り込む（`git merge --no-ff main`、親 2 つを確かめる）。p022 の道具（`plan/ws326/tests/conformance/` の run の script と conform.c）の module の glob を読む。
1. **前の記録**（§6 の B）を取る。
2. `git mv`（Q1 の OK の後）: `userland/desktop/libmedia/h264sw-bits.h` → `vcodec-bitreader.h`、`h264sw-bits.c` → `vcodec-bitreader.c`、`plan/ws326/tests/unit/test-bits.c` → `test-bitreader.c`（module の名前に合わせる。最後の行が `test-bitreader: …` になる以外は同じ）。他に消す・移す file は無い。
3. 機械の置き換え（`perl -pi` を名指しの file の一覧に、`\bh264sw_bits_` → `vcodec_bitreader_`、`\bstruct h264sw_bits\b` → `struct vcodec_bitreader`、`"h264sw-bits.h"` → `"vcodec-bitreader.h"`、guard）。対象は libmedia の h264sw-*・vcodec-*、plan/ws326/tests の C と script。置き換えの後 `grep -rn 'h264sw_bits\|h264sw-bits'` が 0（plan の記録を除く）。
4. 手の編集: `vcodec.h` を作り h264sw.h から約束の文を移す。`vcodec-nal.h/.c` を作り `h264sw_nal_find` と unescape を移す。`vcodec-cabac.h/.c` を作り engine を移す、h264sw-cabac.h/.c を §3.4 の形に。cabac-se.c・macroblock.c・試験の bypass・terminate・pcm・error の呼び出しを置き換える。comment の「h264sw-bits.h」「h264sw_nal_find」などの参照も直す。
5. build と試験の道具: `userland/desktop/libmedia/Makefile` の LIBMEDIA_SOURCES に `vcodec-bitreader.c`・`vcodec-nal.c`・`vcodec-cabac.c`（`h264sw-bits.c` を外す）。
   `run-unit.sh`・`run-stream.sh`・`backend/run-backend.sh`・`backend/run-host-decode.sh`・p022 の適合の script の module の一覧に `vcodec-*.c`。`check-skeleton.sh` は header `h264sw*.h vcodec*.h`、source `h264sw-*.c vcodec-*.c`、BASE の diff も両方。
   `plan/tools/media/run-host-codec.sh`（`h264sw-*.c` を link する登録済みの道具）の 1 行に `vcodec-*.c` を足す。この file は担当の範囲の外なので Q1 が直すか、Q1 の許しで同じ commit に入れる（§8）。
6. 規約: 新しい・変えた関数を coding-style の §14 の checklist で見る（宣言の位置・if ごとの comment・前方宣言・出典の block）。表 9-44・9-45 の `Values referenced:` の comment に H.265 の表 9-52・9-53 の同値を足す（値は変えない）。source の comment に WS・Phase の番号を書かない。
7. **後の記録**（§6 の A）を取り、B と比べる。
8. 記録: design.md §3 の module の表に vcodec の 3 つと vcodec.h、ws.md の header の基点（新しい BASE = この移しの commit）、この phase.md の「結果」。`git commit -m WIP -- <path>`、SHA を Q1 へ。

## 6. 確かめ（前後で同じ）

出力は `/tmp/o-ws326-p026/{before,after}/`（小さい text だけ）。前は p022 の merge の後の tree（1e90322b4）を `git archive` で `/tmp/o-ws326-p026/tree-before` に写して流す（移しの編集と並べるため）。各 command に `timeout`。どの試験も「同じ」に加えて PASS の行そのもの（適合は p022 の `conform-run: PASS (101 in, 103 out)`、run-stream は `streams:` の数が 0 でない）を条件にする（何も走らずに同じになることを防ぐ）。

| 項目 | command（worktree の root） | 同じであることの判定 |
| --- | --- | --- |
| 単体試験 | `timeout 1800 sh plan/ws326/tests/run-unit.sh all` | 全行が同じ（`test-bits` の行が `test-bitreader` になる名前だけが違い、checks の数は同じ）。`run-unit: PASS` |
| 骨格 | `BASE=<p022 の後の基点> sh plan/ws326/tests/check-skeleton.sh`（前）、`sh plan/ws326/tests/check-skeleton.sh`（後、cc と clang で vcodec*.h も単独 compile）、commit の後に `BASE=<この commit> …` | `check-skeleton: PASS` |
| 全体の試験 | `timeout 3600 sh plan/ws326/tests/run-stream.sh` | 出力の全行が同じ（stream ごとの frame の数と stats、fuzz・start の行） |
| backend | `timeout 1200 sh plan/ws326/tests/backend/run-host-decode.sh`、`timeout 600 sh plan/ws326/tests/backend/run-backend.sh` | 出力の全行が同じ、PASS |
| 適合 | `H264SW_CONFORMANCE_DIR=/tmp/o-ws326-p022/conformance sh plan/ws326/tests/conformance/run.sh`（cache は読むだけ、Q1 の指定） | 全行が同じ、最後の行 `conform-run: PASS (101 in, 103 out)` |
| libmedia.so | design.md §6 の make（`build/amd64/dynamic/libmedia.so`） | exit 0、warning 0。`nm -D --defined-only` の公開の symbol の一覧が前後で同じ（vcodec_* は local） |
| 残り | `git diff --check`、`grep -rn 'h264sw_bits\|h264sw-bits\|h264sw_nal_find\|h264sw_cabac_\(bypass\|terminate\|pcm\)\b' userland plan/ws326/tests plan/tools/media` が 0 | — |

`plan/tools/media/run-host-codec.sh` は libavcodec の add-in の試験で、h264sw の link の行を足しただけ。後に 1 回流して PASS を確かめる。

## 7. 他の WS への引き継ぎ

- **WS329**（H.264 の encoder）: p003 が proposed/ の名前を置き換える: `struct h264sw_bits`・`h264sw_bits_*` → `vcodec_bitreader`、`h264sw_cabac_range_lps`・`_trans_lps`・`_trans_mps` → `vcodec_cabac_*`（`vcodec-cabac.h` で宣言済み）。`vcodec-cabac-enc` は `vcodec_cabac_context_init` で context を作れる。
  decoder の読み戻しの名前: `h264sw_cabac_pcm(&c, …)` → `vcodec_cabac_pcm(&c.engine, …)`（bypass・terminate も同じ）、`h264sw_nal_find` → `vcodec_nal_find`。`proposed/h264enc.h` の「Conventions, as h264sw.h」は vcodec.h へ（encoder の EAGAIN を vcodec.h の誤りの表に足すかは WS329 p003 が決める）。
  名前の対称: reader の `vcodec_nal_find`・`_unescape` と writer の `vcodec_nal_escaped_size`・`_append` が同じ接頭辞を分け合う（衝突は無い）。writer を `vcodec_nalwriter_*` に揃えることを提案する。
  p002 の R3 は「context の初期化を `h264sw_cabac_contexts_init` に分ける」だけが残る（表の宣言は済み）。R3 の後も `struct h264sw_cabac` の形（`engine`・`state`）は保つ。R1・R2・R4・R5 はこの Phase の名前の上に足す。check-skeleton の BASE は p026 の commit に、WS329 p002 でもう一度移る。
- **WS330**（H.265）p001: 使える物は vcodec.h・vcodec-bitreader・vcodec-nal（find と unescape。2 byte の header は H.265 の module で）・vcodec-cabac（engine と表と式 9-6）。足りない物の候補:
  1. PCM の標本が 8 bit でない時（H.265 の PcmBitDepthY < 8、u(v)）と WPP・tile の substream の終わり（end_of_sub_stream_one_bit の後の byte の境で engine を再開）。`vcodec_cabac_pcm` を「算術の復号が着いた byte の位置を返す」と「その位置から engine を再開する」の 2 つに分けると両方に使える（H.264 の pcm はその 2 つで書ける）。
  2. context の保存と復元（H.265 9.3.2.4・9.3.2.5、WPP）: context は engine の外の byte の配列なので、その部分は H.265 の側の memcpy で済む（StatCoeff・palette の予測も対象で、それは H.265 の側）。
  4. aligned bypass（H.265 9.3.4.3.6、cabac_bypass_alignment_enabled_flag、Main の外の RExt）は engine に無い。
  3. frame の pool（plane・参照の数・id・時刻・表示の情報）と出力の順（POC 最小の bumping、出力の queue）の共通化: §1 の理由で先送りした。`struct h264sw_frame` の H.264 の部分（motion・frame_num・top/bottom の POC・non_existing）を分ける設計を WS330 p001 で。
- **Q1（WS326 の完了の時）**: `plan/ws326/tests/unit/check.h`・`bitwriter.h`・`cabacwriter.h` と、残す単体試験・`run-unit.sh`・`run-stream.sh`・`conformance/` を `plan/tools/` へ移して tools.md に登録する（WS329 の各 Phase が path で呼び、WS330 が include する）。

## 8. Q1 に頼む事

1. `git mv` 3 つ（§5 の 2）の OK。
2. `plan/tools/media/run-host-codec.sh` の link の行に `userland/desktop/libmedia/vcodec-*.c` を足すこと（Q1 が直すか、この Phase の commit に入れる許し）。足さないと移しの後にこの道具の build が未定義の symbol で落ちる。
3. 移しの後の ws.md の header の基点（BASE）の更新は、この Phase の commit の SHA で P4 が書く（WS326 の記録）。WS329 の記録（design.md §5.3 の BASE の注）への反映は Q1。

## 9. design-reviewer（2026-10-31、1 回）

13 件（blocker 0・major 4・minor 9）。反映:

| # | 指摘 | 反映 |
| --- | --- | --- |
| 1 | vcodec が h264sw に依存しないことを決めていない・確かめない | §3.4 に規則、vcodec-bitreader.h は vcodec.h を include、check-skeleton に「vcodec の file に h264sw の名前が無い」の grep と vcodec*.h の単独 compile |
| 2 | p024 と p026 が同じ hot path を触るのに並べられる、p024・p025 の BASE と凍結に vcodec*.h が無い | ws.md の p024 の依存に p026。p025 の phase.md の BASE と「変えてはいけない物」を直した。p024 の phase.md は P1 が書いているので Q1 が P1 に伝える（Q1 の指示） |
| 3 | frame・DPB の先送りはユーザーの決定 3 の Q1 の解釈を狭める | Q1 に返し、Q1 が技術の判断として認めた（2026-10-31）。ws.md の「進め方」に記録 |
| 4 | 前後の「同じ」が何も走らなくても成り立つ（適合の cache の環境変数、ffmpeg の無い時）、run-host-codec.sh を流していない | §6 に PASS の行そのものを条件にし、適合は環境変数を明示、run-host-codec.sh を後に流す |
| 5 | context_init の m・n の範囲が契約に無い | header に -128..127 |
| 6 | vcodec_cabac_start が engine 全体を初期化することが契約に無い、data NULL | memset と EINVAL を先に、data NULL は size 0 の時だけ（header と実装） |
| 7 | vcodec_nal_unescape の NULL の場合、parse が out を失敗で書き換えない | 契約に足し、parse は local に受ける |
| 8 | H.265 の Annex B の節（B.2〜B.4）、9.3.4.3.6・StatCoeff、版の置き場 | vcodec-nal.h を B.2〜B.4 に、版は vcodec.h の block、表の `Values referenced:` に「ITU-T H.265 (V11) (01/2026) Table 9-52/9-53」。WS330 への引き継ぎに 9.3.4.3.6 と StatCoeff を足した（§7） |
| 9 | include の置き換えの式・`cabac.range`・表の名前の grep・古くなる comment | 置き換えは path の形も含めた、`cabac.range` を表に、test-cabac.c の `extern` と「no header declares」の comment を消し header の宣言を使う、cabac-se.h・macroblock.h の comment を直した |
| 10 | WS329 への引き継ぎの不足（pcm・nal_find・h264enc.h の約束、試験の道具の移し） | §7 に足した |
| 11 | `vcodec_nal_` の接頭辞が reader と writer に分かれる | §7 で WS329 p003 に `vcodec_nalwriter_*` を提案（揃えるかは WS329 が決める） |
| 12 | ASan の試験で時間を比べても意味が無い | 時間の観察を外した（§3.4） |
| 13 | 出力の置き場（AGENTS §4 は fresh-out か build/tmp） | 試験の script の出力は各 script の fresh_out（worktree の build/tmp）。前後の要約の小さい text と前の tree の写しは Q1 の指示（大きな物は /tmp）で /tmp/o-ws326-p026 に置いた。消すのは Q1 |

## 結果（2026-10-31 P4）

- commit: 設計 acb27716b、main の取り込み 1e90322b4（親 acb27716b・f31bbac2e、p022 の e6ca0232e を含む）、移し **92ca2d0e8**（46 file、新しい BASE）、記録はこの次の commit。
- 移した物: `vcodec.h`（新）、`vcodec-bitreader.h/.c`（h264sw-bits から git mv）、`vcodec-nal.h/.c`（新、`vcodec_nal_find`・`vcodec_nal_unescape`）、`vcodec-cabac.h/.c`（新、engine・表・`vcodec_cabac_context_init`）。h264sw-nal・h264sw-cabac は H.264 の部分だけ、`struct h264sw_cabac` は `engine` と `state`。試験 `test-bits.c` → `test-bitreader.c`（git mv、Q1 の OK）。Makefile、run-unit・run-stream・run-backend・run-host-decode・conformance/run.sh・check-skeleton.sh（vcodec*.h の単独 compile、vcodec に h264sw の名前が無い検査、BASE の照合を両方に）、`plan/tools/media/run-host-codec.sh`（Q1 の許し）。
- 前（p022 の後の tree 1e90322b4 の写し）と後（92ca2d0e8 の前の作業の tree、同じ内容）の出力の照合（`/tmp/o-ws326-p026/{before,after}/`、`/tmp/o-ws326-p026/record.sh`）:

| 項目 | 前 | 後 | 差 |
| --- | --- | --- | --- |
| `sh plan/ws326/tests/run-unit.sh all` | 25 本 PASS、`run-unit: PASS` | 同じ | `test-bits: PASS (169 checks)` が `test-bitreader: PASS (169 checks)` になった行だけ |
| `sh plan/ws326/tests/check-skeleton.sh` | PASS | PASS | 無し。commit の後 `BASE=92ca2d0e8` でも PASS |
| `sh plan/ws326/tests/run-stream.sh` | `streams: 22 streams, 408 frames equal to ffmpeg`、fuzz・start も PASS | 同じ | 無し（全行） |
| `sh plan/ws326/tests/backend/run-host-decode.sh` | PASS | PASS | 無し |
| `sh plan/ws326/tests/backend/run-backend.sh` | PASS | PASS | 無し |
| `H264SW_CONFORMANCE_DIR=/tmp/o-ws326-p022/conformance sh plan/ws326/tests/conformance/run.sh` | 212 行、`conform-run: PASS (101 in, 103 out)` | 同じ | 無し（全行） |
| libmedia.so（design.md §6 の make） | exit 0、warning 0、公開の symbol 55 | exit 0、warning 0 | `nm -D --defined-only` の一覧が同じ（vcodec_* は local） |
| `sh plan/tools/media/run-host-codec.sh` | 未実施 | `run-host-codec: PASS` | — |

- `git diff --check` は空。`grep 'h264sw' userland/desktop/libmedia/vcodec*` は 0（check-skeleton の中）。
- 未実施: QEMU・実機（この Phase は host の照合だけ）。速さの測り（§3.4、p024 で）。conformance-reviewer の全文の見直し（p025）。
- 残り: p024・p025 の BASE は 92ca2d0e8（p025 の phase.md は直した。p024 の phase.md は P1 が書いているので Q1 から伝える）。WS329・WS330 への引き継ぎは §7。

<!-- awesome-plan project=zedbsd record=ws329-p002 -->

# ws329-p002 共通化の refactor: CAVLC の表・CABAC の context の選び・context の初期化・表 A-1 を decoder と encoder の共通に

- 親: [WS329](../ws.md)、設計 [design.md](../design.md)。
- 状態: cleared（2026-11-15 Q1、merge c034b02dab）
- 依存: WS326 completed（p026 の名前の移しを含む。header の凍結 BASE=fd1d6f88c が外れた後）。2026-10-31 に completed、p026 は 92ca2d0e8、WS326 の header の基点は 77ed48ac4。
- 担当の想定: Opus 5.5（phase-runner）。decoder の hot path に触れるので Sonnet に回さない。

## 目的と範囲

design §4・§5.3 の R1〜R5 を行う。decoder の振る舞い（出力の画素・誤りの値・試験の結果）を変えない。

- 置き場（2026-11-14 の今の build に合わせた変更）: 新しい `.c` は libmedia の `sources.mk`（make の file、Claude は変えない）の変更を要するので作らず、下の R1〜R4 の公開の関数と表は既存の module の `.c`・`.h` に置く（R1 は h264sw-cavlc、R2 は h264sw-cabac-se、R3 は h264sw-cabac、R4 は h264sw-ps）。名前と仕様は proposed/shared/ のとおり（cbp の表だけ `h264sw_cavlc_cbp_values`、既存の関数 `h264sw_cavlc_cbp` とぶつかるため）。
- R1: `h264sw-cavlc.c` の表を新しい `h264sw-cavlc-tables.[ch]` へ（[proposed/shared/h264sw-cavlc-tables.h](../proposed/shared/h264sw-cavlc-tables.h)）。値・順は変えず、block の `Values referenced:` を保つ。cavlc.c は新しい名前を使う。
- R2: `h264sw-cabac-se.c` の context の導出を `h264sw-cabac-ctx.[ch]` の公開の関数へ（[proposed/shared/h264sw-cabac-ctx.h](../proposed/shared/h264sw-cabac-ctx.h)）。cabac-se.c はそれを呼ぶ。
- R3: `h264sw_cabac_start` の context の部分を `h264sw_cabac_contexts_init` に分け、表 9-44・9-45 の宣言を header に置く（WS326 p026 が算術の engine を汎用の名前に移していれば、その header に）。表の宣言は p026 が `vcodec-cabac.h` に済ませていた（`vcodec_cabac_range_lps` など）。
- R4: `h264sw-ps.c` の `ps_level_limits` を `h264sw-levels.[ch]`（全列、[proposed/shared/h264sw-levels.h](../proposed/shared/h264sw-levels.h)）へ。表 A-1 の全 20 行を PDF の頁の画で照合する（2 つの独立の読み）。
- R5: 共通に使う header の先頭の文の "decoder" を "codec" に（意味を変えない）。
- proposed の名前は WS326 p026 の結果に合わせて直してよい（直した点を結果に書く）。

## 受け入れ

- 変える前と後で `sh plan/tools/h264sw/run-unit.sh all` の各 test の最後の行（件数を含む）が同じ。WS326 p022 の適合の試験（`in` の全 stream）が前後で同じ結果。
- 新しい単体試験 `plan/ws329/tests/unit/test-cabac_ctx.c`（各関数を手の例で: 隣の有無・skip・intra・I_PCM・direct・cat ごとの offset）と `test-levels.c`（表 A-1 の全行、1b の見つけ方）。この 2 本は p003 の run-unit.sh で流す（p002 では WS326 の run-unit.sh の形を一時の command で）。
- WS326 の `check-skeleton.sh`（BASE 無し）PASS、x86_64・arm64・riscv64 の libmedia の build warning 0。
- WS326 の header の基点が移ることを結果に書く（Q1 が WS326 の記録に反映）。

## 変えてはいけない物

- decoder の各 module の振る舞い。新しい public の関数の他に decoder の公開の関数の形を変えない。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない（消す物は path と理由を Q1 へ）。

## 確かめ

- `sh plan/tools/h264sw/run-unit.sh all` を変更の前後で流し、出力を比べる。
- 適合の試験（WS326 p022 の command）を前後で。
- zedBSD の libmedia（x86_64・arm64・riscv64、自分の worktree の build/ で。初回は `make CONFIGMK=config/config.<機種>.mk llvm-toolchain-cached` と `sysroot`）: `timeout 2400 make CONFIGMK=config/config.<機種>.mk -j4 build/<機種>/packages/libmedia/.stamp`（<機種> は x86_64・rpi5・vf2）→ 各 exit 0、warning 0
- `git diff --check`。commit は `git commit -m WIP -- <path>`、push しない。結果の節（host の `date`、command と結果、未実施）を足し、ws.md の行を更新（cleared は Q1）。

## 結果（2026-11-15 01:09、P5 q2092-i01、基点 main fd18bd61ee）

状態: 実装済み・試験 PASS（cleared は Q1）。commit: b753848bf2（source と test-cavlc の名前の追従）、811a3b8e96（新しい単体試験 2 本）、42df2d83bd（design・phase の前提の直し、proposed の参照の直し）、この結果の commit。

### 前提の乖離の確かめ（2026-10-31 の設計 → 2026-11-14 の main）

- WS326 p026 は済み（92ca2d0e8）: `vcodec.h`・`vcodec-bitreader`・`vcodec-nal`・`vcodec-cabac`。表 9-44・9-45 は `vcodec_cabac_range_lps`・`_trans_lps`・`_trans_mps` として vcodec-cabac.h に宣言済みで、R3 の「表の宣言」は残っていなかった。h264sw-*・vcodec-* の source は 77ed48ac4 から fd18bd61ee まで変わっていない（WS322・WS324 の変更は h265sw-*・vkvideo などで、この Phase の file に触れない）。試験の道具は plan/tools/h264sw/ にある（phase.md の command のまま使えた）。
- build: `ZEDBSD_CONFIG`・`config/ci/*`・`build/amd64` の形は無くなった。今は `make CONFIGMK=config/config.<機種>.mk` と `build/<機種>/packages/libmedia/.stamp`。この phase と phase003〜026（23 個）の command、design §13.1 を今の形に直した（-j8 も -j4 に）。
- 新しい `.c` を足すと `userland/desktop/libmedia/sources.mk`（make の file、Claude は変えない）の変更が要る。新しい module は作らず、同じ公開の関数・表を既存の module に置いた（下）。p003 で encoder の `.c` を足す時は sources.mk の変更が要る（Q1 経由でユーザーに差分の案を出す）。
- 名前のぶつかり: proposed の表 `h264sw_cavlc_cbp` は既存の関数 `h264sw_cavlc_cbp` と同じ名前 → 表は `h264sw_cavlc_cbp_values`。
- 仕様の PDF と適合の stream の cache は main の `build/` から消えていた。PDF は ITU の公開の URL（`T-REC-H.264-202108-S!!PDF-E`）から取り SHA-256 `d6e09bcc…cdf` 一致、stream は `conformance/fetch.sh` で 204 本（全部 sha256.txt と一致）。どちらも worktree の `build/` に置いた。
- proposed/ の参照を今の名前に直した（`vcodec-cabac-enc.h` の表の名前、`h264enc-cavlc.h`・`h264enc-cabac-se.h`・`h264enc-cabac-res.h` の header の名前と cbp の表の名前）。

### 共通にした物（decoder と encoder が使う公開の関数・表）

| R | 置き場 | 公開した物 |
| --- | --- | --- |
| R1 | h264sw-cavlc.h/.c | `struct h264sw_vlc_code`、`H264SW_COEFF_TOKEN_COLUMNS`・`_CODES`・`_CHROMA_DC_CODES`・`H264SW_CBP_CODES`、表 `h264sw_cavlc_coeff_token[4][62]`・`_coeff_token_chroma_dc[14]`・`_total_zeros[15][16]`・`_total_zeros_chroma_dc[3][4]`・`_run_before[7][15]`・`h264sw_cavlc_cbp_values[2][48]`（値・順・`Values referenced:` は元のまま） |
| R2 | h264sw-cabac-se.h/.c | `h264sw_ctx_mb_skip`・`_mb_type_inc`・`_transform_8x8`・`_chroma_mode`・`_ref_idx`・`_mvd`・`_cbp_luma`・`_cbp_chroma`・`_qp_delta`・`_coded_block_flag`・`_significance`・`_abs_level`。decoder の各要素の読みはこれを呼ぶ |
| R3 | h264sw-cabac.h/.c | `h264sw_cabac_contexts_init(state, slice_type, cabac_init_idc, qp)`。`h264sw_cabac_start` はこれを呼ぶ（引数の確かめと読みの順は前と同じ） |
| R4 | h264sw-ps.h/.c | `struct h264sw_level`、`H264SW_LEVEL_1B`・`H264SW_LEVELS`、表 `h264sw_levels[20]`（表 A-1 の全列）、`h264sw_level_find`。`ps_max_dpb_mbs` はこれを使う |
| R5 | h264sw.h、h264sw-cabac.h、h264sw-cabac-se.h、h264sw-cavlc.h、h264sw-ps.h | 先頭の文を codec（decoder と encoder）に |

表 A-1 は 2 つの読みで照合した: PDF 316 頁（印刷 294 頁）の `pdftotext -layout` から source の表を、頁の画（`pdftoppm -r 110`）を読んで test-levels.c の表を書き、test-levels が全 20 行 × 9 列の一致を確かめる。CAVLC の表は、移す前と後の全部の値を書き出す一時の program で比べて一致（cmp 一致）。

WS326 の header（h264sw*.h）が変わったので、`check-skeleton.sh` の BASE は b753848bf2 に移る（Q1 が WS326 の記録と tools.md の `BASE=77ed48ac4` を直す）。

### 試験（前 = fd18bd61ee を `git archive` で build/tmp/base に出した tree、後 = この branch）

| command | 前 | 後 |
| --- | --- | --- |
| `sh plan/tools/h264sw/run-unit.sh all` | 24 本 PASS、`run-unit: PASS` | 24 本の最後の行（件数を含む）が前と全部同じ（diff 無し）、`run-unit: PASS` |
| `H264SW_CONFORMANCE_DIR=<cache> sh plan/tools/h264sw/conformance/run.sh` | `conform-run: PASS (101 in, 103 out)`（14526 frame、逆順の slice 6 本） | 出力 212 行が前と全部同じ、`conform-run: PASS (101 in, 103 out)` |
| `sh plan/tools/h264sw/run-stream.sh` | `run-stream: PASS` | 前と同じ（build の path の行を除き diff 無し） |
| `sh plan/tools/h264sw/backend/run-backend.sh` | `run-backend: PASS`（202・112 checks） | 前と同じ |
| `sh plan/tools/h264sw/check-skeleton.sh`（BASE 無し） | PASS | `check-skeleton: PASS` |
| 新しい単体試験（一時の command: run-unit.sh と同じ cc の flag・ASan・UBSan で `plan/ws329/tests/unit/test-<name>.c` を全 module と build） | — | `test-cabac_ctx: PASS (103 checks)`、`test-levels: PASS (271 checks)`。clang -Wpedantic の構文の確かめも通る |
| `sh plan/tools/h264sw/speed/run-speed.sh`（同じ stream で前後、SPEED_PROFILE=0、5 回） | median 34.17・22.82・25.23 fps（clean・grain・cavlc） | 32.93・22.94・25.35 fps。host の load 約 8〜9 で、同じ tree の 2 回の差（38.3 と 34.2）の方が大きく、差は測れない範囲 |
| libmedia の build（`timeout 2400 make CONFIGMK=config/config.<機種>.mk -j4 build/<機種>/packages/libmedia/.stamp`、x86_64・rpi5・vf2） | — | 3 つとも exit 0、warning 0（log の warning の行 0）。`file` で x86-64・aarch64・RISC-V の ELF、新しい symbol 15 個が入り、`nm -D` に h264sw・vcodec の symbol は 0（exports.map のまま） |
| `git diff --check` | — | 出力無し |

### 未実施と残り

- QEMU・実機: 範囲の外（host の試験で足りる refactor、実機は使わない指示）。
- 新しい試験 2 本を流す `plan/ws329/tests/run-unit.sh` は p003 が作る（それまでは上の一時の command）。
- proposed/shared/ の 3 つの header（h264sw-cavlc-tables.h・h264sw-cabac-ctx.h・h264sw-levels.h）は source に入った物に置き換わった。消すのは Q1（`plan/ws329/proposed/shared/`、理由: 内容が source の h264sw-cavlc.h・h264sw-cabac-se.h・h264sw-cabac.h・h264sw-ps.h に移り、古い写しが p003 以降の担当を迷わせる）。
- p003 への引き継ぎ: proposed の header の comment の「H.265 9.3.4.3」→「9.3.5」（10-31 の Q1 の注意）は未着手（p003 の名前の写しの範囲）。encoder の `.c` を足す sources.mk の差分の案。

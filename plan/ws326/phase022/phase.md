<!-- awesome-plan project=zedbsd record=ws326-p022 -->

# ws326-p022 JVT の適合の試験

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: cleared（2026-10-31 Q1、c80e33c37）。
- 依存: p021（依存の Phase の実装が main に入ってから始める）。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 仕様: ITU-T H.264 (08/2021) の ITU-T H.264.1 の conformance の bitstream、design.md §7。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- `conformance/fetch.sh` で全 204 本を取得（cache は Q1 の指定の場所、無ければ既定）。SHA-256 を記録。
- 道具 `plan/ws326/tests/conformance/conform.c` と `run.sh`: 各 zip の bitstream を decode し、参照の YUV と frame ごとに比べる（大きさ・数・MD5）。decoder の ENOTSUP の理由で out を分類。
- `list.txt` の各行を in か out:<理由> に書き換え、readme（Frame/Field Coding、Number Slice Groups など）と照合。
- 受け入れ: in の全部が全 frame 一致。design.md §7 の機能の各々に in の stream が 1 本以上。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- この Phase では h264sw-*.c の修正をしてよい（header は Q1 へ）。不一致の解析は trace（JM の syntax の記録、data）と比べてよい。修正が大きい時は attempt を分ける。この Phase は Opus が担当してもよい（Q1 の判断）。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `BASE=fd1d6f88c timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

## 結果

- 日付: 2026-10-31（host の `date`）。担当 O-p022（Opus、Queue q1760-i01）。状態: 実装済み・試験 PASS（cleared は Q1 が受け入れを見て決める）。
- decoder（`h264sw-*.c`）の修正は 0。適合の bitstream の全部で、in の stream の不一致が 1 つも無かった。header も変えていない。

### 作った物・変えた物（plan/ws326/tests/conformance/）

- `conform.c`: 適合の判定の program。bitstream を NAL ごとに decoder に与え、出力の frame（crop の後、I420）を参照の YUV の次の frame と比べる（大きさ・数・各 frame の MD5、違えば最初の違う sample と違う macroblock の数）。ENOTSUP の時は拒まれた NAL を読み直して理由を出す（data-partitioning、interlace、format、lossless、size、profile-N、slice-groups、sp-slices）。`--aso` は各 picture の slice を逆の順で与える。`FRAMES` の引数は参照が bitstream より長い時の frame 数。
- `run.sh`: conform.c を全 h264sw-*.c（backend を除く）と ASan・UBSan で build し、list.txt の各 stream を流して list.txt の期待（in は全 frame 一致、out:<理由> はその理由で拒む）と照らす。出力は `build/tmp/ws326-conform`（fresh の run）。
- `files.txt`: file の名前で決まらない zip の bitstream と参照（alphaconformanceG、CVFC1_Sony_C、CVSE2_Sony_B・CVSE3_Sony_H・CVSEFDFT3_Sony_E）と、その理由（readme）。
- `list.txt`: 全 204 行を in か out:<理由> に書き換えた（下の表）。`sha256.txt`: 全 204 本の SHA-256（list の順）。
- `fetch.sh`: 展開で JM の trace の file を除く（下の「事故」）。

### 取得

- 全 204 本の大きさを HEAD で確かめた: 3.07 GB（2 GB を超えるので Q1 の指定どおり `/tmp/o-ws326-p022/conformance` の tmpfs に置いた）。`H264SW_CONFORMANCE_DIR=/tmp/o-ws326-p022/conformance sh plan/ws326/tests/conformance/fetch.sh`（公開の ITU-T の site、parameter の無い GET。bot の確かめの page や権限の判定は無し。途中で 1 度 connection reset、同じ条件で 1 度だけ再試行して取れた）。最後に全部を cache から照合して `fetch: PASS (204 zips)`。
- 事故: fetch.sh が trace の text まで展開したため展開後が約 31 GB になり、/tmp（tmpfs 79 GB、他の利用と合わせて）を満杯にした。fetch と試験を止めて Q1 に報告し、Q1 が trace の file と途中の `.zip.part` を消した。満杯の頃の展開の欠けを避けるため全 zip を trace 抜きで展開し直した（展開後 約 7 GB）。fetch.sh は以後 trace を展開しない。

### 判定（host、ASan・UBSan）

- `H264SW_CONFORMANCE_DIR=/tmp/o-ws326-p022/conformance timeout 7000 sh plan/ws326/tests/conformance/run.sh` → `conform: 101 streams (14526 frames) equal to the reference, 103 refused, 6 equal with the slices reversed`、`conform-run: PASS (101 in, 103 out)`（210 行すべて ok、各 stream の log に Sanitizer・runtime error の行 0。list.txt を書き換える前の 1 回目の run も in の不一致 0 で、違いは推測の 4 行だけ）。
- in 101 本: 全部が全 frame で参照の YUV と一致（計 14526 frame、concealed・slice_errors・誤り 0、参照の frame の余りも不足も 0）。
- out 103 本: interlace 73（SPS の frame_mbs_only_flag 0）、format 25（4:2:2 が 21、monochrome が 2、10 bit が 2）、slice-groups 3（FM1_BT_B・FM1_FT_E・FM2_SVA_C）、profile-88 2（SP1_BT_A・sp2_bt_b: SP の slice を持つ Extended、constraint_set0・1 が 0 なので SPS の時点で拒む）。lossless・data-partitioning・size の stream は無かった。
- `--aso`（各 picture の slice を逆順）: BA1_FT_C（298 picture を並べ替え）・BASQP1_Sony_C・CI1_FT_B（Extended、constraint_set で Baseline 相当）・SVA_Base_B・SVA_CL1_E・CABAST3_Sony_E の 6 本が全 frame 一致。適合の bitstream の中で ASO を持つのは FM1_FT_E・FM2_SVA_C だけで、どちらも slice group を使う（out）。それで ASO は既存の stream の slice を並べ替えて確かめた（slice の順は復号の結果を変えない、7.4.1.2.3）。

### list.txt の推測から変わった物と readme との照合

- 推測（名前から）と違った物: CI1_FT_B（推測 out:interlace → in、readme も Frame）、SVA_FM1_E（推測 out:slice-groups → in、readme の Number Slice Groups 1）、SP1_BT_A・sp2_bt_b（推測 out:sp-slices → out:profile-88、Extended の SPS で先に拒む）。`?` の 119 本は全部が in か out に決まった。
- readme と照らした（Frame/Field Coding、Number Slice Groups、解像度の欄の 4:2:2 など）: in は readme のある物が全部 Frame、slice group 1。out:format は readme の 4:2:2・Monochrome、FREH10-1・-2 は readme が無く bitstream の名前が 10bit。out:interlace は readme が Field・MB-AFF・PAFF など。
  readme が Frame と書くのに interlace で拒んだ CVMP_MOT_FRM_L31_B と、readme が「Frame Coded」（HPCA の readme の写し）の HPCAMAPALQ_BRCM_B・HPCVFL_BRCM_A・HPCVFLNL_BRCM_A、readme の無い TOSHIBA の 8 本などは、ffmpeg の trace_headers で field_pic_flag 1 か mb_adaptive_frame_field_flag 1 があることを確かめた（readme の誤り。decoder の分類は正しい）。
- design.md §7 の機能ごとの in の stream（readme・ffmpeg の trace_headers・JM の trace で使っていることを確かめた）: CAVLC の Baseline（BA_MW_D・BA1_Sony_D ほか）、CABAC の Main（CABA1〜3 ほか）、High の 8x8 変換と scaling matrix（FRExt1_Panasonic_D: 8x8 only・Sequence と Picture の Qmatrix、FRExt3_Panasonic_E、FRExt_MMCO4_Sony_B、HCAFR1〜4）、weighted prediction の explicit（CVWP1・CVWP5・CAWP1・CAWP5 は weighted_pred_flag 1、CVWP2 は weighted_bipred_idc 1）と implicit（CVWP3: weighted_bipred_idc 2）、B の temporal direct（CABA3_SVA_B ほか、direct_spatial_mv_pred_flag 0）と spatial direct（CANL4_SVA_B・BA3_SVA_C、flag 1）、MMCO と long-term（MR2_TANDBERG_E・MR3〜5_TANDBERG・MR2_MW_A・FRExt_MMCO4_Sony_B）、複数 slice（SL1_SVA_B ほか）と ASO（上の --aso）、PCM（CAPCM1_Sand_E・CAPCMNL1_Sand_E: JM の trace に PCM の sample）、frame cropping（CVFC1_Sony_C）。全部 1 本以上ある。
- p021 からの引き継ぎ（frame_num の gap での出力の順）: MR9_BT_B は interlace で out。代わりに readme が「Gaps in frame_num: Yes」の MR3_TANDBERG_B・MR4_TANDBERG_C・MR5_TANDBERG_C（各 300 frame）が全 frame・出力の順で一致した。

### 誤りの入力

- `decode-stream --fuzz <stream> 20261031 100`（p021 の道具、ASan・UBSan で build）を適合の 13 本の in の stream に流した: CABA3_SVA_B、BA3_SVA_C、BAMQ1_JVC_C、CABA1_Sony_D、FRExt1_Panasonic_D、MR3_TANDBERG_B、CAPCM1_Sand_E、SL1_SVA_B、HCBP2_HHI_A、FRExt3_Panasonic_E、HCAFR1_HHI_C、MR2_TANDBERG_E、CVWP3_TOSHIBA_E。全部 `PASS (100 cases ...)`、sanitizer の報告 0。IDR が繰り返す FRExt1_Panasonic_D では 62 通りで次の IDR の後が元の decode と一致。
  out の CAPAMA3_Sand_F にも流し、100 通りで落ちず sanitizer の報告 0（元の decode が拒まれるので「whole file decodes」の 1 check だけが FAIL、想定どおり）。
- run.sh の全体の run も ASan・UBSan（halt_on_error、leak の検出）の build で、拒む 103 本を含めて報告 0。

### 確かめの command

- `BASE=8ddb130a9 timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
- zedBSD amd64 の `make ... build/amd64/dynamic/libmedia.so`（確かめの command のとおり）→ exit 0、出力の `warning` 0 件（source は変えていないが流した）。
- conform.c は cc と clang の `-std=c89 -Wall -Wextra -Werror` で warning 0。`git diff --check` → 問題なし。

### 未実施・残り

- zedBSD の上（QEMU・実機）での適合の bitstream の decode は未実施（host だけ）。速さは測っていない（p024）。
- 適合の bitstream の cache `/tmp/o-ws326-p022/conformance`（約 7 GB、tmpfs）は次の試験（p026 の前後の一致）でも使える。要らなくなったら Q1 が消す。
- 要らなくなった出力（この worktree の中）: `build/tmp/ws326-conform.run.*` の古い run、`build/tmp/ws326-conform-dev/`、`build/tmp/conform-full.log`・`fuzz2.log`・`sha-sorted.txt`・`lastcommit.txt`。

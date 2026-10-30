<!-- awesome-plan project=zedbsd record=ws326-p024 -->

# ws326-p024 速さの設計と最初の最適化

- 親: [WS326](../ws.md)、設計 [design.md](../design.md)。
- 状態: 実装済み・試験 PASS（2026-10-31 P1、q1773-i01。設計は下の「設計」、最初の最適化（案 1 と案 2 の bS）の結果は「結果」。cleared は Q1）。
- 依存: p022、p023（依存の Phase の実装が main に入ってから始める）。

## 読む物

1. AGENTS.md の §4（担当の共通の規則）・§7・§9・§10、plan/agents/protocol.md
2. plan/coding-style.md の全文（ANSI C: 宣言は関数の先頭、`for` の中で宣言しない、if・loop・return ごとの comment、関数の呼び出しを条件に書かない、など。§14 の checklist で自分の diff を見直す）
3. plan/ws326/design.md の §1（出典の規則）・§2（決定）・§4（所有と寿命）・§5（単体試験の作り方）
4. `userland/desktop/libmedia/h264sw.h`（誤りの値の約束と上限）
5. 仕様: ITU-T H.264 (08/2021) の design.md §8。PDF `/home/awe/zedBSD-claude1/build/sources/specs/itu-t-h264-202108.pdf`（SHA-256 `d6e09bcc94e5d095e1b2d6d16bf96d82a593d67915a963336ada082a775e9cdf`、git に入れない）。印刷の頁 N = PDF の頁 N + 22。表と行列は PDF の頁の画で読む（Read の tool の `pages`、1 回 20 頁まで）。`pdftotext -layout` の文字は列が崩れることがある。

## 作る物

- この phase.md の「結果」の節と、[ws.md](../ws.md) の Phase の表のこの行の状態。

## 試験の項目

- 設計の Phase（Opus）: 計測の script（1 本、配置 → 実行 → 要約）と目標（Pi 5 で 1080p30 High）、案（表引き・SIMD・thread）を決め、後の Phase に割る。
- 共通: 誤りの入力（範囲外・途中で切れた・巨大な値）で落ちず、header に書かれた誤りを返す。ASan・UBSan の報告が 0。

## 注意

- この行は p022・p023 の後に Q1 と見直す。

## 設計（2026-10-31 P1、q1773-i01）

### 1. 計測の道具

- `sh plan/ws326/tests/speed/run-speed.sh [FILE...]`（`speed-bench.c` と 1 本、host で build → stream を作る → ffmpeg と一致の確かめ → 時間 → profile → 要約）。
  - build: decoder の module（`h264sw-*.c`・`vcodec-*.c`、backend を除く）と `bits.c` を libmedia の build と同じ `-O2`（`SPEED_CFLAGS` で替える）、sanitizer 無し、1 thread。
  - stream: host の ffmpeg（libx264、preset medium、240 frame、1080p30）で 3 本。`high-1080-grain`（High、CABAC・B 3・8x8・weightp、粒の有る絵を 8 Mbps、entropy の重い形）、`high-1080-clean`（High、CRF 23、MC の重い形）、`main-1080-cavlc`（Main の CAVLC、8 Mbps）。引数の file（Annex B か、MP4・MKV・TS を ffmpeg が decode せずに Annex B に直す）も同じに測る。
  - 確かめ: 各 stream の出力の I420 の全体の MD5 が ffmpeg と一致（違えば FAIL、時間は測らない）。
  - 時間: `SPEED_RUNS`（既定 5）回、新しい decoder で全部を decode。NAL の探索・decode・出力の取り出しを含み、file の読みを含まない。best と median の frame/s。
  - profile: `SPEED_PROFILE_STREAM`（既定 high-1080-grain）を perf（`perf_event_paranoid` が 3 の host は `sudo -n perf record`、data は chown して利用者の物に）で、無ければ gprof。関数ごと（self）と module ごと（object の symbol で対応）の割合。
  - 出力: `SPEED_OUT`（既定 `/tmp/ws326-speed`、`fresh_out` の新しい directory）。要約は `summary.txt`、最後の行 `run-speed: PASS`。
- 計測の揺れ: host は他の担当と共有（64 core）。同じ binary で median は ±10% 揺れた（Video の 14.0〜16.3 fps）。前後の比較は同じ時に続けて流し best で見る。

### 2. 基準（2026-10-31、main 8085f5af0、host の Xeon Gold 6130（Skylake-SP、2.1 GHz・turbo 3.7 GHz）、gcc 14.2 -O2、1 thread）

| stream | 内容 | frame | best fps | median fps |
| --- | --- | --- | --- | --- |
| high-1080-grain | High CABAC B 8x8 weightp、7.9 MB | 240 | 16.55 | 16.50 |
| high-1080-clean | High CRF 23、6.4 MB | 240 | 19.08 | 17.05 |
| main-1080-cavlc | Main CAVLC、8.0 MB | 240 | 17.52 | 17.43 |
| 実 stream A（host の `/home/awe/Video.mp4`、git の外） | Main 1080p24 8 Mbps | 149 | 16.30 | 15.89 |
| 実 stream B（host の `/home/awe/zedbsd-media/sample-h264-high.mp4`、git の外） | High 1080p30 13 Mbps、full range | 150 | 12.11 | 11.97 |

- 全部が ffmpeg と全 frame 一致、concealed 0、誤り 0（`SPEED_RUNS=3`、出力 `/tmp/ws326-speed.run.fu5Ewd`）。
- 参考（同じ host、実 stream A、`ffmpeg -threads 1 -f null`）: libavcodec の asm 有り 142 fps、`-cpuflags 0`（C だけ）88 fps。速さの目安にだけ使い、code・構造は読まない（design.md §1）。

### 3. profile（perf、self time、module ごと %）

| module | grain | clean | cavlc | 実 A | 実 B |
| --- | --- | --- | --- | --- | --- |
| interp | 32.8 | 34.1 | 34.2 | 35.3 | 29.8 |
| deblock | 25.1 | 23.5 | 26.6 | 21.9 | 22.6 |
| transform | 8.0 | 2.9 | 5.8 | 3.9 | 12.4 |
| mb（`h264sw_mb_locate`・`h264sw_blk4x4_at`） | 6.6 | 8.6 | 7.6 | 9.8 | 6.9 |
| recon | 6.3 | 6.3 | 6.3 | 5.5 | 5.3 |
| weight | 5.3 | 10.3 | 7.5 | 5.3 | 4.9 |
| entropy（cabac＋cabac-se、cavlc＋bits） | 10.1 | 8.5 | 5.6 | 11.8 | 12.2 |
| 残り（macroblock・direct・mvpred・intra など。clean・cavlc・実 A・B は deblock の clone の関数の約 2〜3% もここ） | 5.8 | 5.8 | 6.4 | 6.5 | 5.9 |

関数の上位（grain）: `h264sw_interp_luma` 17.5、`h264sw_deblock_bs` 9.6、`h264sw_interp_chroma` 8.7、`h264sw_deblock_luma_edge` 8.6、`h264sw_cabac_decision` 5.7、`h264sw_deblock_chroma_edge` 4.1、`h264sw_weights_apply` 3.4、`h264sw_mb_locate` 3.3。
画素の処理（interp・deblock・weight・transform・recon）が約 75%、entropy は約 10%。今の 1 thread の遅さは画素の処理の書き方による。

code を読んで分かった原因:

- interp の luma: 1 sample ごとに Table 8-12 の switch を回し、j（中央の半画素）は 1 sample に 6x6 の tap を毎回計算する。窓（21x21）の取り込みは 1 sample ごとに座標を clamp（int64）。
- interp の chroma: 1 sample ごとに 4 点の座標を clamp（int64）。
- deblock: `h264sw_deblock_bs` を edge ごとに呼び、chroma の edge でも同じ bS を計算し直す。中で `h264sw_blk4x4_at`（別の file の関数）を 8 回、`motion_differs` を呼ぶ。
- mb: neighbour の位置（6.4.12）の導出 `h264sw_mb_locate` を recon・mvpred・cabac-se・intra・direct が毎 block 呼ぶ。
- weight: 重みの無い（default）予測も一般の式で 1 sample ずつ。
- transform: 係数ごとに int64 の clamp、全部の 4x4・8x8 で完全な逆変換（DC だけの block も）。
- 別の file の小さな関数（`h264sw_cabac_decision`・`h264sw_blk4x4_at` など）は inline されない（header を変えず、build の flag も変えないので）。

### 4. 目標

- **Pi 5（Cortex-A76 4 core、2.4 GHz）で 1080p30 の High（8 Mbps 級、x264 の preset medium の B・CABAC・8x8）を落とさず再生**。decoder だけで 4 thread で 40 fps 以上（表示・音・app に 1 core の余り）。
- **5330（Alder Lake の U、i915。Vulkan Video の hardware が有るので software は fallback）で 1080p60 の High**、2 thread 以内。
- 換算（推定、未実測）: Pi 5 の 1 core ≈ この host の 1 thread の 0.6 倍（A76 2.4 GHz と Skylake-SP の turbo の比）、5330 の P-core ≈ 1.3 倍。実機の実測で置き換える（§5 の p031 案）。
- 段の目標（host の 1 thread、high-1080-grain の best）:

| 段 | 内容 | host 1 thread | Pi 5（推定） | 5330（推定） |
| --- | --- | --- | --- | --- |
| 今 | — | 16.5 fps | 1 core 10 fps | 1 thread 21 fps |
| A | C の書き直し（画素の処理の block 化・fast path・表引き・重複の計算の削除） | 50 fps 以上（3 倍） | 1 core 30 fps | 1 thread 65 fps（1080p60 を満たす） |
| B | thread（deblock の行の pipeline、parse と再構成の分離と行の wavefront、または frame の並列） | 3 thread で 120 fps | 4 thread 60〜70 fps（30 の 2 倍の余裕） | — |
| C（必要なら） | SIMD（arm64 の NEON・amd64 の SSE2）: interp・deblock・逆変換・weight | 1 thread 80 fps | 1 core 50 fps | — |

段 A で Pi 5 の 1 core が 1080p30 に届くかは境い目なので、Pi 5 の目標は段 B で満たす計画。段 C は段 B の実測が 40 fps に届かない時だけ。

### 5. 案（効く順、段 A は 1 thread・bit 単位で同じ出力）

| # | 案 | 対象（今の割合） | 見込み |
| --- | --- | --- | --- |
| 1 | interp の luma を block 単位に: 窓が plane の内なら clamp 無しで plane から直に読む fast path、外だけ今の窓。Table 8-12 の位置を block に 1 回選び、b・h・j を行・列の pass で中間値（int16/int32 の行）から作る（j は 6 tap の 2 回の pass）。chroma も内なら clamp 無し、frac 0 は複写 | interp 30〜35% | interp を 1/4 以下、全体 1.3〜1.4 倍 |
| 2 | deblock の bS を macroblock ごとに 1 回（縦 4・横 4 の edge）求めて luma と chroma で使い回す。内側の edge は neighbour 無しの fast path、4x4 の番号は file の中の表。bS が全部 0 の edge は filter を飛ばす。edge の alpha・beta・tC0 を edge に 1 回 | deblock 22〜27% | deblock を 1/2、全体 1.15 倍 |
| 3 | weight: default の単方向は行の複写、双方向は平均の専用の loop。explicit だけ今の式 | weight 5〜10% | 全体 1.05 倍 |
| 4 | mb の neighbour: macroblock の内側の block の neighbour は表から直に（`h264sw_mb_locate` を呼ばない）、外側の macroblock は slice の context に 1 回 | mb 7〜10% | 全体 1.05〜1.08 倍 |
| 5 | transform: DC だけの block は定数の足し込み、範囲が保証される所の clamp を外す（壊れた stream で未定義の動作をしない範囲は保つ）。macroblock の syntax の 3 KB の memset を、書いた block だけ消す形に | transform 3〜12%、macroblock 3% | 全体 1.05〜1.1 倍 |
| 6 | entropy: CABAC の decision・bypass の file の中の fast path、residual の block の significance の表引き。別の file の関数の inline は LTO（libmedia の `-flto`）か header の変更が要る → Q1 の判断（今の Phase では行わない） | entropy 8〜13% | 全体 1.05 倍 |
| 7 | thread（段 B）: (a) deblock を行の遅れで別の thread（picture の終わりの全体の pass を行の pipeline に）、(b) parse（entropy、slice の中は直列）と再構成（行の wavefront、右上の macroblock を待つ）の分離（macroblock の syntax を picture の大きさで持つ、約 6 MB）、(c) frame の並列（参照の行の待ち）。(a)+(b) を先に。thread は WS318 の `thread.c`（名前の付いた thread の台帳）を使う。WS318 の thread.c の前なら同じ規則（名前・本数・signal の block）で libmedia の中に | — | 4 core で 3 倍前後 |
| 8 | SIMD（段 C）: interp の 6 tap・deblock の filter・逆変換・weight。arm64 の NEON と amd64 の SSE2 を compile の時に分け、C の道を正として単体試験で一致 | — | 画素の処理を 2〜4 倍 |

出力の picture への複写（1080p で 3 MB/frame）の削減は WS318 p006a（ws.md の 2026-10-31 の引き継ぎ）、この Phase の外。

### 6. 後の Phase への割り（案。番号は付けない。1 Phase = 1 回の起動）

2026-10-31 Q1: WS326 は p024 の最初の最適化（案 1、時間が有れば案 2 の bS）と p025（規約の見直し）で閉じる。下の続きは案のまま置き、別の WS にするかは Q1 がユーザーと決める。LTO・header の変更は今はしない。

| 案の Phase | 内容 | 担当の案 | 試験 |
| --- | --- | --- | --- |
| p024（この Phase） | 計測・設計（済み）と最初の最適化: 案 1（interp の luma と chroma）、時間が有れば案 2 の bS | Opus | run-unit・check-skeleton・run-stream・run-host-decode・適合が前と同じ、run-speed の前後 |
| 続き a | 案 2 の残り（deblock の filter）と案 3（weight） | Sonnet 5.5 high（標準廉価実装手順: header を変えず、1 module の .c の中、単体試験と全体の試験が oracle） | 同上 |
| 続き b | 案 4（neighbour）・案 5（transform・syntax の消し）・案 6 の file の中の部分 | Sonnet 5.5 high（mb・transform・macroblock・cabac は別々の .c、並べてよい） | 同上 |
| 続き c | 段 B の thread の設計と実装（案 7 の (a)(b)）。decoder.c の流れ・slice の context の持ち方を変える | Opus（WS318 の thread.c の状態を Q1 と確かめてから） | 同上と ThreadSanitizer、run-speed の thread の数ごと |
| 続き d | 段 C の SIMD（必要な時だけ） | Opus か Sonnet（module ごと） | 同上と arm64 の build、QEMU の aarch64 の単体試験 |
| 続き e | Pi 5（Linux の上で同じ speed-bench を build）と 5330 の実測、換算の置き換え、目標の判定 | Q1 が実機を割り当てる担当 | run-speed を実機で（1 本の script） |

- 依存: a・b は p024 の後（file が別なので並べてよい）、c は a・b の後（画素の処理が速くなった後の割合で thread を決める）、d・e は c の後。
- 変えてはいけない物（各 Phase 共通）: `userland/desktop/libmedia/h264sw*.h`。`vcodec*.h`（`vcodec.h`・`vcodec-bitreader.h`・`vcodec-nal.h`・`vcodec-cabac.h`、p026 から）も。check-skeleton の BASE は 92ca2d0e8（p026 の後）。
- 段 A の各 Phase の受け入れ: 出力が bit 単位で同じ（run-stream・run-host-decode・適合の in 101 本が全 frame 一致、単体試験の数が同じか増える）、run-speed の best が前より上がる（数を phase.md に）。

## 結果（2026-10-31 05:59 P1、q1773-i01）

- 基点: main 8085f5af0 から。p026 の merge（main a058c08d7）を 94b21eff8 で取り込んだ後に最適化を入れた。
- 変えた file: `userland/desktop/libmedia/h264sw-interp.c`（案 1）、`h264sw-deblock.c`（案 2 の bS）、`plan/ws326/tests/speed/`（計測、p026 の改名 `vcodec_nal_find` に追従）。header は変えていない。
  - interp: 窓（luma は前 2・後 3、chroma は後 1）が plane の内なら plane から直に読み、外なら列の clamp を 1 回に纏めた窓へ写す。luma は Table 8-12 の位置を block に 1 回選び、b・h・j を block 単位に作って（j は行ごとに縦の中間値を width + 5 列作ってから横の 6 tap）、四分の一の位置は 2 つの block の平均。chroma は内なら clamp 無しの loop。
  - deblock: macroblock ごとに縦横 8 本の edge の bS を 1 回求め（`edge_strengths`、4x4 の番号は file の中の表）、luma と chroma の edge で使い回す。bS が全部 0 の edge は飛ばす。`h264sw_deblock_bs`（公開、単体試験が使う）は同じ `edge_strengths` を呼ぶ。filter の順と式は変えていない。
- 確かめ（worktree、94b21eff8 ＋ 上の変更）:
  - `BASE=92ca2d0e8 timeout 300 sh plan/ws326/tests/check-skeleton.sh` → `check-skeleton: PASS`。
  - `sh plan/ws326/tests/run-unit.sh all` → `run-unit: PASS`、24 本、各本の checks の数は前と同じ（test-interp 23460・test-deblock 3313・test-recon 33584 など、test-bits は p026 で test-bitreader 169）。
  - `sh plan/ws326/tests/run-stream.sh` → `run-stream: PASS`（22 本 408 frame が ffmpeg と一致、壊した stream 200 通り・開始 63 通り PASS、前と同じ）。
  - `sh plan/ws326/tests/backend/run-host-decode.sh` → `run-host-decode: PASS`（5 本 各 50 picture、h264-software）。
  - `H264SW_CONFORMANCE_DIR=/tmp/o-ws326-p022/conformance sh plan/ws326/tests/conformance/run.sh` → `conform-run: PASS (101 in, 103 out)`、in 101 本 14526 frame が参照と一致、slice を逆順の 6 本も一致（前と同じ）。
  - 旧 interp との差分の乱数試験（worktree の外の使い捨て、ASan・UBSan、plane 1〜60、座標 ±25 と INT_MIN・INT_MAX の近く、block 0〜17、frac 0〜7）200 万件で出力の差 0。
  - run-unit all と run-stream を流し始めた後に h264sw-deblock.c に comment だけを足した（if・loop の意図）。その後に check-skeleton と `run-unit.sh deblock interp recon` を流し直し PASS（数は同じ）。run-host-decode・適合・run-speed は comment の後の file で流した。
  - zedBSD amd64 の `build/amd64/dynamic/libmedia.so`（確かめの command の make）→ exit 0、warning 0。`git diff --check` → 差無し。cc と clang の `-std=c89 -Wall -Wextra -Werror -Wpedantic` で 2 file とも warning 0。
- 速さ（`run-speed.sh`、host の Xeon Gold 6130、-O2、1 thread、各 5 回の best、同じ stream の file。前 `/tmp/p1-speed-before.run.pldTbf`、後 `/tmp/p1-speed-after.run.8Nix0m`、両方 ffmpeg と全 frame 一致）:

| stream | 前 fps | 後 fps | 倍 |
| --- | --- | --- | --- |
| high-1080-grain（High CABAC B 8x8、8 Mbps） | 16.71 | 24.13 | 1.44 |
| high-1080-clean（High CRF 23） | 19.72 | 36.25 | 1.84 |
| main-1080-cavlc（Main CAVLC、8 Mbps） | 17.30 | 27.30 | 1.58 |
| 実 stream A（Main 1080p24、8 Mbps） | 18.44 | 35.69 | 1.94 |
| 実 stream B（High 1080p30、13 Mbps） | 13.02 | 17.54 | 1.35 |

- 後の profile（grain、module ごと）: deblock 24.5%（luma の filter 11.1%、`edge_strengths` 5.2%）、interp 15.7%、transform 11.7%、vcodec-cabac 9.6%、recon 8.9%、weight 8.2%、mb 7.5%。次に効くのは deblock の filter・weight・neighbour・transform（§5 の案 2 の残り〜5、§6 の続き a・b）。
- 段 A の目標（grain で 50 fps）まで残り約 2 倍。Pi 5・5330 は未実測（§4 の換算は推定のまま）。
- 未実施: QEMU・実機（Pi 5・5330）での計測、Video Player での再生の確かめ、arm64 の build。

## 変えてはいけない物

- `userland/desktop/libmedia/h264sw*.h`（全部）と `vcodec*.h`（`vcodec.h`・`vcodec-bitreader.h`・`vcodec-nal.h`・`vcodec-cabac.h`、p026 から）。header の不備・矛盾・足りない口を見つけたら、直さずに止めて Q1 に返す: どの header のどの関数か、何が足りないか、案の差分。
- 担当の module 以外の `h264sw-*.c`（他の Phase の物）。既存の libmedia の file（bits.c・h264.c・vkvideo.c・decoder.c など）、Makefile、`plan/ws326/tests/run-unit.sh`・`check.h`・`bitwriter.h`・`check-skeleton.sh`（不備は Q1 へ）。
- HAL・toolchain・共有の build/（AGENTS.md §7）。rm などの削除をしない。
- 上の「注意」がこの Phase に明示に許す物だけは、この節の限りでない。

## 確かめの command（worktree の root で）

- `BASE=92ca2d0e8 timeout 300 sh plan/ws326/tests/check-skeleton.sh`（2026-10-31 p026 の merge 2a13ccfc5 の後の BASE） → `check-skeleton: PASS`（header が p001 の物から変わっていない、cc と clang で warning 0）。
- zedBSD amd64: `timeout 1200 make ZEDBSD_CONFIG=config/ci/config-amd64.mk ZEDBSD_TARGET_LLVM_BIN=/home/awe/zedBSD-claude1/build/llvm/bin ZEDBSD_SYSROOT_AMD64=/home/awe/zedBSD-claude1/build/amd64/sysroot -o /home/awe/zedBSD-claude1/build/amd64/sysroot/.zedbsd-sysroot-complete -j8 build/amd64/dynamic/libmedia.so` → exit 0、出力に warning が 0。
- `git diff --check`。

## 終わりの条件

- 上の command が全部 PASS（出力を自分で読んで確かめる）。
- 自分の diff を coding-style の §14 の checklist で見直した。表の block ごとに `Values referenced: ITU-T H.264 (08/2021) Table ...` の複数行の comment がある。source の comment・log に WS・Phase の番号や plan/ への参照が無い。
- `git commit -m WIP -- <変えた path>`（push しない）。SHA・実行した command と結果・未実施・残りを Q1 に返す。
- この phase.md に「結果」の節（日付は host の `date`、実行した command、PASS の数、未実施）を足し、ws.md の表のこの行を「実装済み・試験 PASS」にする（cleared は Q1 が受け入れを見て決める）。

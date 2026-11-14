<!-- awesome-plan project=zedbsd record=ws338-design -->

# WS338 設計: H.264 の software の decoder を速くする

- 親: [ws.md](ws.md)。p001（2026-11-15 P1、q2121-i01、基点 main 08ad5228d9）。
- 前の設計: WS326 p024（`git show 684d0512b8^:plan/ws326/phase024/phase.md`、phase.md は完了で消えた）の案 a〜e。WS326 の module の設計は [../ws326/design.md](../ws326/design.md)（§1 の出典の規則はこの WS の全 Phase に効く）。
- ユーザーの判断を待つ点: [user-questions.md](user-questions.md)（Q338-1〜3）。敵対的レビューと反映: §11。
- 計測の要約（/tmp の出力の写し）: [measure/](measure/)。

## 1. 決まった事

- **build の flag（2026-11-15 ユーザーの決定、Q1 経由）**: userland の program と shared library の両方を `-O2 -Wall -Wextra -Werror` にする（`userland/base/package.mk`）。Q1 が 3 機種の全 userland の build で warning 0 を確かめてから main に当てる。この設計は libmedia が -O2 で build される前提で、受け入れの数も -O2 で取る。
  - 経緯: p001 の乖離の確かめで、shared library の object の flag（`STANDALONE_SHLIB_CFLAGS`）に `-O` も警告の flag も無く、libmedia.so が -O0 で compile されていることを見つけた（x86_64 の `make V=1` の出力で確かめた）。host の同じ source で clang -O0 は -O2 の 1/4.2〜1/4.4（§3.1）。
- 出典（WS326 design.md §1）: 他の実装（FFmpeg・openh264・JM・x264 など）の code・comment・構造・名前を写さない。仕様（ITU-T H.264 (08/2021)）と自分の profile から導く。CABAC の renormalization、deblock の転置、行の thread の形は他の実装に典型の形がある所なので特に。
- 置き場: 新しい `.c` は足さない（sources.mk は make の file で承認が要り、Windows の browser の Makefile も読む）。既存の module の `.c`・`.h` に置く。
- thread の数・口は技術の判断（§7）。公開の API（`media.h`・`media-decoder.h`・exports.map）は変えない。

## 2. 乖離の確かめ（記録 → 2026-11-15 の main）

| # | 記録 | 今 | 扱い |
| --- | --- | --- | --- |
| 1 | WS326 p024 の計測は host の gcc -O2 | zedBSD の libmedia.so は -O0 だった（§1）。Windows の browser の build（`apps/browser-win32/Makefile`）は libmedia を -O2 -Wall -Wextra -Werror で compile している。check-skeleton も h264sw・vcodec を `-Wall -Wextra -Werror -Wpedantic` で compile する | §1 の決定で解消 |
| 2 | zedBSD の compiler は clang（`build/llvm/bin/clang` 23.1.0） | p024 の数は gcc 14。host の clang（Debian 19.1.7）-O2 は gcc -O2 の 1.10〜1.14 倍 | 受け入れの数は clang -O2（p002 で run-speed に compiler の指定を足し、zedBSD の clang 23 で測る） |
| 3 | ws.md の 2026-11-01 09:30 の Q1 の記録: SIMD は WS329 の 11-01 の答えと同じ形 | WS329 の 11-01 の答えは 2026-11-14 に取り消され、encoder は SIMD を範囲の外にした | 今の方針と矛盾。Q338-1 でユーザーに聞く |
| 4 | WS326 の試験は `plan/ws326/tests/`、BASE 92ca2d0e8・77ed48ac4 | `plan/tools/h264sw/`、BASE b753848bf2（WS329 p002） | phase.md は今の path |
| 5 | 適合の stream の cache は main の `build/sources/h264-conformance/` | main の `build/` から消えていた。`plan/tools/h264sw/conformance/fetch.sh` で自分の worktree の `build/` に取り直す（204 本、展開後 約 7 GB） | phase.md に手順 |
| 6 | p024: thread は WS318 の `thread.c` | `thread.c` は無い。`media.h` の `struct media_threads`（名前付きの thread を呼び手が作る口、engine の thread だけが使う）と `media-os.h` の thread・mutex・cond（POSIX と Win32、cond の待ちは時間の上限付きだけ） | §7 |
| 7 | 11-01 以後 h264sw-* に速さの変更は無い | 正しい。変更は WS329 p002（b753848bf2、CAVLC・CABAC の表と context の導出の共通化）だけ。intrinsics・vector 型・decoder の thread は無い | — |
| 8 | WS326 の段 A の目標は grain で 1 thread 50 fps | 目標は Pi 5 の 4 thread（ゲート）で決める。§4 の 1.6 倍（clang の今の数で約 40）を受け入れ、2 倍を狙い | §9 |
| 9 | WS329（encoder）の設計は `h264sw_recon_mb`（動き＋画素）・`h264sw_deblock_picture`・`h264sw_slice_ctx` を使う（plan/ws329/proposed/） | WS329 p003 以後はまだ main に無い | §6 で今の口を互換のまま残す |

## 3. 計測（host、2026-11-15、1 thread）

host: Xeon Gold 6130（他の担当と共有、load 8〜12、同じ binary の揺れ ±5〜10%）。stream は `run-speed.sh` が作る 3 本と実 stream 2 本（git の外: A `/home/awe/Video.mp4` Main 1080p24 8 Mbps、B `/home/awe/zedbsd-media/sample-h264-high.mp4` High 1080p30 13 Mbps）。全部 ffmpeg と全 frame 一致、concealed 0。要約は [measure/host-base-summary.txt](measure/host-base-summary.txt)、[measure/host-flags.txt](measure/host-flags.txt)。

### 3.1 速さ（best fps）

| stream | gcc -O2（run-speed、3 回） | 比較（同じ時、3 回、[measure/flags.sh](measure/flags.sh)） |
| --- | --- | --- |
| high-1080-grain（High CABAC B 8x8 weightp 8 Mbps、x264 medium） | 21.99 | gcc -O2 22.53、**clang 19 -O2 24.86**、clang -Os 19.61、**clang -O0 5.63** |
| high-1080-clean（High CRF 23） | 33.67 | — |
| main-1080-cavlc（Main CAVLC 8 Mbps） | 25.69 | — |
| 実 A（Main 1080p24） | 33.38 | — |
| 実 B（High 1080p30 13 Mbps） | 16.83 | gcc -O2 16.01、**clang -O2 18.27**、clang -Os 14.42、**clang -O0 4.40** |

- WS326 p024 の後の数（grain 24.13・B 17.54、gcc -O2）より grain が 9% 低い。揺れと WS329 p002 の context の導出の関数化の両方がありうる。p004 以後は同じ時に前後を続けて測る。

### 3.2 profile（perf、self time、gcc -O2、[measure/host-profile-B.txt](measure/host-profile-B.txt)）

| module | grain | 実 B |
| --- | --- | --- |
| deblock（luma の filter 10.8・`edge_strengths` 3.6〜4.9・chroma 4.1〜4.6・edge の loop） | 23.8 | 21.4 |
| interp（`h264sw_interp_luma` 6.8〜7.9・chroma 4.1〜5.3・縦の 6 tap 3.5〜4.0・中央 1.3〜3.1） | 16.9 | 19.0 |
| transform（`transform_line8` 3.0〜6.3・line4・scale・idct・add_residual） | 11.1 | 15.0 |
| CABAC の engine（`vcodec_cabac_decision` 8.4〜9.0） | 9.0 | 10.0 |
| recon | 8.7 | 6.8 |
| weight | 8.1 | 6.4 |
| mb（`h264sw_mb_locate` 4.6〜4.8、`h264sw_blk4x4_at`） | 7.2 | 6.8 |
| cabac-se | 5.7 | 5.6 |
| macroblock | 4.3 | 4.4 |
| direct・mvpred・intra・decoder・他 | 4.2 | 3.9 |

- 画素の処理（deblock・interp・transform・recon・weight）約 70%、entropy と syntax 約 19〜20%、neighbour と動きの導出 約 10%。後の 2 つ（約 30%）は slice の中で直列（§7 の P の段）。

### 3.3 compiler の自動の vector 化（clang 23 -O2、armv8-a と x86-64、[measure/vectorize-remarks.txt](measure/vectorize-remarks.txt)）

| module | vector 化された loop | されない loop |
| --- | --- | --- |
| interp | 41〜42（多くは幅 16、interleave 1〜2） | 2 |
| intra | 11〜16 | 2 |
| transform | 4〜8（幅 4） | 9 |
| weight | 3〜4 | 0 |
| deblock | 0 | 6 |
| recon | 0 | 13 |

- remark が出ても、vector の幅 × interleave が block の幅（4・8・16、chroma 2・4・8）より大きいと、vector の本体が走らず残りの scalar だけが走る。interp の 8 と 4 の幅の block は今そうなりうる。§5 の確かめはこれを見る。

### 3.4 Pi 5 の見積もり（未実測）

Pi 5 の 1 core を host の 1 thread の 0.6〜0.75 倍と置く（clock と IPC の比、推定）。p003 の実測で置き換える。

| 段 | 実 B | grain |
| --- | --- | --- |
| -O2（§1） | 11〜14 fps | 15〜19 fps |
| ＋ 1 thread の書き直し（§5、1.6 倍） | 18〜22 | 24〜30 |
| ＋ 4 thread（§7、2.4 倍） | 42〜53 | 57〜72 |

## 4. 手段の全体

| 段 | 手段 | Phase |
| --- | --- | --- |
| 0 | build の -O2（§1、Q1 が当てる） | WS の外（前提） |
| A | 1 thread の C の書き直し（bit 単位で同じ出力） | p004〜p007 |
| B | 段の分離（1 thread のまま） | p008 |
| C | thread（picture の中の段の pipeline） | p009 |
| D（条件付き） | picture の重なり（次の picture の P を前の picture の R・D と重ねる） | p011 |
| E（条件付き、Q338-1） | short vector 型の SIMD | p012 |

## 5. 段 A: 1 thread の C の書き直し

各 Phase の確かめ（共通）:
- 出力が bit 単位で同じ（§9 の正しさの項目）。
- 書いた hot な loop が clang 23 -O2 の armv8-a と x86-64 で vector 化され、**vector の幅 × interleave が実際の block の幅以下**（block の幅ごとの loop か、回数が定数の loop にする）。Q338-1 が C なら外す。
- module の**時間**（割合でなく、perf の self の samples か計時）を前後で同じ時に測る。

| Phase | module | 手段 | 目標（module の時間、host、clang -O2、1 thread） |
| --- | --- | --- | --- |
| p004 | deblock.c | (1) 横の edge（16 列が行に沿って並ぶ）は 16 列を 1 つの loop で、分岐の無い形（filter の要否と bS<4・bS=4 の結果を mask で選ぶ）に。(2) 縦の edge は 16 行×8 列（chroma 8×4）を作業の block に転置して (1) と同じ loop を通し、戻す。(3) bS: macroblock の係数の有無の 16 bit の mask、motion の比較は同じ partition の組を飛ばす。(4) alpha・beta・tC0 は edge に 1 回、bS が全部 0 の edge は飛ばす。header は変えない | 0.5 倍以下 |
| p005 | interp.c・weight.c | (1) `h264sw_interp_luma` の呼び出しあたりの固定費（位置の選び・窓の判定・中間の複写）を減らす。(2) chroma の bilinear を幅（2・4・8）ごとの loop に。(3) weight の default の平均・explicit の式を行の loop で vector 化。口（interp.h・weight.h）と recon.c は変えない | 2 module で 0.6 倍以下 |
| p006 | transform.c・mb.c | (1) 逆変換の列の pass を「列を lane にした行の演算」に書き、行の pass と合わせて vector 化。(2) DC だけの block の速い道。範囲が保証される所の clamp を外す（壊れた stream で未定義の動作をしない範囲は保つ）。(3) `h264sw_add_residual` を行の loop に。(4) `h264sw_mb_locate`: 内側の 4x4・8x8 の neighbour は file の中の表。口は変えない（recon.c の chroma の residual は p008 で） | 2 module で 0.6 倍以下 |
| p007 | vcodec-cabac.h/.c・h264sw-cabac.h/.c・h264sw-cabac-se.c・h264sw-macroblock.c | (1) decision の速い道を `vcodec-cabac.h` の `static __inline` の関数に、`h264sw_cabac_decision`（ctx_idx を検査する wrapper）も `h264sw-cabac.h` の inline に（file をまたぐ呼び出しを消す）。(2) residual の significance の loop の context の index を表から。(3) macroblock の syntax の消しを書いた block だけに。header を変えるので BASE が移る（Q1 が WS329 と順を決める、§10） | entropy＋syntax で 0.7 倍以下 |

- 段 A を合わせた目標（p007 の後、p002 の基準と同じ時に）: **high-1080-grain と実 B が基準の 1.6 倍以上**（狙いは 2 倍）。
- p004〜p006 は別の .c だけを変え並べてよい。p007 は header を変えるので Opus、WS329 の encoder の Phase と header が重ならない時に。
- VF2（riscv64、V 拡張なし）では分岐の無い形が scalar で遅くなりうる。p010 で VF2 の 1 thread の前後を記録する（ゲートにしない）。

## 6. 段 B: 段の分離（p008、1 thread のまま）

thread の前に、流れを段に分ける refactor を 1 thread で入れ、出力が同じことを確かめる（thread の誤りと分離の誤りを分ける）。

| 段 | 中身 |
| --- | --- |
| P（parse） | NAL・slice header・参照の list・entropy・macroblock の syntax・QP・**動きの導出**（`recon_motion` から mvpred・P_Skip・direct）・intra の予測の mode の導出（隣の mode と availability だけを読む）・`mb->nonzero`・`intra4x4_mode` の記録・**R が今返す誤りの検査の全部**（参照の frame・plane の有無・大きさの違い・vector の範囲。recon_predict_block・recon_vector・recon_partition の EINVAL）・**隠蔽の数**（参照の missing の stand-in、今の `ctx->concealed++`）。結果は picture の `mbs` と、行の ring の「macroblock の仕事」の記録 |
| R（再構成） | intra の予測・inter の補間と weight・逆量子化と逆変換・residual の足し込み。**誤りを返さない**（P が検査を済ませた物だけが来る）。画素だけを書く |
| D（deblock） | macroblock の行ごと。行の範囲を取る関数 |

- **macroblock の仕事の記録**（ring の 1 entry）: 係数を int16 に詰めた物と係数の有る block の mask、PCM の画素、partition ごとに解いた参照（list ごとの frame の pointer と missing の扱い）と weight の係数（`struct h264sw_weight_factors` の値）、intra の mode。**slice header や `struct h264sw_weights` への pointer を持たない**（decoder の header は次の slice で上書きされる）。slice ごとの写しは持たない（1080p で数十 MB になるため）。
- ring の大きさ: K 行（K は p009 で決める。thread の数 ＋ 2 を初めの値）。1080p で 1 行 約 100 KB（120 macroblock × 約 0.8 KB）。Pi 5 の L2（512 KB/core）を超えうるので、p008 の 1 thread の目標は host と Pi 5 の両方で確かめる（p010）。
- **picture の終わりの順**: R の全部 → `decoder_conceal`（slice = −1 の macroblock の画素と entry）→ 残りの D → DPB。D は隠蔽の済んでいない行に触れない（§7 の規則）。
- **誤り**: slice の中の誤りは今と同じく誤った macroblock の entry だけを −1 に戻し（後で隠蔽）、ring にすでに置いた macroblock は必ず R・D まで行う。ring を捨てるのは `decoder_drop_picture`（reset・destroy）の時だけで、R・D が止まってから frame を unref する。
- **口の互換**: `h264sw_recon_mb`（動き＋画素、1 回で macroblock を再構成）と `h264sw_deblock_picture` は今の振る舞いのまま残す（WS329 の encoder と単体試験が使う）。新しい関数（動きの導出だけ・記録からの画素の再構成・行の範囲の deblock）を足し、`h264sw_recon_mb` は中でそれらを順に呼ぶ形に。p005・p006 で残した recon.c の中間の buffer の削減（予測を weight の出力へ直に、chroma の residual）もここで。
- 変わる header: `h264sw-context.h`（ring と仕事の記録）、`h264sw-recon.h`・`h264sw-deblock.h`（足す関数）、`h264sw-decoder.h`、R の関数が `struct h264sw_slice_ctx` を取らない形にするなら `h264sw-intra.h` も。BASE が移る（§10）。
- 目標: 出力が bit 単位で同じ。1 thread の速さが p007 の後の 0.95 倍以上。P の段の時間の割合を計測して記録し、p009 の目標をそれで計算し直す。

## 7. 段 C: thread（p009）

### 7.1 構成

- thread の数 N: 既定 min(online の CPU の数, 4)。N = 1 は p008 の 1 thread の道。
- 呼び手の thread（backend を呼ぶ thread）が P を行い、**ring が満ちた時と picture の終わりの待ちの間は R・D の仕事も取る**。N − 1 本の worker が R・D を取る。
- **R の待ち**: P が macroblock ごとに「R が画素を読む隣」（intra の macroblock の、同じ slice で available な A・B・C・D）を記録する。R の macroblock はその隣の R が済むのだけを待つ。inter・PCM の macroblock は今の picture の画素を読まないので待たない。ASO（slice の順が任意）・slice の欠けでも、待つ相手は decode 済みの macroblock だけなので止まらない。
- **D の規則**:
  - D(r)（行 r）は、行 r の全部と行 r＋1 の全部が「済み」（R か隠蔽）の後（行 r＋1 の intra は行 r の deblock の前の最下の line を読み、D(r) は行 r の 16 line 全部を書くため）。最後の行は picture の隠蔽の後。
  - **D は 1 本の鎖**: D(r) は D(r−1) の後（D(r) の上の edge は D(r−1) が直した行 r−1 の下 3 line を読み書きする。仕様の順は macroblock の address の順）。同時に 2 本の D は走らない。
  - 隠蔽が要る行（slice = −1 の macroblock を含む行）とその上の行の D は picture の終わりの隠蔽の後。
- 仕事の順: D を先（画素が cache にある間に）、次に R の若い行。

### 7.2 同期

- 進みの値（macroblock ごとの R の済み、行ごとの D の済み、ring の head・tail）は全部 mutex の中で読み書きする（ANSI C に atomic は無く、volatile の flag は arm64 の弱い記憶の順で壊れる）。
- 通知はまとめる: 待つ者がいる時だけ、または 4〜8 macroblock ごと。行の進みの値どうしは cache line（64 byte）を分ける。
- `media-os` に足す口（libmedia の中、HAL でない）: 時間の上限の無い `media_os_cond_wait`、1 つを起こす `media_os_cond_signal`、CPU の数の `media_os_cpu_count`（POSIX `sysconf(_SC_NPROCESSORS_ONLN)`、Win32 `GetSystemInfo`）。POSIX と Win32 の両方に書き、Windows の build（llvm-mingw の x86_64・arm64、-Werror）でも確かめる。

### 7.3 thread の作りと寿命

- 道: `h264sw_decoder_create` に thread の数と `const struct media_threads *`（NULL 可）を足す（`h264sw-decoder.h` の変更）。backend は libmedia の中の非公開の口で engine から `media_threads` を受ける（engine は `media_decoder_open` の中の道に渡す。公開の `media_decoder_open` の形は変えない。直に `media_decoder_open` を呼ぶ app（media-app など）は NULL で system の thread）。名前は "h264-decode-1" 〜 "h264-decode-3"。
- 試験の口: speed-bench・conform・decode-stream に thread の数の引数を足す（h264sw を直に呼ぶので環境変数は要らない）。
- 寿命: worker は decoder の寿命の間居る。reset・drop は仕事を取り消して静止（全部の worker が待ちに入る）を待つ（thread は残す）。flush は picture を終えるだけ。destroy の順は「静止 → drop_picture → worker を止めて join」。
- 起動の失敗: 起動できた本数で続ける（0 本なら N = 1）。decoder の作りを失敗させない（software の再生を失わない）。
- 1 process に decoder が複数ある（browser の page の動画など）時の thread の総数の上限は、この WS では決めない（記録して Future Work に）。

### 7.4 見込みと目標

- p007 の後の P の割合は推定 30〜35%（画素の処理が減るので割合は上がる）。呼び手も R・D を取るので、4 thread の上限は約 3 倍、2 thread は約 2 倍。
- 目標（host、clang -O2、p008 と同じ build、grain と実 B、taskset で別々の物理 core に固定し 1 thread と交互に 3 回）: **4 thread で 1 thread の 2.4 倍以上、2 thread で 1.6 倍以上**。p008 で測った P の割合からの上限の 80% に満たない時は理由を調べて記録。
- 1〜4 thread の出力が bit 単位で同じ。ThreadSanitizer の報告 0（host）。aarch64 でも N = 4 の run-stream（QEMU の user mode でよい）、host を 1〜2 CPU に絞った N = 4 の run-stream（待ちの順の偏りを出す）。

## 8. 段 D・E（条件付き）

- p011 picture の重なり: p010 で Pi 5 が 40 fps に届かず、P の段が上限（呼び手の thread の時間）を決めていると分かった時だけ。次の picture の P を前の picture の R・D と重ね、R の inter は参照の frame の D の済んだ行を待つ（vector の縦の到達 ＋ 補間の 3 行）。DPB の marking・出力の順・`decoder_finish_picture` の分け方を変える。Opus、設計を Phase の中で。
- p012 short vector 型: Q338-1 の答えが A で、p010（と p011 があればその後）が 40 fps に届かない時に、ユーザーに改めて聞いてから。

## 9. 受け入れの数（WS の完了）

| 項目 | 数 | 測り方 |
| --- | --- | --- |
| 正しさ | 適合の試験の in 101 本が全 frame で参照の YUV と一致、out 103 本が同じ理由で拒む（前と同じ 212 行）。run-stream の 22 本 408 frame・壊した stream 200 通り・途中の開始 63 通りが前と同じ出力。run-host-decode PASS。run-unit の各 test の件数が同じか増える。p009 以後は thread の数 1〜4 で全部が同じ | 各 Phase の前後 |
| arm64 の正しさ | Pi 5 の zedBSD で run-speed の stream の出力の MD5 が host の出力と同じ | p003・p010 |
| host 1 thread（段 A） | grain と実 B が p002 の基準の 1.6 倍以上 | run-speed（zedBSD の clang 23 -O2）、前後を同じ時に |
| host thread | 4 thread で 2.4 倍以上、2 thread で 1.6 倍以上 | p009 |
| **Pi 5（ゲート）** | **zedBSD の上で、Q338-3 のゲートの stream（推し: high-1080-grain）が 4 thread で 40 fps 以上**。60 秒以上の連続の decode の平均（best でなく）、3 回の median。温度・clock が取れれば記録 | p010、Q1 が割り当てる実機 |
| 5330 | Q338-2（推し: 1・2・4 thread の数の記録だけ） | p010 |
| 再生 | Pi 5 の Video Player で実 B を再生し、止まらない。app に落とした frame の数の口があればその数、無ければ「未実施」と書き Q1 へ | p010 |

計測の道（p002・p003）: zedBSD の上の speed-bench は Makefile を使わず、`build/llvm/bin/clang` と自分の worktree の sysroot で libmedia と同じ flag（`-O2 -fPIC` と arch の flag）で decoder の .c と speed-bench.c を直に cross compile して 1 本の program にする script（`plan/ws338/tests/`）。libmedia.so の exports.map が h264sw_* を隠すので、出荷の libmedia.so を通す計測は `media_decoder_*` を使う別の 1 本（p010）。

## 10. Phase の表

型: O = Opus（phase-runner）、S = Sonnet 5.5 high（phase-runner-low、1 module の .c の中、試験が oracle）、D = conformance-reviewer。

| Phase | 目的 | 型 | 依存 |
| --- | --- | --- | --- |
| p001 | 設計（この文書） | O | WS326 |
| p002 | host の計測の道具（run-speed の compiler の指定・module の時間・taskset・thread の数の口の受け皿、zedBSD 用の speed-bench の build の script、Pi 5・5330 の 1 本の script）と host の基準 | O | p001 |
| p003 | Pi 5・5330 の基準の実測（-O2、1 thread と 4 本の 1 thread の process を同時、mutex・cond の往復の費用、arm64 の MD5）と §3.4 の換算の置き換え | O | p002、§1 の flag が main に、実機（Q1） |
| p004 | deblock（§5） | S | p002 |
| p005 | interp・weight（§5） | S | p002 |
| p006 | transform・mb（§5） | S | p002 |
| p007 | entropy の速い道（§5、header の inline） | O | p002、WS329 と header の順（Q1） |
| p008 | 段の分離（§6、1 thread） | O | p004〜p007 |
| p009 | thread（§7） | O | p008 |
| p010 | Pi 5・5330（・VF2 の 1 thread の記録）の実測と目標の判定、出荷の libmedia.so を通す計測、Pi 5 の再生 | O | p009、p003、実機（Q1） |
| p011 | （条件付き）picture の重なり（§8） | O | p010 |
| p012 | （条件付き、Q338-1）short vector 型 | O | p010（p011） |
| p013 | 規約の全文の見直し | D | p010（p011・p012 があればその後） |

- check-skeleton の BASE: header を変える Phase（p007・p008・p009）の後に Q1 が新しい BASE を示す。WS329 p003 が足す `vcodec-*.h` も check-skeleton の glob に入るので、両 WS の header の Phase は Q1 が順に並べる（同時に進めない）。
- WS329 との関係: p008 は今の口（`h264sw_recon_mb`・`h264sw_deblock_picture`・`h264sw_slice_ctx` の field）を互換のまま残すので、WS329 の proposed は直さなくてよい見込み。互換で残せない所が出たら、p008 の担当が止まって Q1 に返す。

## 11. design-reviewer（2026-11-15）

design-reviewer 1 本（source・build・計測の出力・WS326 p024・WS329 を読む敵対的レビュー）。指摘 29 件（blocker 3・major 11・minor 15）。確かめて正しかった事: §1 の -O0 の主張、§3 の数と remark の件数、動きの導出は P に移せる（recon_inter は動きを全部導いてから予測する、decoded_4x4 は macroblock の中だけ、intra の mode の導出は画素を読まない）、D の 1 行の遅れの規則。

| # | 指摘 | 反映 |
| --- | --- | --- |
| B1 | P の誤りで ring を捨てると、entry だけ書かれ画素の無い macroblock が隠蔽から漏れる | §6: slice の誤りでは ring を捨てない。誤った entry だけ −1。捨てるのは drop の時だけ |
| B2 | ASO・slice の欠けで wavefront と D の待ちが deadlock、隠蔽の前に D が走る | §7.1: R は記録した intra の隣だけを待つ。picture の終わりは R → 隠蔽 → 残りの D。隠蔽の要る行の D は後 |
| B3 | D の行どうしの順が無く並べると競合 | §7.1: D は 1 本の鎖 |
| M1 | R にある誤りの検査・隠蔽の数・nonzero・intra4x4_mode の書き込み・slice_ctx の口 | §6 の P に列挙、R は誤りを返さない、intra.h の口 |
| M2 | weights が header への pointer、slice の写しが大きすぎる | §6: 仕事の記録に解いた参照と weight の値、slice の写しは持たない |
| M3 | 2 thread の目標が構成で届かない、直列の割合の見積もり | §7.1 呼び手も R・D を取る、§7.4 目標を 2.4・1.6 倍に、p008 で P の割合を測る、picture の重なりを p011 に |
| M4 | WS329 の encoder が recon_mb・deblock_picture・slice_ctx を使う、check-skeleton の glob | §6 口の互換、§10 BASE と header の Phase の順は Q1 |
| M5 | 呼び手の thread を decoder へ渡す道が無い、試験の口 | §7.3: decoder_create の引数、engine から非公開の口、試験は引数 |
| M6 | p005 と p006 が recon.c と header で衝突、p007 の wrapper、p002 の実機待ち | §5: p005・p006 は口と recon.c を変えない、recon.c は p008。p007 に h264sw-cabac.h。p002（host）と p003（実機）に分けた |
| M7 | Pi 5 の best of 3、熱、speed-bench の作り方、arm64 の正しさ、host の 4 thread の揺れ | §9: 60 秒の連続の平均の median、温度・clock、build の script、MD5、taskset |
| M8 | remark だけでは block の幅より広い vector が走らない | §3.3・§5: VF×IC ≤ block の幅、module の時間と組 |
| M9 | 出典の規則が無い | §1、各 phase.md の「読む物」 |
| M10 | 同期と記憶の順、通知の費用 | §7.2、p003 で mutex・cond の費用 |
| M11 | reset で止めた worker の再起動、起動の失敗 | §7.3 |
| m1 | wavefront の理由の誤り（inter は今の picture の画素を読まない） | §7.1 |
| m2 | warning 0 の記述の一部の誤り、Windows は -O2 | §2 の 1 |
| m3 | -Os・strict aliasing の危険 | §1 の決定で問いから外れた（Q1 が全 build で確かめる） |
| m4 | 受け入れの compiler | §2 の 2、§9（clang 23） |
| m5 | 段 A の目標を 50 fps から下げた理由 | §2 の 8 |
| m6 | VF2 の scalar での後退 | §5、p010 で記録 |
| m7 | 新しい .c は sources.mk | §1 |
| m8 | Win32 の build | §7.2 |
| m9 | false sharing | §7.2 |
| m10 | 1 process の thread の総数 | §7.3（Future Work） |
| m11 | -O0 なら段 A が逆効果 | §1 の決定で解消 |
| m12 | 割合でなく時間 | §5 |
| m13 | ws.md の Status の文 | ws.md を直した |
| m14 | /tmp の要約 | measure/ に写した |
| m15 | ring と L2 | §6 |
| 問い | WS329 との順、ゲートの stream、公開の ABI、新しい .c | WS329 の順と BASE は Q1（§10）、ゲートの stream は Q338-3、ABI は変えない（§1・§7.3、技術の判断）、新しい .c は足さない（§1） |

<!-- awesome-plan project=zedbsd record=ws326 -->

# WS326 libmedia の H.264 の software の decoder（独自の実装）

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG006。
- Status: completed（2026-10-31 Q1 の q1780 で完了の形に。全 Phase cleared、main 8fe150be4）
- 目標: libmedia に、既存の library を使わない H.264 の software の decoder を作る。範囲は progressive の 8 bit 4:2:0、Constrained Baseline・Main・High（interlace（PAFF・MBAFF）・4:2:2・10 bit は範囲の外）。
  Vulkan Video の hardware が無い機械（Pi 5 など）で libavcodec に頼らずに再生する道。libmedia の backend の順は Vulkan Video → この独自の software の decoder → libavcodec（2026-10-30 ユーザー「libmedia の backend の順番は、Vulkan Video --> 独自ソフトウェアdecoder --> libavcodecです。」）。

## ユーザーの決定（2026-10-30、原文）

1. 「H.264デコーダのソフトウェア実装は、どれくらいの規模見積もりになりますか？LWでいうと。」（Q1 の見積もり: 正しく decode できるまで約 60〜80 LW、1080p を CPU で実用の速さにするまで含めて 80〜120 LW）
2. 「libmediaへのH.264デコーダの実装について、Claudeの安いモデルに外注することを検討しています。全体の骨格を、モジュールをわけてファイルを作り、モジュールのインタフェース（定数、構造体、関数）をヘッダで定義しておき、中身はヘッダの関数のコメントで仕様を定義しておいて、モジュールの.cファイルの実装は、実装するLLMに任せる、という構成にしたいです。WSを立てて、phaseを作成し、最初のphaseでこのモジュール設計だけを実施して、実装のフェーズはSonnet 5.5 Highのサブエージェントで実行する、というストラテジにしたいです。」

3. 「WS326ですが、共通部分はH.265でも使えるように、h264_のような名前でなく、汎用にするよう、記録しておいてください。」

## 進め方（上の原文の解釈、Q1）

- p001（設計、Opus）: module に分け、各 module の header（定数・構造体・関数）を書き、各関数の comment に仕様（入力・出力・前提・誤りの扱い・ISO/IEC 14496-10 の節）を書く。.c は空の骨格だけ（関数は未実装の印）。module ごとの host の単体試験の仕様と、全体の適合の試験（JVT の conformance の bitstream と hash の照合）の道も決める。module の依存の順と、実装の Phase の割り振り（1 Phase = Sonnet の 1 回の起動で済む大きさ）を表にする。
- p002 以降（実装、Sonnet 5.5 の high の subagent）: header を変えずに .c を実装し、単体試験を通す。header の不備は直さず Q1 に返す（Q1 か Opus が header を直す）。
- 最後に組み込み（libmedia の backend）、適合の試験、速さ、規約の全文の見直し。
- 決定 3 の解釈: H.265 でも使える部分（例: bit の読み、Exp-Golomb、NAL・RBSP（emulation prevention）、CABAC の算術の engine と状態の遷移の表、frame・plane の確保、DPB・出力の順の共通の部分、試験の道具）は `h264sw` の名前でなく codec に依らない名前の module にする。
  今動いている実装の Phase（header は凍結）を乱さないため、名前の移しは p026（適合の試験 p022 の後、規約の見直し p025 の前）で行い、単体試験と適合の試験が前後で同じであることで確かめる。
  どこまでを共通にするかと名前は p026 の設計（Opus）で決め、[WS329](../ws329/ws.md)（H.264 の encoder）・[WS330](../ws330/ws.md)（H.265 の decoder・encoder）の共通化の設計はその上に足す。
  p026 の設計（2026-10-31）: bit の読み・NAL（Annex B の分割と emulation prevention）・CABAC の算術の engine と表・共通の約束を `vcodec.h`・`vcodec-bitreader`・`vcodec-nal`・`vcodec-cabac` に移した。frame・plane の確保と DPB・出力の順は、H.265 の相手の設計なしでは推測の API になるので、WS330 の設計へ先送りする（Q1 の技術の判断、2026-10-31）。WS329 の encoder も `h264sw_frame`・`h264sw_dpb` を使うので、WS330 で decoder と encoder の両方を移す。

## Q1 の判断（2026-10-30、p001 の問い）

1. h264sw.h の「節の番号は ITU-T H.264 (08/2021)」の記述は定数の block の comment なら guardrail に合う（file の先頭でない）。
2. 適合の bitstream の cache は main の共有の `/home/awe/zedBSD-claude1/build/sources/h264-conformance/`（git の外、消さない場所）。実装の担当は `H264SW_CONFORMANCE_DIR` にそこを渡す。p001 で取った 3 本は写し済み。
3. profile の範囲: Extended（constraint_set0/1 が 1）や High 10・4:2:2・4:4:4 Predictive の profile_idc でも、中身が 8 bit 4:2:0 の frame なら受ける（A.2）は、ユーザーの「Constrained Baseline・Main・High」を狭めない技術の判断として採る（ユーザーに報告）。
4. 実装の Phase は、依存の無い物から並べて Sonnet 5.5 high の subagent を別々の worktree で動かす（p002・p008・p010 から）。

## 結果（2026-10-31 完了）

- 作った物: `userland/desktop/libmedia/` の H.264 の decoder（`h264sw-*.c`・`h264sw*.h` の module 群と decoder の全体の流れ `h264sw-decoder`）、codec に依らない module（`vcodec.h`・`vcodec-bitreader`・`vcodec-nal`・`vcodec-cabac`、p026）、libmedia の backend `h264sw-backend`（p023）。設計と module の仕様は [design.md](design.md) と各 header の comment。
- 適合（ITU-T H.264.1 の JVT の conformance の bitstream、p022）: 204 本を取得し、範囲の中の 101 本が全 frame（計 14526 frame）で参照の YUV と一致。範囲の外の 103 本（interlace 73・4:2:2／monochrome／10 bit の format 25・slice group 3・Extended の profile 88 が 2）は決めた理由で拒む。slice を逆順にした 6 本も一致。p022 での decoder の修正は 0。
- 全体の試験（p021）: ffmpeg で作った 22 本 408 frame が ffmpeg の出力と一致、壊した stream 200 通り・途中からの開始 63 通りで落ちずに決めた誤りを返す。単体試験は module ごとに 24 本（ASan・UBSan）。
- backend の順（p023、ユーザーの決定）: Vulkan Video → この software の decoder（`h264-software`）→ libavcodec。open の拒否と受け入れ後の拒否（ENOTSUP・ENODEV・EBUSY）で次の backend へ切り替え、key frame まで捨てる。host で MP4・TS・複数 slice を `h264-software` で decode し ffmpeg と一致（run-host-decode）。
- 速さ（p024、host の Xeon Gold 6130・-O2・1 thread・5 回の best）: 補間の block 化と deblocking の bS の書き直しで 1.35〜1.94 倍。1080p で High CABAC B 8 Mbps 16.7 → 24.1 fps、High CRF 23 19.7 → 36.3 fps、Main CAVLC 8 Mbps 17.3 → 27.3 fps、実 stream Main 1080p24 8 Mbps 18.4 → 35.7 fps、実 stream High 1080p30 13 Mbps 13.0 → 17.5 fps。出力は前と bit 単位で同じ。
- 規約の全文の見直し（p025）: style-check の違反 0、全試験が前と同じ、header は h264sw-ps.h の comment の大きさの記述だけ後で直した（77ed48ac4）。
- 試験の道具は [plan/tools/h264sw/](../tools/h264sw/) に移した（WS329・WS330・WS318 も回帰に使う）。header の照合の基点は 77ed48ac4（`BASE=77ed48ac4 sh plan/tools/h264sw/check-skeleton.sh`）。

## 制限・移管

- 範囲の外（ユーザーの決定どおり）: interlace（PAFF・MBAFF）・4:2:2・4:4:4・10 bit・monochrome・slice group（FMO）・data partitioning・SP／SI の slice。
- 速さの続き（p024 の設計の案 a〜e）: a deblocking の filter と weight、b neighbour・transform・syntax の消し・CABAC の file の中の fast path、c thread（deblock の行の pipeline、parse と再構成の分離）、d SIMD（NEON・SSE2）、e Pi 5・5330 の実測と目標（Pi 5 で 1080p30 の High を decoder 4 thread で 40 fps 以上）の判定。別の WS にするかは Q1 がユーザーに聞いている（2026-10-31）。段 A の目標（High CABAC B の 1080p で 1 thread 50 fps）まで残り約 2 倍。
- frame・plane の確保と DPB・出力の順の codec に依らない形への共通化は [WS330](../ws330/ws.md)（H.265）の設計へ移した（WS329 の encoder も含めて WS330 で移す）。
- 未実施: QEMU の Video Player での再生、実機（Pi 5・5330）での再生と計測、arm64 の build。
- 出力の複写の削減は WS318 p006a へ（WS318 の設計からの引き継ぎ）。

## Phase の一覧（全部 cleared、2026-10-30〜31）

phase.md は完了で消した（git の履歴に残る）。

| Phase | 目的 | 結果 |
| --- | --- | --- |
| ws326-p001 | module の設計（header の interface と仕様の comment、骨格、試験の仕様、実装の割り振り、Opus） | fd1d6f88c |
| ws326-p002 | bit reader と NAL | test-bits 169・test-nal 95 checks |
| ws326-p003 | 共有の表と parameter set | test-tables 846・test-ps 41887 |
| ws326-p004 | slice header | test-slice 141694 |
| ws326-p005 | frame と POC | test-frame 284・test-poc 382 |
| ws326-p006 | DPB（marking・gap・出力の順） | test-dpb 129447 |
| ws326-p007 | reference picture list | test-reflist 278758（p021 の header の直しの後 281194） |
| ws326-p008 | macroblock の型と neighbour | test-mb 1938 |
| ws326-p009 | CAVLC | test-cavlc 87868 |
| ws326-p010 | CABAC の context の初期値の表 | 1840 対を仕様の PDF と独立に照合、4159 checks |
| ws326-p011 | CABAC の engine | test-cabac 195654 |
| ws326-p012 | CABAC の syntax element | test-cabac_se 168241 |
| ws326-p013 | 変換と逆量子化 | test-transform 33158 |
| ws326-p014 | intra 予測 | test-intra 593 |
| ws326-p015 | 補間と重み | test-interp 23460・test-weight 159 |
| ws326-p016 | MV の予測と P_Skip | test-mvpred 83 |
| ws326-p017 | B の direct | test-direct 259 |
| ws326-p018 | slice data と macroblock の syntax | test-macroblock 1107981 |
| ws326-p019 | macroblock の再構成 | test-recon 33584 |
| ws326-p020 | deblocking | test-deblock 3313 |
| ws326-p021 | decoder の全体の流れと最初の全体の試験（h264sw-reflist.h の直し 8ddb130a9） | run-stream 22 本 408 frame 一致 |
| ws326-p022 | JVT の適合の試験 | in 101 本 14526 frame 一致、out 103 本 |
| ws326-p023 | libmedia の backend と切り替え | host 202＋112 checks、run-host-decode PASS |
| ws326-p024 | 速さの設計と最初の最適化 | 1080p 1 thread で 1.35〜1.94 倍 |
| ws326-p025 | 規約の全文の見直し | style-check 0 |
| ws326-p026 | codec に依らない module（vcodec）へ移す | 92ca2d0e8、前後で全試験と nm -D が同じ |

## 試験（plan/tools/h264sw/、host）

- `sh plan/tools/h264sw/run-unit.sh all`（単体 24 本）、`BASE=77ed48ac4 sh plan/tools/h264sw/check-skeleton.sh`、`sh plan/tools/h264sw/run-stream.sh`（ffmpeg が要る）、`sh plan/tools/h264sw/backend/run-backend.sh`・`run-host-decode.sh`、
  `H264SW_CONFORMANCE_DIR=<dir> sh plan/tools/h264sw/conformance/run.sh`（bitstream は `conformance/fetch.sh` で取得、展開後 約 7 GB）、`sh plan/tools/h264sw/speed/run-speed.sh`（計測）。

## Resume point

- 2026-10-31 P4（q1780-i01）: 完了の片付け。試験を plan/tools/h264sw/ へ移し、phase001〜026 を消した。2026-10-31 ユーザーから、libmedia の実装の WS を昼までに終えるよう優先度を上げる、という趣旨の指示を受けた。

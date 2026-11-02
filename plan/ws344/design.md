<!-- awesome-plan project=zedbsd record=ws344-design -->

# WS344 libwebp-compat（自前の WebP の decoder）の設計（p001）

- 親: [ws.md](ws.md)。作成: ws344-p001（P2、2026-11-02）。正本は header（`include/libc/compat/webp/*.h`、`userland/desktop/libwebp-compat/*.h`）の comment。この文書は全体の地図・決定・試験の道。
- 進め方: 標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)）。p001 が header・骨格・試験の道具・Phase の表を作り、実装の Phase（Sonnet 5.5 high、phase-runner-low）は header を変えずに .c と単体試験を書く。統合の Phase は Opus を推す。

## 1. 出典

| 物 | 場所（git の外） | SHA-256 |
| --- | --- | --- |
| RFC 9649（WebP Image Format、2024-11。§2 container・§3 VP8L・§2.7.1.2 ALPH） | `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc9649.txt` | `f4d7cd92b015c0b340f10a932bb03e564a758a17df50cbb992c16d8459d60e58` |
| RFC 6386（VP8 Data Format and Decoding Guide、2011-11） | `/home/awe/zedBSD-claude1/build/sources/specs/webp/rfc6386.txt` | `c044997ae5e26f74dc28569f8866937412b2783e887c96e0c09372af78be411c` |
| libwebp 1.5.0 の release の tarball（BSD-3-Clause。値と状態の規則の確かめだけ、tree に入れない） | `/home/awe/zedBSD-claude1/build/sources/specs/webp/libwebp-1.5.0-ref/libwebp-1.5.0.tar.gz` | `7d6fab70cf844bf6769077bd5d7a74893f8ffd4dfb42861745750c63c2a5c92c` |

- RFC は text（頁の区切り `Bankoski, et al.` の行を挟む）。節の番号で引く。読み方は `sed -n '/^13\.3\./,/^13\.4\./p' rfc6386.txt`、表は vp8-tables-extract.py が名前で抜く。
- RFC 6386 §1: 物語（narrative）と §20 の参照 decoder（dixie）が食い違えば参照 decoder が正しい。§20 は仕様として読んでよいが、code・comment・構造・名前は写さない（§20.26 の BSD の code。自分の名前・構造で書く）。
- 既知の RFC の誤り（実装の担当に phase.md で重ねて書いた）:
  - §13.3 の pseudocode の plane の番号（Y2 = 0 など）は誤り。同じ節の list（0 = Y after Y2、1 = Y2、2 = chroma、3 = Y with DC）と §20.16 が正しい。
  - §13.3 の pseudocode は `prevCoeffWasZero = true` を無条件に置き、`currentBlockHasCoeffs` を非 0 の token だけで立てる。正しくは「直前の token が DCT_0 の時だけ EOB の枝を飛ばす」、context の値は「位置が最初の位置より進んだか」（§20.16 `t = (c != !type)`、0 の token を含む）。
  - §15.1 の「In step 4, we filter across the vertical subblock edges」は step 2 の誤り。
  - §7.3 の `read_signed_literal` は 2 の補数。frame header の符号付きの値は「大きさ L(n) の後に符号 L(1)」（§9.3、§9.6、§19.2）で別物。
- RFC 9649 が書いていない点と、この実装の読み（header に書いた）:
  - 正準の prefix code の符号の割り当てと bit の順: RFC 1951 §3.2.2 の割り当て、stream から最初に読む bit が符号の最上位（libwebp の encoder の書き方）。
  - normal code length code の max_symbol は「code length code から読む symbol の数」（repeat は 1 と数える）。libwebp の encoder（StoreFullHuffmanCode の trimmed_length）がこの数を書く。
  - predictor の mode は green の下位 4 bit、14 と 15 は mode 0 と同じ（libwebp の読み）。
- **libwebp は host の試験の oracle だけ**（ユーザーの決定）: Debian の `webp`・`libwebp-dev` 1.5.0（`sudo apt-get install webp libwebp-dev`、p001 で導入済み）。cwebp・img2webp・fixture-encoder.c（libwebp の encode API）で試験の画像を作り、dwebp と webp-driver.c（`-DWEBP_DRIVER_LIBWEBP`）で期待の出力を作る。単体試験の期待の値は仕様と手の計算から作る（oracle を使わない）。
- p001 で libwebp 1.5.0 の source の 3 file（`src/dsp/yuv.h`、`src/dsp/alpha_processing.c`、`src/dec/webp_dec.c`・`buffer_dec.c`）を設計の担当が読んだ。目的は、互換の interface が返す状態（ParseHeadersInternal の順と状態）・出力の変換の定数・buffer の検査の規則を、header に自分の言葉で書くため。code は写していない。riff.c・decode.c・output.c の該当の block の comment には「Values referenced: libwebp 1.5.0 src/dsp/yuv.h（BSD-3-Clause）」「Procedure referenced: libwebp 1.5.0 src/dec/webp_dec.c ParseHeadersInternal（状態の規則）」の形で書く（Q1 の判断 1、§11）。実装の担当は libwebp の source を読まない。

## 2. 範囲

- 静止画: 非可逆の VP8 key frame（RFC 6386 の key frame の全部: segment、loop filter の simple と normal、token partition 1〜8、B_PRED）、可逆の VP8L（4 つの transform、color cache、meta prefix code、LZ77）、ALPH（raw と可逆、filter 0〜3）、RIFF の simple 形式・VP8X の拡張形式（ICCP・EXIF・XMP・未知の chunk は読み飛ばす）、RIFF の無い裸の VP8・VP8L。
- 外: animation（WebPGetFeatures は has_animation を返し、decode は VP8_STATUS_UNSUPPORTED_FEATURE）、incremental decode（WebPI*）、cropping・scaling、4444・565 の色空間、可逆の画像の YUV の出力、dithering（libwebp の既定 0 と同じ出力）、ICC の色の管理、inter frame。範囲の外は全て ENOTSUP → UNSUPPORTED_FEATURE（簡単な呼び出しは NULL）。
- 出力は libwebp 1.5.0 と byte ごとに同じ（同じ入力・同じ設定）。壊れた入力の状態の値は libwebp と違ってもよいが、buffer の外を読み書きせず必ず終わる。

## 3. 互換の interface（libwebp-compat）

- ユーザーの決定（2026-11-02、Q1 経由）: libpng-compat・libjpeg-compat・libz-compat と同じ形の **libwebp-compat** にする。libwebp の interface の部分集合を、自分の宣言（同じ名前・値、libwebp の header の code を含まない）で `include/libc/compat/webp/types.h`・`decode.h` に置く（libwebp 1.5.0 の `<webp/types.h>`・`<webp/decode.h>` に対応、WEBP_DECODER_ABI_VERSION 0x0209）。構造体は field の名前が同じで、配置は約束しない（SONAME は libwebp-compat.so、gif_lib.h と同じ扱い）。
- 部分集合（使う人から選んだ。libbrowser は RGBA か BGRA/ARGB の Into、試験は全部）: WebPGetDecoderVersion、WebPGetInfo、WebPGetFeatures（macro → WebPGetFeaturesInternal）、WebPDecodeRGBA・ARGB・BGRA・RGB・BGR・YUV と各 Into、WebPInitDecoderConfig（macro → Internal）、WebPDecode、WebPInitDecBuffer（macro → Internal）、WebPFreeDecBuffer、WebPMalloc、WebPFree。libwebp の static inline の helper（WebPIsRGBMode など）は ANSI C のため macro にした。
- 出力の色空間: RGB・RGBA・BGR・BGRA・ARGB・rgbA・bgrA・Argb、YUV・YUVA（非可逆だけ）。option: bypass_filtering、no_fancy_upsampling、flip、use_threads（無視）。
- 状態の規則は libwebp の通り（riff.h と decode.h の comment）: 12 byte 未満は NOT_ENOUGH_DATA、拡張形式の部分の data は VP8X から features を返す、WebPDecode は header の NOT_ENOUGH_DATA を BITSTREAM_ERROR にする、animation は UNSUPPORTED_FEATURE など。webp-driver.c が両方の library で同じ行を出すことで確かめる。
- 外部の buffer の負の stride は libwebp と違い INVALID_PARAM（decode.h の差分の list）。

## 4. 決定（p001、Opus）

| 決定 | 内容と理由 |
| --- | --- |
| 置き場所 | `userland/desktop/libwebp-compat/`（png・jpeg・gif の compat の隣、master-design-policy の compat の置き場）。header は `include/libc/compat/webp/`。内部の接頭辞は `webpc_`・`WEBPC_`（libwebp の内部名 VP8*・VP8L*・WebP* と重ならない）。|
| 共有 library と static | zedBSD は `libwebp-compat.so`（`platform/common/userland-dynamic.mk` の規則、version script `exports.map`、check-dynamic-elf）を p001 で足し、warning 0 で link できることを確かめた（21 の export）。Windows（OmniGrammaton）は `apps/browser-win32/Makefile` の WIN32_COMPAT_LIBRARIES に足す（p020）。両方とも `sources.mk` の同じ list。compat の header は Windows の build が `include/libc/compat/*/*.h` を写すので追加の作業は無い。|
| 新しい header と sysroot | `include/libc/compat/webp/` は sysroot（共有の `build/amd64/sysroot`）に写される物で、その作り直しは toolchain の作業（Q1）。担当は `plan/ws344/tests/target-build.sh` で worktree に sysroot の複写を作り、そこへ compat/webp を写して target を build する（共有の sysroot を変えない）。main に merge した後の通常の build は sysroot の stamp が header の一覧に依るので作り直しが走る（Q1 の判断 2）。|
| 全体を読んでから decode | 非 incremental。libbrowser は body を全部受けてから img_decode を呼ぶ（page/images.c）。incremental は後（Future Work）。|
| VP8 の再構成と loop filter | 全 macroblock を再構成してから、frame 全体を raster の順で filter する（RFC 6386 §15.1「intra prediction は filter 前の画素を使う」）。plane は macroblock の整数倍（右と下の余りも decode・filter し、出力で切る。端の画素は余りの画素で filter される）。行ごとの filter と遅延の最適化は速さの Phase（Future Work）。|
| VP8 の bool decoder の終わり | libwebp と同じ「8 bit の窓が data の終わりを越えて bool を読んだら ended」（bool.h）。正しい stream では起きない。ended は sticky で、macroblock ごとと行ごとに確かめて ENOBUFS（NOT_ENOUGH_DATA）。|
| 係数と変換の幅 | 逆量子化した係数と WHT の出力、IDCT の 1 段目の結果を 16 bit に wrap（参照 decoder が short に置く）。WHT の中間は int32 のまま（16 bit の入力の和は溢れない。dixie は 1 段目も short、libwebp は int）。正しい stream では全部同じ値で、壊れた stream では実装ごとに違いうる（完全な一致は定義できない）ので、未定義の動作が起きない形だけを決めた。|
| loop filter の level | §20.6（segment の値を足した後と delta の後の 2 回 clamp）に従う（Q1 の判断 1: codec の算術は RFC から）。libwebp は 1 回目の clamp を持たないが、cwebp の file（segment は絶対値、delta 無し）では同じ。decode.h の約束を「libwebp の encoder の file で byte 一致」にし、差を列挙した。|
| segment の既定 | segmentation が有効で update_data 0 の key frame は §20.4 の状態（delta、値 0 → base_q と header の level）。libwebp は absolute 1・値 0（q 0）。encoder は作らない。decode.h の差の list。|
| simple code の alphabet の外の symbol | RFC 9649 が黙る所なので libwebp の読み（code から外す、残りが無ければ EINVAL）。|
| inner edge の filter | §15.1 と §20.6: B_PRED か、どれかの block の token が直後の EOB だけでない（0 の token を含む）時。libwebp は WHT の出力の DC の 0 も数えるため、特異な stream（Y2 の係数が全部 0 の token）で違いうる（cwebp は作らない）。|
| YUV → RGB | libwebp の既定: BT.601 の limited range、14 bit 固定小数点（output.h の式と定数、libwebp 1.5.0 src/dsp/yuv.h の値）、chroma は fancy（9-3-3-1、端は座標を clamp）か no_fancy（2x2 の複製）。p001 で dwebp 1.5.0 の -yuv と -pam（fancy・nofancy）に対して全画素一致を確かめた（37x23 の試料）。premultiply は floor(x * a / 255)（libwebp の (x * a * 32897) >> 23 と全 x・a で同じ、libwebp の rgbA の出力と一致を確かめた）。|
| ALPH の reserved・pre-processing の bit | RFC 9649 に従い無視する（libwebp は Rsv が 0 でない時と P が 2・3 の時に誤り）。圧縮の方法 2・3 と payload の 1 byte 以下は EINVAL。ALPH の decode の失敗は画像全体の失敗（libwebp と同じ）。可逆の bitstream の file の ALPH は has_alpha にだけ効く（decode しない、libwebp と同じ）。|
| 複数の ALPH | 最後の物（libwebp と同じ）。RFC は still image の重複を定めない。|
| VP8L の prefix table の上限 | 画像 1 つの table の entry の合計を WEBPC_VP8L_TABLE_ENTRIES_MAX（2^24、64 MiB）までにする（越えると ENOMEM → OUT_OF_MEMORY）。entropy image が名指さない group は読み捨てる。65536 group の悪意の file が数百 KB で数百 MB を取るのを防ぐ。|
| 画像の大きさの上限 | 形式の上限のまま（VP8 16383、VP8L 16384、VP8X の canvas 2^24・面積 2^32 未満）。library は画素数の上限を持たず、呼び手（libbrowser の IMG_PIXELS_MAX = 64 Mi 画素）が WebPGetInfo の大きさで断る。allocation の失敗は ENOMEM。|
| thread | global の状態を持たない（定数の表だけ）。2 つの decode を 2 thread で並べてよい。|
| common.c | 小さく他の全部が使うので p001 で実装し test-common.c（32 check）で確かめた。依存の無い Phase を並べて始めるため。|

## 5. module

全部 `userland/desktop/libwebp-compat/`。各 header の comment が仕様（入力・出力・前提・誤り・節）。

| module（.h/.c） | 内容（節） | 使う module |
| --- | --- | --- |
| common | 誤りの値・上限・算術の規則、le16/24/32、clamp255、asr（floor の shift）、wrap16（p001 で実装済み） | — |
| riff | RIFF・VP8X・optional chunk・bitstream の chunk、VP8 と VP8L の header の大きさ、features（RFC 9649 §2、RFC 6386 §9.1、§3.4） | common |
| lbits | VP8L の LSB-first の bit reader（§3.2） | common |
| prefix | 正準の prefix code の table（2 段、root 8 bit、pool）と symbol の読み（§3.7.2.1、RFC 1951 §3.2.2） | lbits |
| vp8l-codes | simple・normal code length code、5 つの code の group（§3.7.2.1、§3.7.2.2） | prefix |
| vp8l-image | entropy-coded image: color cache、meta prefix code と entropy image、literal・LZ77（Table 4・Figure 20）・cache（§3.6、§3.7.2.2、§3.7.2.3） | vp8l-codes |
| vp8l-transform | 逆変換: predictor（Table 2）・color・subtract green・color indexing（Table 3、bundling）（§3.5） | common |
| vp8l | VP8L の header・transform の読み・全体（§3.4、§3.5、§3.8）、ALPH の header の無い stream | vp8l-image、vp8l-transform |
| alpha | ALPH: header の byte、raw・可逆、逆 filter（RFC 9649 §2.7.1.2） | vp8l |
| bool | bool decoder・literal・符号付き・tree（RFC 6386 §7.3、§8.1） | common |
| vp8-tables | 定数の表（tree・確率・係数の既定と更新の確率・band・zigzag・category・量子化）と mode・token の番号（§8.1、§9.3、§11、§13、§14.1） | common |
| vp8-header | frame tag と key frame の header、partition、量子化と逆量子化の係数、確率の更新（§9、§19.2、§13.4、§14.1、§20.4） | bool、vp8-tables |
| vp8-modes | macroblock の header: segment・skip・ymode・16 の bmode（context）・uvmode（§10、§11、§19.3） | vp8-header |
| vp8-tokens | 係数の token・context・逆量子化（§13、§14.1、§20.16） | vp8-header |
| vp8-idct | 逆 WHT・逆 DCT と予測への加算（§14.3、§14.4、§14.5） | common |
| vp8-predict | intra 予測: 16x16・8x8 の 4 mode、4x4 の 10 mode（§12、§20.14） | common |
| vp8-filter | loop filter の制御値・simple・normal の edge・macroblock の 4 段（§15、§20.6） | vp8-header |
| vp8 | key frame 全体: macroblock の loop、edge の値、再構成、frame 全体の filter（§5、§12〜§15） | 上の vp8-* 全部 |
| output | YUV → RGB（fancy・no_fancy）、alpha、premultiply、ARGB → 各配置、flip（RFC 9649 §2.5、libwebp の既定の規則） | common |
| decode.c（header 無し） | `<compat/webp/decode.h>`・`types.h` の呼び出し全部: features、各 decode、buffer の検査と確保、状態の写像 | 全部 |

依存の順（実装の Phase の順）: riff・lbits・vp8l-transform・bool・vp8-tables・vp8-idct・vp8-predict・output は common だけに依る（並べて始められる）→ prefix → vp8l-codes → vp8l-image → vp8l → alpha。vp8-header → vp8-modes・vp8-tokens・vp8-filter → vp8。全部 → decode.c → 組み込み。

## 6. 所有と寿命

- `struct webpc_container` は呼び手の data を指すだけ（所有しない）。
- `struct webpc_prefix_pool` が 1 つの画像の全 code の table を持つ（realloc で伸びるので code は offset で table を指す）。webpc_vp8l_decode_image が作って返る前に解放する。
- `webpc_vp8l_decode` は ARGB の配列を malloc して返し、呼び手が free。`webpc_vp8l_decode_stream`・`webpc_alpha_decode` は呼び手の buffer に書く。
- `struct webpc_vp8_picture` は 3 plane を 1 つの allocation に持つ（memory）。`webpc_vp8_picture_release` が解放。
- decode.c: 簡単な呼び出しの出力は WebPMalloc（= malloc）で 1 つの allocation（YUV は Y・U・V 続き）、呼び手が WebPFree。WebPDecode の内部の確保は `output.private_memory`、WebPFreeDecBuffer が解放（外部の memory は解放しない）。失敗の時は何も残さない。
- 中間の物（VP8 の picture、VP8L の ARGB、alpha の plane）は decode.c の 1 回の呼び出しの中で確保と解放をする。

## 7. 上限と信用できない入力

- 全ての読みは buffer の大きさで止まる: lbits と bool は data の外を 0 と読み、ended・consumed_end を立てる。decoder はそれを ENOBUFS にする。
- 配列の添字にする値（symbol、mode、segment、context、距離、長さ、table の index、color cache の index）は使う前に範囲を確かめるか、構成上範囲の中（prefix の symbol は code の symbol だけ、tree の葉は定数の表）。
- loop の上限: 画素数、macroblock 数、symbol の数（alphabet）、group の数（65536）、transform の数（4）、partition の数（8）。LZ77 の copy は残りの画素数で止め、越えれば EINVAL。
- memory: 画素数に比例（VP8 は plane 1.5 byte/画素 + context、VP8L は ARGB 4 byte/画素 + subresolution 画像 + table（上限 64 MiB））。
- 算術: common.h の規則（積は範囲を先に絞るか広い型、負の値の shift は webpc_asr、狭い型へは wrap16・clamp255）。
- 時間: 必ず終わるが、時間は画素数に比例する。VP8L は 5 つの code が全部 1 symbol なら 1 画素 0 bit で、数十 byte で 16384x16384 を decode する（libwebp も同じ）。呼び手（libbrowser）は WebPGetInfo の大きさで断る。fuzz-target は 2^22 画素を越える入力を decode しない（libwebp の fuzzer と同じ形）。大きな 0 bit の画像は p019 の単体試験で 1 回。
- 確かめ: mutate.sh（全 fixture × 切り詰め 88 位置 + 置き換え 128 通り、ちょうどの大きさの複写で ASan が 1 byte の読み過ぎも捕まえる、1 file 120 s）、fuzz.sh（libFuzzer、既定 600 s）。p001 の骨格で両方の道具が動くことを確かめた（mutate 2 file 434 入力、fuzz 10 s 1000 万回）。

## 8. 試験の道具（p001 が作り、実装の担当は変えない）

全部 `plan/ws344/tests/`。出力は `build/tmp/ws344-*`（fresh_out の新しい directory）。

| 道具 | 内容 | command・最後の行 |
| --- | --- | --- |
| run-unit.sh | `unit/test-<name>.c` を全 library の .c・compat header・`rfc-vp8-tables.h`（RFC 6386 の表を text から抜いた物）と ASan・UBSan で build して流す | `sh plan/ws344/tests/run-unit.sh <name>... \| all` → `run-unit: PASS` |
| unit/check.h・lbitwriter.h・boolwriter.h | 判定と乱数、VP8L の bit の書き手（§3.2 の逆）、VP8 の bool の encoder（RFC 6386 §7.3 の encoder、tree の書き手つき）。test-helpers.c がこの 2 つを手計算と独立の decoder で確かめる（26 check PASS） | — |
| check-skeleton.sh | header の単独 compile、全 .c（cc・clang、`-std=c89 -Wall -Wextra -Werror -Wpedantic`）、include の範囲、exports.map と宣言の一致、source の開発の管理情報、`BASE=` で header と道具の凍結 | `BASE=<基点> sh plan/ws344/tests/check-skeleton.sh` → `check-skeleton: PASS` |
| target-build.sh | worktree の sysroot の複写 + compat/webp で zedBSD の target を build（既定 `build/amd64/dynamic/libwebp-compat.so`）、warning を数える | `sh plan/ws344/tests/target-build.sh [target...]` → `target-build: PASS (..., 0 warnings)` |
| gen-sources.py・fixtures.list・fixture-encoder.c・craft.py・make-fixtures.sh | 試験の画像: 10 種 × 13〜14 の大きさの源（1x1〜1024x683）、cwebp の設定（q・m・segment・sns・filter・sharpness・simple・preset・alpha の q/filter/method・lossless の z/m/q/near_lossless/exact）、fixture-encoder の token partition 1〜8、craft の container の変種（裸・trailing・VP8X・未知の chunk・ICC/EXIF/XMP・2 つの ALPH・canvas の食い違い・短い VP8X・長すぎる RIFF の size）、img2webp の animation。364 file、期待の出力は dwebp（-pam、-nofancy、-yuv、-nofilter -yuv）と libwebp の driver の報告（.ref）。2 回の生成で全 file の SHA-256 が同じ（fixtures.sha256 に記録） | `sh plan/ws344/tests/make-fixtures.sh` → `make-fixtures: PASS (364 files, 350 decoded by dwebp, hashes same; ...)` |
| fixture-check.c・run-fixtures.sh | 内部の module だけで fixture を dwebp と比べる: lossless（ARGB = -pam）、lossy（plane = -yuv と -nofilter -yuv）、alpha（ALPH = -pam の alpha） | `sh plan/ws344/tests/run-fixtures.sh lossless\|lossy\|alpha` → `run-fixtures: PASS (...)` |
| webp-driver.c・compare.sh | 同じ source を compat と libwebp の両方で build し、全呼び出し・全色空間・option・Into・padding・小さすぎる buffer・partial の features・引数の誤りを 1 行ずつ報告。compat の報告が libwebp の .ref と一致 | `sh plan/ws344/tests/compare.sh [NAME...]` → `compare: PASS (364 files)` |
| fuzz-target.c・mutate.sh・fuzz.sh | 悪意の入力: 決まった変異（mutate）と libFuzzer（fuzz） | `sh plan/ws344/tests/mutate.sh` → `mutate: PASS (...)`、`sh plan/ws344/tests/fuzz.sh [秒]` → `fuzz: PASS (...)` |
| vp8-tables-extract.py | RFC 6386 の text から 20 の表を名前で抜き、C の配列にする（run-unit.sh が `rfc-vp8-tables.h` を作る）。表の値の独立の照合の片方 | — |

- 期待の値の作り方（単体試験）: 仕様の表・式、手の計算、逆の処理（lbitwriter・boolwriter で組んだ stream の往復）、性質（DC だけの IDCT は平ら、完全な prefix code の Kraft の和は 1 など）。oracle（libwebp）は fixture の全体の試験（run-fixtures・compare）だけ。
- 全体の試験の判定は全一致（libwebp と byte ごとに同じ）。

## 9. 実装の Phase

表は [ws.md](ws.md)。各 phase.md が手順（読む物・作る物・試験の項目・注意・変えてはいけない物・command・終わりの条件）。

- 実装（Sonnet 5.5 high、phase-runner-low）: p002〜p016、p018。
- 統合（Opus を推す）: p017（VP8 の frame 全体、run-fixtures lossy）、p019（decode.c、compare・mutate・fuzz）、p020（platform: Windows の static、config、QEMU の guest の driver）、p021（libbrowser）。
- 規約の全文の見直し: p022（conformance-reviewer、header の持ち越しの直し、試験の道具の plan/tools への移動の依頼）。

## 10. 組み込み（p020・p021 の見通し）

- p020: `apps/browser-win32/Makefile` に `include userland/desktop/libwebp-compat/sources.mk`、WIN32_COMPAT_LIBRARIES に webp-compat。`config/ci/*.mk` の ZEDBSD_USER_PROGRAMS に libwebp-compat（libbrowser のある 6 つ: amd64・rpi4・rpi5・odroid-c5・odroid-c5-keiland・visionfive2-keiland。p001 の時点で `config-amd64-server.mk`（ユーザーの物）は libbrowser を持たないので変えない）。pcat・pc98 は libbrowser を build しないので規則を足さない。guest の試験: gif-guest.sh と同じ形の webp-guest.sh（driver を zedBSD 用に build して /lib/libwebp-compat.so と link、QEMU で fixture を流し、libwebp の .ref と比べる）。
- p021: `image/image.h` に IMG_WEBP と img_decode_webp、`img_sniff` に「RIFF」+ 4 byte + 「WEBP」（MIME Sniffing の image の表、12 byte）、`image/webp.c`（WebPGetInfo → img_bitmap_create（IMG_PIXELS_MAX で断る）→ byte の順で WebPDecodeBGRAInto（little endian）か ARGBInto（big endian）で bitmap の 0xAARRGGBB に直に書く。失敗は EINVAL、animation は ENOTSUP でなく EINVAL（SVG に回さない））、sources.mk、Makefile の依存、userland-dynamic.mk の libbrowser.so の link と `--needed libwebp-compat.so`、Windows の build、`net/http.c` の画像の Accept に `image/webp`（と comment の直し）。確かめ: host の libbrowser の image の試験（fixture → img_decode → 期待の RGBA を詰めた値）、QEMU で WebP の画像を持つ local の頁（ws074 の browser の試験の道具）の画面、`make browser-win64` の build。

## 11. Q1 の判断が要る点

1. libwebp 1.5.0 の source（BSD）を設計の担当が読み、互換の状態の規則・変換の定数・buffer の検査を header に自分の言葉で書いた。block の comment に「Values referenced:」「Procedure referenced:」で書く形でよいか（guardrail の出典の規則の範囲。code は写していない）。
2. 新しい compat header は共有の sysroot に写る物で、main に merge した後の build で sysroot の作り直しが走る（stamp が header の一覧に依る）。その build の時期（toolchain の作業）を Q1 が決める。担当は target-build.sh の複写で進める。
3. VP8 の特許: VP8 には Google の royalty-free の特許の許諾（libvpx・libwebp の PATENTS、RFC 6386 §20.27）と、MPEG LA との合意の後の VP8 の cross-license がある。自前の実装に対する許諾の範囲をユーザーが確かめるか（製品の危険の受け入れ）。
4. Animation（about.google などで使われうる）は範囲の外。必要なら別の WS（demux の interface、libbrowser の動く画像）。
5. 公式の試験の data（design-reviewer の指摘 6）: libwebp-test-data（`https://chromium.googlesource.com/webm/libwebp-test-data`、lossless_vec_*・alpha・md5 付き）と libvpx の vp80-00-comprehensive-001〜017 の key frame（`https://storage.googleapis.com/downloads.webmproject.org/test_data/libvpx/`、IVF の最初の frame を RIFF で包む）を取得して fixture に足すか。cwebp が作らない構文（lf の delta、segment の delta mode、update_data 0、color_space・clamping_type 1、scale の bit、libvpx の encoder の partition）を通す。取得してよければ p019 か新しい Phase で、SHA-256 付きの取得の script を作る。

Q1 の答え（2026-11-02）は [ws.md](ws.md) の「Q1 の判断」: 1 は interface の振る舞いと出力の変換の規則に限って可（codec の算術は RFC から）、2 は Q1 が merge の後に sysroot を作り直す、3 はユーザーが危険を受け入れて自前の VP8 で進める（許諾の条件は後でユーザーが確かめる、閉じた）、4 は範囲の外のまま。

## 12. design-reviewer（2026-11-02）

design-reviewer を 1 本（header・design.md・phase.md・試験の道具を、RFC 9649・6386（§20 を含む）と libwebp 1.5.0 の source に照らす敵対的レビュー）。指摘 25 件（blocker 0・major 6・minor 19）と反映:

| # | 指摘 | 反映 |
| --- | --- | --- |
| 1 | p007・p008・p017 の run-fixtures は riff.c を使うのに p002 に依存しない | 依存に p002 を足した（ws.md と phase.md） |
| 2 | 0 bit の VP8L で数十 byte から 2^28 画素を decode し、fuzz が時間切れになる | fuzz-target は 2^22 画素を越える入力を decode しない。時間は画素数に比例と §7 と decode.h に書き、p019 に大きな画像の単体試験 |
| 3 | mutate・driver・fixture-check が 1 byte 余分に確保し、1 byte の読み過ぎを ASan が捕まえない | 全部ちょうどの大きさ（0 は 1 byte を確保して大きさ 0）に |
| 4 | segmentation 有効・update_data 0 の absolute の既定が無い | vp8-header.h に §20.4 の既定（delta、値 0）を書き、p011・p016 の試験の項目に足した。libwebp の差を decode.h に |
| 5 | decode.h の byte 一致の約束と設計の差（filter の 1 回目の clamp、simple code の範囲の外の symbol、ALPH の P、inner edge） | 約束を「libwebp の encoder の file で byte 一致」に弱め差を列挙。simple code は libwebp の読みに変えた（RFC が黙る）。clamp は RFC のまま（Q1 の判断 1）。P と Rsv は §4 と alpha.h に |
| 6 | 公式の適合の stream が無い | §11 の 5 で Q1 に取得の可否を聞く |
| 7 | vp8.h の「coded の無い block の IDCT を飛ばす」が WHT の DC を落とす | 「WHT の後で 16 の係数が全部 0 の時だけ、coded の bit では判断しない」と書いた |
| 8 | alpha.h（size 1 以上の前提）と riff.h（0 がありうる）の矛盾 | size 0・1 は EINVAL と alpha.h に |
| 9 | 可逆の bitstream と ALPH の組み合わせが無い | riff.h に「lossy の時だけ使う、可逆では has_alpha だけ」 |
| 10 | flip の YUV・YUVA の規則、driver に YUV の flip と YUVA の pad が無い | decode.h に plane ごとの規則、driver に YUVA-flip・YUV-external-flip と plane の pad の確かめ |
| 11 | 状態の順と buffer の再利用（is_external_memory ≤ 0、既にある private_memory）が無い | decode.h に順と libwebp と同じ再利用の規則 |
| 12 | riff.h step 7 の「10 byte」が宣言の大きさか手元の大きさか曖昧 | available_size と書いた |
| 13 | vp8-filter.h の limit と (a + 1) >> 1 が紛らわしい | limit は mb_limit・sub_limit そのもの（倍にしない）、(f1 + 1) >> 1 と書いた |
| 14 | 使わない group の scratch の pool の上限が header と phase.md で食い違う | header に「別の pool、同じ limit、group ごとに空」、phase.md を合わせた |
| 15 | check-skeleton が新しい header と common.c・build の file・fixtures.sha256 を見ない | 凍結の list に足し、追跡されていない・BASE の後に足した header と道具を FAIL に |
| 16 | target-build.sh が link 経由で共有の sysroot に書きうる | realpath が main の checkout の下なら FAIL |
| 17 | `aa2426a5763975c358cf667aff3e17bfb841b7e2` が残る | BASE の commit の直後に SHA に置き換える（この p001 の 2 つ目の commit） |
| 18 | mutate.sh の時間の上限の comment の食い違い、空の出力で shell が止まる | comment を直し、空を 0 に。実の時間は p019 で測る |
| 19 | fixture-check の strstr が NUL で終わらない buffer を読む | header を NUL で終わる複写にしてから読み、NULL を確かめる |
| 20 | driver の部分の data が 40 byte まで | 0〜160 byte の全ての長さに |
| 21 | ALPH の P の差が §4 に無い | §4 の ALPH の行に足した |
| 22 | libwebp から得た手順・値の block の出典の comment が無い | riff.h・output.h・decode.h の該当の block に複数行の「Procedure referenced:」「Values referenced:」。compat の header の先頭の「libwebp の名前と値」の文は png.h・gif_lib.h の前例の形のまま（interface の名前の出所で、手順の参照ではない） |
| 23 | p012・p013・p016 の依存が強すぎる | p012・p013 は p009・p010、p016 は p001 に |
| 24 | Figure 20 の範囲の検査が緩い（xi は −7〜8） | p005 を直した |
| 25 | WHT と IDCT の wrap の扱いが不揃い（情報） | §4 の「係数と変換の幅」に理由を書いた |

レビューが「確かめて正しい」とした所: riff.h の step 1〜8 と complete 0 と has_alpha（libwebp の ParseHeadersInternal ほかと一致）、interface の状態と version の検査、buffer の検査と flip、構造体と enum（libwebp 1.5.0 と同じ）、VP8 の partition の規則と逆量子化（132 の上限、×155/100）、frame の level 0、bool の ended（libwebp と完全に一致）、token の context と skip の規則、loop filter の閾値・hev・limit、IDCT と WHT の定数と段の順、intra の端の値と above-right、YUV → RGB の定数と fancy の同値、premultiply、VP8L の code・max_symbol・mode 14/15・Select の同点・indexing・LZ77・cache・group・eos、ALPH の unfilter の端、引いた節の番号、boolwriter と vp8-tables-extract.py。確かめていない所: gen-sources.py・fixture-encoder.c・lbitwriter.h の細部、p020〜p022 の細部と Windows、表の全要素（道具に任せた）、§12.3 の 10 の subblock の式、指摘 2・18 の実際の時間、公式の stream の入手。

## 13. Future Work（この WS の範囲の外）

- animation（`<webp/demux.h>` の WebPAnimDecoder の部分集合、libbrowser の動く画像）。
- incremental decode（WebPIDecoder、画像の届く途中の表示）。
- cropping・scaling、4444・565、可逆の画像の YUV。
- 速さ: VP8 の行ごとの filter、IDCT・filter・YUV 変換の SIMD（SSE2・NEON）、VP8L の table の 2 段の最適化、thread。
- Linux・FreeBSD の Keiland の build（Makefile.linux・Makefile.freebsd）: 使う program ができた時。

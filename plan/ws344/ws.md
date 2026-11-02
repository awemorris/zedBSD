<!-- awesome-plan project=zedbsd record=ws344 -->

# WS344 自前の WebP の decoder（libwebp-compat、libbrowser の画像、VP8・VP8L・ALPH・RIFF）

- Master: [plan/master.md](../master.md)（2026-11-02 11:31 作成）。Primary Milestone: MG006。
- Status: completed（2026-11-02 18:43。全 Phase cleared、Q1 が main に merge 済み）
- 目標: WebP の静止画（非可逆の VP8 の key frame、可逆の VP8L、透明度の ALPH、RIFF・VP8X の入れ物）を読む自前の library **libwebp-compat**（libwebp 1.5.0 の decode の interface の部分集合）を作り、libbrowser の img_sniff・img_decode に RIFF/WEBP の枝を足す。zedBSD（共有 library）と Windows 版のブラウザ（static link）の両方で使う。
- 設計: [design.md](design.md)（module・header・決定・design-reviewer の 25 件の表）。出典は RFC 9649（WebP）と RFC 6386（VP8）。libwebp（BSD）は host の試験の oracle としてだけ使い、tree に取り込んでいない。

## ユーザーの決定

- （2026-11-02 11:31）ユーザーから、BUG-405（about.google の画像が WebP で届き表示されない）を、libwebp の package でなく自前の WebP の decoder で直す、という趣旨の答えを受けた。
- （2026-11-02 11:32）ユーザーから、libwebp-compat（libwebp と互換の API を持つ自前の library、libpng-compat などと同じ形）を標準廉価実装手順で実装する、という趣旨の指示を受けた。共有 library の package の規則と Windows の .exe の static の build を持ち、libbrowser の img_decode がそれを呼ぶ（Q1 の具体化）。
- （2026-11-02 12:45）ユーザーから、VP8 の特許の危険を受け入れて自前の VP8 の decoder で進め、許諾の条件は後でユーザーが確かめる、という趣旨の答えを受けた。

## Q1 の判断（要点）

- libwebp 1.5.0 の source を参照してよいのは RFC が決めていない interface の振る舞い（状態・誤りの規則、API の約束、buffer の検査、bit 一致に要る YUV → RGB と fancy upsampling の規則）だけ。block ごとに「Procedure referenced:」「Values referenced:」で書く。codec の算術は RFC から。libwebp の値（YUV → RGB の定数）のため libwebp の BSD の表示を license の監査と配る形に入れた。
- libwebp-test-data と libvpx の vp80-00-comprehensive の key frame を追加の試験の画像として取得してよい（tree の外、SHA-256 を記録）。
- `coded` の bit（bit 31 だけを使う）と WHT の 1 段目の 16 bit の包みは、全ての試験の画像で libwebp と一致したのでそのまま。
- animation は範囲の外（Future Work）。
- Q1 の記録の「51 … is not a constructor」の script の誤りは、この WS（画像の decode）と関係が無い。

## 結果

- library: `userland/desktop/libwebp-compat/`（19 の header と 20 の .c、内部の接頭辞 `webpc_`）、公開の header `include/libc/compat/webp/types.h`・`decode.h`（libwebp 1.5.0 の名前と値、ABI 0x0209、21 の呼び出し）。zedBSD の `libwebp-compat.so`（version script、check-dynamic-elf、amd64・arm64・riscv64 で warning 0）、Windows の static library（OmniGrammaton に link）。libbrowser の `image/webp.c` が bitmap の画素に直に decode し、`img_sniff` が RIFF/WEBP を見分け、画像の Accept に `image/webp`。libbrowser のある 6 つの config に libwebp-compat。
- 出力: libwebp 1.5.0 の encoder の file で libwebp と byte ごとに同じ（364 の試験の画像と、libwebp-test-data・vp80 の key frame・多 partition の 214 file。API の全呼び出し・色空間・option・Into・padding・部分の data の features を 1 行ずつ比べて全一致）。host・Wine（Windows の static library）・zedBSD の QEMU の guest（/lib/libwebp-compat.so）の 3 つで同じ。
- 安全: 単体試験は ASan・UBSan、mutate（約 12 万入力）・libFuzzer（600 s）で報告 0。悪意の入力の上限は design.md §7（prefix の table 64 MiB など）。
- BUG-405 は fixed: about.google の WebP の画像（拡張子 .png の物を含む 7 枚）が host の browser と Windows 版（Wine）で出た。
- license: `userland/base/licenses/libwebp-compat/libwebp-COPYING`、`tools/release/license-components.json` の `libwebp-values`、Windows の `share/licenses/libwebp-COPYING.txt`。
- 試験の道具: `plan/tools/webp/`（[plan/tools.md](../tools.md) に登録）。WS の中だけの check-skeleton.sh（header の凍結）と target-build.sh（sysroot の複写）は消した。

## 制限・移管

- Future Work: animation（`<webp/demux.h>` の WebPAnimDecoder の部分集合と libbrowser の動く画像）、incremental decode（WebPIDecoder、届く途中の表示）、cropping・scaling、4444・565 の色空間、可逆の画像の YUV の出力、速さ（VP8 の行ごとの filter、IDCT・filter・YUV 変換の SIMD、thread）、Linux・FreeBSD の Keiland の build（使う program ができた時）。
- libwebp と違う点（decode.h の先頭に列挙）: RFC 6386 の参照 decoder に従う所（segment の data の無い key frame、segment の filter level の 1 回目の clamp、0 の token だけの macroblock の内側の edge）、ALPH の reserved・pre-processing の bit を無視、外部の buffer の負の stride を断る、壊れた file の状態の値。どれも libwebp の encoder は作らない。
- 試験の C（`plan/tools/webp/unit/` と driver）の規約の全文の見直しはしていない（library の code は済み）。
- Windows の実機での OmniGrammaton の UAT はユーザーが拾える試験の一覧へ（Wine では確かめた）。

## Phase

| Phase | 目的 | 結果 |
| --- | --- | --- |
| ws344-p001 | 設計（module・header・骨格・試験の道具・Phase の表、design-reviewer） | 19 header と骨格、試験の道具、364 の試験の画像。BASE aa2426a576 |
| ws344-p002 | container（riff.c） | libwebp の状態の規則どおり、test-riff 42324 checks |
| ws344-p003 | VP8L の bit reader と prefix code（lbits.c・prefix.c） | 2 段の table と pool、単体試験 PASS |
| ws344-p004 | VP8L の code の読み（vp8l-codes.c） | simple・normal、max_symbol は token の数、95925 checks |
| ws344-p005 | VP8L の entropy-coded image（vp8l-image.c） | cache・meta prefix・LZ77、90215 checks |
| ws344-p006 | VP8L の逆変換（vp8l-transform.c） | 4 つの変換、457604 checks |
| ws344-p007 | VP8L の decoder（vp8l.c） | lossless の 174 試験の画像が dwebp と全画素一致 |
| ws344-p008 | ALPH（alpha.c） | alpha の 54 試験の画像が一致 |
| ws344-p009 | bool decoder（bool.c） | ended の規則が libwebp と同じ、8376 checks |
| ws344-p010 | VP8 の表（vp8-tables.c） | RFC の text から抜いた表と全要素一致 |
| ws344-p011 | VP8 の frame header（vp8-header.c） | 45344 checks |
| ws344-p012 | VP8 の macroblock header（vp8-modes.c） | 627570 checks |
| ws344-p013 | VP8 の係数（vp8-tokens.c） | 336179 checks |
| ws344-p014 | VP8 の逆変換（vp8-idct.c） | 単体試験 PASS（WHT の 1 段目の包みは RFC どおり） |
| ws344-p015 | VP8 の intra 予測（vp8-predict.c） | 5281 checks |
| ws344-p016 | VP8 の loop filter（vp8-filter.c） | 166661 checks |
| ws344-p017 | VP8 の decoder（vp8.c） | lossy の 352 と conformance の 342 の check が dwebp と全 sample 一致 |
| ws344-p018 | 出力の変換（output.c） | 536115 checks |
| ws344-p019 | interface（decode.c） | compare 364・conformance 214 が libwebp と全行一致、mutate・fuzz 600 s PASS |
| ws344-p020 | platform（Windows の static、config、QEMU の guest） | Wine と QEMU の guest で 578 file 一致、arm64・riscv64 の build |
| ws344-p021 | libbrowser への組み込み（BUG-405） | about.google の画像が host と Wine で出た |
| ws344-p022 | 規約の全文の見直しと持ち越し | style-check 74 → 0、license の表示、fixtures の partition、新しい BASE a6859dc064 |

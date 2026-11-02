<!-- awesome-plan project=zedbsd record=ws344 -->

# WS344 自前の WebP の decoder（libwebp-compat、libbrowser の画像、VP8・VP8L・ALPH・RIFF）

- Master: [plan/master.md](../master.md)（2026-11-02 11:31 作成）。Primary Milestone: MG006。
- Status: incomplete（p001 の設計は P2 が実施・commit 済み、Q1 の受け入れ待ち）
- 目標: WebP の静止画（非可逆の VP8 の key frame、可逆の VP8L、透明度の ALPH、RIFF・VP8X の入れ物）を読む自前の library **libwebp-compat**（libwebp 1.5.0 の decode の interface の部分集合、`include/libc/compat/webp/`）を作り、libbrowser の img_sniff・img_decode に RIFF/WEBP の枝を足す。zedBSD（共有 library）と Windows 版のブラウザ（static link）の両方で使う。animation は後（範囲の外）。
- 出典: RFC 9649（WebP）、RFC 6386（VP8）。試験の期待の値は libwebp（BSD）の出力と照らす（libwebp は試験の道具としてだけ host で使い、tree に取り込まない）。
- 進め方: 標準廉価実装手順（docs/agent/spec-module-delegation.md）。Opus が設計（module の分け方・header・試験の道具）、Sonnet が実装。設計は [design.md](design.md)。
- 置き場所: `userland/desktop/libwebp-compat/`（内部の接頭辞 `webpc_`）、header `include/libc/compat/webp/types.h`・`decode.h`。
- 基点（header と試験の道具を凍結した commit）: `aa2426a5763975c358cf667aff3e17bfb841b7e2`。実装の Phase は `BASE=aa2426a5763975c358cf667aff3e17bfb841b7e2 sh plan/ws344/tests/check-skeleton.sh` で header が変わっていないことを確かめる。

## ユーザーの決定

- （2026-11-02 11:31）ユーザーから、BUG-405（about.google の画像が WebP で届き表示されない）を、libwebp の package でなく自前の WebP の decoder で直す、という趣旨の答えを受けた。
- （2026-11-02 12:45）ユーザーから、VP8 の特許の危険を受け入れて自前の VP8 の decoder で進め、許諾の条件は後でユーザーが確かめる、という趣旨の答えを受けた（design.md §11 の 3 は閉じた）。
- （2026-11-02 11:32）ユーザーから、libwebp-compat（libwebp と互換の API を持つ自前の library、libpng-compat などと同じ形）を標準廉価実装手順で実装する、という趣旨の指示を受けた。共有 library の package の規則と Windows の .exe の static の build を持ち、libbrowser の img_decode がそれを呼ぶ（Q1 の具体化）。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws344-p001 | 設計（標準廉価実装手順 §4）: module・header・骨格・試験の道具・実装の Phase の表、試験の画像（cwebp で作る 364 file） | cleared（2026-11-02 13:25 Q1、BASE aa2426a57 を main に merge。main で sysroot を作り直して libwebp-compat.so を build）。実施済み・道具の確かめ PASS（P2 q1939、2026-11-02、BASE aa2426a576、Q1 の受け入れ待ち） | — |
| ws344-p002 | container（riff.c、test-riff） | cleared（2026-11-02 13:39 Q1、main で test-riff 42324・run-unit all PASS）。実装済み・試験 PASS（Sonnet q1946-i01、2026-11-02、test-riff 42324 checks、Q1 の受け入れ待ち） | p001 |
| ws344-p003 | VP8L の bit reader と prefix code（lbits.c・prefix.c） | cleared（2026-11-02 13:36 Q1、main で test-lbits・test-prefix PASS）。実装済み・試験 PASS（Sonnet 5.5、2026-11-02。cleared は Q1） | p001 |
| ws344-p004 | VP8L の code の読み（vp8l-codes.c） | cleared（2026-11-02 13:55 Q1、main で test-vp8l_codes 95925 PASS）。実装済み・試験 PASS（Sonnet 5.5、2026-11-02。cleared は Q1） | p003 |
| ws344-p005 | VP8L の entropy-coded image（vp8l-image.c） | cleared（2026-11-02 14:17 Q1、main で test-vp8l_image 90215 PASS）。実装済み・試験 PASS（Sonnet 5.5 q1957-i01、2026-11-02、test-vp8l_image 90215 checks。cleared は Q1） | p004 |
| ws344-p006 | VP8L の逆変換（vp8l-transform.c） | cleared（2026-11-02 14:14 Q1、main で test-vp8l_transform 457604 PASS）。実装済み・試験 PASS（Sonnet 5.5 q1956-i01、2026-11-02、test-vp8l_transform 457604 checks、run-unit all・check-skeleton・target-build PASS。cleared は Q1） | p001 |
| ws344-p007 | VP8L の decoder（vp8l.c、run-fixtures lossless） | cleared（2026-11-02 14:32 Q1、main で test-vp8l 9490・run-fixtures lossless 174 PASS は担当の結果、main で run-unit all PASS）。実装済み・試験 PASS（Sonnet q1962-i01、2026-11-02、test-vp8l 9490 checks・run-fixtures lossless 174 PASS、Q1 の受け入れ待ち） | p002, p005, p006 |
| ws344-p008 | ALPH（alpha.c、run-fixtures alpha） | cleared（2026-11-02 14:42 Q1、main で test-alpha PASS（run-fixtures alpha 54 は担当の結果））。実装済み・試験 PASS（Sonnet q1964-i01、2026-11-02、test-alpha 2293 checks・run-fixtures alpha 54 PASS、Q1 の受け入れ待ち） | p002, p007 |
| ws344-p009 | bool decoder（bool.c） | cleared（2026-11-02 13:29 Q1、main で test-bool 8376 PASS）。実装済み・試験 PASS（Sonnet q1948-i01、2026-11-02。cleared は Q1） | p001 |
| ws344-p010 | VP8 の表（vp8-tables.c、RFC の text と照合） | cleared（2026-11-02 13:37 Q1、main で test-vp8_tables 37674 PASS）。実装済み・試験 PASS（Sonnet q1949-i01、2026-11-02。cleared は Q1） | p001 |
| ws344-p011 | VP8 の frame header（vp8-header.c） | cleared（2026-11-02 13:55 Q1、main で test-vp8_header 45344 PASS）。実装済み・試験 PASS（Sonnet q1951-i01、2026-11-02。cleared は Q1） | p009, p010 |
| ws344-p012 | VP8 の macroblock header（vp8-modes.c） | cleared（2026-11-02 13:46 Q1、main で test-vp8_modes 627570 PASS）。実装済み・試験 PASS（Sonnet q1952-i01、2026-11-02。cleared は Q1） | p009, p010 |
| ws344-p013 | VP8 の係数（vp8-tokens.c） | cleared（2026-11-02 13:58 Q1、main で test-vp8_tokens 336179 PASS）。実装済み・試験 PASS（Sonnet 担当、2026-11-02） | p009, p010 |
| ws344-p014 | VP8 の逆変換（vp8-idct.c） | cleared（2026-11-02 14:07 Q1、main で test-vp8_idct PASS）。実装済み・試験 PASS（Sonnet q1958-i01、2026-11-02。cleared は Q1） | p001 |
| ws344-p015 | VP8 の intra 予測（vp8-predict.c） | cleared（2026-11-02 14:19 Q1、main で test-vp8_predict 5281 PASS）。実装済み・試験 PASS（Sonnet q1960-i01、2026-11-02 14:18。cleared は Q1） | p001 |
| ws344-p016 | VP8 の loop filter（vp8-filter.c） | cleared（2026-11-02 14:41 Q1、main で test-vp8_filter 166661 PASS）。実装済み・試験 PASS（Sonnet q1961-i01、2026-11-02 14:38、Q1 の受け入れ待ち） | p001 |
| ws344-p017 | VP8 の decoder（vp8.c、run-fixtures lossy）— Opus を推す | in-progress（2026-11-02 14:41 P2 q1966-i01） | p002, p011〜p016 |
| ws344-p018 | 出力の変換（output.c） | cleared（2026-11-02 14:30 Q1、main で test-output 536115 PASS）。実装済み・試験 PASS（Sonnet q1963-i01、2026-11-02 14:28、cleared は Q1 が決める） | p001 |
| ws344-p019 | interface（decode.c、compare・mutate・fuzz）— Opus を推す | planned | p002, p008, p017, p018 |
| ws344-p020 | platform（Windows の static、config、QEMU の guest の driver）— Opus を推す | planned | p019 |
| ws344-p021 | libbrowser への組み込み（img_sniff・img_decode・Accept、BUG-405）— Opus を推す | planned | p020 |
| ws344-p022 | 規約の全文の見直し（conformance-reviewer） | planned | p021 |

最初に並べて起動できる Phase（依存が p001 だけ）: p002、p003、p006、p009、p010、p014、p015、p016、p018。

## Q1 の判断

- （2026-11-02 14:07、p014 の報告）vp8-idct.c の WHT の最初の段は RFC 6386 14.3・20.8 のとおり 16 bit に包む（header の comment の「int32_t のまま」は誤り。凍結の header なので p022 で直す）。普通の encoder の出力では違いは出ない。p017 で fixture が libwebp と一致するかを確かめる。

- （2026-11-02 13:58、p013 の報告）vp8-tokens.c の `coded` の bit は header と phase.md のとおり context の値 t（block が最初の位置を越えたか）にした。RFC 6386 20.16 の `eob_mask |= (c > 1) << i` とは、位置 0 から始まり token が 1 つだけの block で違う。p017（VP8 の decoder の統合）で、この bit の使い方（IDCT の DC だけの近道など）が fixture で libwebp と一致するかを必ず確かめる。

- （2026-11-02 13:31、design.md §11-5）libwebp-test-data と libvpx の vp80-00-comprehensive の key frame を追加の試験の画像として取得してよい。tree に入れず共有の `build/sources/` に置き、SHA-256 を記録して照合する。期待の値は libwebp 1.5.0 の出力と照らす（試験の道具の扱いは他の試験の画像と同じ）。取得と組み込みは p017・p019 の担当が行う。
- （2026-11-02 13:31）共有の sysroot（amd64・arm64・riscv64）を Q1 が作り直した。新しい compat/webp の header が入ったので、以後の Phase は target-build.sh の複写を使わなくてよい。

- （2026-11-02 Q1、design.md §11 への答え）
  1. libwebp 1.5.0 の source を参照してよいのは、RFC が決めていない interface の振る舞い（状態・誤りの規則、API の約束、buffer の検査、bit 一致の出力に要る fancy upsampling の YUV → RGB の規則）だけ。block ごとに「Procedure referenced: libwebp 1.5.0 <file> (BSD-3-Clause)」「Values referenced: …」と書く。codec の算術（VP8・VP8L・ALPH）は RFC 9649・6386 から。code・構造は写さない。表の値を libwebp から取ったら（output.c の YUV → RGB の定数）、libwebp の BSD の表示を license の監査に足す（p022）。
  2. 共有の sysroot は BASE の commit の merge の直後に Q1 が作り直す。Q1 が知らせるまで target-build.sh の複写を使う。
  3. VP8 の特許の点は Q1 がユーザーに聞いた（答えは「ユーザーの決定」、閉じた）。
  4. animation は範囲の外のまま（Future Work）。

## Resume point

- 2026-11-02 11:31 Q1: 作成。p001 の担当を割り当てる。
- 2026-11-02 P2: p001 の設計を実施（[phase001](phase001/phase.md)）。基点 `aa2426a5763975c358cf667aff3e17bfb841b7e2`。次は Q1 の受け入れと §11 の判断、実装の Phase の起動。

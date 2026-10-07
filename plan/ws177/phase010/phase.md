<!-- awesome-plan project=zedbsd record=ws177-p010 -->

# ws177-p010: keiland-preview の仕上げ（案 J）

Parent: [WS177](../ws.md)
Status: test-wait（2026-10-08 P1 q886 の 2: 実装・host PASS・zedBSD と Linux の build warning 0。QEMU は T1 の AAT `apps.files.thumbnails`）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q886 の 2（P1、2026-10-08）
Origin: [backlog-p2](../backlog-p2.md) の 131・133・142（WS168 ws168-p003・p004）、[案](../phasing-20261008.md) の J、設計 [ws168-p001](../../ws168/phase001/phase.md) の §4.3・§5.2

## 設計と変更（2026-10-08 P1）

| 行 | 形 |
| --- | --- |
| 131 埋め込まれていない font | libpdf に `pdf_font_memory_add(name, data, size)`（`include/libc/pdf.h`・`exports.map`、userland の library の API の追加）: program が memory で渡した代わりの font を、その file の名前（`keiland.ttf` など）で、file を探す前に使う（最大 8、文書を開く前に 1 つの thread から）。memory の font は companion の file を開かない。keiland-preview は `preview/fonts.c` で desktop の Mahora（sans・bold・mono、計 111 KB）を assembler の `.incbin` で program に入れ、確認・閉じ込めの後に libpdf に渡す（`preview_fonts_register`）。serif は sans で描く。Mahora は ASCII だけで、他の文字は描かない（fallback の font は program が開けない file）。設計 §4.3 の Inter 0.9 MB は今の desktop の font（Mahora）に読み替えた。build の規則は変えない（`fonts.c` を package の source に足しただけ。.incbin の path は repository の top から） |
| 133 爆弾・fuzz | code の変更は無し（今の上限で止まった）。試験 `host-preview-fuzz.{sh,py}` を足した: 子の上限（1 GiB・CPU 10 秒・48 MiB、linux/spawn.c と同じ）と 10 秒で、爆弾（PNG の 65535² の header・8192² の zlib の爆弾・幅 2^24 の行、JPEG 65000²、GIF 65535² の画面、PPM 100000²、64 MiB を越える入力、10^9 の page）は 2〜4 で断り、深い配列・200 万の op・page の輪・自分を指す長さの PDF は status で終わる。良い file（PNG・JPEG・GIF・PPM・PDF）をでたらめに壊した N 個は全部 0〜4 で終わる（signal・時間切れ無し） |
| 142 縮小画像の子を 2 つ同時 | Files の `thumb.c`: maker を 2 つ（`FM_THUMB_MAKERS`、設計 §5.2）、頼む file の list も 2 つ（`thumb_wanted[2]`）。一つの tick で空いた maker に頼みを渡す。slot の選びは作っている slot を全部避ける。子の起動を log（`ZFILES THUMB start path= pid=`） |
| 142 失敗の印を cache に | `thumb-cache.c`: 失敗の記録（1 行目 `KF`、2 行目に同じ stamp）を記録の場所に rename で置く（`fm_thumb_cache_fail`）。`fm_thumb_cache_read` は今の file の失敗の記録に EINVAL を返し、Files は子を起こさない（log `cached=1 failed=1`）。file が変われば stamp が合わず試し直す。印を残すのは file の責の失敗（形が分からない・壊れ・大きすぎ・memory・時間切れ・読み戻せない出力）で、書けない（EIO）は残さない |
| 142 Quick Look・Today を待たずに | `client.c` に `preview_picture_begin`・`preview_picture_follow`・`preview_picture_cancel`・`preview_status_error`（`preview_picture` はこれらで書き直した。時間切れ・signal は EIO から ETIMEDOUT に）。Files の `fm_picture_begin/follow/cancel/busy`（FM_PICTURE_PEEK・HERO）。Quick Look・preview の欄の絵（`fm_peek_picture` は頼むだけ、`fm_peek_tick` が受け取る）と Today の hero（`fm_home_tick`、待つ間は gradient）は子を待たずに、終わった tick で描き直す。`fm_ui_wait` は子のある間 20 ms ごとに見る。file を移る・閉じる時は子を止める。Settings の背景の tile は前から loader の thread の中で待つので窓は止まらない（変えない） |

- 変更: `userland/base/libpdf/font.c`・`exports.map`、`include/libc/pdf.h`、`userland/desktop/preview/fonts.c`（新）・`main.c`・`preview.h`・`client.c`・`client.h`・`Makefile`・`Makefile.linux`、`userland/desktop/files/files.h`・`thumb.c`・`thumb-cache.c`・`peek.c`・`ui-home.c`・`ui.c`。
- 試験: `plan/ws177/tests/host-preview-fuzz.{sh,py}`（新）。登録済みの道具の追従: `plan/tools/files/host-model.c`（2 つ同時・失敗の印・待たない絵）、AAT `plan/tools/aat/scenarios/helpers_preview.py` と `tests/scenarios/apps/files/thumbnails.md`（2 つ同時・失敗の印・PDF の文字）。WS168 の `plan/ws168/tests/run-host-preview.sh` の source に `fonts.c` を足した（無いと link できない）。

## 確認（host・build、2026-10-08）

- `sh plan/ws177/tests/host-preview-fuzz.sh 300 1`・`1000 2` → PASS。`ASAN=1`（ASan/UBSan、閉じ込めと memory の上限なし）で `1000 3`・`5000 4`（fonts.c の前）・`1000 6`（後）→ PASS。埋め込まれていない Helvetica の「HELLO」が描かれる（暗い画素 1781）。
- `sh plan/ws168/tests/run-host-preview.sh build/p1-ws168/host-preview` → host-preview の 30 の ok、host-client PASS ×2。
- `sh plan/tools/files/host-model.sh` → files-model PASS（2 つ同時、壊れた PNG の失敗の印、次の session で読む、変わった file は試し直す、待たない絵 64x43 と EINVAL）。
- build（warning 0）: `make -j16 BUILD=build/p1-j ZEDBSD_CONFIG=plan/ws089/tests/config-amd64-settings.mk build/p1-j/bin/keiland-preview build/p1-j/bin/files build/p1-j/bin/settings build/p1-j/dynamic/libpdf.so`（keiland-preview に Mahora、libpdf.so に `pdf_font_memory_add`）。Linux: `make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/p1-linux build/p1-linux/libexec/keiland-preview build/p1-linux/bin/files`。style-check: 変えた file は 0 件。

## 未実施

- QEMU（T1）: AAT `--only 'apps\.files\.thumbnails'`（zedBSD の sandbox の中の Mahora の描画、2 つ同時、失敗の印、SANDBOX deny が増えない）。
- Quick Look・Today の待たない描画の QEMU での確認（log の PEEK picture の行は AAT の他の scenario にある）。FreeBSD の build（freebsd/spawn.c は変えていない）。

## Event

2026-10-08 / q886-i02（P1）: 実装と host・build の確認。途中で T1-412 の p004 の直しのために区切った（692f9e0f7）。

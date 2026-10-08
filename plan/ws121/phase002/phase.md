<!-- awesome-plan project=zedbsd record=ws121-p002 -->

# ws121-p002: libmedia（mediafile の source・再生の engine・共有の library）

Status: cleared（2026-10-07 Q1 の判定: T1-315 の AAT browser.video（`MEDIA play position_ms=0`・`pause position_ms=5083`）と playing の PNG で絵と操作の帯（一時停止の印・進みの bar）を Q1 が目視。音は QEMU では聞いていない）（旧: in-progress（2026-10-07 q831 P2））
Disposition: normal
Parent: [WS121](../ws.md)
Queue: q831（2026-10-07、P2）
依存: [p001](../phase001/phase.md)（設計の第 3 版。U0〜U7 はユーザーの判断待ちのまま、既定（推奨）で進める: Q1 に 2026-10-07 に送った）、WS122 p003・p004

## 範囲（正常系）

- `userland/desktop/mediafile`: `mf_open_source`（`struct mf_source` の `read_at`・`size`・`context`、読みは全て `mf_read_at` の 1 か所を通る。先頭の 12 byte も）。
- `userland/desktop/libmedia/`（新規、`/lib/libmedia.so`、NEEDED は libc だけ）: `media.h`、`engine.c`（file の open は engine の thread で、wake の pipe、video の track は任意（音だけの file）、`MEDIA_SOUND` が無い・audiod が無い時の音だけの file は無音で monotonic の時計、picture は呼び手が自分の pixel の大きさで取る）、`media_set_log`（library の log の hook。codec.c・audio.c の `vp_log` もここを通る）。source は今の所 videoplayer の `codec.c`・`bitstream.c`・`audio.c` と mediafile をそのまま compile する（移動・名前の変更はしない。設計 D1 の「接頭辞を media_ に揃える」「videoplayer をそれに移す」は後: backlog）。
- build: `platform/amd64/vmunix.mk` の libmedia.so の規則、`userland/desktop/libmedia/Makefile`・`exports.map`（`media_set_log`・`media_engine_*` だけ）。

## 確かめ

- host: `sh plan/tools/media/run-host-mediafile.sh` → PASS（sample.mp4 を source で読んで mf_open と同じ出力）。
- host: `sh plan/ws121/tests/run-host-engine.sh` → PASS 10（sample.mp4 を source から: open の wake・320x240・20 s、停止中の最初の絵、1 秒の再生で絵が 15 枚以上・時計 0.8〜1.5 s、5 s への seek、一時停止で時計が止まる。2 s の AAC の m4a を path から: 音だけ・audiod 無しで無音の再生・終わり。MP4 でない file は MEDIA_FAILED）。ASan・UBSan、host の FFmpeg 7（major 61）を dlopen。
- zedBSD: `make ZEDBSD_CONFIG=plan/ws120/tests/config-amd64-music.mk BUILD=build/ws120-zed build/ws120-zed/dynamic/libmedia.so` warning 0。
- style-check 0（engine.c・media.h・mediafile.c）。

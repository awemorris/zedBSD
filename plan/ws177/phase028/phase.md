<!-- awesome-plan project=zedbsd record=ws177-p028 -->

# ws177-p028: MPEG-TS の reader（案 T の 2）

Parent: [WS177](../ws.md)
Status: cleared（2026-10-08 夜 Q1: T1-469、4 つの container が OPENED error=0・PLAY・dropped=0・絵が出た。回帰の videoplayer・music は fail なし。apps.browser.video の音なしは H3 (c)（browser の音はベータ3）で想定どおり、scenario の期待を直す）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q906（P2、承認: ユーザー 2026-10-08 夜「Tは通常優先度でスケジューリングをお願いします」）
Origin: [backlog-p2](../backlog-p2.md) の 116〜123 のうち browser の video を除く分、[案](../phasing-20261008.md) の T。
依存: ws177-p027（同じ試験の道具）

## 範囲

- MPEG-TS（188 byte の packet、PAT・PMT、PES、H.264（Annex B のまま）・AAC の ADTS（ADTS の header を外して AudioSpecificConfig を作る）・MP3、90 kHz の PTS・DTS、PCR は使わない）。
- 長さは最初と最後の PTS から、seek は PTS で二分（key は H.264 の IDR・random access indicator）。
- 切れた file・同期の外れ（0x47 を探し直す）。

## 実装（2026-10-08 P2）

- `userland/desktop/mediafile/ts.c`（新規）: 188 byte と 192 byte（M2TS）の packet（先頭の 3 packet の sync で判定、`mf_ts_detect`）、PAT の最初の program の PMT（1 packet に収まる section だけ）、stream type 0x1b H.264・0x24 H.265・0x0f AAC（ADTS）・0x03/0x04 MPEG audio・0x06 private（最初の PES が ADTS なら AAC。ffmpeg の M2TS がそう書く）。
- PES は PID ごとに組み立て、次の unit start・PES の長さ・file の終わりで閉じる。video は PES を 1 packet（Annex B のまま。private data は無く、player の bitstream は素通し）、audio は ADTS・MPEG audio の frame に切って 1 frame 1 packet、時刻は PES の PTS + 前の frame の sample（90 kHz で）。key は adaptation field の random access か、最初の slice が IDR（H.264）・IRAP（H.265）。
- 時刻は 90 kHz の 33 bit を、先頭の track の最初の PTS の最小を 0 として、約 3 時間前までは負、それより先は wrap として読む。長さは末尾 2 MiB の最大の PTS。H.264 の幅・高さは SPS（high profile の scaling list、cropping を含む）、音の rate・channel は最初の frame の header。
- 壊れ方: sync byte が外れたら次の 2 つ続く sync を探す（読む時も scan の時も）、continuity counter の欠け（lost packet）・長さに足りない PES・16 MiB 超の PES は落として `dropped_count` に数える、同じ counter の重複は捨てる、transport error の印の packet は使わない。
- seek: 先頭の video track の PES の PTS で file を二分 → その場所から 256 KiB（倍々で先頭まで）戻って、目標以前の最後の key frame の packet から読む。
- `mediafile.c`: 先頭 584 byte を読んで判定（TS は MP4 の後）、`mediafile-private.h` に `mf_ts_format`・`mf_ts_detect`。3 つの Makefile（libmedia・music・videoplayer）に ts.c。
- 他の WS の host 試験の source の一覧に ts.c（Q1 の許可 2026-10-08 夜）: plan/tools/media/run-host-mediafile.sh・run-host-codec.sh（あわせて `rm -rf` を `fresh_out` に）、plan/ws121/tests/run-host-engine.sh、plan/ws074/tests/host-build.sh。
- 範囲の外（記録）: 複数の packet にまたがる PAT・PMT の section、PMT の更新、LATM の AAC・AC-3・Opus in TS、H.265 の SPS の幅・高さ（0 のまま、decoder が知る）、PCR。Files の拡張子の表（`files/mime.c`）に m2ts・mts が無い（".ts" は TypeScript と重なる）→ p031 で Q1 に相談。

## 確認

- host: `sh plan/ws177/tests/host-media-t.sh ts mp4` → PASS（ASan・UBSan）。ffmpeg で作った 5 つ（H.264+AAC、H.264+MP3、M2TS、`-output_ts_offset 1000`、`-output_ts_offset 95442`（3 秒の間に 33 bit が wrap））を ffprobe の packet（PTS・DTS を同じ start から、size・key・Adler-32）と照合、seek 5 点。変えた 3 つ: packet の間に 0 を 100 byte（全部一致、seek も）、途中の byte で切る（各 track が ffprobe の先頭と一致、落とす数 1 以下）、audio の packet を 1 つ抜く（その PES だけ落ちて dropped 1、video は全部一致）。
- host（他の WS）: `run-host-mediafile.sh` PASS、`run-host-codec.sh` PASS、`plan/ws121/tests/run-host-engine.sh` PASS、`plan/ws074/tests/host-build.sh plain`（BROWSER_HOST_BUILD を build/tmp に）exit 0。
- build: `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/amd64 build/amd64/bin/videoplayer build/amd64/bin/music build/amd64/dynamic/libmedia.so` exit 0・warning 0。style-check 指摘なし。
- QEMU: 未実施（p031 の後に T1 へ）。

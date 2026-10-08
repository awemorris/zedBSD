<!-- awesome-plan project=zedbsd record=ws177-p030 -->

# ws177-p030: AVI の reader（案 T の 4）

Parent: [WS177](../ws.md)
Status: test-wait（T1-469、2026-10-08 夜 Q1）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q906（P2、承認: ユーザー 2026-10-08 夜「Tは通常優先度でスケジューリングをお願いします」）
Origin: [backlog-p2](../backlog-p2.md) の 116〜123 のうち browser の video を除く分、[案](../phasing-20261008.md) の T。
依存: ws177-p029

## 範囲

- RIFF・LIST hdrl（avih・strl の strh・strf）・LIST movi・idx1（無ければ movi を走査）、OpenDML の AVIX は範囲外（読める所まで）。
- MPEG-4 Part 2・H.264・MJPEG、PCM・MP3・AAC。時刻は strh の rate/scale と frame の番号（音は byte 数か block）。
- index の外を指す entry を落として数える（p027 と同じ dropped_count）。

## 実装（2026-10-08 P2）

- `userland/desktop/mediafile/avi.c`（新規）: RIFF 'AVI '（`mf_avi_detect`）。hdrl の strl ごとに strh（vids・auds、scale・rate・sample size）と strf（BITMAPINFOHEADER の幅・高さ・fourcc、WAVEFORMATEX の tag・channel・rate・bit）。video: XVID・DIVX・DX50・FMP4・MP4V・M4S2 → MPEG-4（strf の後ろの VOL を private data に）、H264・X264・AVC1・DAVC → H.264（Annex B）、HEVC・H265・HVC1、MJPG。sound: PCM（codec_name に pcm_u8・pcm_s16le・pcm_s24le）、MP3（0x55）、AAC（0xff は AudioSpecificConfig を private に、0x1600 は ADTS）。他の stream は番号だけ持って読まない。
- packet: idx1 があればそれで（offset の基準は movi の list か file の先頭か、最初の entry が chunk の code を指す方）、無ければ movi の chunk を順に（LIST rec の中も、key は H.264 の IDR・MPEG-4 の I-VOP を中身で）。OpenDML の後ろの RIFF AVIX の movi も chunk を順に読んで足す。時刻: video は frame の番号×scale/rate、sound は sample size があれば前の byte 数÷sample size（block）、0 なら chunk の番号。空の chunk（落とした frame）は時間だけ進めて packet にしない。
- index が file の外を指す entry・MF_PACKET_MAX 超は落として数える（`dropped_count`）。packet は file の順で渡し、seek は先頭の video track の時刻以前の最後の key frame から。長さは各 track の数えた単位から。
- `mediafile.h` に `MF_CODEC_MJPEG`（12）・`MF_CODEC_PCM`（13）。`mediafile.c` の判定（AVI は Ogg の前）、3 つの Makefile と他の WS の 4 つの試験の一覧に avi.c。
- 範囲の外（記録）: OpenDML の indx（super index）、AVIX の中の index、palette change（##pc）、MJPEG・PCM の decoder の対応（p031 で codec_names に）、packed B-frame の DivX の時刻の並べ替え（pts は frame の番号のまま）。

## 確認

- host: `sh plan/ws177/tests/host-media-t.sh mp4 ts ogg avi` → PASS（ASan・UBSan）。ffmpeg で作った 3 つ（MPEG-4＋MP3（VBR、空の frame あり）、H.264＋PCM 8 kHz、MJPEG）を ffprobe と照合（pts・dts・size・key・Adler-32）、seek 5 点。変えた 3 つ: idx1 を JUNK に（chunk を順に読んで同じ packet と key）、idx1 の 1 entry を file の外に（その 1 つだけ落ちて数える）、6 割で切る（index ごと失い、各 track が ffprobe の先頭と一致）。
- host（他の WS）: ws122 の 2 つ・ws121 の engine・ws074 の host-build（exit 0）。
- build: videoplayer・music・libmedia.so exit 0・warning 0。style-check 指摘なし。
- QEMU: 未実施（p031 の後に T1 へ）。

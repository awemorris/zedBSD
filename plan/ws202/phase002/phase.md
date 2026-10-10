<!-- awesome-plan project=zedbsd record=ws202-p002 -->

# ws202-p002: 試験の stream と参照、host 試験の共通の枠

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 5 LW
依存: p001、J5（1080p の試料）

## 目的

AAC と H.264 の Phase が使う試験の stream と参照を、host の ffmpeg で合成の素材から再現できる形で作る（design §10.1）。

## 成果

- `plan/ws202/tests/make-streams.sh`: design §10.1 の表の stream を `plan/ws202/tests/streams/` に作る。頭に ffmpeg・libx264 の版と各 stream の command。
  `-fflags +bitexact -flags:v +bitexact -flags:a +bitexact -map_metadata -1`、x264 は `threads=1`。
  - AAC: `-c:a aac -profile:a aac_low`。`aac-lc-96k.m4a` は 30 kHz と 36 kHz の sine を含める（down sampling の折り返しの試験、M2-04）。PNS・intensity・TNS は ffmpeg の既定で有効なので、波形を比べる stream は `-aac_pns 0 -aac_is 0` 等を明示する（design §15 E5、
    表の通り）。5.1 は `-ch_layout 5.1`（`-channel_layout` ではない、§15 E8）。.m4a は `-f ipod`。`aac-adts.ts` は `-c:a copy -f mpegts`。
  - H.264: `plan/ws083/tests/make-streams.sh` の 6 本と同じ素材・同じ x264 の引数で、**x264 で直に mp4 へ** encode（`-fps_mode passthrough -f mp4`）。probe に渡す
    Annex B は同じ mp4 から `-c copy -bsf:v h264_mp4toannexb -f h264` で取り出し、`streams/` に置く（両方の試験に同じ bytes）。raw の .h264 を `-c copy` で mp4 に
    しない（ctts が無くなる、design §15 E1）。例外は `h264-nocts.mp4`（わざと ctts を無くす回帰の試料）。
  - `h264-high-b-aac.mp4`（`bframes=3:b-pyramid=normal:open-gop=1:keyint=30:min-keyint=30:scenecut=0`）、`h264-main-crop-sar.mp4`、`h264-baseline-small.mp4`、
    `h264-nocts.mp4`、`h264.mkv`・`h264.ts`（`-c copy`）、**`h264.avi` は `-c copy -bsf:v h264_mp4toannexb`**（`-c copy` だけでは ffmpeg の AVI は長さ付きの NAL と `strf` の
    avcC になり読めない、review-002 M2-08）。
  - `h264-gap.mp4`（M2-09、第 4 版で直した M3-03）: x264 の `ref=3:bframes=3:b-pyramid=strict:open-gop=1:keyint=25:min-keyint=25:scenecut=0` の 5 s の `h264-gap-orig.mp4`
    （非 IDR の I が後に 2 つ以上ある）を作り、1 つ目か 2 つ目の GOP の中の参照の picture（nal_ref_idc ≠ 0）の packet を
    `ffmpeg -i h264-gap-orig.mp4 -c copy -bsf:v "noise=drop=eq(n\,K)" …` で抜く（残りの pts・dts・ctts は保たれる: review-003 R3-3）。`plan/ws202/tests/gen-gap.py`
    （`python3 -I`）はその mp4 の avcC（と in-band）の SPS の `gaps_in_frame_num_value_allowed_flag` を 1 に立てるだけ（RBSP を unescape して field を順に読み、bit を立て、
    escape し直す）。SPS の長さが変わる時は avcC・stsd・stbl・minf・mdia・trak・moov の box の大きさを直す（moov が mdat の後なら stco は変わらない。前なら stco も直す）。
    抜く K は、後の picture の少なくとも 1 つが欠けた参照を list の有効な範囲に持たないように選べればそれを選び（`trace_headers` の `num_ref_idx_active` と modification を
    見る。U17）、選べなければ最初の参照の P を抜き、どちらを選んだかを script の出力に書く。
  - `--large`（J5 の推しの時）: 合成の 1080p（testsrc2・mandelbrot、x264 の Baseline・Main・High、10 s、AAC 付き）と参照を、走らせた者の worktree の
    `build/ws202-large/` に作る（tree に入れない）。bitexact なので T1 が自分で走らせれば同じ bytes（L2-13）。
- 参照:
  - video: `ffmpeg -i X.mp4 -fps_mode passthrough -pix_fmt nv12 -f framehash -hash sha256 -` から、1 行に **pts（µs）と hash** の `X.sha256`（WS083 の hash だけの形と違う、
    design §10.1、L2-01）。frame の数を `ffprobe -count_frames` の `nb_read_frames` と照らす。full range も `-pix_fmt nv12` でよい（design §15 E11）。
  - `h264-gap.mp4` の参照は `h264-gap-orig.mp4` の参照。
  - `h264-nocts.mp4` の参照（M3-05）: ffmpeg から作らない（ctts の無い B のある mp4 で frame を出さないことがある: review-003 R3-1）。hash は `h264-high-b-aac.mp4` の参照の hash、
    pts は `h264-nocts.mp4` の packet の時刻を整列して表示の順に当てた値（script が mp4 の stts から作る）。
  - audio の float の参照は tree に入れない（p006 の試験が実行の時に作る）。target の `.rms` は p011 で作る（design §10.3）。
- `plan/ws202/tests/host-common.sh`: `plan/tools/fresh-out.sh` の `fresh_out` で `build/ws202-host-<名>` を作る、cc の flag
  （`-std=gnu99 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer -I.`）。rm を書かない。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/make-streams.sh` を 2 回 | 2 回とも同じ `sha256sum plan/ws202/tests/streams/*` |
| `ffprobe` で各 stream | codec・profile・channel・rate・SAR・colour が design §10.1 の通り |
| mp4 の box を読む小さい script（`python3 -I`） | B のある全 mp4（`h264-nocts.mp4` を除く）の video の stbl に `ctts`、`h264-high-b-aac.mp4` の `stss` に非 IDR の I（`trace_headers` の nal_unit_type 1）がある（§15 E1・E3 の作り方） |
| `ffmpeg -c:a aac -i aac-*.m4a -c:a pcm_f32le -f f32le -` | 全 AAC の stream が decode できる、elst の media_time 1024 |
| 最初の video の packet の先頭の bytes（`ffmpeg -c copy -f data` か box・PES を読む script） | `h264.ts`・`h264.avi` は start code（`00 00 00 01` か `00 00 01`）で始まる（M2-08） |
| `ffmpeg -i h264-gap.mp4 -c copy -bsf:v trace_headers -f null -` | SPS の gap の flag が 1、frame_num が 1 つ飛ぶ、抜いた後に非 IDR の I が 2 つ以上。`gen-gap.py` の出力に抜いた picture と U17 の選び方、avcC の長さが元と同じか box を直したか |
| `h264-nocts.sha256` の行の数 | `h264-nocts.mp4` の video の packet の数と同じ（M3-05） |
| `.sha256` の行 | pts と hash の 2 列、pts が表示順に増える |
| `sh plan/tools/media/run-host-mediafile.sh` | 既存の PASS |

## 注意

- tree の外の 1080p の sample（`/home/awe/zedbsd-media/`）は UAT だけに使う。
- 試験の image の入力は tree の中の file だけ。

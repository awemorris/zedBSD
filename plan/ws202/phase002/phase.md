<!-- awesome-plan project=zedbsd record=ws202-p002 -->

# ws202-p002: 試験の stream と参照、host 試験の共通の枠

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 3 LW
依存: p001

## 目的

AAC と H.264 の Phase が使う試験の stream と参照を、host の ffmpeg で合成の素材から再現できる形で作る。

## 成果

- `plan/ws202/tests/make-streams.sh`: design §10.1 の表の stream を `plan/ws202/tests/streams/` に作る。頭に ffmpeg・libx264 の版（host の
  `ffmpeg -version` の 1 行目と libx264 の版）と、各 stream の command を書く。何度走らせても同じ bytes になるように `-fflags +bitexact
  -flags:v +bitexact -flags:a +bitexact`・`-map_metadata -1`・`-threads 1` を付ける（x264 は `-x264-params threads=1`）。
- 参照: 同じ script が
  - video: `ffmpeg -i X.mp4 -f rawvideo -pix_fmt nv12 -` の出力を frame の大きさで分けて SHA-256 を 1 行ずつ（表示順）→ `X.sha256`。
    計算は vkvideo-probe の `--expect` と同じ形（`plan/ws083/tests/make-streams.sh` の参照の作り方を読んで合わせる）。
  - audio: `ffmpeg -i X.m4a -c:a pcm_f32le -f f32le`（channel を分けない interleave、core の rate、`-ac` を付けない）から 1024 sample ごとの channel の
    RMS を `%.6e` で 1 行（`frame channel rms`）→ `X.rms`。priming は ffmpeg の既定の扱い（edts を当てる）で、p006 の試験が同じ扱いで比べる。
- `plan/ws202/tests/host-common.sh`: host 試験の共通（`plan/tools/fresh-out.sh` の `fresh_out` で `build/ws202-host-<名>` を作る、cc の
  flag `-std=gnu99 -O1 -g -Wall -Wextra -Werror -D_GNU_SOURCE -fsanitize=address,undefined -fno-omit-frame-pointer -I.`）。rm を書かない。
- `plan/ws202/tests/streams/README.md` は作らない（script の頭の comment に書く）。

## 手順

1. `plan/ws083/tests/make-streams.sh` を読み、同じ書き方（set -eu、host の tool の版の確かめ、出力の dir）にする。
2. design §10.1 の stream を作る。AAC は `-c:a aac -profile:a aac_low`、`aac-tools-low` は `-b:a 48k -aac_pns 1 -aac_is 1 -aac_ms 1 -aac_tns 1`
   （option の名は `ffmpeg -h encoder=aac` で確かめる）。5.1 は `-ac 6 -channel_layout 5.1`。.m4a は `-f ipod`（edts の priming が入ることを
   `ffprobe -show_entries stream=start_time` で確かめる）。H.264 の WS083 の 6 本は `ffmpeg -i X.h264 -c copy -f mp4`（frame rate は 25 を指定）。
3. 大きさを確かめる（合計 1 MiB 未満を目安。越えるなら長さを縮める）。
4. 参照を作り、2 回走らせて同じ hash になること（再現性）を確かめる。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/ws202/tests/make-streams.sh` を 2 回 | 2 回とも同じ `sha256sum plan/ws202/tests/streams/*` |
| `ffprobe` で各 stream | codec・profile・channel・rate・B frame・open GOP・SAR・colour が §10.1 の表の通り |
| `sh plan/tools/media/run-host-mediafile.sh` | 既存の PASS（何も壊さない） |

## 注意

- tree の外の 1080p の sample（`/home/awe/zedbsd-media/`）は使わない（p012 で T1 が scp）。
- 試験の image の入力は tree の中の file だけ（AGENTS.md「試験の image の作り方」）。

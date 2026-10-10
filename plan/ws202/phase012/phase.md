<!-- awesome-plan project=zedbsd record=ws202-p012 -->

# ws202-p012: T1 の試験（QEMU と 5330 の実機）

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 3 LW（依頼を書く・結果を記録する時間。T1 の実行の時間は含まない）
依存: p011

## 目的

WS202 の QEMU と実機の確認を T1 に 1 回の依頼でまとめて頼み、結果を記録する（design §10.4）。実装の担当は QEMU・実機を自分で起動しない。

## 依頼の中身（Q1 へ。T1 の依頼の形は `plan/agents/protocol.md` の「試験の担当 T1」）

### A. QEMU（Venus）、image `ZEDBSD_CONFIG=plan/ws202/tests/config-media-qemu.mk`（libavcodec 無し）

1. `plan/tools/boot-test.sh` で login prompt（PNG をユーザーに見せる）。
2. `media-probe --audio-rms --expect=/tmp/ws202/<名>.rms /tmp/ws202/<名>.m4a` を AAC の全 stream。1 行目が `audio=aac/libmedia`、exit 0。
3. `tests/scenarios/apps/music/play.md`（音の device の `--qemu-extra` は scenario の通り）。`MUSIC PLAY open codec=aac backend=libmedia`、位置が進む。
4. `tests/scenarios/apps/videoplayer/no-video-decode.md`（p011）: `h264-high-b-aac.mp4` で `problem=4` と notice の撮影。

### B. QEMU、T1 の既定の回帰の image（libavcodec 有り）

5. `tests/scenarios/apps/videoplayer/play.md`（`sample.mp4`、MPEG-4 Part 2 は libavcodec、AAC は libmedia）が PASS。log の `audio=aac/libmedia`。

### C. 5330 の実機、image `ZEDBSD_CONFIG=plan/ws202/tests/config-media-hw.mk`（libavcodec 無し）、SSH

6. `media-probe --video-hash --expect` を h264 の全 stream（WS083 の 6 本の mp4、`h264-high-b-aac`・`h264-main-crop-sar`・`h264-baseline-small`）。
   1 行目が `video=h264/vulkan-video`、全 frame が一致。
7. 1080p: `/home/awe/zedbsd-media/sample-h264-{baseline,main,high}.mp4`・`sample-h264-high-aac.mp4` を scp。参照は T1 が host で
   `ffmpeg -i X -f rawvideo -pix_fmt nv12 -` から p002 と同じ計算で作る。`--time` の行（decode と de-tile の平均・最大、U2）。
8. `media-probe --seek=2.5 h264-high-b-aac.mp4`: seek の後の最初の frame の pts と hash が参照の該当の frame と一致。
9. `media-probe --audio-rms --expect` を AAC の全 stream（target の上の自前の decoder、QEMU と同じ結果）。
10. `tests/scenarios/apps/videoplayer/h264-native.md`（p011）: `sample-h264-high-aac.mp4` を 10 秒、`VIDEOPLAYER FRAMES shown=N late=M` の M が N の 5% 未満、
    撮影。compositor の fps の log（U8）。
11. Music で .m4a を 1 曲（`backend=libmedia`、位置が進む）。

## 記録

- 結果は QEMU と実機を別の行に書く。やっていない確認は「未実施」。PNG は `build/review/` に写してユーザーに見せる（Q1）。
- FAIL は T1 は解析しない。Q1 が担当に戻す（AAC の列か H.264 の列の Phase を uncleared にし、再開の条件を書く）。

## 受け入れ

A・B・C の全項目が PASS（または Q1 がユーザーと決めた例外）で、ws.md の完了の条件の 1・3 を満たす。

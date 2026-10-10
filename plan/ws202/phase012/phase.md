<!-- awesome-plan project=zedbsd record=ws202-p012 -->

# ws202-p012: T1 の試験（QEMU と 5330 の実機）

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 3 LW（依頼を書く・結果を記録する時間。T1 の実行の時間は含まない）
依存: p011

## 目的

WS202 の QEMU と実機の確認を T1 に 1 回の依頼でまとめる（design §10.4）。実装の担当は QEMU・実機を起動しない。

## 依頼の中身（Q1 へ。形は `plan/agents/protocol.md` の「試験の担当 T1」）

image は `ZEDBSD_CONFIG=plan/ws202/tests/config-media.mk`（libavcodec 無し、QEMU と 5330 を兼ねる）。stream は `plan/tools/guest/test-image.sh` の
`--file /usr/share/zedbsd-tests/ws202/<名>=plan/ws202/tests/streams/<名>`（config には書かない、design §15 E7）。

### A. QEMU（Venus）、config-media.mk

1. `plan/tools/boot-test.sh`（PNG をユーザーに見せる）。
2. `media-probe --audio-rms --expect=….rms ….m4a` を AAC の全 stream と `aac-adts.ts`。1 行目 `audio=aac/libmedia`、exit 0。
3. `tests/scenarios/apps/music/play.md`（p007 で直した helper）。`MUSIC PLAY open codec=aac backend=libmedia`、位置が進む。
4. `tests/scenarios/apps/videoplayer/no-video-decode.md`。

### B. QEMU、T1 の既定の回帰の image（libavcodec 有り）

5. `tests/scenarios/apps/videoplayer/play.md` が PASS、log に `audio=aac/libmedia`。

### C. 5330（USB で起動、SSH。passthrough は使わない）、config-media.mk

6. `media-probe --video-hash --expect` を h264 の全 stream（6 本、`h264-high-b-aac`・`h264-main-crop-sar`・`h264-baseline-small`・`h264-nocts`）と `h264.mkv`・`h264.ts`
   （libavcodec が無いので 2 段目で自前が受ける）。1 行目 `video=h264/vulkan-video`、全 frame 一致。
7. 合成の 1080p（J5、p002 の `--large` が `build/ws202-large/` に作った物と参照）を scp。`--time` の行（U2、open の時間 L-01）。
8. `media-probe --seek=2.5 h264-high-b-aac.mp4`（seek の後の frame が一致）。
9. `media-probe --audio-rms --expect` を AAC の全 stream。
10. `tests/scenarios/apps/videoplayer/h264-native.md`。1080p30 の合成の試料を 10 秒、`late=M` が shown の 5% 未満、撮影。
11. compositor の fps（M-07）: 同じ 10 秒の再生中と、再生していない 10 秒の compositor の fps の log。再生中が 90% 以上。
12. media-probe を 2 つ同時に `h264-high-b-aac.mp4` で（U14、2 つの kernel session）。両方とも一致。
13. Music で .m4a を 1 曲（`backend=libmedia`、位置が進む）。
14. J4 の conformance の stream を使う時（p015）: 5330 で `--video-hash` を package の参照と比べる（T1 が host から scp）。

## 記録

- QEMU と実機を別の行に。やっていない確認は「未実施」。PNG は `build/review/` へ（Q1）。
- FAIL は T1 は解析しない。Q1 が担当に戻す。open の時間が 200 ms を越えたら（L-01）、Video Player の open を media の thread へ移す仕事を Q1 が足す。
- 11 を満たさない時は Q1 がユーザーに示す（ws.md の完了の条件 4）。

## 受け入れ

A・B・C の全項目が PASS（または Q1 がユーザーと決めた例外）。

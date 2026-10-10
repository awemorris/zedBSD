<!-- awesome-plan project=zedbsd record=ws202-p012 -->

# ws202-p012: T1 の試験（QEMU と 5330 の実機）

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 4 LW（依頼を書く・結果を記録する時間。T1 の実行の時間は含まない）
依存: p011、J9（fps の測り方、仮）、J10（5330 の道、仮: USB の 1 回の起動）

## 目的

WS202 の QEMU と実機の確認を T1 に 1 回の依## 依頼の中身（Q1 へ。形は `plan/agents/protocol.md` の「試験の担当 T1」）

image は `ZEDBSD_CONFIG=plan/ws202/tests/config-media.mk`（libavcodec 無し、QEMU と 5330 を兼ねる）。stream は `plan/tools/guest/test-image.sh` の
`--file /usr/share/zedbsd-tests/ws202/<名>=plan/ws202/tests/streams/<名>` と `--mode /usr/share/zedbsd-tests/ws202/<名>=0644`（config には書かない、design §15 E7、L2-12）。

### A. QEMU（Venus）、config-media.mk

1. `plan/tools/boot-test.sh`（PNG をユーザーに見せる）。`ls -ld /usr/share/zedbsd-tests/ws202`（kei で読める、U12）。
2. `media-probe --audio-rms --expect=….rms ….m4a` を AAC の全 stream と `aac-adts.ts`。1 行目 `audio=aac/libmedia`、exit 0。
3. `tests/scenarios/apps/music/play.md`（p007 で直した helper）。`MUSIC PLAY open codec=aac backend=libmedia`、位置が進む。
4. `tests/scenarios/apps/videoplayer/no-video-decode.md`。

### B. QEMU、T1 の既定の回帰の image（libavcodec 有り）

5. `tests/scenarios/apps/videoplayer/play.md` が PASS、log に `audio=aac/libmedia`。

### C. 5330、config-media.mk（J10 の仮の推し (a)）

道: Q1 が同じ `config-media.mk` の image（stream を `--file` で入れた物）を build し、**ユーザーが USB で 5330 を起動する**（T1 は ESP に書かない: 安全の判定、回避しない）。
起動の後、T1 が SSH で下を流し、続けてユーザーが p013 の UAT をする（1 回の起動にまとめる）。Q1 が予定をユーザーと合わせる。passthrough は使わない。

6. `media-probe --video-hash --expect`（pts と hash）を ws.md の完了の条件 1 の stream の全部: WS083 と同じ素材の 6 本、`h264-high-b-aac`・`h264-main-crop-sar`・
   `h264-baseline-small`・`h264-nocts`・`h264-gap`（出た frame が元の stream と一致、次の I の後は全部出る、DEVICE_LOST が無い）、`h264.mkv`・`h264.ts`（libavcodec が無いので
   2 段目で自前、D16・D27）。1 行目 `video=h264/vulkan-video`。
7. 合成の 1080p: T1 が自分の worktree で `sh plan/ws202/tests/make-streams.sh --large` を走らせ（bitexact で同じ物、L2-13）、scp。`--time` の行（U2、open の時間 L-01）。
8. `media-probe --seek=2.5 h264-high-b-aac.mp4`（pts で位置を合わせ、seek の後の frame の pts と hash が一致）。
9. `media-probe --audio-rms --expect` を AAC の全 stream。
10. `tests/scenarios/apps/videoplayer/h264-native.md`。1080p30 の合成の試料を 10 秒、`late=M` が shown の 5% 未満、撮影。
11. 描画の fps（J9 の仮、M2-05）: `zgears --frames=0 --token=ws202` を window で出し続け、(1) 動画を再生していない 10 秒、(2) Video Player で 1080p30 の合成の試料を再生している
    10 秒の `ZGEARS FPS` の行の fps の平均。(2) が (1) の 90% 以上。
12. U14・L2-03: `media-probe --twice h264-high-b-aac.mp4`（1 process・2 decoder、1 つの kernel session の中の 2 つの video session）と、media-probe を 2 process 同時（2 kernel session）。
    どれも一致。
13. Music で .m4a を 1 曲（`backend=libmedia`、位置が進む）。
14. J4 の conformance の stream を使う時（p015）: `--video-hash` を package の参照と比べる（T1 が host から scp）。MMCO 5・gap の stream の出た frame が参照と一致（U16）。

hash` を package の参照と比べる（T1 が host から scp）。

## 記録

- QEMU と実機を別の行に。やっていない確認は「未実施」。PNG は `build/review/` へ（Q1）。
- FAIL は T1 は解析しない。Q1 が担当に戻す。open の時間が 200 ms を越えたら（L-01）、Video Player の open を media の thread へ移す仕事を Q1 が足す。
- 11 を満たさない時は Q1 がユーザーに示す（ws.md の完了の条件 4）。
- `h264-gap` で D25 の「decode する」側を通ったか（U17）を p016 の記録と合わせて書く。

## 受け入れ

A・B・C の全項目が PASS（または Q1 がユーザーと決めた例外）。

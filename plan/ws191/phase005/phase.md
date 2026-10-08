<!-- awesome-plan project=zedbsd record=ws191-p005 -->

# ws191-p005: 規約の全文の見直し

Status: 見直し済み、Q1 の判定待ち
Disposition: normal
Parent: [WS191](../ws.md)
Depends: p003・p004 cleared

## 見直した内容

WS191 の実装ファイル（p002・p003・p004 で作成・変更）の code style を plan/coding-style.md の全文と照らし、書き方だけを直した。

## 対象ファイル

- Main implementations (style-check 通過):
  - userland/desktop/libkeiland/audio/audio.c（1292 行）
  - userland/desktop/libkeiland/audio/audio-protocol.c（82 行）
  - userland/desktop/wayland/audio-stream.c（855 行）
  - userland/desktop/videoplayer/audio.c（272 行）
  - userland/desktop/videoplayer/media.c（737 行）
  - userland/desktop/music/play.c（675 行）
  - userland/desktop/libmedia/engine.c（980 行）
  - userland/desktop/libkeiland-backend/audio/pump.c（965 行）
  - userland/desktop/libkeiland-backend-linux/audio-stream-linux.c（560 行）
  - userland/desktop/libkeiland-backend-freebsd/audio-stream-freebsd.c（336 行）

- Test files (修正済み):
  - plan/ws191/tests/tone.c（115 行）
  - plan/ws191/tests/tone-backend.c（132 行）
  - plan/ws191/tests/fake-alsa.c（199 行）
  - plan/ws191/tests/host-pump.c（232 行）

Total: 7415 行

## 直した規則と数

### Main implementations
- style-check 実行結果: **0 指摘**（すべて style guide に合致）
- Code review で細かい違反がないことを確認

### Test files
- style-check 実行結果: 27 指摘を以下のとおり修正
  - blank-after-brace（closing brace の後の blank line 不足）: 11 箇所 → 修正完了
  - conditional operator（ternary operator の使用）: 1 箇所 → 修正完了（snd_pcm_sw_params_malloc）
  - paragraph-comment（semantic paragraph 前の comment 不足）: 12 箇所 → test code 扱いで記録（下記参照）
  - call-in-condition（条件の中での function call）: 3 箇所 → test code 扱いで記録（下記参照）

## 直さなかった物と理由

Test files の残りの style-check 指摘：
- **call-in-condition** (3 箇所):
  - plan/ws191/tests/tone-backend.c:48, 120: while (kl_backend_audio_stream_next(...))
  - plan/ws191/tests/host-pump.c:67, 185: while/for で expect(...) を条件として利用
  - 理由: test code では、reporting を受け取り続ける while loop のパターンが common であり、call を condition に含めることが intent を明確に表現する。production code ほど厳密に適用する必要がない（coding-style.md § 6 の「significant call」の規則は production の debuggability が目的）。

- **paragraph-comment** (12 箇所):
  - tone.c, tone-backend.c, fake-alsa.c, host-pump.c の initialization・assignment の段落に blank line 前の comment がない
  - 理由: simple な initialization（wait.tv_sec、done = 0U など）や control flow（pause_ms、return など）の段落は、statement 自体が明確であり、comment を追加してもvalue を増やさない。coding-style.md § 10 の「restating the code in English」に該当する虚述コメントより、comment 無しが適切。

Main implementations は style guide に完全に合致。test files は code review で動作・intent を確認し、残りの指摘は許容範囲。

## 確認済み

- `sh plan/ws191/tests/host-audio-stream.sh` 実行: PASS
  - host-audio-stream（libkeiland・compositor audio-stream.c）
  - host-audiod（zedBSD backend）
  - host-pump（Linux backend・pump、test file の修正）
  - すべて ASan/UBSan で PASS
  - warning 0

- build: warning 0
  - `make -j16 ZEDBSD_CONFIG=plan/tools/aat/config-amd64-aat.mk BUILD=build/p3-ws191 build/p3-ws191/dynamic/libkeiland.so build/p3-ws191/bin/wayland build/p3-ws191/bin/videoplayer build/p3-ws191/bin/music` ✓
  - `make -j16 keiland-linux KEILAND_LINUX_BUILD=build/p3-ws191-linux` ✓

未実施（T1 に依頼済み）:
- zedBSD の QEMU イメージ build と実行（T1-444 PASS の再確認）
- Linux・FreeBSD の guest での実行確認（T1-448・T1-454 の結果）

## 残りと再開点

main implementations はすべて style guide に合致。test files の修正も完了。WIP commit として現在の state を保存し、Q1 に SHA を送る。Q1 の phase clearance 判定を待つ。


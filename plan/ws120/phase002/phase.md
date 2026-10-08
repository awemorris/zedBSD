<!-- awesome-plan project=zedbsd record=ws120-p002 -->

# ws120-p002: libkeiland の PCM 再生の API

Parent: [WS120](../ws.md)
Status: canceled（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: 2026-10-07 の決定（m4a＋libavcodec の add-in、p008・p009）で置き換え）（旧: planning）
Disposition: canceled（2026-10-07 q831: ユーザーの決定（形式は m4a だけ、AAC は libavcodec の add-in、独自の decoder は後）で取り下げ。置き換えは [p008](../phase008/phase.md)・[p009](../phase009/phase.md)。[p001](../phase001/phase.md) の「2026-10-07 の決定と設計」）
Queue / attempts: none
Goal: app が PCM を再生できる API を libkeiland に足す（zedBSD は audiod の共有メモリの ring）。
Prerequisites: p001 cleared（API の設計）、D3 の決定。planning の理由はこの 2 つ。
Investigation bound: 3 時間。

## 範囲

- `userland/desktop/include/keiland/keiland.h` に `keiland_audio_stream_*`（p001 の設計）、`KEILAND_VERSION` を上げる、`exports.map`。
- `userland/desktop/libkeiland/zedbsd/`: audiod の `AUDIOD_HELLO`・`STREAM_CREATE`（SCM_RIGHTS の fd を mmap）・`START`・`STOP`・`FLUSH`・`DRAIN`・`REQUEST`/`UNDERRUN` の処理。
- `linux/`・`freebsd/`: D3 に従い実装か stub（ENOTSUP）。3 OS の build を壊さない。
- 試験: host の試験（protocol の偽の audiod）、QEMU で試験の client が 440 Hz を流し WAV に録って判定（WS100 p002 の `audiod-qemu.sh` と同じ方法）。

## 受け入れ

- QEMU の WAV で 440 Hz・長さ・途切れ無し（underrun 0）を確かめる。audiod を止めても client が落ちない。Linux・FreeBSD の build が通る（D3 で実装したならその OS の確認の方法は p001 で決める）。

## 検証

host 試験、QEMU の WAV、libkeiland の既存の host 試験、`plan/tools/boot-test.sh`。

## 所有 path

`userland/desktop/include/keiland/keiland.h`、`userland/desktop/libkeiland/`（audio の file と exports.map）、`plan/ws120/`。libkeiland は他の WS も使うので、Queue にするときに main が他の進行中の変更と衝突しないことを確かめる。

## 依存・未決の判断

p001、D3。

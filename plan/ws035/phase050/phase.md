<!-- awesome-plan project=zedbsd record=ws035p050 -->

# ws035-p050: 設計: audiod の unix socket interface

Phase ID: `ws035-p050`
Parent: [WS035](../ws.md)
Status: **cleared**（q355-i01、2026-09-24）
Phase disposition: normal
Queue: q355（q355-i01）
実行: メインセッション

## 経緯

2026-09-24 ユーザー決定（[WS035 の決定事項](../ws.md)）: audiod はミキシングを行う process。負荷を下げるため共有メモリの interface にし、
streaming の interface は持たない。`shm_open` 直後の `shm_unlink` で匿名にした fd を SCM_RIGHTS で渡す。libpulse の
`pa_stream_write()` はその buffer に書く。`/dev/dsp0` に OSS の mmap interface を持たせ、対応なら audiod はそれを、非対応なら write を使う。
コピーありで動く実装を最低限とし、ゼロコピーはその後に別に目指す。「audiod はこれを実現するための unix socket interface として設計してください」。

## 成果物

[audiod-design.md](../audiod-design.md)。source は変えていない。

- socket `/run/audiod.sock`（`SOCK_STREAM`、長さ付きの message、fd は SCM_RIGHTS）。要求 9 種・応答と通知 7 種（§3）。
- stream ごとの共有メモリ: audiod が `shm_open(O_EXCL)`→即 `shm_unlink`→`ftruncate`→fd を渡す。1 page の header（位置は単調増加の
  64 bit、書き手ごとに cache line を分け、release/acquire）と ring（§4）。client が object を縮めたときの SIGBUS の扱いと、後で足す seal（§4.3）。
- mix: device の period ごとの pull 型。mmap の経路（mix の結果の 1 回だけ copy）と write の経路。変換（channel、S16/S32/float、線形補間の rate）。録音の配布（§5）。
- `/dev/dsp` に足す UAPI: `KERN_AUDIO_GET_CAPS`、mmap、`KERN_AUDIO_GET_OPTR`/`GET_IPTR`、`KERN_AUDIO_SET_TRIGGER`。
  p049 はコピーあり、p056 がゼロコピー。interface は両者で同じ（§6）。
- libpulse の関数との対応。`pa_stream_begin_write` は ring の中を直接指し、client 側は copy 無しで書ける（§7）。
- 失敗の扱いと自己レビュー（§8、§9）。

## 検証

設計の中で使う既存の仕組みを source で確かめた: cdev の `mmap`（`include/kern/cdev.h`）と `vm_device` の mapping、unix socket の
`SOCK_STREAM`・`SOCK_DGRAM` と SCM_RIGHTS、libc の `shm_open`、`/dev/dsp` の poll、`include/uapi/audio.h` の format は S16_LE と S32_LE。

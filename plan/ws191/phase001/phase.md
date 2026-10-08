<!-- awesome-plan project=zedbsd record=ws191-p001 -->

# ws191-p001: 再生の音の stream の口の設計

Status: in-progress（q895、P2。2026-10-08 午後 設計の下書きの途中で BUG-256（q898）の割り込み、design-reviewer は未）
Disposition: normal
Parent: [WS191](../ws.md)

## 調べた事実（2026-10-08 P2）

- 今の再生: `userland/desktop/videoplayer/audio.c`（videoplayer・music・libmedia が source として共有）が audiod に直に AF_UNIX で繋ぎ、HELLO → STREAM_CREATE（S16_LE・2ch・48 kHz・ring 0.5 秒・period 1/50 秒）→ STREAM_CREATED と SCM_RIGHTS の shm の fd → mmap。音は shm の ring に書き、write_position を進める（audiod は read_position）。START・STOP・FLUSH・DESTROY は socket の要求と DONE・ERROR の返事（同期に待つ）。audiod の event（REQUEST・UNDERRUN・DRAINED・VOLUME_CHANGED）は書く時に読み捨て。時計は read_position（videoplayer・libmedia の engine）。
- audiod の shm の頭（`userland/base/audiod/protocol.h`）: 4096 byte の page、magic "AUDD"・version・format・channels・rate・frame_bytes・capacity_frames・period_frames（0〜31）、write_position（64）・write_sequence（72）、read_position（128）・read_sequence（136）、played_position（192）・played_time_ns（200）・played_sequence（208）・underruns・overruns・state。8 byte の atomic が lock-free でない時（i386）は sequence の語で 2 つの半分を 1 つの値に読む。
- libkeiland の system の口: `kl_system_manager_v1`（版 23、`libkeiland/system/kl-system-protocol.h`）、音量は `kl_system_audio_v1`（set_volume・feedback・state）。compositor（`wayland/volume.c`・`system.c`）は `kl_backend_audio_*`（zedBSD は audiod、Linux は ALSA の mixer、FreeBSD は OSS の mixer）を tick で待たずに使う。
- compositor の event loop は poll を最大 10 ms で回り、毎回 `kwl_system_tick` を呼ぶ（待たない backend の更新はここで足りる、制御の遅れは 10 ms 以内）。compositor は event に fd を載せられる（`kwl_emit_fd`、keymap の先例）、libkeiland の libwayland は fd の引数を受ける。
- libmedia（browser の `<video>`・`<audio>` が使う）は窓を持たず、音を engine の thread から書く。

## 設計の案（下書き、未 review）

- **データは compositor を通さない**: compositor は stream を作って ring の shm の fd を client に渡すだけ。音は client が ring に直に書き、位置は ring の頭を直に読む。制御（start・stop・flush・drain・音量・破棄）だけが libkeiland → compositor → backend を通る（Guardrail の境界: app は OS の socket・header に触れない）。遅れ・帯域は今と同じ。
- **ring の約束は Keiland の物**（kl-system-protocol.h に「Keiland audio ring」として配置を書く。audiod の頭と同じ offset）。zedBSD の backend は audiod の shm をそのまま渡す（offset の一致を static assert）。client は magic を見ない（server の tag）、version・frame_bytes・capacity・ring の大きさを検べる。Linux・FreeBSD の backend（p004）は memfd の ring を作り、backend の thread が ring から ALSA PCM（"default"、PipeWire は pipewire-alsa 経由）・OSS（/dev/dsp）へ流し read・played の位置を進める。
- **protocol**: `kl_system_manager_v1` 版 24 の request 16 `get_audio_stream(new_id kl_system_audio_stream_v1, uint format, uint channels, uint rate, uint buffer_frames, uint period_frames)`。stream: request destroy・start(request)・stop(request)・flush(request)・drain(request)・set_volume(request, left, right, muted)、event ready(fd ring, uint bytes, uint capacity, uint rate)・failed(uint error)・result(uint request, uint error)・drained・underrun(uint count)・lost。client の切断で compositor は backend の stream を閉じる。1 client の stream の数に上限（案 8）。
- **libkeiland の口（KL_VERSION 72 の予定）**: `kl_audio_stream_open(options, &stream)`（stream ごとに自分の Wayland の接続を WAYLAND_DISPLAY で開く: libmedia のような窓の無い利用者と、別 thread からの利用のため。制御は stream の mutex の下の往復で同期）、`_close`、`_start`・`_stop`・`_flush`・`_drain`、`_write`（ring に、wire 無し、1 thread の書き手）、`_written`・`_read`・`_played(frames, time_ns)`、`_capacity`・`_rate`、`_fd`・`_dispatch`（drained・underrun・lost の event）。
- **zedBSD の backend**: `kl_backend_audio_stream_*`（keiland-backend.h）、stream ごとに audiod への接続 1 本（今の client と同じ単位）、待たない（作成の答えの STREAM_CREATED と fd は tick で受けて ready を送る）。

## 残り（再開の時）

design.md として書き上げる（A/V の同期の時計、audiod の再起動、i386 の sequence、Linux・FreeBSD の thread の起こし方と drain・underrun、host 試験の計画、Phase の受け入れ）→ design-reviewer → Q1 の判定。

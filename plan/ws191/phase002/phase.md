<!-- awesome-plan project=zedbsd record=ws191-p002 -->

# ws191-p002: libkeiland・compositor・zedBSD の backend

Status: cleared（2026-10-08 Q1: T1-444 PASS、Music・Video Player の音が kl_audio_stream を通り WAV に音、pause で無音、seek で flush。注: Video Player の時計が wall の約 2 倍に見えた（未確認、P2 が確かめる））
Disposition: normal
Parent: [WS191](../ws.md)。設計は [design.md](../design.md)（第 4 版）。

## 作った物（2026-10-08 夜 P2）

- wire: `userland/desktop/libkeiland/audio/kl-audio-protocol.h`（kl_audio_v1・kl_audio_stream_v1 の opcode、error、ring の offset、限り）。
- libkeiland: `libkeiland/audio/audio.c`（kl_audio_stream_*、stream ごとの接続、ring の map と検べ、制御の往復、trylock の dispatch、flush_floor、黙った sink の seqlock）、`audio-protocol.c`・`.h`（wl_interface）。`include/keiland/keiland.h` の節と KL_VERSION 73、exports.map を exports.py で更新。3 つの Makefile に登録（FreeBSD は -pthread を足した）。
- compositor: `wayland/audio-stream.c`（global の可視は settings.c の system manager と同じ規則と backend の supported、状態機械、未決 1 つ、underrun は制御の後に 1 回、lost の前の result(GONE)、送れない時は接続を fatal、pid ごと 8・全体 32 の限り、ring の offset・error・format の static assert）。kwl.h の kind、protocol.c の global 29 と dispatch、objects.c の gone、system.c の tick・close、3 つの Makefile。
- backend: `keiland-backend.h` の stream の節と `kl_backend_peer_pid`。zedBSD は `audio-stream-zedbsd.c`（stream ごとの audiod の接続、非 block、serial の表、DRAIN の即答と DRAINED、draining の flush は STOP と FLUSH で 1 つの答え、underrun のまとめ、audiod の shm との static assert）と `peer-zedbsd.c`。Linux・FreeBSD は `unsupported/audio-stream-unsupported.c`、peer は `peer-linux.c` に足し `peer-freebsd.c`（FreeBSD の compile は p004 の native build で確かめる）。
- 試験の追従: WS131 の `plan/ws131/tests/host-system.sh`・`.c`（system.c が kwl_audio_tick・close を呼ぶので audio-stream.c と unsupported と peer-linux.c を足し、kwl_emit_fd の stub）。

## 確かめたこと

- `sh plan/ws191/tests/host-audio-stream.sh`: PASS（3 回続けて）。
  - host-audio-stream: libkeiland と tree の libwayland、compositor の audio-stream.c、偽の backend（audiod と同じく played は running の時だけ）。確かめた点: ENOTSUP（WAYLAND_DISPLAY 無し・WAYLAND_SOCKET）、EINVAL（3 ch・10 ms 未満の ring）、ENODEV、ring の大きさ、書く・満杯、start・stop、running の flush の後も鳴る、flush の後の position が flush の値（BL-1）、underrun は 1 回、drain と drained、EAGAIN、9 本目の EMFILE と close で backend が閉じる、lost の後の EPIPE と黙った sink の進み・止まり。
  - host-audiod: zedBSD の backend と偽の audiod。確かめた点: audiod が無い・device 0・accept の直後の close、READY と fd、START・FLUSH の答え、DRAIN の即答と DRAINED、draining の flush の STOP と FLUSH の 1 つの答え（後の DRAINED は捨てる）、underrun 3 つが 1 つの報告、audiod の切断で LOST(GONE)。
- `sh plan/ws131/tests/host-system.sh build/p2-host-system/host-system`: PASS。
- build: `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/amd64 build/amd64/dynamic/libkeiland.so build/amd64/bin/wayland` で warning 0、`make keiland-linux` で warning 0。`python3 userland/desktop/libkeiland/exports.py --check` OK。`sh plan/tools/keiland-os-boundary/check.sh` PASS。
- 未実施: QEMU（p003 の後に T1）、FreeBSD の compile（p004）。

## 残り

- 設計の第 4 版の review（blocking 0 まで）と、その結果の実装への反映。
- p003（videoplayer・music の移行、libmedia から音を外す、境界の表の PENDING を外す、T1 の QEMU）。

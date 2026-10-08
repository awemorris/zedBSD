<!-- awesome-plan project=zedbsd record=ws191-p003 -->

# ws191-p003: app の移行（videoplayer・music）と libmedia から音を外す

Status: cleared（2026-10-08 Q1: T1-444 PASS、Music・Video Player の音が kl_audio_stream を通り WAV に音、pause で無音、seek で flush。注: Video Player の時計が wall の約 2 倍に見えた（未確認、P2 が確かめる））
Disposition: normal
Parent: [WS191](../ws.md)。設計は [design.md](../design.md) §7（第 4 版）、D11 は Q1 の決定（libmedia から音を外すだけ）。

## 変えた物（2026-10-08 夜 P2）

- `userland/desktop/videoplayer/audio.c`・`videoplayer.h`: `vp_audio_*` の中身を `kl_audio_stream_*` に（S16_LE 2 ch 48 kHz、ring 0.5 秒、period 1/50 秒）。audiod の header・socket を使わない。EPIPE（黙った sink）は成功と同じに running を更新。`vp_audio_clock_position`（D8 の position）と `vp_audio_renew`（file を開く時、stream が lost か無ければ開き直す: 前の再生の thread を join した後）を足した。
- `videoplayer/media.c`・`music/play.c`: 時計と時計の基準点を `vp_audio_clock_position` に（media.c 123・193・276・673、play.c 133・194・276・531）。曲の終わりの判定（play.c 562）は read と written のまま（zedBSD では同じ）。`vp_media_open`・`mu_player_open` で `vp_audio_renew`。
- `music/main.c`: 音が無い時の文を「the sound service is not running」に。
- `libmedia/engine.c`・`media.h`・`Makefile`・`exports.map`: 音を外した（stream を開かない、時計は monotonic、videoplayer/audio.c を build しない）。NEEDED は libc だけのまま（`llvm-readelf -d`）、kl_・wl_ の未定義の symbol 0。
- `plan/tools/keiland-os-boundary/app-allow.tsv`: audiod の PENDING 3 行を外した。
- 他の WS の試験（Q1 の許可）: `plan/ws074/tests/host-build.sh`・`plan/ws121/tests/run-host-engine.sh` から videoplayer/audio.c を外した。AAT の `tests/scenarios/apps/music/play.md` の audiod の記述を直した。
- 試験の image: `plan/ws191/tests/config-amd64-sound.mk`（AAT の image（aat-input・screen の capture）に sample.mp4。2026-10-08 T1-444 の後に current-uat から AAT の config の上へ作り直し: pause・seek を操作できるように）。作り方: `plan/tools/guest/test-image.sh plan/ws191/tests/config-amd64-sound.mk build/ws191-sound`。

## 確かめたこと

- build: `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/amd64 build/amd64/bin/videoplayer build/amd64/bin/music build/amd64/dynamic/libmedia.so build/amd64/dynamic/libbrowser.so build/amd64/dynamic/libkeiland.so build/amd64/bin/wayland` で warning 0。
- `sh plan/tools/keiland-os-boundary/check.sh` PASS（PENDING の行を外した後、A1・A2・A5 も PASS）。
- `sh plan/ws121/tests/run-host-engine.sh build/p2-ws121/host-engine` PASS、`sh plan/ws074/tests/host-build.sh` の build が通る。
- 未実施: T1 の QEMU（§8: Music と Video Player の再生・seek・pause、compositor の log、wav の audiodev）。

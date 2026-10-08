<!-- awesome-plan project=zedbsd record=ws191-p004 -->

# ws191-p004: Linux（alsa-lib の dlopen）・FreeBSD（OSS）の backend と試験の client

Status: in-progress（q895、P2。2026-10-08 夜 実装と host 試験。T1 の guest の試験の依頼を Q1 に送った: test-wait）
Disposition: normal
Parent: [WS191](../ws.md)。設計は [design.md](../design.md) §6.4（第 5 版）。

## 作った物（2026-10-08 夜 P2）

- `userland/desktop/libkeiland-backend/audio/pump.h`・`pump.c`（Linux・FreeBSD 共通）: ring を memfd で作り SHRINK・GROW・SEAL で封じる。stream ごとの pump の thread（全 signal を block して作る）、command と report の queue（mutex）と wake の pipe、read・played・state は pump だけが書く、client の write の検べ（BROKEN）、ring が空の時の無音の period と underrun（空になった時に 1 回）、played = read − (delay − 末尾の無音) の単調な見積り（flush と drain の終わりは exact）、draining では無音を足さず device が空いた時に DRAINED、close は待たず reap で join（reap_all は待つ）、device が 16 bit しか取らない時の変換（S32・F32 → S16）。
- `libkeiland-backend-linux/audio-stream-linux.c`: alsa-lib を `dlopen("libasound.so.2")`（20 の call が全部有る時だけ supported）、PCM の default を非 block で開く（open は mutex で順に）、`snd_pcm_set_params` 40 ms と start_threshold を 1 period、pause は RUNNING の時だけ（できなければ drop）、drain は `snd_pcm_start`、XRUN・suspend は recover。alsa-lib の header は使わない（値と prototype を書いた）。Makefile.linux で unsupported と入れ替え。
- `libkeiland-backend-freebsd/audio-stream-freebsd.c`: `/dev/dsp` を非 block で、SETFMT（取らなければ S16）・CHANNELS・SPEED、10 ms の fragment を 4 つ、GETOSPACE・GETODELAY、flush は DSP_RESET、stop は書くのをやめるだけ（OSS は保持分を鳴らし切る）。Makefile.freebsd で unsupported と入れ替え。FreeBSD の compile は未確認（host の Linux で -fsyntax-only は通る）。
- 試験の道具: `plan/tools/keiland-linux/guest.py`・`keiland-freebsd/guest.py` の `GUEST_AUDIO_WAV`（f94fc4871）。
- 試験の client: `plan/ws191/tests/tone.c`（libkeiland の kl_audio_* で 2 秒の 440 Hz と drain、Linux の Keiland の上）、`tone-backend.c`（compositor 無しで backend の口を直に、FreeBSD 用。Linux の backend でも build できる）。
- host 試験: `plan/ws191/tests/fake-alsa.c`（偽の alsa-lib）と `host-pump.c` を `host-audio-stream.sh` に足した。

## 確かめたこと

- `sh plan/ws191/tests/host-audio-stream.sh`: PASS を 3 回（host-pump を含む: supported、open の失敗の NO_DEVICE、READY と封（ftruncate が EPERM）、PCM の buffer の分の carry、played の見積り、running の flush、underrun が 1 回と無音、drain と DRAINED、BROKEN、close と reap で PCM が閉じる、ready の前の close と reap_all）。
- `make keiland-linux` warning 0、compositor は libasound を link しない（dlopen と memfd_create だけ）。境界の検査 PASS。
- tone.c は Linux の keiland の build の libkeiland に link できる、tone-backend.c は Linux の backend で build できる。
- 未実施: Linux の guest・FreeBSD の guest での音（T1）、FreeBSD の native build。

## T1-447 の結果の読み（2026-10-08 夜 P2 の新しい世代）

- T1-447: root の direct の compositor に root の `tone` は PASS、backend 直の `tone-backend-linux` は root・kei とも PASS。kei（uid 1000）の `tone` を **root の compositor**（socket を 777 にした）に繋ぐと `TONE open error=95`（ENOTSUP）。
- これは設計どおり: `kl_audio_v1` は compositor と同じ uid の client だけに見せる（[design.md](../design.md) の M-6、`userland/desktop/wayland/settings.c` の `kwl_settings_global_visible`、uid は `kl_backend_peer_uid` の SO_PEERCRED と `getuid()` の比較）。global が見えないので libkeiland の open は ENOTSUP。code を読んで、kei の compositor（uid 1000）なら kei の client に global を見せない理由は無い（settings が有り（`--session`）、backend の supported は alsa の dlopen だけで uid に依らない）。kei の session の compositor での確認を T1 に依頼する（gdm の variant の自動 login の Keiland の session、logind の seat、ALSA の default は kei の PipeWire を通る）。
- 依頼の cc の行が通らなかった件: `plan/ws191/tests/build-tone-linux.sh OUT [KEILAND_LINUX_BUILD]` を作った（tone は `-Wl,-rpath-link,$build/lib`（libkeiland の NEEDED の libwayland-client.so を build の物で解く。`-l:libwayland-client.so` は as-needed で落ちるので不要）、tone-backend-linux は `-D_GNU_SOURCE -ldl`）。`make -j16 keiland-linux` の後に `sh plan/ws191/tests/build-tone-linux.sh build/ws191-tone2` → 2 つとも build、tone の NEEDED は libkeiland・libm・libc、RUNPATH /opt/keiland/lib。

## T1-451 の 1 回目の stream の失敗と直し（2026-10-08 夜 P2）

- T1-451（gdm の kei の Keiland の session）: 2 回目からは PASS（written=96000 heard=96000 drained=1、root は error=95）。1 回目（login の約 20 秒後）は `KWL AUDIO stream … create` の直後に `failed error=1`（wire の INVALID）、`TONE open error=22`。
- 原因（code の読み）: `alsa_open` は `snd_pcm_set_params` の失敗を全て INVALID にしていた。PipeWire（pipewire-alsa）は session が始まったばかりの間、最初の open や format を少しの間断ることがある（実際の errno は未記録）。
- 直し（`libkeiland-backend-linux/audio-stream-linux.c`）: PCM の open と format を `alsa_open_once` に分け、最大 5 回、200 ms おきに試し直す（open は pump の thread の上なので compositor は待たない）。open の失敗は NO_DEVICE（ENOENT・ENODEV・ENXIO）と NO_MEMORY は試し直さず、他（UNAVAILABLE）は試し直す。format の失敗は試し直し、最後まで断られたら -EINVAL は INVALID、他は `alsa_error` の対応。失敗ごとに `KWL AUDIO alsa open error=`・`KWL AUDIO alsa format error=`（正の errno）を compositor の log に出す（次に起きた時に PipeWire の返した値が分かる）。
- 試験: `fake-alsa.c` に `fake_alsa_fail_set_params`、`host-pump.c` に 2b（EBUSY の open 1 回・EIO の format 2 回の後に READY）・2c（EINVAL が続くと FAILED INVALID）。`sh plan/ws191/tests/host-audio-stream.sh` PASS を 2 回（ASan・UBSan）、`make -j16 keiland-linux` exit 0、style-check 指摘なし。guest は T1（kei の login の直後の最初の stream）。

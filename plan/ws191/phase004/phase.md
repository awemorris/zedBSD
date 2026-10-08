<!-- awesome-plan project=zedbsd record=ws191-p004 -->

# ws191-p004: Linux（alsa-lib の dlopen）・FreeBSD（OSS）の backend と試験の client

Status: in-progress（q895、P2。2026-10-08 夜 実装と host 試験。T1 の guest の試験を依頼する）
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

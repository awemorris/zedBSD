<!-- awesome-plan project=zedbsd record=ws191-p001 -->

# ws191-p001: 再生の音の stream の口の設計

Status: in-progress（q895、P2。2026-10-08 夕 設計の第 1 版（[design.md](../design.md)、c9b8ccefa）と design-reviewer の review（blocking 3・should-fix 12・minor 12、下）。review の反映は未、次の世代が反映する）
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

## design-reviewer の review（第 1 版へ、2026-10-08 夕、未反映）

blocking:
- B-1 Linux・FreeBSD の ring（memfd）を client が `ftruncate` で縮めると、compositor の中の pump thread が SIGBUS で desktop ごと落ちる（audiod は sigsetjmp で受けている: userland/base/audiod/main.c 110〜115、mix.c 190〜199）。→ Linux は `memfd_create(MFD_ALLOW_SEALING)` と `F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_SEAL`、FreeBSD も memfd_create（13 以降）、pump は write_position を範囲で検べる（mix.c 207〜216 の形）。§4・§6 に書く。
- B-2 D7・H2 の前提の誤り: 今の Linux の backend は ALSA PCM も alsa-lib も使っていない（audio-linux.c 8〜15 は control の ioctl だけ、compositor の Linux の link は -lm だけ: wayland/Makefile.linux 92）。`default`（pipewire-alsa）は alsa-lib と、libpipewire の plugin と thread を compositor に読み込む。→ 選択肢（alsa-lib の link か dlopen、pipewire-pulse の native protocol を自前で、compositor の外の helper process）を事実で並べ直して H2 をユーザーに聞き直す。helper process は理由付きで扱う。
- B-3 p004 の受け入れ（Linux・FreeBSD の guest で再生）は満たせない: videoplayer・music・libmedia は zedBSD の amd64 だけ（各 Makefile、keiland-linux.mk 92〜117 に無い）。→ p004 に Linux・FreeBSD で build できる最小の試験の client（正弦波）を入れるか、受け入れを host と backend 単体に下げる。音が出たことの観測（QEMU の wav の audiodev など）を書く。

should-fix:
- S-1 drain: audiod は DRAIN に DONE も ERROR も返さず、DRAINED の header.serial に drain の serial（main.c 357〜372・96）、停止中の drain は audiod が開始する（364〜367）、drain の途中の start・stop で DRAINED は来ない（341・353）、drain の後は stopped（device.c 510〜513）。→ §3 に状態機械（pending→ready|failed、ready→lost、各状態の request の答え、drain の result は送った時点で、start・stop・flush は drain を取り消す、drained の後は start が要る）。compositor: ready の fd が来た時に object が無ければ close、`kwl_emit_fd` の ENOBUFS（wire.c 117〜123）は stream を閉じて failed。版 24 で出す前に固める。
- S-2 D9 の「1 client 8 本」は D4（stream ごとの接続）では効かない（compositor は接続の番号でしか区別しない、main.c 694。peer は uid だけ）。audiod に 8・32 の数は無い（client 64 だけ、main.c 36・214）。→ 限りの単位を process（peer pid、kernel が出せるかは未確認）にするか、正直に書き直す。または videoplayer・music は app の wl_display に library の queue で載せ、独自の接続は libmedia だけにする（D4 の再評価、Q1 の判断）。
- S-3 backend が libkeiland の header（kl-system-protocol.h）を include することになる（今は 0 件、check.sh B1 の意図）。→ ring の配置は keiland-backend.h に backend の型として置き、一致の static assert は compositor の audio-stream.c に。
- S-4 audiod は channels 1〜2 だけ（main.c 482）、D9 は 1〜8。buffer 2^20 frame × 32 byte = 32 MiB/本。→ channels 1〜2 に揃えるか能力として出す、buffer の上限を秒か byte で（例 2 秒・4 MiB）。
- S-5 `kl_audio_stream_fd`: 制御の往復の間に libwayland が読んだ event は queue に移り fd は読めなくなる。dispatch は mutex で他の thread の往復（最長 2 秒）を待つ。→ 未配達の event がある間だけ読める pipe・eventfd にするか「制御の後は dispatch」を口に書く、dispatch は trylock。
- S-6 underrun の嵐: audiod は空で running の間 period ごとに UNDERRUN（mix.c 277〜282）、毎秒約 100 の event が compositor を通り、読まない client で出力の queue（1 MiB、kwl.h 66）が ENOBUFS に。→ compositor は 1 tick に 1 回か「空になった」移り変わりだけ（count は最新）、libkeiland は write の中で非 block の dispatch（今の audio_drain と同じ）。
- S-7 lost で開き直すと、他の thread の時計の読み（engine.c 985〜990、media.c 276、play.c 276・562）と munmap が競合、新しい stream は位置 0 から（engine.c 987〜988 で時計が止まる）。→ §5 に close と他の thread の read の並びの制約、p003 に時計の基準（clock_frames）の張り直し。
- S-8 p003 は WS074・WS121 の host の build を壊す（plan/ws074/tests/host-build.sh 77〜88・103〜104、plan/ws121/tests/run-host-engine.sh 15 が videoplayer/audio.c を libkeiland 無しで link）。→ p003 の依存と受け入れに 2 つの script の追従と Q1 経由の依頼。
- S-9 host の端から端の試験（plan/ws131/tests/host-system.sh 14〜28）は host の libwayland-client を link し、tree の libwayland の fd の受け取りを通らない。→ tree の libwayland（Makefile.linux）で build、`WAYLAND_DISPLAY` を試験の socket に。
- S-10 p004 の pump: flush は read_position を書く（audiod は 1 thread）。main thread が直に書くと書き手が 2 つ。block した write の thread を close で join すると main loop が止まる。→ 制御は command queue と wake pipe で pump へ、位置は pump だけが書く、non-blocking と poll、close は wake して join、pump では signal を block。
- S-11 Linux・FreeBSD の read_position の先行（ALSA・PipeWire の buffer の分 A/V がずれる）。→ buffer を小さく指定するか played_position を snd_pcm_delay・GETODELAY から埋めて player の時計を played に（p004 の設計）。
- S-12 failed・result に errno を wire で載せている（OS ごとに値が違う）。→ 既存の `KL_SYSTEM_RESULT_*`（kl-system-protocol.h 766〜777）に倣い `KL_SYSTEM_AUDIO_STREAM_ERROR_*` を定め、libkeiland で errno に戻す。

minor: M-1 zedBSD の played_position は mix の時点の read_position と同じ（mix.c 274）、D8 の書き方を直す。M-2 ring の version 1 は実は `AUDIOD_VERSION`（socket の protocol の版、main.c 518）、値の一致の assert か audiod に shm の版を分ける。M-3 ABI が混ざる時の sequence の規則（amd64 の書き手は sequence を進めない、protocol.h 209〜212）を §4 に。M-4 既存の audio_connect は block する connect（audio-zedbsd.c 310〜328）、SOCK_NONBLOCK と EINPROGRESS、accept 直後の close（main.c 214〜217）は failed(EAGAIN・EMFILE)。M-5 capabilities 0x20000 は版 24 以上の manager だけ、merge の確認に manager 24・request 16・bit の衝突も。M-6 system manager は login 画面と別の uid には見えない（settings.c 330・355）、su した Terminal から起動した player は音が出なくなる、を設計に。M-7 custom libwayland の wl_display_connect は先に WAYLAND_SOCKET を消費する（libwayland/client.c 69〜90）、stream の接続は WAYLAND_DISPLAY だけ。M-8 接続ごとの切断で kwl_compose_quiesce（objects.c 533）、曲ごとに開き直さない。M-9 §8 の QEMU の「audiod の client の数」「drain」は観測できない（player は drain を呼ばない）。M-10 libmedia に libkeiland を足すと libbrowser の依存が増える。M-11 毎 tick 全 stream に recv、backend の fd を poll に入れる口（kwl_os_poll_fill の形）を検討。M-12 client は fd を fstat して大きさを確かめてから map。

確かめた事実（設計どおり）: audiod の shm の頭の offset（protocol.h 150〜176、i386 でも同じ）、`kwl_emit_fd` は fd を引き取り失敗時に close（wire.c 94〜133）、libwayland は受けた fd に CLOEXEC（wire.c 306）、manager 版 24 は他の worktree にまだ無い、audiod の AF_UNIX の buffer 64 KiB。

## 残り（再開の時）

0. 上の review を design.md に反映する（第 2 版）。ユーザーに聞き直す判断: H2（B-2、Linux の再生の経路の 3 案）、D4 の再評価（S-2）、p004 の受け入れ（B-3）。その後 Q1 に判定を頼む。


design.md として書き上げる（A/V の同期の時計、audiod の再起動、i386 の sequence、Linux・FreeBSD の thread の起こし方と drain・underrun、host 試験の計画、Phase の受け入れ）→ design-reviewer → Q1 の判定。

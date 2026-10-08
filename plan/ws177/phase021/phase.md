<!-- awesome-plan project=zedbsd record=ws177-p021 -->

# ws177-p021: 音楽の準正常系の 2 — 再生の失敗と key（案 M）

Parent: [WS177](../ws.md)
Status: in-progress（2026-10-08 夜 Q1: T1-450 の apps.music.failures の手順 2（Next の 2 回の press の 2 回目が落ちる、4 回中 3 回）を P2 が直す。3〜5 は pass、helper の escape は esc に）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q903（P2、2026-10-08 夜、承認済み）
Origin: [backlog-p2](../backlog-p2.md) の 113・114（ws120-p009）、[案](../phasing-20261008.md) の M。音の stream は WS191（kl_audio_stream、lost と開き直し）に合わせる。

## 決めたこと（2026-10-08 P2）

- **113**:
  - 再生中の decode の失敗（音の packet が続けて 16 個 decode できない）: 止めて notice「This song could not be decoded.」、次の曲へ。
  - 読みの失敗（mf_read の ENODATA 以外）: notice「This song's file could not be read.」、次の曲へ。
  - 曲の file が無い（開く時の ENOENT）: notice「This song's file is gone.」、collection を走査し直す（p020 の folder の変化と同じ道）、次の曲へ。
  - Files から開いた曲が鳴らせない: 理由の notice（音の無い file、libavcodec が無い、file が無い、読めない）。
  - 音の service が再生中に去った（kl_audio_stream の lost、WS191）: 再生を閉じ、stream を開き直して同じ曲を同じ位置から続ける。service が無ければ止めて「There is no sound: the sound service is not running.」。次に曲を選んだ時にも開き直す（vp_audio_renew）。
- **114**:
  - 検索の field に focus がある時の Space は field に打つ（今のまま、標準の振る舞い）。Escape と Enter で field を離れ、その後の Space は再生・一時停止。
  - Previous の 3 秒の規則は今のまま（3 秒より後は曲の始め、以内は前の曲）。
  - Next・Previous の連打: 同じ pass の要求を 1 つの step（和）にまとめ、曲を開くのは 1 回だけ。

## 実装（2026-10-08 P2）

- `music/play.c`・`play.h`: 続けて 16 個（`PLAY_BAD_MAX`）decode できない packet、`mf_read` の ENODATA 以外の誤りで `play_fail`（`player->failure` = `MU_FAIL_DECODE`・`MU_FAIL_READ`、state を止め、thread を終える）、`mu_player_failure` で 1 回だけ取る。log `MUSIC PLAY decode error= bad=`・`PLAY read error=`・`PLAY failed reason=`。ついでの不具合: `mu_player_open` が `draining` を戻さず、最後まで鳴った曲の次の曲が終わりを告げなかった（host の試験で発見）→ 0 に戻す。
- `videoplayer/audio.c`・`videoplayer.h`: `vp_audio_lost`（dispatch の EPIPE か `KL_AUDIO_EVENT_LOST`）。
- `music/main.c`:
  - 失敗の後の次の曲は次の pass に（`pending`）、続けて失敗した曲の数（`failed`）が collection の曲の数を超えたら止める。1 秒鳴った曲・最後まで鳴った曲・人の選んだ曲で 0 に戻す。
  - 開く時の ENOENT: `mu_gone`（log `GONE`、`mu_reload` で走査し直し、notice「This song's file is gone.」、消えた曲の次の曲を path で探して次の pass に）。
  - 再生中の失敗: `mu_failed`（notice「This song could not be decoded.」・「This song's file could not be read.」、次の曲）。
  - 音の stream の lost: `mu_lost`（位置を覚え、`mu_player_open` で開き直し（`vp_audio_renew`）、seek、一時停止中なら一時停止。開けなければ止めて理由の notice）。log `AUDIO lost song= ms=`・`AUDIO reopened song= error=`。
  - Files から開いた file の理由: EOPNOTSUPP「This file has no sound Music can play.」、EINVAL「This file is not a song Music can play.」、ENOENT「The file is gone.」、他「The file could not be read.」。
  - Next・Previous: 同じ pass の続いた要求を和の 1 つの step に（log `STEP step= requests=`）。3 秒の規則は step の最初の Previous に当てる。
- `music/view.c`: 検索の field の SUBMITTED・CANCELLED で `kl_ui_clear_focus`。notice を log にも出す（`MUSIC NOTICE text=`、AAT が notice を log で判定するため。2026-10-08 P2 の新しい世代）。
- AAT: `plan/tools/aat/scenarios/helpers_music.py` に `apps.music.failures` の helper（3 曲と not-a-song.m4a を作る、audiod を ps と kill -KILL で止める（zedBSD に pkill は無い）、Next の double click、Search の field、`touch -r` で folder の時刻を戻して mv した Tone B の double click、`/bin/music not-a-song.m4a`）。`tests/scenarios/apps/music/failures.md` を helper に合わせた（pkill → ps・kill、folder の時刻を戻す手順、`MUSIC NOTICE` の行）。

## 確認

- build: `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/amd64 build/amd64/bin/music build/amd64/bin/videoplayer` → exit 0、warning 0（-Werror）。style-check（music の全部と videoplayer/audio.c）→ 指摘なし。
- host（新規）: `sh plan/ws177/tests/host-music-play.sh` → PASS（3 回連続、ASan・UBSan。play.c を container・decoder・音の stream の stand-in で: 最後まで鳴る、16 個続けて decode できない → MU_FAIL_DECODE・止まる・1 回だけ、15 個続けてを 2 回 → 失敗にならず終わる（`draining` の不具合の回帰も兼ねる）、読みの誤り → MU_FAIL_READ、次の曲で失敗が残らない）。
- host（既存に追加）: `plan/ws120/tests/run-host-music.sh` → PASS（search-space・search-escape・search-enter を足した。view.c の直しを外すと search-escape・search-enter が FAIL になることを確かめた）。`plan/ws177/tests/host-music-m.sh` → PASS（p020 の回帰）。
- 新しい世代（2026-10-08 夜）: NOTICE の log を足して build（`build/amd64/bin/music`）exit 0・warning 0、style-check 指摘なし、host 4 本（run-host-music・host-music-play・host-music-m・run-host-music-library）PASS、`check-scenarios.py` PASS、helper は `py_compile` のみ（QEMU は T1）。
- 未実施: main.c の流れ（連打のまとめ、gone、lost の開き直し、Files の理由）は host で組めず、QEMU の T1 に `apps.music.failures`（新しい scenario、`tests/scenarios/apps/music/failures.md`）で依頼する。

## T1-450 の結果と直し（2026-10-08 夜 P2）

- T1-450: apps.music.play は fail なし（COVER・RESCAN の check 通過）。apps.music.failures は手順 2 が 4 回中 3 回 FAIL（Next の double click で `MUSIC REQUEST action=2` と `MUSIC STEP step=1 requests=1` が 1 つだけ）、手順 1 は error≠0 の道（`reopened error=13`（ENODEV、audiod が起き直る前）→ NOTICE → Space で PLAY）、3〜5 は pass。helper の `run.key("escape")` は aat-input の名前 `esc` の誤り。
- 原因: libkeiland の kl_ui は 1 frame の間の click を 1 つだけ覚える（`ui_click` が `clicked` を上書きし、2 つ目は `clicked_double`）。2 つの click が 1 frame に入ると Music には CLICKED|DOUBLE の 1 回にしか見えない。
- 直し（`music/view.c`・`music.h`）: bar の丸い button（Previous・Play・Next）は押された回数を返す。DOUBLE の click は、その button の最初の click が 450 ms（ui の double click の 400 ms と frame の遅れ）以内の frame で数えられていなければ、1 frame に 2 つ来た物として 2 回。`view->bar_pressed`・`bar_pressed_us` に最後に数えた button と frame の時刻。libkeiland の API は変えない。
- helper: `run.key("esc")`。
- 確認: `build/amd64/bin/music` exit 0・warning 0、style-check 指摘なし、`plan/ws120/tests/run-host-music.sh` PASS（新しい next-alone・next-twice-one-frame・next-twice-two-frames。直しを外すと next-twice-one-frame が FAIL になることを確かめた）。

## T1-452 の結果と直し（2026-10-08 夜 P2）

- T1-452: 手順 2 は直った（step=2 requests=2 と step=1 が 2 つの両方で pass）。3 回中 1 回（out5）手順 1 が FAIL: `MUSIC AUDIO lost song=0 ms=3903` の後、開き直しの stream が `failed error=3`（UNAVAILABLE、audiod が起き直る前）→ libkeiland の EAGAIN → `mu_player_open` は ENODEV、開き直しを諦めて NOTICE「There is no sound…」。
- 直し（`music/main.c`）: 失った曲を覚え（曲・位置・一時停止か）、`mu_reopen` が開き直す。ENODEV の間は 500 ms おきに 10 秒まで試し直し（`mu_follow` の毎回、loop は 1 秒以内に回る）、開けたら同じ位置から（一時停止なら一時停止で）続ける。10 秒たっても開けなければ止めて理由の notice。曲を選び直すと試し直しは終わる。`MUSIC AUDIO reopened` の行は最後の試しで 1 回。helper の reopened の待ちを 15 秒に。
- 確認: music の build exit 0・warning 0、style-check 指摘なし、`run-host-music.sh`・`host-music-play.sh` PASS（開き直しは host で組めない、T1 の apps.music.failures）。

## T1-455 の結果と直し（2026-10-08 夜 P2）

- T1-455: 手順 1 の開き直しは 3 回とも OK（`open error=24` の後 `reopened error=0`、seek は lost の ms と同じ）。手順 3（Space）が 3 回とも FAIL。
- 読み（log）: 数え違いではない。開き直しで Tone A（8 秒）を 3.6 秒から続けたので、手順 1 の待ちの間に Tone A が自分で終わり（`MUSIC ENDED song=0` → `PLAY song=1`）、手順 2 の Next の 2 回（`REQUEST action=2` が 2 つ、別の frame で `STEP step=1` が 2 つ）で song 2 の次へ進んで最後の曲を越え `STOP song=2`、手順 3 の Space は止まった曲の再生し直しになった。2 回の click は 2 回と数えている（host の `next-twice-two-frames` が「1 回目の click を前の frame で数えた後の DOUBLE」の case で、2 回の request を確かめている）。
- 直し（試験の側）: この scenario の 3 曲を 20 秒に（`make_songs` に長さ、`failures.md` の準備に注記）。

<!-- awesome-plan project=zedbsd record=ws177-p021 -->

# ws177-p021: 音楽の準正常系の 2 — 再生の失敗と key（案 M）

Parent: [WS177](../ws.md)
Status: in-progress（2026-10-08 夜 P2 q903 の 2）
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
- `music/view.c`: 検索の field の SUBMITTED・CANCELLED で `kl_ui_clear_focus`。

## 確認

- build: `make -j16 ZEDBSD_CONFIG=config/current-uat.mk BUILD=build/amd64 build/amd64/bin/music build/amd64/bin/videoplayer` → exit 0、warning 0（-Werror）。style-check（music の全部と videoplayer/audio.c）→ 指摘なし。
- host（新規）: `sh plan/ws177/tests/host-music-play.sh` → PASS（3 回連続、ASan・UBSan。play.c を container・decoder・音の stream の stand-in で: 最後まで鳴る、16 個続けて decode できない → MU_FAIL_DECODE・止まる・1 回だけ、15 個続けてを 2 回 → 失敗にならず終わる（`draining` の不具合の回帰も兼ねる）、読みの誤り → MU_FAIL_READ、次の曲で失敗が残らない）。
- host（既存に追加）: `plan/ws120/tests/run-host-music.sh` → PASS（search-space・search-escape・search-enter を足した。view.c の直しを外すと search-escape・search-enter が FAIL になることを確かめた）。`plan/ws177/tests/host-music-m.sh` → PASS（p020 の回帰）。
- 未実施: main.c の流れ（連打のまとめ、gone、lost の開き直し、Files の理由）は host で組めず、QEMU の T1 に `apps.music.failures`（新しい scenario、`tests/scenarios/apps/music/failures.md`）で依頼する。

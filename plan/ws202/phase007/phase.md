<!-- awesome-plan project=zedbsd record=ws202-p007 -->

# ws202-p007: Music を libavcodec なしで

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 2 LW
依存: p006

## 目的

Music が libavcodec の無い image で .m4a を再生できるようにする（design §9.4）。

## 成果

- `userland/desktop/music/main.c`: 起動の時の `media_codec_load()` の門と「Playing needs libavcodec (the libavcodec package).」の 2 か所を外す。
  曲を開けない時に `mu_player_open` の問題（`MEDIA_PROBLEM_*`）から文を出す: MISSING「This song's format needs FFmpeg's libavcodec, which is not
  installed.」、PROFILE「This song's format is not supported.」、他は今の文。
- `userland/desktop/music/play.c`: open の log を `MUSIC PLAY open codec=%s backend=%s container=%s duration_ms=%lld` に（`media_decoder_backend`）。
  問題の code を `player->problem` に入れる（今の形を確かめて合わせる）。
- `play.h`・`Makefile`・`play.c` の頭の comment の古い記述（codec.c・bitstream.c・「libavcodec で decode」）を今の形に直す。
- `tests/scenarios/apps/music/play.md`: 準備の「image に libavcodec の package」を外し、正解に `backend=libmedia` を足す。`paths` に
  `userland/desktop/libmedia/aac.c` を足す（tests/ は source の扱い。Q1 へ差分を送るか許可を受けて直す）。`failures.md` に libavcodec の無い時の
  文の変化があれば合わせる。

## 確認

| コマンド | 期待 |
| --- | --- |
| music の build | warning 0 |
| `sh plan/tools/media/run-host-codec.sh` | PASS |
| Music の host 試験（`plan/master.md` の Tools 節に Music の host 試験があればそれ、無ければ無し） | PASS |

QEMU の確認は p012（T1）。この Phase では QEMU を起動しない。

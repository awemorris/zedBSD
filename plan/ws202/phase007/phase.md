<!-- awesome-plan project=zedbsd record=ws202-p007 -->

# ws202-p007: Music を libavcodec なしで

Status: planned
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 3 LW
依存: p006

## 目的

Music が libavcodec の無い image で .m4a を再生できるようにし、AAT の helper と scenario を合わせる（design §9.4）。

## 成果

- `userland/desktop/music/main.c`: 起動の時の `media_codec_load()` の門と「Playing needs libavcodec (the libavcodec package).」の 2 か所を外す。曲を開けない時に問題から文:
  MISSING「This song's format needs FFmpeg's libavcodec, which is not installed.」、PROFILE「This song's format is not supported.」、他は今の文。
- `userland/desktop/music/play.c`: open の log を `MUSIC PLAY open codec=%s backend=%s container=%s duration_ms=%lld` に。seek の後（`play_seek` の flush の後）に
  `media_decoder_trim` を呼び、0 なら frame の単位の `skip_before` の捨てを使わない（ENOTSUP なら今のまま）。
- `play.h`・`Makefile`・`play.c` の頭の comment の古い記述を直す。
- WS の外の file（差分を Q1 へ、または許可を受けて直す）:
  - `plan/tools/aat/scenarios/helpers_music.py`（M-08）: `MUSIC CODEC load error=` の待ちと「libavcodec did not load」の判定（78・87 行付近）を外し、`MUSIC PLAY open codec=aac
    backend=` の行を確かめる形に。
  - `tests/scenarios/apps/music/play.md`: 準備の「image に libavcodec の package」を外し、手順 1 の正解（`MUSIC CODEC load`）を変え、`backend=libmedia` を足す。`paths` に
    `userland/desktop/libmedia/aac.c`。`failures.md` も文の変化に合わせる。

## 確認

| コマンド | 期待 |
| --- | --- |
| music の build | warning 0 |
| `sh plan/tools/media/run-host-codec.sh` | PASS |
| `python3 -I -m py_compile plan/tools/aat/scenarios/helpers_music.py`（直した時） | 成功 |

QEMU の確認は p012（T1）。

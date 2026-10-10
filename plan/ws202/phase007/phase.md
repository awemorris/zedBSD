<!-- awesome-plan project=zedbsd record=ws202-p007 -->

# ws202-p007: Music を libavcodec なしで

Status: uncleared（software実装あり、全条件の確認は未完）
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 3 LW
依存: p006

## 目的

Music が libavcodec の無い image で .m4a を再生できるようにし、AAT の helper と scenario を合わせる（design §9.4）。

## 成果

- `userland/desktop/music/main.c`: 起動の時の `media_codec_load()` の門と「Playing needs libavcodec (the libavcodec package).」の 2 か所を外す。曲を開けない時に問題から文:
  MISSING「This song's format needs FFmpeg's libavcodec, which is not installed.」、PROFILE「This song's format is not supported.」、他は今の文。
- `userland/desktop/music/play.c`: open の log を `MUSIC PLAY open codec=%s backend=%s container=%s duration_ms=%lld` に。seek（`play_seek`）は pre-roll（D30、M2-10）:
  `media_decoder_frame_us` が 0 でなければ `media_file_seek(目標 − 1 frame)`（0 で止める）で読み、flush の後に `media_decoder_trim(目標)` を呼び、0 なら frame の単位の
  `skip_before` の捨てを使わない。frame_us が 0 か trim が ENOTSUP（libavcodec）なら今のまま。
- `play.h`・`Makefile`・`play.c` の頭の comment の古い記述を直す。
- WS の外の file（差分を Q1 へ、または許可を受けて直す）:
  - `plan/tools/aat/scenarios/helpers_music.py`（M-08）: `MUSIC CODEC load error=` の待ちと「libavcodec did not load」の判定（78・87 行付近）を外し、`MUSIC PLAY open codec=aac
    backend=` の行を確かめる形に。
  - `tests/scenarios/apps/music/play.md`: 準備の「image に libavcodec の package」を外し、手順 1 の正解（`MUSIC CODEC load`）を変え、`backend=libmedia` を足す。`paths` に
    `userland/desktop/libmedia/aac.c`。`failures.md` も文の変化に合わせる。

## 確認

| コマンド | 期待 |
| --- | --- |
| design §10.6 の build（`ZEDBSD_CONFIG=config/ci/config-amd64.mk`、libmedia・libbrowser・videoplayer・music） | exit 0、`grep -c 'warning:'` が 0 |
| `sh plan/tools/media/run-host-codec.sh` | PASS |
| `python3 -I -m py_compile plan/tools/aat/scenarios/helpers_music.py`（直した時） | 成功 |

QEMU の確認は p012（T1）。


## 構造改訂と部分結果（2026-10-10）

Musicはnative-firstをappの共通adapterで行い、非対応の場合だけapp所有のFFmpeg adapterをdlopenする。libraryのmedia_codec_loadへの起動依存を除く。曲ごとのbackend/notice/seek/寿命を区別する。 [変更理由・依存・結果](../policy-20261010.md)。旧記録は保持し、対象外の未実施条件をclearedとしない。共有投影/他担当/GitHubはQ1へpending。

## Native再生software結果（2026-10-10）

Event: `ws202-native-playback-software-20261010-p007`。Queue: [codex-ws202-playback](../policy-20261010.md#自走の実行承認-codex-ws202-playback)。

Musicをapp native-first adapterへ変更、native codec/backend log・notice・seek preroll/trim・read/drain failureを実装。buildとadapterの実host混在再生を確認。共有AAT追従はproposal、QEMU Music/耳でのUATは未実施。

[最終source/command/結果・限界](../playback-result-20261010.md)、[Q1統合](../handoff-20261010.md)、[T1の準備済み依頼](../t1-playback-request-20261010.md)。旧第2版の手順・昔のpartial outcomeを保存し、最新記録が未実装記述の現在状態を置換する。whole criteriaを満たしたとは扱わず、Q1の意味の統合と未実施matrix/実機結果が再開条件。main/共有投影/GitHubの更新はQ1 pending。


## main統合の追記（2026-10-10）

Event: `ws202-main-integration-20261010-p007`。ユーザー「mainへの統合はあなたがやってOKです。」によりsourceと記録をmainへ統合。最新の承認済み方針・手順・確認・残件は[統合記録](../main-integration-20261010.md)と[policy](../policy-20261010.md)。上の設計時点の推奨、旧未実装/統合pendingは履歴として保存する。software出力の有無とwhole clearanceを区別する。標準readbackの依存はp017、ref-listは既存p016。p012/T1→p013/User UAT→whole p014の確認は未実施、Master/共有Board/GitHubへの投影はQ1に保持。

共有AAT helper/scenarioと旧host-codec/layoutの移管追従は今回mainへ適用。実行結果は統合記録に記載し、旧proposal/pendingを現在の状態として扱わない。

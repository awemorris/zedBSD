<!-- awesome-plan project=zedbsd record=ws202-p004 -->

# ws202-p004: mediafile の pasp・colr・表示の終わり

Status: uncleared（software実装あり、全条件の確認は未完）
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 2 LW
依存: p001

## 目的

mp4 の reader（既存）に、縦横比・色・表示の終わりを足す（design §4）。

## 成果

- `mediafile.h` の `struct media_track` に `sar_num`・`sar_den`（0 は未知）、`colour_matrix`（0 未知、H.273 の値）・`full_range`（-1 未知、0・1）、`end_us`（0 未知）。
  J1 で container を絞る時だけ `container`（`media_file_format_name` と同じ文字列、mediafile.c が全 track に入れる。L2-15）。
- `mp4.c`:
  - `read_visual_entry`: 子の `pasp`（hSpacing・vSpacing）、`colr` の `nclx`（primaries・transfer・matrix 各 16 bit、full_range_flag 1 bit）と `nclc`（primaries・
    transfer・matrix、full range は -1 のまま）。avcC 等を見つけた後も続く子を読む（loop の形を変える）。
  - `read_edit_list`: 最初の実の edit の segment_duration を覚え、`finish_tracks` で `end_us` = 遅延 ＋ segment の長さ（無い・0 は 0）。
- `plan/tools/media/make-media.py` に pasp・colr（nclx・nclc）・edts の長さを持つ試料、`host-mediafile.c` に期待（WS の外: 差分を Q1 へ）。
- U1: fragmented の 100〜200 MB の合成の file（ffmpeg `-movflags frag_keyframe+empty_moov`、低い bitrate で長く）を**自分の worktree の `build/`** に作り、
  `media_file_open` の時間を測って 1 GB に外挿して記録する。file は消さず、path を Q1 に送る（rm は Q1）。

## 確認

| コマンド | 期待 |
| --- | --- |
| `sh plan/tools/media/run-host-mediafile.sh` | 既存と新しい期待が PASS |
| design §10.6 の build（`ZEDBSD_CONFIG=config/ci/config-amd64.mk`、libmedia・libbrowser・videoplayer・music） | exit 0、`grep -c 'warning:'` が 0 |


## 構造改訂と部分結果（2026-10-10）

containerのmetadata追加は維持。library/appの双方に同じ公開trackを渡し、FFmpeg依存をreaderに入れない。 [変更理由・依存・結果](../policy-20261010.md)。旧記録は保持し、対象外の未実施条件をclearedとしない。共有投影/他担当/GitHubはQ1へpending。

## Native再生software結果（2026-10-10）

Event: `ws202-native-playback-software-20261010-p004`。Queue: [codex-ws202-playback](../policy-20261010.md#自走の実行承認-codex-ws202-playback)。

pasp/colr nclx/nclc/end_us、track末尾のdecode_order_timesを実装。実MP4の2秒音声終端/no-cttsとnamed buildを確認。fragmented巨大fileの外挿時間と共有mediafile全回帰は未実施。

[最終source/command/結果・限界](../playback-result-20261010.md)、[Q1統合](../handoff-20261010.md)、[T1の準備済み依頼](../t1-playback-request-20261010.md)。旧第2版の手順・昔のpartial outcomeを保存し、最新記録が未実装記述の現在状態を置換する。whole criteriaを満たしたとは扱わず、Q1の意味の統合と未実施matrix/実機結果が再開条件。main/共有投影/GitHubの更新はQ1 pending。


## main統合の追記（2026-10-10）

Event: `ws202-main-integration-20261010-p004`。ユーザー「mainへの統合はあなたがやってOKです。」によりsourceと記録をmainへ統合。最新の承認済み方針・手順・確認・残件は[統合記録](../main-integration-20261010.md)と[policy](../policy-20261010.md)。上の設計時点の推奨、旧未実装/統合pendingは履歴として保存する。software出力の有無とwhole clearanceを区別する。標準readbackの依存はp017、ref-listは既存p016。p012/T1→p013/User UAT→whole p014の確認は未実施、Master/共有Board/GitHubへの投影はQ1に保持。

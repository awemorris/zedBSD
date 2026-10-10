<!-- awesome-plan project=zedbsd record=ws202-p014 -->

# ws202-p014: 全文規約の見直しと試験の整理

Status: planned（ベータ3、2026-10-08 ユーザー「コーディング規約による整形はベータ3でやります」）
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 4 LW
依存: p013（WS の全 code の変更の後）

## 現在の適用方針

[最新ユーザー決定](../policy-20261010.md)が以下の旧第2版手順に優先する。具体的手順の改訂/reviewは未了。

## 目的

WS202 が足した・変えた C の全部を `plan/coding-style.md` の全文（§14 の checklist を含む）と照らし、書き方だけを直す（動作は変えない）。残す試験を master.md に登録し、
開発の途中だけの試験を片付ける（2026-10-06 の試験の整理の基準、review-001 L-11）。

## 範囲

- `userland/desktop/libmedia/`（新しい file と、`decoder.c`・`media-decoder.h`・`media-private.h`・`avcodec.c`・`Makefile` の差分）、`userland/desktop/mediafile/` の差分、
  `userland/desktop/videoplayer/`・`music/` の差分、`userland/tests/media-probe/`。
- `plan/ws202/tests/` の host の C。
- 生成した `aac-tables.c` は script の出力の形を直す（手で直さない）。

## 手順

1. WS202 の commit を列挙（`git log --format=%H -- <上の path>`）し、差分を全文と照らして直す。評価の順・所有・寿命・誤りの報告・振る舞いを変えない。
2. `git diff --check`、build（warning 0）、host 試験の全部が PASS。動作を変えていないので QEMU・実機は回さない。
3. 試験の整理（Q1 へ依頼。master.md・`plan/tools/` は WS の外）:
   - 回帰に残す物: `make-streams.sh`（と `streams/`）、`run-host-aac.sh`・`run-host-aac-parse.sh`・`run-host-h264.sh`・`run-host-vkvideo.sh`・`run-host-picture.sh`・
     `run-host-sound.sh`、`gen-aac-tables.py`、`host-media-rms.c`、`media-probe`。`plan/tools/media/` へ移し、master.md の Tools・試験の一覧に登録する差分を Q1 に送る。
     scenario（`h264-native.md`・`no-video-decode.md`）は `tests/` にあるので suite（`tests/suites/`）への登録を Q1 に。
   - 開発の途中だけの物（例 `gen-asc.py` の一時の file、`fetch-conformance.sh` を残さないと決めた時）: path を Q1 に送る（rm は Q1）。文書からの参照も外す。
4. WS の完了の形（ws.md を書き直し、Phase の directory の削除は Q1）は Q1 の判定の後。

## 記録

範囲の file、直した規則の種類と数、確認の結果、Q1 に送った試験の整理の一覧。


## 構造改訂と部分結果（2026-10-10）

native-only library、app共有adapter/FFmpeg移管、必要なdriver/readback補完と新table生成器を全文レビュー対象に追加。規格数値と外部codeを区別する。共有Tool/Guardrail投影はQ1へ。 [変更理由・依存・結果](../policy-20261010.md)。旧記録は保持し、対象外の未実施条件をclearedとしない。共有投影/他担当/GitHubはQ1へpending。

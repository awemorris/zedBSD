<!-- awesome-plan project=zedbsd record=ws202-p014 -->

# ws202-p014: 全文規約の見直し

Status: planned（ベータ3、2026-10-08 ユーザー「コーディング規約による整形はベータ3でやります」）
Disposition: normal
Parent: [WS202](../ws.md)
見積もり: 3 LW
依存: p013（WS の全 code の変更の後）

## 目的

WS202 が足した・変えた C の全部を `plan/coding-style.md` の全文（§14 の checklist を含む）と照らし、書き方だけを直す（動作は変えない）。

## 範囲

- `userland/desktop/libmedia/`（`bits.c`・`picture.c`・`sound.c`・`aac*.c`・`aac.h`・`h264*.c`・`h264.h`・`vkvideo*.c`・`vkvideo.h`・`decoder.c`・
  `media-decoder.h`・`media-private.h`・`avcodec.c` の open の引数の変更）。
- `userland/desktop/mediafile/mp4.c`・`mediafile.h` の差分、`userland/desktop/videoplayer/`・`music/` の差分、`userland/tests/media-probe/`。
- `plan/ws202/tests/` の host の C（試験も規約の対象）。
- 生成した `aac-tables.c` は生成の script の出力の形を規約に合わせる（手で直さず script を直す）。

## 手順と確認

1. WS の最初の commit からの差分を列挙（`git log --format=%H -- <上の path>` で WS202 の commit を確かめる）。
2. 全文と照らして直す。評価の順・所有・寿命・誤りの報告・振る舞いを変えない。
3. `git diff --check`、build（warning 0）、host 試験（`plan/ws202/tests/run-host-*.sh` の全部と `plan/tools/media/run-host-*.sh`）が PASS。
4. 動作を変えていないので QEMU・実機は回さない（書き方だけの変更、AGENTS.md の試験の方針）。

## 記録

範囲の file、直した規則の種類と数、確認の結果。直さなかった物は理由（例外の承認があればその出典）。

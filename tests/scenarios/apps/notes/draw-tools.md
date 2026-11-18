---
id: apps.notes.draw-tools
title: Notes の筆・鉛筆・水彩をペンで描き、描く道具の帯で Marker を選び、保存して開き直す
status: active
areas: [notes, libpdf]
paths: [userland/desktop/notes/, userland/desktop/libpdf/, userland/tests/peninject/]
machine: either
human: look
since: ws216-p007
---

## 目的
Notes の描く道具（筆・鉛筆・水彩）が試験のペンの筆圧で描け、描く道具の button の帯から道具を選べ、undo・redo と保存・開き直しで線が保たれ、保存した PDF に水彩の Multiply と鉛筆の粒の pattern が入ることを確かめる（WS216、ws216-p003〜p006）。

## 準備
- image は試験のペン（`peninject`）のある AAT の image（`plan/tools/aat/config-amd64-notes-pen.mk`）。`peninject` の無い image では線は mouse の物（筆圧なし）になり、補助は最後に fail（理由: 試験のペンが無い）を返す。
- 補助（`plan/tools/aat/scenarios/helpers_notes_edit.py`）が target に scenario の folder（`/tmp/aat-work/notes-draw-tools-<時刻>/`）を作り、user として Notes をその中の新しい notebook `draw.pdf` で開く（`NOTES START`）。座標は Notes の `NOTES LAYOUT`（頁の位置）・`NOTES BUTTONS`（toolbar と帯の button）と窓の位置から決める。
- ペンの線: `aat-input` が注入の口を全部持つので、補助は `aat stop` → `peninject`（root、画面全体に写る tablet、傾き (20, −15)、筆圧 0 → 4095 → 0 の sine）→ `aat start` の順に流す。

## 操作と確認
1. 操作: key B（筆）、toolbar の黒と中の幅、ペンで頁の上の方に波の線。
   確認事項: 筆の線。正解: `NOTES TOOL 49 name=brush`、`NOTES STROKE page=0 … tool=2 … pressure=A..B tilt=1`（B > 30000）。確認方法: log。
2. 操作: key L（鉛筆）、ペンで 2 本目の波の線。撮影 `brush-pencil`。
   確認事項: 鉛筆の線と粒。正解: `NOTES TOOL 50 name=pencil`、`NOTES STROKE … tool=3 … tilt=1`（筆圧あり）。撮影で鉛筆に紙の粒があり、区間の継ぎ目に線が無い。筆は入りと抜きが細く中ほどが太い。確認方法: log、撮影（人が見る）。
3. 操作: key W（水彩）、青でペンの横の線、赤でそれに交わる斜めの線。撮影 `watercolor`。
   確認事項: 水彩の重なり。正解: `NOTES TOOL 51 name=watercolor`、`tool=4` の `NOTES STROKE` が 2 つ。log に `NOTES PIPE made name=grain` と `NOTES PIPE made name=multiply` があり、`NOTES PIPE fallback` が無い。撮影で 2 本は半透明、交わる所が濃く、青と赤が絵の具のように（減法で）混ざる。確認方法: log、撮影（人が見る）。
4. 操作: 描く道具の button（action 48）を押す。撮影 `strip`。帯の Marker（action 2）を押し、mouse で頁に線。撮影 `all-tools`。
   確認事項: 描く道具の帯。正解: `NOTES DRAWS open`、帯の button（49 など）のある `NOTES BUTTONS`。撮影で帯が toolbar の下に Pen・Brush・Pencil・Water・Marker・Fill の順に並ぶ。Marker で `NOTES TOOL 2 name=highlighter` と `NOTES DRAWS closed`、`NOTES STROKE … tool=1`。確認方法: log、撮影（人が見る）。
5. 操作: Ctrl+Z、Ctrl+Shift+Z。
   確認事項: undo・redo。正解: `NOTES UNDO page=0 strokes=N−1`、`NOTES REDO page=0 strokes=N`（N は 4 の後の線の数、5）。確認方法: log。
6. 操作: 黒と Pen に戻し（key P）、Ctrl+S、Notes を閉じ（Ctrl+W）、同じ file を開き直す。撮影 `reopened`。
   確認事項: 保存と開き直し。正解: `NOTES SAVE reason=request … strokes=N`、`NOTES OPEN … strokes=N kind=notes`。開き直した後も `NOTES PIPE fallback` が無い。撮影が 4 の `all-tools` と同じ線（筆の太さ、鉛筆の粒、水彩の重なり）。確認方法: log、撮影（人が見る）。
7. 操作: host へ `draw.pdf` を取り、`qpdf --check`、`qpdf --qdf` で展開、`pdftoppm -r 72` で 1 頁を描く（`…-poppler.png`）。
   確認事項: PDF の中身。正解: `qpdf --check` に誤りが無い。展開した PDF に `/Multiply`（水彩）と `/PatternType 1`（鉛筆の粒）がある。Poppler の頁が画面の線に近い。確認方法: host の command、画像（人が見る）。

## 合格
1〜7 の正解。判定は log と撮影（PNG）で、撮影の見た目は人（または T1）が見る（needs-person）。

## 注記
- 補助は `apps.notes.draw-tools`（`helpers_notes_edit.py`）。色と道具は Notes の設定に残るので、最後に黒と Pen に戻す。
- `NOTES GRAIN hash=` の行を記録に残す（Pi 4 の p009 で amd64 の値と比べる）。
- 実機 5330 のペンの感触（筆の入り抜き、鉛筆の濃さ、水彩の重ね）はユーザーが見る（ws216-p007 の別の項目）。

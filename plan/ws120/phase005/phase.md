<!-- awesome-plan project=zedbsd record=ws120-p005 -->

# ws120-p005: Music の app の MVP

Parent: [WS120](../ws.md)
Status: canceled（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: 2026-10-07 の決定（m4a＋libavcodec の add-in、p008・p009）で置き換え）（旧: planning）
Disposition: canceled（2026-10-07 q831: ユーザーの決定（形式は m4a だけ、AAC は libavcodec の add-in、独自の decoder は後）で取り下げ。置き換えは [p008](../phase008/phase.md)・[p009](../phase009/phase.md)。[p001](../phase001/phase.md) の「2026-10-07 の決定と設計」）
Queue / attempts: none
Goal: Keiland の音楽アプリを作り、QEMU で M1〜M6 を確かめる。
Prerequisites: p002 cleared、p003 cleared（MP3 を含むなら p004 も）、D4 の決定。
Investigation bound: 4 時間。

## 範囲

- `userland/desktop/music/`（p001 で決めた名前）: window、file・folder を開く、一覧（題・artist・album・長さ）、再生・一時停止・次・前・seek の bar、経過の時間、cover、keyboard の操作、エラーの表示（M6）。3 OS の Makefile（他 OS で音が出ないなら表示する、D3）。
- decode は再生の thread（または poll の loop）で先読みし、再生の API の ring を満たす。44.1 kHz の曲は audiod の変換に任せる（線形補間。音質の改善は後）。
- 音量: `keiland_audio_*` の device の音量を表示・操作する（M4）。
- Files の関連付け（`userland/desktop/files/apps.c`・`mime.c` に audio の MIME と Music）。menuconfig と image への登録、app の一覧（launcher）への登録。
- 試験 `plan/ws120/tests/`: QEMU で file を開いて再生し WAV を判定、操作の注入で一時停止・seek・次。

## 受け入れ

[WS120](../ws.md) の M1〜M6。M7 は実機の機会があればユーザーと（無ければ未実施）。

## 検証

QEMU の自動の試験と画面、WS127（Files）の既存の試験の回帰、`plan/tools/boot-test.sh`（PNG をユーザーに見せる）。

## 所有 path

`userland/desktop/music/`、Files の関連付けの行（`apps.c`・`mime.c`、WS127 と順を main が調整）、`plan/ws120/`。

## 依存・未決の判断

p002、p003、p004、D4。

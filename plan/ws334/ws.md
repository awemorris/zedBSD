<!-- awesome-plan project=zedbsd record=ws334 -->

# WS334 libbrowser の Web ブラウザ OmniGrammaton を Windows に移植する（libkeiland の Win32 の実装、Windows 用の .exe）

- Master: [plan/master.md](../master.md)（2026-10-31 作成）。Primary Milestone: MG006。
- Status: planning（2026-10-31 23:59 新設）
- 目標: libbrowser を使う Web ブラウザを Windows（Win32）で動く .exe にする。その移植を通して、libbrowser が platform に依らない事を結果として確かめる。
  そのために libkeiland の Win32 の実装（window・入力・描画・thread・socket ほか）を作る。ブラウザの名前は OmniGrammaton。
- 関係: [WS074](../ws074/ws.md)（libbrowser と browser の shell、zedBSD の上の Web ブラウザ）、[WS327](../ws327/ws.md)（libkeiland の thread・socket の
  Win32 の実装は compile と link まで、design §5.4）、Future Work F-WS327-win32-build（libkeiland の Windows の build の約束）。

## ユーザーの決定

- 2026-10-31 ユーザーから、次の趣旨の指示を受けた。
  1. libbrowser のブラウザを Windows に移植する WS を作る。
  2. libbrowser が platform に依らない事を、移植を通して結果として確かめる。
  3. libkeiland の Win32 API の実装を作る。
  4. Windows 用の .exe を作る。
  5. ブラウザの名前は OmniGrammaton。

## Resume point

- p001（設計）から。担当はまだ割り当てていない。

## Phase

| ID | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計: libbrowser と browser の shell の platform の依存の洗い出し、libkeiland の Win32 の backend（window・入力・描画・IME・clipboard・file・thread・socket）、build（cross compile の toolchain、.exe・DLL・resource）、名前 OmniGrammaton の扱い、試験の方法、Phase の分け方、ユーザーへの問い | planned | — |

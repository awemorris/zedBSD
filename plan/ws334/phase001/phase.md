<!-- awesome-plan project=zedbsd record=ws334-p001 -->

# ws334-p001: 設計（OmniGrammaton の Windows 移植）

Status: planned（2026-10-31 23:59 新設）
Parent: [WS334](../ws.md)

## 範囲

1. 今の事実を読む: libbrowser（`userland/desktop/libbrowser/`）と browser の shell（`userland/desktop/browser/`）が使う OS・Keiland の口（file・network・
   thread・時計・font・image・GPU の描画・入力・IME・clipboard）。platform に依る所が libbrowser の中にあれば一覧にする（直すのは libbrowser の外か、
   libbrowser の platform の層か）。
2. libkeiland の今の backend（zedBSD・Linux・FreeBSD）の構造と、Win32 の backend の形（window と message loop、入力、描画: Vulkan か GDI・Direct3D か
   software か、DPI、IME、clipboard、file の選び、thread・socket は WS327 の Win32 の実装）。
3. build: Windows 用の cross compile（mingw-w64 か LLVM の Windows の target、toolchain の変更は Q1 の許可が要る）、.exe と DLL、resource（icon・版の情報）、
   TLS（https）と証明書、font の入手と license。
4. 名前 OmniGrammaton を、Windows の .exe・窓の題・版の情報にどう出すか（zedBSD の上の名前との関係）。
5. 試験: host での cross build と warning 0、Wine での起動の可否、実機の Windows での確かめ（ユーザーの環境）、libbrowser の既存の試験（Interop の計測など）を
   Windows でどこまで流すか。
6. 実装の Phase の分け方と見積もり（LW）、標準廉価実装手順に回せる module。design-reviewer の敵対的レビュー、ユーザーへの問い（AGENTS.md §3 の前置きの形）。
7. code は書かない。

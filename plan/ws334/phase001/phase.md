<!-- awesome-plan project=zedbsd record=ws334-p001 -->

# ws334-p001: 設計（OmniGrammaton の Windows 移植）

Status: cleared（2026-11-01 01:25 Q1 の判定。設計 1deb49bff、main b2a7e8454、ユーザーの答え U1〜U15 が揃った）
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

## 結果（2026-11-01 P7、q1787-i01）

- 成果物: [design.md](../design.md)（設計）、[user-questions.md](../user-questions.md)（ユーザーへの問い U1〜U15、AGENTS.md §3 の前置きの形）、
  [design-review.md](../design-review.md)（design-reviewer の指摘 高 4・中 14・低 6 と反映）。
- 途中で受けた決定 11〜15（toolchain と Windows の host、`make browser-win64`、`apps/browser-win32/`、libkeiland は browser に要る部分から、UI は libkeiland の上の今の UI のまま）を
  反映した。決定 15 の前に書いた「共通の shell と toolkit ごとの層」の形は、予備の案 B として design.md §4.2 に残した。
- 行った確かめ: libbrowser・libmedia・libkeiland・browser の shell・libtruetype・compat・一番上の Makefile の source を grep と読みで照合（行の番号は design.md に）。
  host の道具（mingw-w64 gcc 14 の spec、UCRT の版の package が apt の候補にある事、wine・Vulkan の package）。補助の Explore 2 本の報告の主な行を照合（design.md §2.12）。
- 未実施: code の変更・build・試験（範囲の外）。他の実装（Chrome・Ladybird・node.js）と Microsoft の文書の出典の取得（設計の判断はそれらの細部に頼らない、design.md §10）。
  llvm-mingw の遅延の import・OpenSSL の arm64 の target・winpthreads の MONOTONIC の cond は未確かめ（design.md §12、各 Phase の最初に確かめる）。
- 残り: Q1 の確認、ユーザーの問いの答え、Phase の表を ws.md に載せる（design.md §8 の p002〜p018、約 131 LW）、P8 との libbrowser の file の調整（design.md §9）。

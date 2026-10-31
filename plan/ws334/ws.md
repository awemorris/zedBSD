<!-- awesome-plan project=zedbsd record=ws334 -->

# WS334 libbrowser の Web ブラウザ OmniGrammaton を Windows に移植する（libkeiland の Win32 の実装、Windows 用の .exe）

- Master: [plan/master.md](../master.md)（2026-10-31 作成）。Primary Milestone: MG006。
- Status: planning（2026-11-01 00:24 P7 が p001 の設計を q1787 で始めた。2026-10-31 23:59 新設）
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
- 2026-11-01 ユーザーから、WS334 は計画だけ進める（担当の割当と実装はまだ）、という趣旨の指示と、次の設計の方針を受けた。
  6. libtruetype・libpng-compat・libgif-compat・libjpeg-compat・libmedia は、そのまま Windows の build に使う。
  7. libmedia は Vulkan と Vulkan Video の上なので、そのまま使える見込み。合わない所は直す（libmedia も、Vulkan と Vulkan Video の実装の側も直してよい）。
  8. libkeiland の移植は、Qt6 を使う libkeiland-qt6 と、Win32 を直に使う形の 2 つを比べて検討する（Q1 の答えを [design-notes.md](design-notes.md) に）。
  9. libbrowser を、レンダリングエンジン・スクリプトエンジン・ストレージバックエンドに分ける。
     - 3 つはそれぞれ interface を持ち、その実装を libbrowser の instance に登録して使う。各エンジンは単体でも使えるように保つ。
     - レンダリングエンジンの DOM への access をきちんと interface にする。スクリプトエンジンも同じ。
     - スクリプトエンジンは、core と、レンダリングエンジンへの access の部分を分けて実装する。core だけで build して library にできるようにする。node.js を参考にする。
     - ストレージバックエンドは、cookie・cache と、その他 local に保存しないとブラウザとして働かない物を受け持つ。
  10. 2026-11-01 ユーザーから、レンダリングエンジンと JS エンジンは今の source tree で分かれているので、その interface の分離を保てばよい、という趣旨の見立てを受けた（DOM を別の部品に出す事はしない）。
  11. 2026-11-01 ユーザーから、cross compile は mingw-w64 でよく、clang 版の mingw（llvm-mingw の類、arm64 の Windows にも対応）でもよい、shell をある程度作った後は開発を Windows の host に移せる（Wine では Vulkan があまり動かない見込みのため）、という趣旨の指示を受けた。
  12. 2026-11-01 ユーザーから、`make browser-win64` で build できるとよい、という趣旨の希望を受けた（x86_64 の Windows 用。arm64 の target の名前は設計で案を出す）。
  13. 2026-11-01 ユーザーから、tree の一番上に `apps/` を作り、Windows 版は `apps/browser-win32/`（その Makefile）に置く、一番上の Makefile からも呼ぶ（`make browser-win64` など）、という趣旨の指示を受けた（最初は `apps/browser/` と言い、同日 `apps/browser-win32/` に改めた。後で `apps/browser-qt`・`apps/browser-gtk4` も足す予定）。
  14. 2026-11-01 ユーザーから、libkeiland はまず browser が動くのに要る部分だけを部分的に移植して動かすところまで進め、全体の移植は後でじっくり行う、という趣旨の指示を受けた（決定 3 の進め方。全体の移植を WS334 の後半か WS234 などに分けるかは設計で案を出す）。
  15. 2026-11-01 ユーザーから、ブラウザの UI はまず libkeiland の Win32 の backend の上で今の Keiland の UI のまま移植する（A）、libkeiland で足りなければ後で Win32 を直に使う版を作ればよい（大きな実装ではないため）、という趣旨の指示を受けた。Chrome も Fluent を使わない事、font は自前でよい事、menu は最近のブラウザに無く要る時は HMENU に写せばよい事、libkeiland が作るのは HWND 1 つである事が理由。
  16. 2026-11-01 ユーザーから、https の libssl は当面 OpenSSL の物を使い、後で libssl-compat を開発した時にそれへ切り替える、という趣旨の指示を受けた（U10 の A に当たる。切り替えやすいように、OpenSSL への依存は libbrowser の net/tls.c などの口に閉じ込めておく）。

## Resume point

- 2026-11-01 ユーザーから、P8（WS074）を止めずに P7 で WS334 に取りかかる、libbrowser の修正の範囲は限られ、その後は shell と libkeiland の移植の開発になる、という趣旨の指示を受けた。P7 が p001（設計）を q1787 で始めた。
- 2026-11-01 P7: p001 の設計を [design.md](design.md) に、ユーザーへの問い U1〜U13 を [user-questions.md](user-questions.md) に、design-reviewer の指摘を [design-review.md](design-review.md) に書いた。
  実装の Phase の案は design.md §8（p002〜p016、約 112 LW）。ws.md の Phase の表に載せるのは Q1 の確認と問いの答えの後。libbrowser の変える file の一覧は design.md §9.1（P8 との調整用）。

## Phase

| ID | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計: libbrowser の 3 つのエンジン（レンダリング・スクリプト・ストレージ）への分割と interface（DOM の access、スクリプトの core と binding の分離）、libkeiland の Windows の backend（Win32 直か Qt6 か、window・入力・描画・IME・clipboard・file・thread・socket）、libmedia と Vulkan・Vulkan Video の Windows での使い方、既存の library（libtruetype・png・gif・jpeg の compat）の build、cross compile の toolchain、.exe・DLL・resource、名前 OmniGrammaton、試験の方法、Phase の分け方、ユーザーへの問い | in-progress（2026-11-01 00:24 P7 q1787。設計の文書・問い・レビューを書いた、Q1 の確認待ち） | — |

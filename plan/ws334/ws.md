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

## Resume point

- 2026-11-01 ユーザーから、P8（WS074）を止めずに P7 で WS334 に取りかかる、libbrowser の修正の範囲は限られ、その後は shell と libkeiland の移植の開発になる、という趣旨の指示を受けた。P7 が p001（設計）を q1787 で始めた。

## Phase

| ID | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計: libbrowser の 3 つのエンジン（レンダリング・スクリプト・ストレージ）への分割と interface（DOM の access、スクリプトの core と binding の分離）、libkeiland の Windows の backend（Win32 直か Qt6 か、window・入力・描画・IME・clipboard・file・thread・socket）、libmedia と Vulkan・Vulkan Video の Windows での使い方、既存の library（libtruetype・png・gif・jpeg の compat）の build、cross compile の toolchain、.exe・DLL・resource、名前 OmniGrammaton、試験の方法、Phase の分け方、ユーザーへの問い | in-progress（2026-11-01 00:24 P7 q1787） | — |

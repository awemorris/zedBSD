<!-- awesome-plan project=zedbsd record=ws334 -->

# WS334 libbrowser の Web ブラウザ OmniGrammaton を Windows に移植する（libkeiland の Win32 の実装、Windows 用の .exe）

- Master: [plan/master.md](../master.md)（2026-10-31 作成）。Primary Milestone: MG006。
- Status: incomplete（2026-11-01 01:26 p001 cleared、P7 が q1790 で p002 から実装）
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
  17. 2026-11-01 ユーザーが llvm-mingw を host の `~/opt/llvm-mingw-20261020-ucrt-ubuntu-22.04-x86_64`（clang 23.1.4、UCRT、x86_64・aarch64 の Windows の target）に入れた（U3。Q1 が使用を許可、共有の toolchain/ は変えない）。
  18. 2026-11-01 ユーザーから、信頼する証明書は userland/packages の ca-certificates を使い、その上で Windows の証明書の store も見る、という趣旨の指示を受けた。
  19. 2026-11-01 ユーザーから、Windows の試験の機械は QEMU の guest でもよい、という趣旨の指示と、Vulkan が使えるかの問いを受けた（Q1 の答えは design-notes.md）。
  20. 2026-11-01 ユーザーから、Windows の host での試験はだいぶ後に回し、host（Linux）の Wine と llvmpipe（Mesa の lavapipe、CPU の Vulkan）で開発する、効率が悪ければ Windows の host に移し、その場合は Q1 ではなく Windows の上の別の Claude の session で実装する、という趣旨の指示を受けた（U12 の答え。QEMU の Windows の guest は前の決定 19 で可）。
  21. 2026-11-01 ユーザーから、残りの問い U1・U2・U4〜U9・U11・U13〜U15 は推しの案でよい、という趣旨の答えを受けた（内容は [user-questions.md](user-questions.md) の推し: U1 Win32 を直に使う backend・最小の部分は WS334・全体は WS234、U2 UI の source は今の場所、U4 第 1 段は拡大しない、U5 cache の disk は第 2 段、U6 同梱の font、U7 libtruetype・libgif-compat の小さな直しだけ、U8 Windows の Chrome の名乗り、U9 名前は両方 OmniGrammaton・icon は今の物・発行者 Awe Morris、U11 第 1 段は zip、U13 公開の header は作らない、U14 第 1 段は平文・DPAPI は後半、U15 差し替えはストレージバックエンドだけ）。
  22. 2026-11-01 ユーザーから、今回のブラウザ（WS334）は標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)）を使う、という趣旨の指示を受けた。設計の担当（Opus、P7）が module・header・.c の骨格・試験の道具と実装の Phase の phase.md を作り、実装の担当（Sonnet 5.5 high）が module ごとに実装する。

## Resume point

- 2026-11-01 p001 の設計が cleared（ユーザーの答えが揃った）。次は p002 から（実装の担当の割当は Q1 がユーザーの指示で）。開発と試験は host の Wine＋llvmpipe（決定 20）。

## Phase

| ID | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計: libbrowser の 3 つのエンジン（レンダリング・スクリプト・ストレージ）への分割と interface（DOM の access、スクリプトの core と binding の分離）、libkeiland の Windows の backend（Win32 直か Qt6 か、window・入力・描画・IME・clipboard・file・thread・socket）、libmedia と Vulkan・Vulkan Video の Windows での使い方、既存の library（libtruetype・png・gif・jpeg の compat）の build、cross compile の toolchain、.exe・DLL・resource、名前 OmniGrammaton、試験の方法、Phase の分け方、ユーザーへの問い | cleared（2026-11-01 01:25、設計 main b2a7e8454、問い U1〜U15 は全部答えが出た） | — |
| p002 | build の土台（sources.mk の分け、`apps/browser-win32/Makefile`、`make browser-win64`・`browser-winarm64`）（design.md §8、約 6 LW） | in-progress（2026-11-01 01:26 P7 q1790）
| p003 | libbrowser の OS の層（posix・win32）と OS に直に触れる所の直し（design.md §8、約 9 LW） | planned | p002 |
| p004 | network の Windows（loader・http・tls）、libmedia の wake の callback（design.md §8、約 10 LW） | planned | p003 |
| p005 | instance と API v4、cookie・Web Storage を instance へ、storage の thread、ストレージバックエンド（design.md §8、約 12 LW） | planned | p004 |
| p006 | 永続の HTTP の cache（U5 の答えで第 2 段、後へ）（design.md §8、約 6 LW） | planned | p005 |
| p007 | script の core の archive と link の確かめ（design.md §8、約 2 LW） | planned | p003 |
| p008 | libmedia の Windows と mediafile（design.md §8、約 5 LW） | planned | p004 |
| p009 | browser の UI の準備（browser_main への分け、libkeiland の header の追加）（design.md §8、約 5 LW） | planned | p005 |
| p010 | 鍵の表 scan code → evdev（標準廉価実装手順 T1）（design.md §8、約 3 LW） | planned | p002 |
| p011 | libkeiland の Win32 の最小の部分その 1（app・window・loop・入力・枠）（design.md §8、約 14 LW） | planned | p007・p008・p009・p010 |
| p012 | その 2（題の帯、IMM32、clipboard、DPI）（design.md §8、約 12 LW） | planned | p011 |
| p013 | OpenSSL の Windows の build と証明書（packages の ca-certificates ＋ Windows の store）（design.md §8、約 5 LW） | planned | p004 |
| p014 | 配る形（zip、license、icon、版の情報）（design.md §8、約 5 LW） | planned | p012・p013 |
| p015 | Wine＋llvmpipe での確かめの第 1 回（決定 20。実機の Windows は後）（design.md §8、約 8 LW） | planned | p014 |
| p016 | 後半の Windows の口（TSF・drag と drop・touchpad・accent・Mica・DPAPI、arm64）（design.md §8、約 14 LW） | planned | p015 |
| p017 | libbrowser の試験を Windows（Wine）で（design.md §8、約 5 LW） | planned | p015 |
| p018 | 規約の全文の見直し（design.md §8、約 4 LW） | planned | p016・p017 |

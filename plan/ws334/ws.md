<!-- awesome-plan project=zedbsd record=ws334 -->

# WS334 libbrowser の Web ブラウザ OmniGrammaton を Windows に移植する（libkeiland の Win32 の実装、Windows 用の .exe）

- Master: [plan/master.md](../master.md)（2026-10-31 作成）。Primary Milestone: MG006。
- Status: incomplete（2026-11-01 02:43 設計の段 p002〜p004 cleared、基点 d393d3267。実装の第 1 群 p005・p009・p010・p013（Sonnet）と p020（Opus）を起動）
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
  22. 2026-11-01 ユーザーから、今回のブラウザ（WS334）は標準廉価実装手順（[docs/agent/spec-module-delegation.md](../../docs/agent/spec-module-delegation.md)）を使う、という趣旨の指示を受けた。設計の担当（Opus、P7）が module・header・.c の骨格・試験の道具と実装の Phase の phase.md を作り、実装の担当（Sonnet 5.5 high）が module ごとに実装する。 同日、設計をきちんと終えてから実装に入る Phase の組み方にする、という趣旨の指示も受けた（全部の module の header・骨格・試験の道具・実装の phase.md と design-reviewer が済むまで、実装の Phase を始めない）。

## Resume point

- 2026-11-01 01:30 決定 22（標準廉価実装手順）で Phase の表を組み直した: 設計の Phase p002〜p004 で全部の module の header・.c の骨格・試験の道具・実装の phase.md・design-reviewer を済ませ、
  p004 が cleared になるまで実装の Phase（p005 以降）を始めない。統合（Opus）の Phase の phase.md も設計の Phase で書き、実装の間に設計の判断を残さない（Q1 の条件）。module の Phase（Sonnet）は表の順が優先の順で、Q1 が 4〜6 本ずつ起動する（目安: 1 組目 p005・p009・p010・p013・p020、2 組目 p006・p007・p008・p011・p012・p014、3 組目 p015〜p019）。2026-11-01 P7 が q1790 で p002〜p004 を済ませた（基点 d393d3267、design-reviewer 34 件を反映、design.md §16）。次は Q1 の確認（p002〜p004 の cleared）の後、1 組目の実装の Phase（p005・p009・p010・p013 と glue の p020）。開発と試験は host の Wine＋llvmpipe（決定 20）。

## Phase

| ID | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計: libbrowser の 3 つのエンジン（レンダリング・スクリプト・ストレージ）への分割と interface（DOM の access、スクリプトの core と binding の分離）、libkeiland の Windows の backend（Win32 直か Qt6 か、window・入力・描画・IME・clipboard・file・thread・socket）、libmedia と Vulkan・Vulkan Video の Windows での使い方、既存の library（libtruetype・png・gif・jpeg の compat）の build、cross compile の toolchain、.exe・DLL・resource、名前 OmniGrammaton、試験の方法、Phase の分け方、ユーザーへの問い | cleared（2026-11-01 01:25、設計 main b2a7e8454、問い U1〜U15 は全部答えが出た） | — |
| [p002](phase002/phase.md) | 設計 A（libbrowser・libmedia・build）: module の分け、header（`base/os.h`・`os-win32.h`・`os-system.h`・`os-file.h`・`os-net.h`・`os-time.h`・`os-zones.h`、`storage/records.h`・`files.h`・`queue.h`、公開の `include/browser/storage.h`、`libmedia/media-os.h`）、.c の骨格、build の骨格（`apps/browser-win32/Makefile`・一番上の `browser-win64`・`browser-winarm64`、骨格の .c を compile）、試験の道具（`plan/ws334/tests/`: run-unit.sh・check-skeleton.sh・check.h。Windows の側は llvm-mingw で cross compile して Wine で流す）、実装の phase.md（p005〜p013・p020〜p027・p030） | cleared（2026-11-01 02:43 Q1 の判定、基点 d393d3267、main 5ea437eb7） | p001 |
| [p003](phase003/phase.md) | 設計 B（libkeiland の Win32・apps）: module の分け、header（`libkeiland/win32/keys-table.h`・`frame.h`・`wide.h`・`input.h`・`cf-html.h`、`platform/platform.h` の OS ごとの形、`apps/browser-win32/tools/ico.h`）、.c の骨格、試験の道具の追加、実装の phase.md（p014〜p019・p028・p029・p031〜p035） | cleared（2026-11-01 02:43 Q1 の判定、基点 d393d3267、main 5ea437eb7） | p002 |
| [p004](phase004/phase.md) | 設計の確定: design-reviewer の 1 本と指摘の反映（design.md に表）、header の確定の commit（= 基点の SHA、全 phase.md に記録）、骨格の照合の PASS | cleared（2026-11-01 02:43 Q1 の判定、基点 d393d3267、main 5ea437eb7） | p003 |
| [p005](phase005/phase.md) | module（Sonnet）: `base/os-win32.h/.c`（Win32 の誤り → errno、UTF-8 ↔ UTF-16）と `base/os-system.h` → `os-system-posix.c`・`os-system-win32.c`（揃えた確保・乱数・thread の名前・library の読み込み・名乗り・data の directory・根の証明書の列挙） | cleared（2026-11-01 03:12 Q1、main 888e552ea。test-os-system 220 posix・231 win32、test-os-win32 3126 win32、変異 26/27（Disallowed は Wine で確かめられず、実機で）） | p004 |
| [p006](phase006/phase.md) | module（Sonnet）: `base/os-file.h` → `os-file-posix.c`・`os-file-win32.c`（file の読み書き・置き換え・full path・作業の directory・directory の作りと一覧・file: の path ↔ URL・file の lock） | in-progress（2026-11-01 03:13 Sonnet で起動） | p005 |
| [p007](phase007/phase.md) | module（Sonnet）: `base/os-net.h` → `os-net-posix.c`・`os-net-win32.c`（socket・待ち・起こし） | in-progress（2026-11-01 03:27 Sonnet で起動） | p005 |
| [p008](phase008/phase.md) | module（Sonnet）: `base/os-time.h` → `os-time-posix.c`・`os-time-win32.c`（単調の時計・壁の時計・地域の時差・時刻帯の名前） | cleared（2026-11-01 03:40 Q1、main ff026d05a。os-time 実装、run-unit os-time posix 197・win32 743 PASS（Q1 再実行で確認）、Wine の TZ を変えた手動の DST 確認 PASS。header の comment の SystemTimeToTzSpecificLocalTimeEx は Wine で stub のため GetTimeZoneInformationForYear 経由にした（comment の直しは p035）） | p005・p009 |
| [p009](phase009/phase.md) | module（Sonnet、表の data）: `base/os-zones.h/.c`（Windows の時刻帯の key → 英語の長い名前、CLDR の data） | cleared（2026-11-01 02:53 Q1、main 70f0a3bf1。test-os-zones 1293 checks posix・win32、127 行の表、CLDR 48） | p004 |
| [p010](phase010/phase.md) | module（Sonnet）: `storage/records.h/.c`（key の作り・FNV-1a 64・衝突の番号、cookie・cache・LOCAL v1/v2 の記録の encode と decode） | cleared（2026-11-01 03:06 Q1、main 90b60dfc1。test-records 60948 checks posix・win32、変異 24/24） | p004 |
| [p011](phase011/phase.md) | module（Sonnet）: `storage/files.h/.c`（files の backend、公開の `browser_storage_calls` の実装） | planned | p006・p010 |
| [p012](phase012/phase.md) | module（Sonnet）: `storage/queue.h/.c`（instance の storage の thread と queue、write-behind と読みの頼み） | 実装済み・試験 PASS（2026-11-01、test-queue 13174 checks posix・win32、変異 21 通りのうち 20 検出・残り 1 は同値）。cleared は Q1 | p005・p010 |
| [p013](phase013/phase.md) | module（Sonnet）: `libmedia/media-os.h` → `media-os-posix.c`・`media-os-win32.c`（Vulkan の loader の読み込み、file の開き・大きさ・位置の読み） | cleared（2026-11-01 03:05 Q1、main 82365a39b。test-media-os posix 159・win32 158（LC_ALL=C.UTF-8）、変異 15/18） | p004 |
| [p014](phase014/phase.md) | module（Sonnet、表の data、旧 T1）: `libkeiland/win32/keys-table.h/.c`（scan code → evdev の番号） | cleared（2026-11-01 03:11 Q1、main b7a65df9a。test-keys-table 5010 checks posix・win32、152 行、変異 15/15） | p004 |
| [p015](phase015/phase.md) | module（Sonnet）: `libkeiland/win32/frame.h/.c`（custom frame の幾何: hit test・題の帯の button の配置・DPI の倍率） | cleared（2026-11-01 03:09 Q1、main a448d9343。test-frame 1079182 checks posix・1079798 win32、変異 約 22 と境目 2 を追加） | p004 |
| [p016](phase016/phase.md) | module（Sonnet）: `libkeiland/platform/platform-win32.c`（`platform.h` の起こし・lock・thread を Win32 で） | cleared（2026-11-01 03:30 Q1、main 174e9ed0c。test-platform-win32 313 checks win32、変異 23（hang と UBSan を含め全部見つかった）） | p004 |
| [p017](phase017/phase.md) | module（Sonnet）: `libkeiland/win32/wide.h/.c` と `input.h/.c`（UTF-16 ↔ UTF-8・surrogate の組、wheel の積み・pointer・修飾の鍵の写し） | cleared（2026-11-01 03:28 Q1、main 74d4174b2。test-wide 3001071・test-input 1008 checks posix・win32、変異 17/17） | p004 |
| [p018](phase018/phase.md) | module（Sonnet、旧 T2）: `libkeiland/win32/cf-html.h/.c`（CF_HTML の encode と decode） | cleared（2026-11-01 03:30 Q1、main 4d3237d17。test-cf-html 155072 checks posix・win32、変異 約 40（残り 2 は到達不能・未規定）） | p004 |
| [p019](phase019/phase.md) | module（Sonnet、旧 T3）: `apps/browser-win32/tools/ico.h/.c`（ICO の writer、PNG と BMP の entry） | cleared（2026-11-01 03:26 Q1、main 875c16efb。test-ico 39722 checks posix・win32、変異 26/27（残り 1 は同値）） | p004 |
| [p020](phase020/phase.md) | glue（Opus、旧 p002）: `sources.mk` への source の一覧の分け（libbrowser・libmedia・libtruetype・compat・libkeiland・browser の UI）、`apps/browser-win32/Makefile` の本体（library・DLL・.def・Vulkan の header の取得と遅延の import）、libtruetype `companion.c` の 1 関数と gif の `O_BINARY`、`cross-build.sh` | cleared（2026-11-01 03:15 Q1、main 29c250f65。sources-same PASS、zedBSD amd64・arm64 の 9 target と keiland-linux warning 0、cross-build PASS（Wine で遅延の import）、check-skeleton PASS） | p004 |
| [p021](phase021/phase.md) | 統合（Opus、旧 p003）: libbrowser の OS に触れる所（O13〜O24・O28〜O31・O34）を OS の層へ、`wb_memmem`、Makefile の posix・win32 の選び | planned | p005・p006・p008・p020 |
| [p022](phase022/phase.md) | 統合（Opus、旧 p004）: network の Windows（O1〜O10・O32・O33 を os-net へ）、libmedia の wake の callback と `net_loader_ring` | planned | p007・p021 |
| [p023](phase023/phase.md) | 統合（Opus、旧 p005）: instance と API v4、cookie の jar・Web Storage を instance へ、storage の module をつなぐ、利用側の v4 | planned | p010・p011・p012・p022 |
| [p024](phase024/phase.md) | 統合（Opus、旧 p006）: 永続の HTTP の cache（U5 で第 2 段、後へ） | planned | p023 |
| [p025](phase025/phase.md) | 統合（Opus、旧 p007）: script の core の archive と link の確かめ | planned | p021 |
| [p026](phase026/phase.md) | 統合（Opus、旧 p008）: libmedia の Windows（M2・M4・M7・M9 を media-os へ）と mediafile | planned | p013・p022 |
| [p027](phase027/phase.md) | 統合（Opus、旧 p009）: browser の UI の準備（`browser_main`、libkeiland の header の追加と Wayland の実装、present・CPU の道・鍵の text・時計） | planned | p023 |
| [p028](phase028/phase.md) | 統合（Opus、旧 p011）: libkeiland の Win32 その 1（共通の file の棚卸し、kl_app・kl_window・loop・入力・枠・暗い mode・Vulkan の surface・CPU の表示・stub）、`apps/browser-win32/` の入口・単一の起動・resource。Wine＋xvfb-run で画面を撮る | planned | p014・p015・p016・p017・p025・p026・p027 |
| [p029](phase029/phase.md) | 統合（Opus、旧 p012）: その 2（題の帯の controls、IMM32、clipboard、DPI） | planned | p018・p028 |
| [p030](phase030/phase.md) | 統合（Opus、旧 p013）: OpenSSL の Windows の build と根（packages の ca-certificates ＋ Windows の store） | planned | p005・p022 |
| [p031](phase031/phase.md) | 統合（Opus、旧 p014）: 配る形（zip、license、icon、版の情報） | planned | p019・p029・p030 |
| [p032](phase032/phase.md) | 試験（Opus、旧 p015）: Wine＋llvmpipe での確かめの第 1 回（決定 20） | planned | p031 |
| [p033](phase033/phase.md) | 統合（Opus、旧 p016）: 後半の Windows の口（TSF・drag と drop・touchpad・accent・Mica・DPAPI、arm64） | planned | p032 |
| [p034](phase034/phase.md) | 試験（Opus、旧 p017）: libbrowser の試験を Windows（Wine）で | planned | p032 |
| [p035](phase035/phase.md) | 規約の全文の見直し（旧 p018）と header の持ち越しの直し | planned | p033・p034 |

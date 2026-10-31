# Cockpit

ユーザーが監視する状況の一枚（2026-10-30 ユーザーの指示、[master.md](master.md) の「Cockpit の運用」）。書き手は Q1 だけ。
最終更新: 2026-11-01 05:07（Q1）

## 担当（P1〜P8）

**2026-10-31 16:42 host の再起動の前に全担当をラップアップした。** 再開の手順は [master.md](master.md) の「動いている担当と Queue」。

| 担当 | WS | WS の説明 | 今している事 | 作業の予定 |
| --- | --- | --- | --- | --- |
| P1 | — | — | 返却。BUG-384 を直した（main 98179a6a2、Pi 5 の実機で起動）。空き | — |
| P2 | — | — | ラップアップ（2026-11-01 06:53）。BUG-382 を直した（main ecad7fc74、5330 で確かめ） | — |
| P3 | [WS291](ws291/ws.md) | GPU の描画の速さ（RTX） | p024: NVK と段ごとに比べ、差 G1〜G4 を詰める（q1785-i03） | RTX の区切りの後に BUG-383（http の page から file: を読む）→ BUG-381 の残りの driver → RTX に戻る |
| P4 | — | — | 返却（2026-11-01 06:53）。main の計測: Pi 5 は前後で差なし（c74 約 50 fps、desktop 59〜67）、VF2 は c1 168・c13 56.0 fps（記録 141・52.5）、gpu-selftest 13/13 | — |
| P5 | — | — | 返却（2026-11-01 04:53）。WS336 の設計を仕上げた（推しの答え、TODO の一覧 T1〜T31、基点 d4ab182d2 のまま）。実装は Sonnet が p002 から | — |
| P6 | [WS335](ws335/ws.md) | 自前の libssl-compat・libcrypto-compat | 設計の仕上げ（q1789-i02）: 推しの答え、Windows は 1 つの .exe に static link、新しい基点 | 終われば Sonnet が p002 から実装 |
| P7 | [WS334](ws334/ws.md) | OmniGrammaton（libbrowser のブラウザ）の Windows 移植 | 設計 p002〜p004 cleared（基点 d393d3267）。module の p005〜p020 cleared（最新 p011 files、main c590d87fa） | P7 が p021（統合: OS に触れる所を OS の層へ）を実行中 |
| P8 | [WS074](ws074/ws.md) | Web ブラウザ（libbrowser）: Interop 2025 で Chromium が通る試験を全部通す | 自走の 14 回目（q1727-i14）: dev141 = 3,084（88.82%）、shape-outside の 16 件、main cee9d88c2 | 直交の flex の項目 → grid の baseline |

実装の担当（Sonnet・Opus の単発）: WS336 の p010・p012・p013（Sonnet、worktree s-ws336-pNNN）。p002 は cleared。p003 は WS335 の新しい基点を待つ。

## WS ごとの進捗

（2026-11-01 05:07 ユーザーの指示でクリア。これより後の進捗をここに書く。各 WS の状態は [master.md](master.md) と各 ws.md）

## open の Bug（今日見つかった物）

| Bug | 中身 | 状態 |
| --- | --- | --- |
| [BUG-373](bugs/BUG-373.md) | main の kernel の Wi-Fi の ioctl の検査の順の誤りで、古い networkd が EINVAL を受け network が上がらない | closed（VF2 の実機で確かめた） |
| [BUG-371](bugs/BUG-371.md) | Wi-Fi の初期化の失敗だけで init が有線を含む networking を飛ばす | closed（VF2 の実機で確かめた） |
| [BUG-372](bugs/BUG-372.md) | VF2 の serial の console が入力を受けない（元から未実装） | closed（VF2 の実機で確かめた） |
| [BUG-375](bugs/BUG-375.md) | direct scanout の後に window mode へ戻る時、compositor が EBUSY で終わる（VF2） | closed（P7: 原因は GPU の kernel の命令の buffer が満杯の時に待たずに失敗する事。待つ形に直し、VF2 で 8 回とも落ちず。main 2c5897f26） |
| [BUG-383](bugs/BUG-383.md) | libbrowser が http の page からも file: の副資源を読む（local の file の読み出し、安全） | open（WS074 の領域） |
| [BUG-382](bugs/BUG-382.md) | 5330 の NVMe の p7 を discard して起動した直後に mkfs が失敗（間欠） | open |
| [BUG-379](bugs/BUG-379.md) | Pi 4 の session の中で compositor が描かない。原因は kernel の fork の livelock | closed（Pi 4 の session で描く） |
| [BUG-380](bugs/BUG-380.md) | Pi 4 で mview（3D）の pipeline が作れない（V3D の executor） | open |
| [BUG-378](bugs/BUG-378.md) | arm64 の kernel の text が 2 MiB を越えると起動の途中で止まる（Pi 4、全 arm64 の board に効く） | closed（実機の Pi 4 で起動） |
| [BUG-374](bugs/BUG-374.md) | RTX の mouse の cursor の色が他の GPU と逆 | 診断の kernel を用意済み（矢印の色で原因が分かれる）。ユーザーの目視待ち（下の「拾える試験」） |
| [BUG-370](bugs/BUG-370.md) | Photos で保存すると全面表示が閉じる | closed（QEMU で確かめた） |
| [BUG-376](bugs/BUG-376.md) | PS/2 の mouse が無くても aux の node を作り、open のたびに約 80 ms 待つ | closed（QEMUと実機 で確かめた） |
| [BUG-222](bugs/BUG-222.md) | ue0 の SCP が約 10 Mbps（10-06 の UAT） | closed（10.0.10.3 の ue0 で 2500 Mbps、scp 62〜86 MB/s） |

## ユーザーの判断を待つ事

disk の空きの件（下の 00）だけが作業を遅らせている。

### 00. host の disk の一括の片付け（2026-10-31 02:3x、急ぎ）

- **済み（2026-10-31 14:39）**: 同日 ユーザーから、古い worktree を消してよい、という趣旨の指示を受けた。今動いている担当・午後の試験・VF2 と RTX の続き・git に無い記録を持つ 16 個を残し、211 個を `git worktree remove` で消した（branch は全部残す）。host の / の空きは 4.7 GB → 499 GB。

- 背景: host の / が満杯になり（938G 中 891G、空き 0）、P1・P4・P8・P3・P6 の build が「No space left on device」で落ちた。担当が頼んだ一時物を Q1 が消したが、新しい担当の sysroot の写し（1 つ約 1.7 GB）などで 04:3x に空きは約 3.8 GB（危ない）。担当には出力を全部 /tmp に置かせ、消せる物を挙げさせている。大きいのは古い担当の worktree（`/home/awe/zedBSD-worktrees/` に約 250 個、合わせて約 559 GB）の `build/`（試験の image・kernel・sysroot の写し）。
- Q1 がしようとした事: 今使っていない worktree（返却済み・merge 済みの担当の物）の `build/` の中身を消す（`build/review` の PNG は残す、今動いている担当の worktree と WS313 の ODROID の image は残す、git の中身と branch は消さない）。**権限の判定（取り返せない削除）で止められた**ので、回避せずユーザーに聞く。
- 決めてほしい事: この片付けをしてよいか。よければ settings に許可を足すか、ユーザーが `! ` で直に走らせる。それまで担当は大きな image を /tmp（tmpfs、空き約 69G）に作る。

### 01. AMD の CPU の仕様書の取得（2026-10-31 04:5x、権限の判定で止められた）

- **済み（2026-10-31 14:50）**: ユーザーが 2 つの PDF を置いた。Q1 が `/home/awe/zedBSD-specs/24594_APM_v3-r3.02.pdf`・`ACPI_Spec_6_5_Aug29.pdf` に写した（git の外）。「仕様未取得」で外した部分（WS333 p008・p010 の AMD と _PSS など、WS310 の _OSC の確かめ）は、次にその WS を進める時に入れ直す。

- 2026-10-31 ユーザーから URL を問われ、Q1 が答えた（AMD 24594 の PDF、ACPI 6.5 の PDF と第 8 章の HTML。URL は Q1 の知識からで記録に無かった）。置き場所は git の外の `/home/awe/zedBSD-specs/`。ユーザーが置くのを待つ。

- 背景: WS333 p007（x86 の CPU の register を読む module の設計）で、P7 が AMD の公開の仕様書 APM Vol 3（24594、CPUID の HwPstate・CPB の bit の位置の出典）を amd.com から取ろうとした。1 回目は bot の確かめの page が返り、2 回目に browser の header を付けた取得が**権限の判定（bot の確かめの回避）で止められた**。P7 は回避せず止め、AMD の取得はしない。
- 影響: AMD の CPU の P-state の bit を仕様で裏付けられないので、p008 の範囲から外し「仕様未取得」と記録して進める。5330（Intel）には影響しない。Intel の SDM は取得済み。
- 同じ形でもう 1 件: ACPI 6.5 の第 8 章（_PSS・_PSD・_PPC、firmware が示す P-state の表の形の出典）を uefi.org から取ったら Cloudflare の challenge の page が返り、P7 は再試行せず止めた。p010 から _PSS などの解析を「仕様未取得」として外す。06:3x に WS310（_OSC の設計、P6）でも同じ ACPI 6.5 の PDF の取得が bot の確かめで 403 になり、回避せず「仕様で未確認」の印で進めている。
- 決めてほしい事: ユーザーが AMD APM Vol 3 と ACPI 6.5 の PDF を手で置くか（置き場所は Q1 が決める、git の外）、その 2 つの部分は後回しにするか。

### 02. 共有の /tmp の試験の作業 directory の片付け（2026-10-31 05:1x、権限の判定で止められた）

- **済み（2026-10-31 14:39）**: 同日 ユーザーから、消してよい、という趣旨の指示を受けた。`/tmp/boot-test.*` 300 個と担当が挙げた出力 10 個を消した。返却済みの P1 の WS299 の QEMU 2 つ（9 時間余り動いていた）も、ユーザーの許可を受けて止めた。

- 背景: plan/tools/boot-test.sh は /tmp/boot-test.* に作業の directory を作る。夜の間に QEMU の起動の試験を多く流したので溜まっている。Q1 が QEMU の使っていない物だけを消そうとしたら、権限の判定（共有の scratch の一括の削除）で止められた。回避していない。
- 影響: /tmp（RAM の上）の空きは今約 56 GB あり、急ぎではない。
- 06:5x にも、担当が消してよいと挙げた /tmp の試験の出力（/tmp/ws326-speed.run.*・/tmp/p1-speed-*.run.*・/tmp/p1-stage・/tmp/p1-video-check、scratchpad の interp）を wildcard で消そうとして、同じ判定で止められた。
- 決めてほしい事: 00 の worktree の片付けと合わせて、許可を足すか、ユーザーが `! sh plan/tools/q1-clean.sh --tmp` を走らせるか。

### 0. WS333（/dev/thermal で統一する電力・熱の枠組み）の設計の問い Q333-1〜7（2026-10-31 新しい）

- 背景: WS333 は、CPU の休み方（idle）と速さ（P-state）、温度・fan・方針（strategy）・危ない温度での電源断を、1 つの device `/dev/thermal` と道具 `/sbin/thermal` にまとめる WS（WS301・WS303 を統合）。P5 が設計（p001）を終え、敵対的レビュー（major 8・minor 24）を反映した。先に入れた p002（MWAIT）で 5330 の暇な時の電力は 13.00 → 11.70 W（Linux は 3.2〜3.5 W）。
- 問いの全文・用語の説明・推しの理由は [ws333/user-questions.md](ws333/user-questions.md)。要点と推し:
  - Q333-1 起動の時の strategy（速さ優先か省電力かの方針）: **推し B**＝最後に選んだ物を /etc に保存して起動の時に渡す、保存が無ければ balanced。（A は firmware のまま、C は kernel が毎回 balanced）
  - Q333-2 「細かく電源を切る」軸が何に効くか: **推し C**＝保存と表示だけ、効かせる先（PCIe・NVMe・GPU）は各 WS が足す。
  - Q333-3 root が CPU の周波数を直に書いた時: **推し A**＝直の値が優先して「custom」と表示、次に strategy を選ぶと消える。
  - Q333-4 manual の 3 つの軸（速さ・騒音・電源断）の 0〜100 の向き: **推し A**＝どれも「100 = その物が多い」（騒音 100 = 一番うるさくてよい）。**UAPI の意味になるので後で直しにくい**。
  - Q333-5 Dell の BIOS の Quiet（fan を遅くして CPU の電力を下げる mode）を選んでよいか: **推し A**＝manual で騒音を低く（≤ 33）した時だけ。
  - Q333-6 電力（RAPL、CPU の電力の積算の計数）と CPU の休みの深さの割合を /dev/thermal で読めるようにするか: **推し A**＝入れる（読みだけ）。
  - Q333-7 CPU の速さの制御（`CONFIG_CPU_PERF`、今は既定で無効）の既定をいつ有効にするか: **推し A**＝5330 の受け入れの試験（p014）が通ったら amd64 の既定で有効（server の設定は変えない）。
- 追加の問い（Q1 から）: 起動の時に非常の停止を止める switch（`thermal.emergency=off`、診断用）を入れてよいか。ユーザーの決定（危ない温度で止める）を止める物なので、答えまで作らない。
- 止めている物: 無し。Q1 は推しで進める（CPU の道 p003→p006→p011→p014 は問いを待たない）。Q333-4 だけは UAPI を実装する Phase の前に答えがほしい。

### 0b. WS306（GPU の「ゲーム最適化」の UAPI と driver の識別）の設計の問い Q306-7・Q306-9（2026-10-31 新しい）

- 背景: WS306 は、ゲームが環境変数（`VULKAN_ZEDBSD_GAME` など）で GPU の driver の最適化の ON/OFF と個別の option を選べるようにし、GPU の名前と driver の版を取れるようにする WS。option の一覧は vulkaninfo（Khronos の標準の道具、移植する）に出す。P5 が設計の第 3 版を書き、敵対的レビュー（major 7）を反映した（main 843847185）。実装は p002〜p011、約 54 LW。全文は [ws306/user-questions.md](ws306/user-questions.md)。
- 用語: NGG は AMD RDNA の新しい形の頂点の処理の段。culling は見えない三角形を早く捨てる事。boot の行は起動の設定に書く kernel への指定。
- Q306-7 boot の行 `amdrdna.vk.ngg` を残すか: **推し (a) 診断用に残す**（既定 0、desktop には入れない、書いた時だけ効く）。(b) 消す。
- Q306-9 WS291（GPU の速さ）の RDNA の段の完了を待たずに、枠組みの Phase（UAPI・kernel の核・各 driver の名前と版・libvulkan・vulkaninfo・文書）を先に進めるか: **推し (a) 進める**。NGG を option に移す p007 と、その文書の p010 だけ RDNA の完了を待つ。費用は、kernel の GPU の共通の file（gpu.c）を他の WS と同じ時期に変えるので merge の衝突が出うる事（Q1 が merge の順で抑える）。(b) 決定のとおり、全部を RDNA の完了の後に。
- 止めている物: WS306 の実装（Q306-9 の答えまで始めない。元の決定が「WS291 の後」のため）。

### 0c. WS318（libmedia の作り直し）の設計の問い Q318-1〜14（2026-10-31 新しい）

- 背景: WS318 は、libmedia を Keiland の app の唯一の動画・音声・camera の入り口にする WS。app の Vulkan の device を共有し、codec を問い合わせて選び、encoder と camera を足し、libavcodec は libmedia の中の最後の予備にする。P5 が設計（public の header 2 つの案、Phase p001〜p028）を書き、敵対的レビュー（blocker 3・major 20）を反映した（main 85fbeb091）。HAL は変えない。全文・用語・推しは [ws318/user-questions.md](ws318/user-questions.md)。
- 用語: Vulkan Video は GPU の動画の decode・encode の標準の口。vkfake は試験用の偽の Vulkan の device。MJPEG は camera がよく出す JPEG の連続の形式。B-frame は前後の frame を参照する frame。
- 問いの要点（どれも推しが付いている）:
  - Q318-1 hardware の encode と H.265 の decode は、WS318 では vkfake で確かめ、実機の確かめは GPU ごとの driver の WS（WS319〜）の受け入れにする（推し）。
  - Q318-2 zedBSD の libavcodec の package は encoder を無効のまま。libavcodec の encode の予備は、encoder 付きの libavcodec の機械でだけ働く（推し）。
  - **Q318-3 Vulkan の header の更新**: 今は 1.3.269 で、KHR の encode が無い。1.3.274 以降の tag に固定して更新したい。共有の sysroot の作り直し（toolchain の扱い）が要る。
  - Q318-4 MJPEG の camera は Huffman を CPU、IDCT と色の変換を GPU の描画で（推し）。
  - Q318-5 AAC-LC の encoder に、短い窓の切り替えと TNS を足す（推し）。
  - Q318-6 camera の実機の Phase（p027）だけが WS282（UVC）を待つ（推し）。
  - Q318-7 MP4 の書き出し（録画の file）は範囲の外（推し）。
  - Q318-8 H.265 は Main profile だけ（推し）。
  - Q318-9 hardware の encode は IDR と P から、B-frame 無し（推し）。
  - Q318-10 i915 の driver の小さな変更（decode の出力を sample できる形に）は WS319 で（推し）。
  - Q318-11 browser の `<video>` は libavcodec を使わない（network から来た信頼できない入力を同じ process で decode するため）（推し）。
  - **Q318-12** 2026-10-12 の決定（毎 frame の CPU の複写をしない）の例外として、software の decoder の出力を GPU の memory へ、import の無い機械の camera の frame の、2 つの複写を認めるか（推し: 認める）。
  - Q318-13 player の文言で「libavcodec が無い」を先に出す（推し）。
  - **Q318-14** WS326 の完了を待たずに、WS326 に触れない Phase（p002・p003a・p003b・p011a・p011b・p016〜p019）を先に始めるか（推し: 始める）。
- 止めている物: WS318 の実装（設計の受け入れと Q318-14 の答えまで始めない）。

### 0d. WS329（libmedia の H.264 の software の encoder）の設計の問い Q329-1〜5（2026-10-31 新しい）

- 背景: WS329 は、libmedia に既存の library を使わない H.264 の encoder を作る WS（標準廉価実装手順）。P5 が設計（header の案 26、Phase p002〜p026）を書き、敵対的レビュー（major 10）を反映した（main 455a7d5b4）。encoder は decoder（WS326）の部品と再構成をそのまま使うので、encoder と decoder の画がずれない。実装は WS326 の完了の後。全文は [ws329/user-questions.md](ws329/user-questions.md)。
- 用語: BD-rate は同じ画質に要る bit の量の差（+40% は x264 より 4 割多く要る）。preset は速さと画質の釣り合いの段。x264 veryfast は広く使われる速い設定。
- Q329-1 画質の目標: **推し A** 釣り合いの preset で x264 veryfast に対し BD-rate +40% 以内、速い preset で +80% 以内。
- Q329-2 速さの目標: **推し A** 5330 で速い preset の 720p30 を実時間に（slice の 2〜4 thread）。1080p30 は目標にしない（SIMD を使わないため）。
- Q329-3 B-frame: **推し A** 入れる（Main・High、参照にしない B を最大 3 枚、既定は 0）。
- Q329-4 WS318 と並べて進める: **推し A** WS326 の完了の後に p002〜p023 を WS318 と並べ、p024 は WS318 p012a、p025 は WS318 p008 を待つ。
- Q329-5 libmedia の encoder の設定に速さの欄を足す: **推し A** WS318 の media_encoder_config に `speed` を 1 つ足す。
- 止めている物: 無し（実装は WS326 の完了待ち）。

### 0e. WS326（H.264 の software の decoder）の速さの続きをどこで行うか（2026-10-31 新しい）

- 背景: WS326 の decoder は正しく decode できる（JVT の適合で範囲の中の 101 本が全 frame 一致）。今の速さは host の 1 thread で 1080p が 12〜19 fps（ffmpeg の C だけで 88 fps）。重いのは補間（30〜35%）と deblocking（22〜27%）。P1 の p024 の設計は、段 A（C の書き直しで 1 thread 3 倍）→ 段 B（thread）→ 段 C（SIMD）で、目標は Pi 5 で 1080p30 High（4 thread で 40 fps 以上）。
- Q1 の進め方: WS326 は p024 の最初の最適化（補間の block 化）と p025（規約の見直し）で閉じる（昼までに終える指示）。
- 決めてほしい事: 速さの続き（deblock・weight の書き直し、neighbour・transform、thread、SIMD、Pi 5・5330 の実測）を **(a) 新しい WS にする（推し）** か、(b) WS326 の中に Phase を足して完了を遅らせるか。

### 0f. WS330（libmedia の H.265 の software の decoder と encoder）の設計の問い Q330-1〜4（2026-10-31 新しい）

- 背景: WS330 は、H.265 の decoder と encoder を既存の library を使わずに作る WS（実装は WS329 の後、ユーザーの決定）。P5 が設計（header の案 38、Phase p002〜p049）を書き、敵対的レビュー（blocker 1・major 11）を反映した（main 299a2f4b3）。H.264 と共通の部品（bit の読み・NAL・CABAC・frame・DPB）は `vcodec-` の名前で共有する。全文は [ws330/user-questions.md](ws330/user-questions.md)。
- 用語: Main は 8 bit 4:2:0 の基本の profile、Main 10 は 10 bit（HDR など）。WPP・tile は 1 枚の画を並列に decode できる分け方。
- Q330-1 範囲: **推し A** Main（8 bit）だけ。10 bit は libavcodec に回し、WS318 が 10 bit の出力を持った後に別の WS で。
- Q330-2 H.265 の解析を WS318（GPU の Vulkan Video の道）と共有するか: **推し A** 1 つにする（WS318 p016 がこの設計の header を採る）。
- Q330-3 速さと画質の目標: **推し A** decoder は 5330 で 1080p30（並列にできない stream は 720p30）、Pi 5 は 720p30。encoder は 5330 で速い preset。SIMD は範囲の外。
- Q330-4 decoder の部分を早く始めるか: **推し A** 決定どおり WS329 の完了の後（共通化を 1 回で済ませる）。
- 止めている物: 無し（実装は WS329 の後）。

### 0g. BUG-339（VF2 で U-Boot の control の DTB を渡すと起動が VFS で止まる）の恒久の直し（2026-10-31 新しい）

- 背景: VF2 の U-Boot が持つ DTB（control の DTB）は vendor の書き方（binding）で、zedBSD の VF2 の driver は全部 Linux の mainline の書き方を読む。そのため control の DTB で起動すると SoC の部品が見つからず止まる。今は ESP に置いた mainline の vf2.dtb を渡す道で毎回起動しているので、普段の使い方には影響しない（P2 が実機で確かめ、log に「ESP の dtb を渡す」案内を 1 行足した）。
- 用語: DTB は機械の部品の一覧を書いた表。U-Boot は VF2 の最初に動く起動の program。HAL の handoff は loader が kernel に渡す情報の約束（変えるには差分ごとのユーザーの承認が要る）。
- 案（どれも判断が要るので P2 は着手していない）:
  - (1) loader が ESP の DTB を別の板の表として渡す。HAL の handoff に key を足すので、ユーザーの事前承認が要る。
  - (2) kernel に mainline の DTB を埋め込む。base system に外部の物を入れない方針に触れる。
  - (3) loader を差し替える。OpenSBI の reserved-memory と MAC の address を失う危険がある。
  - (4) 今のまま（ESP の vf2.dtb の道だけを支え、control の DTB は案内の log で止める）。Q1 の推し（他の案の費用と危険に見合う利点が無い）。
- 決めてほしい事: どの案にするか。

### 0h. WS302（デスクトップのスレッドバランサ DTB）の設計の問い Q302-1〜7（2026-10-31 新しい）

- 背景: WS302 は、kernel が thread の振る舞い（GPU を使う・入力に応える・長く計算する）と nice から、速い P-core と省電力の E-core への割り付けを scheduler に提案する仕組み（CONFIG_DTB、既定 off、ユーザーの決定）。WS333 の CPU の口が入ったので、P5 が設計（約 22 LW、Phase p002〜p007）を書き、敵対的レビュー（blocker 2・major 11）を反映した（main 896d3e3db）。HAL は変えない。全文は [ws302/user-questions.md](ws302/user-questions.md)。
- 用語: P-core は速い core、E-core は省電力の core（5330 は P 2・E 8）。SMT は 1 つの core を 2 つの CPU に見せる仕組み。starvation guard は優先度の低い thread が長く走れない事を防ぐ見張り。
- Q302-1 規則の順: **推し A** nice → 長い計算（E-core）→ 画面を出す process（compositor）→ GPU の app → terminal → その他。（E は terminal を長い計算より先にする案。A は compiler を terminal の規則に含めない読み）
- Q302-2 nice 0 の長い計算が空いた P-core にあふれてよいか: **推し A** よい（nice > 0 はあふれない）。
- Q302-3 優先度の段（user の既定 10、compositor 8、GPU の app 9、nice を 8/9/11/12 に）と 100 ms の見張りを DTB が有効な時だけ: **推し A**。（B は上げない）
- Q302-4 compositor から focus の hint の口: **推し B** v1 には入れない（5330 の数で要れば条件つきの p005）。
- Q302-5 統計は CONFIG_DTB=y の build だけで集める: **推し A**。
- Q302-6 観察の口（読みだけの sysctl `kern.dtb`）: **推し A**。
- Q302-7 受け入れの数: **推し** design.md §6.4 の表（A1〜A7）。
- 止めている物: WS302 の実装（p002 は Q302-5、p003 は Q302-2・3、p004 は Q302-1・6 の答えの後）。

### 0i. WS310（ACPI の _OSC を呼び、システム全体の挙動を厳重に確かめる）の設計の問い Q310-1〜6（2026-10-31 新しい）

- 背景: _OSC は OS が firmware に「この機能は OS が扱う」と伝える ACPI の method。WS310 は、ユーザーの決定（最終的には呼ぶが、別の WS で厳重に確かめる）の WS。P6 が設計（Phase p002〜p009）を書き、敵対的レビュー（major 12）を反映した（main a2aa89c4f）。HAL は変えない。全文は [ws310/user-questions.md](ws310/user-questions.md)。
- 5330 の表で分かった危ない所: PCIe の _OSC は「問い合わせ」だけでも firmware が設定を書き、root port の hot-plug・PME の割り込みが止まり得る（試験の道具で再現）。platform の _OSC の確定は Thunderbolt の制御の切り替えと、電源を切っても残る CMOS への書きの道がある。
- 用語: CMOS は電源を切っても残る小さな設定の記憶。PME は device が電源の復帰を知らせる仕組み。CPPC は OS が CPU の性能を指示する仕組み。
- Q310-1 確かめが通った後、既定で呼ぶか: **推し A** 受け入れが通れば既定 on（`acpi.osc=off` で戻せる）。
- Q310-2 5330 で platform の _OSC が Thunderbolt の制御と CMOS に触れる時: **推し A** 先に値を読み、切り替えも CMOS の書きも無い時だけ呼ぶ（起きると分かったら改めて聞く）。
- Q310-3 processor の _OSC で申告する物: **推し A** 0（何も求めない）。
- Q310-4 PCIe で OS が得る制御の範囲: **推し A** 今は 0。WS300 の省電力の code と一緒に PCIe Capability と LTR だけ。
- Q310-5 一晩の耐久試験: **推し A** 5330 の比べの後に 1 晩。
- Q310-6 ACPI 6.5 の仕様の入手: 上の 01 と同じ件（Linux の定数で実装は止まらない）。
- 止めている物: WS310 の実装（p002 以降、答えと 5330 の割当の後）。

### 0j. WS291 p014（RTX の描画の速さ）の subs-1000 をどう進めるか（2026-10-31 新しい）

- **決まった（2026-10-31 14:39）**: ユーザーから、(b) で進める、という趣旨の指示を受けた。あわせて、image の並びは普通は client が求めるが、そこをどう扱うかを問われた（Q1 の答え: swapchain の image の並びは WSI と driver が決め、読む相手の都合で compositor が読める並びを知らせる形にする。設計で確かめる）。[ws291-p023](ws291/phase023/phase.md) を新設し、P3 が設計（q1781）。

- 背景: RTX 2070 の big-1000 は今夜 67 → 116 fps（NVK 136）まで上がった。subs-1000（1 frame に submit が 1000 回）は 11 fps のまま（NVK 75）。P3 の診断で、zedBSD は GPU の 1 回の実行に約 60 µs（うち pass の終わりの全画面の copy 約 29 µs）かかり、NVK は約 5〜7 µs と分かった。
- copy を省くには、次のどちらかが要る（どちらも範囲が広い）:
  - (a) libvulkan が「誰も見ない私用の fence」を kernel に伝える形に、libvulkan と全 GPU の driver の protocol を変える。
  - (b) 画面に出す image を block-linear（GPU の得意な並び）にする（p013 の (B) の案、WSI の変更）。
- 決めてほしい事: (a)・(b) のどちらかで進めるか、subs-1000 は今の値で止めるか。Q1 の推しは (b)（copy 自体が要らなくなり、desktop の他の GPU にも効く見込み）。

### 0k. WS291 p023（共有の image の画素の並び、RTX の block-linear）の問い Q1・Q2（2026-10-31 15:24 新しい）

- **2 回目の目視（2026-10-31 18:39）**: 正しい（ユーザー: desktop が正常）。P3 の直し effb3d149（block-linear の kind の context DMA）の後。既定を 1 にして計測し直す（P2）。

- **目視の結果（2026-10-31 18:14）**: 誤り。表示はされるが block 単位で乱れる（ユーザーの写真 build/review/ws291/p023-bl-visual-1.jpg）。既定は 0 のまま、P3 が直す（q1781-i03）。直るまで RTX は image を貸さず window mode で合成（P2 の規則）。

- **変わった（2026-10-31 17:42）**: ユーザーの指示で、block-linear の画面出しは目視の前に既定で有効にする。目視は拾える試験として残す。

- **決まった（2026-10-31 15:39）**: Q1 は (b) 目視（CRC は作らない。block-linear の画面出しはユーザーが見るまで既定で切る、確かめの手順は P3 が phase.md に書き「拾える試験」へ）。Q2 は (b) 続けて内訳を測る（[p024](ws291/phase024/phase.md) を新設）。

- 問いの本文: [user-questions.md](ws291/phase023/user-questions.md)。どちらも実装を止めない。
- Q1: RTX の display が block-linear を正しく出したかの確かめ（(a) 推し: display の CRC を読む仕組みを足す、約 3 LW／(b) 目視を拾える試験に、確かめまで既定で切る）。
- Q2: subs-1000 を NVK の 75 fps に近づける続き（(a) 推し: 実装の結果を見てから別の Phase で決める／(b) 続けて内訳を測る Phase を足す／(c) 約 16 fps で止める）。

### 0l. WS334（OmniGrammaton の Windows 移植）の設計の問い U1〜U15（2026-11-01 01:12 新しい）

- **全部決まった（2026-11-01 01:25）**: U3・U10・U12 は決定 17・16・20、残りは推しの案（決定 21）。p001 は cleared。実装は p002 から（担当の割当はユーザーの指示で）。

- 問いの本文: [ws334/user-questions.md](ws334/user-questions.md)（前置き・用語の表つき）。設計は [ws334/design.md](ws334/design.md)。どれも推しの案があり、推しのままなら「推しで」。
- U3（toolchain の llvm-mingw と Debian の gcc-mingw-w64-ucrt64 の導入）は Q1 の許可も要る（ユーザーが推しでよければ Q1 が許可する）。

### 0m. WS291 p024（RTX の描画の速さ）の続きの進め方（2026-11-01 02:19、2026-11-01 04:45 答え: profile で NVK より遅い所を直し目標まで、(a) は本筋なら、(c) は他の driver に副作用が無ければ、(b) は説明を求められた）

- 背景: RTX の subs-1000 は 43 fps（目標 NVK 75.4）、big-1000 は 119 fps（目標 136.2）。submit 1 回 15.8 µs の中に 3.5 µs を越える項目が無く、小さな削りでは半分にならない。big-1000 は app の側の命令の書き出し 3.5 ms（GPU は空く）が残り。
- 残る道（P3 の案）: (a) kernel の VM の小さな copyin・copyout の速い道（WS291 の外、kernel の VM）、(b) 命令の記録の時に push buffer を組み、submit では ring に載せるだけにする（大きな作り変え、設計の Phase が要る）、(c) fence の無い submit に job を作らない（製品の判断、前から問い中）。
- 決めてほしい事: 続けるか、どの道か。

### 0n. WS336（自前の sshd・ssh）の設計の問い Q1〜Q9（2026-11-01 03:03、2026-11-01 04:45 答え: 推し、TODO の一覧を WS に）

- 問いの本文: [ws336/user-questions.md](ws336/user-questions.md)（前置き・用語つき）。推し: Q1 password の login を入れる、Q2 port の転送は入れない、Q3 sftp-server だけ（scp -O を使う道具 4 本は -O を外す）、Q4 agent は入れない、Q5 OpenSSH と同じ深さの権限の分離、Q6 QEMU の相互運用の試験が通ったら既定を替える（OpenSSH の package は残す）、Q7 鍵の交換は mlkem768x25519-sha256 だけ、Q8 chacha20-poly1305 だけ、Q9 passphrase の無い鍵だけ。

### 0o. WS335（libssl-compat・libcrypto-compat）の設計の問い U1〜U6（2026-11-01 03:04、2026-11-01 04:45 答え: 推し、Windows は static link）

- 問いの本文: [ws335/user-questions.md](ws335/user-questions.md)。推し（どれも A）: U1 範囲の外の一覧、U2 net/tls.c の切り替えは -DWB_TLS_COMPAT（dlopen の名前だけ、p036 の案）、U3 ssh の鍵の交換 mlkem768x25519-sha256、U4 名前と置き場所、U5 Windows の ROOT の store から Disallowed を除き、DLL の横の cert.pem を足す、U6 証明書の CN への fallback をしない。推しでない答えは header の変更（新しい基点）になる。

### 1. WS299 で見つかった、5330 の NVMe 以外の課題

- 背景: 2026-10-28 の P1 の報告（[ws299/ws.md](ws299/ws.md) の「結果（2026-10-28 21:50）」）。idle と HWP は WS333 に入れた。WS299 そのものは、5330 が空いたら担当を付ける（2026-10-31 の指示、q1744）。
- 残り 3 つ:
  - 時刻の読み（clock_gettime）が 1 ms 刻み。vDSO（kernel に入らずに時刻を読む仕組み）が無いため。
  - program が main から return した時に、stdout の buffer が書き出されない（libc の不具合）。
  - libc の memcpy・memset の速さの続き（大きな copy、build の flag -fno-builtin -Os）。
- 決めてほしい事: この 3 つを Bug にするか、WS にするか。

### 2. P4 が 10-27 の記録の監査（q1660）の時に拾った候補（詳しくした物）

- 背景: 10-27 に P4 が、完了した WS の記録を片付ける作業（記録の監査）の途中で、判断が要りそうな物を一覧にした。記録が短く、Q1 が調べ直した結果を書く。
- (a) WS162（FIDO2 の login）を canceled にするか → 調べ直すと、WS162 は 2026-10-27 に「WS172・WS199 で達成」として completed になっていた。**判断は要らない（解決済み）**。
- (b) WS279 p009（Pi 5 の GPU の license と類似性の監査）の担当 → WS279 は完了したが、p009（Linux・Mesa の code を参考にした所が、license の上で問題無いかと、写しになっていないかの監査）は未実施のまま閉じた。誰がいつ行うかが未決。
- (c) T1-691（試験の担当 T1 の 691 番の試験）で見た「P4m の時々の失敗」を Bug にするか → 記録に中身が残っておらず、何の失敗かが今は分からない。Q1 が history を調べるか、取り下げるか。
- (d) `nvrtx.disp.own` の既定 → RTX の driver の起動の parameter で、1 にすると driver が画面を受け取った時に console ではなく driver 自身の画面をすぐに出す。既定を 0 のままにするか 1 にするか。
- (e) `plan/ws004/tests/pci-msi-qemu.c` を残すか → QEMU の EDU という試験用の PCI の device で MSI の割り込みが届くかを見る kernel の試験の code。これを有効にする唯一の config（CONFIG_KERNEL_TEST_CHECKPOINTS）が消えたので、今は build されない。残す（config を作り直す）か、消すか。
- (f) master-design-policy.md へ移す候補 約 25 件 → 各 WS の ws.md に書かれたユーザーの決定のうち、WS をまたいで効く物（設計の方針）を、全体の方針の文書に写すかどうか。一覧は Q1 が作り直して改めて聞く。

### 3. UEFI の loader の GOP の mode（画面の解像度）の選び方

- 背景: UEFI の firmware は、起動の時の画面の mode（解像度と pixel の形）を GOP（Graphics Output Protocol）という口で出す。zedBSD の UEFI の loader（`bootloader/uefi/video.c`）は、kernel に渡す前に mode を選ぶ。今の動きは次の 3 段。
  1. 起動の設定（zedbsd.cfg）に `video=幅x高さ` があれば、その大きさの mode にする。
  2. 無ければ、loader が持つ「好みの大きさ」（今は 1920x1080、起動の splash の大きさ）を firmware が持っていればそれにする。
  3. 持っていない、または設定に失敗した時は、firmware が起動の時に選んでいた mode のまま。
- 問題になりうる所: 画面の本来の解像度（monitor の EDID が示す最適な大きさ）が 1920x1080 でない時（4K の monitor、1366x768 の laptop の panel など）、2 で 1920x1080 にすると、画面がぼやける・余白が出る。逆に firmware の既定が小さい（800x600 など）こともある。amd64 の起動の設定（zedbsd.cfg）には `video=640x480` の行がある（WS009 で、BIOS の起動の時に kernel が「知らない parameter」と log に出す件が見つかった）。
- 案:
  - (a) 今のまま（1920x1080 を好み、無ければ firmware のまま）。
  - (b) monitor の最適な大きさ（GOP の EDID の口、EFI_EDID_ACTIVE_PROTOCOL で読める時）を第一にし、読めなければ (a)。
  - (c) firmware の mode の中で一番大きい物（4K の monitor でも最大になる。splash や console の文字が小さくなる）。
  - あわせて、amd64 の zedbsd.cfg の `video=640x480` を残すか外すか。
- 決めてほしい事: (a)〜(c) のどれにするか。推しは (b)（monitor に合う、読めない時は今と同じ）。

### 4. WS153（Settings の Apps の頁と third-party の app の repository）の U2〜U15

- 背景: [WS153](ws153/ws.md) は、第三者の app を repository から入れる仕組み（package の形、配り方、署名、Settings の Apps の頁）の WS。2026-10-05 に U1（導入の範囲は system 全体の /apps/）はユーザーが決めた。残りの U2〜U15 は「ユーザーが検討中（質問しない）」として止めていて、WS153 は p001 の後で保留。設計の文書は [ws153/design.md](ws153/design.md)（各項目に Q1 の推しがある）。
- 項目（推しは design.md の表）:
  - U2 package の形: 自己完結の bundle（推し、依存の衝突が無い）か、共有の依存の package か。
  - U3 配り方（HTTP と署名か HTTPS か）と U4 署名の検証の実装（Ed25519 などを自前で書くか）。
  - U5 公式の repository: 運営するか、誰が鍵を持つか、image に既定で入れるか、鍵が漏れた時。
  - U6 sandbox が無い事（段 1 は無し、入れた app は利用者の全ての権限で動くと画面で示す）。
  - U7 package の名前と拡張子（`.kapp`）。U8 開発者向けの SDK は別の WS にするか。
  - U9 第三者の binary に、どの library のどんな変更までを互換と約束するか（platform の ABI の安定の方針）。
  - U10 手元の署名の無い `.kapp` の導入を許すか（段 1 は許さない）。U11 更新の確認の自動の問い合わせ（1 日 1 回、既定 on）。
  - U12 展開の上限（package 256 MiB・展開 1 GiB など）。U13 導入の確認の dialog を誰が描くか（compositor）。U14 既定の app の UI を段 1 に入れるか（段 2）。U15 system 全体に入れる特権の helper（setuid root の小さな program）の口。
- 決めてほしい事: 今は進めないなら、このまま保留（聞かない）。進めるなら、design.md の推しで決めてよい項目と、話し合いたい項目を教えてほしい。

### 5. 以前からの物（変わっていない）

- WS282〜WS285（camera など）: 実装を始める時期、WS282 の試験の画面を出す PC と買う webcam。
- BUG-345（Pi 5 の click が画面に出ない窓、再発したら知らせてもらう）。

## ユーザーが拾える試験（頼まない、お手すきの時に）

- ODROID-C5 の実機で、BUG-384 の直しの後の kernel が U-Boot の booti で起動するか（image_size が BSS を含む大きさになった、main 98179a6a2）。
- VF2: 今の main の kernel で HDMI に console の画面が出るか（WS331 p005 の目視の残り、BUG-372 は直った）。
- installer（BUG-360）: 大きさの欄を全部消して打ち直せるか（QEMU は disk の空きの都合で未実施）。Settings（BUG-306）: 回転を 2 回目に 90° にしても窓が残るか。
- **5330 の電源の入れ直し（済み、2026-11-01 上がっているのを確かめた。P1 が q1788 で試験中）**: 入れ直した後、担当が WS299 の image で NVMe の速さを測り、WS333 p012 の前後の暇な時の電力・p011 の HWP・RAPL・p018 の Dell の mode と fan を実機で確かめる（p014）。 2026-10-31 02:2x、P1 が main の kernel で起動した後に応答しない（遠隔の電源が無い）。入れ直すと Debian に戻る。古い userland と新しい kernel の組み合わせで network が上がらないだけの見込み（BUG-373・371）。P1 はそれまで QEMU で WS299 を進める。
- **Pi 5（2026-10-31 の午後にユーザーが行う予定）**: image は出来ている（P4、main d26c657c7＋Pi 5 の GPU の branch、戻しの kernel 入り）: `/home/awe/zedBSD-worktrees/p4-ws291pi/build/ws291-p021c/rpi5-p021.img`（sha256 dcadd719…）。Q1 が SD に書く（`sudo dd … bs=4M conv=fsync`）か、ユーザーが書く。起動したら Q1 に知らせると担当が SSH で GPU の clock と fps を測る。起動しなければ config.txt の `kernel=vmunix-fallback`。
- **ODROID-C5（2026-10-31 の午後、VF2 の最適化の後）**: 試験の image で HDMI に画面が出なかった（ユーザー）。serial（今は VF2 につないでいる）を C5 につないで、起動のどこで止まるかを見る。Q1 が担当（WS313）を起動して声をかける。
- **10.0.10.3（BUG-374、RTX の cursor の色）**: 診断の kernel（白を赤・黒を青に塗る）で desktop の矢印の内側と外枠の色を見る。2026-10-31 01:44 に一度用意したがユーザーは不在になったので戻した。入れ直しの手順は [BUG-374](bugs/BUG-374.md)、Q1 が声をかける。

## 決まった事

（2026-11-01 05:07 ユーザーの指示でクリア。決定は各 ws.md の「ユーザーの決定」と [master-design-policy.md](master-design-policy.md) にある）


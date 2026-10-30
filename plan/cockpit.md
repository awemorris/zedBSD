# Cockpit

ユーザーが監視する状況の一枚（2026-10-30 ユーザーの指示、[master.md](master.md) の「Cockpit の運用」）。書き手は Q1 だけ。
最終更新: 2026-10-31 06:33（Q1）

## 担当（P1〜P8）

2026-10-31 01:50 から昼頃までユーザーは不在（自走を続ける趣旨の指示を受けた）。判断が要る物は uncleared にして下の「判断を待つ事」に置き、先へ進む。

| 担当 | WS | WS の説明 | 今している事 | 作業の予定 |
| --- | --- | --- | --- | --- |
| P1 | [WS326](ws326/ws.md) | libmedia の H.264 の software の decoder | q1778: p025 規約の全文の見直し（WS326 の最後の Phase） | 終われば WS326 を完了の形に |
| P2 | — | — | 返却（06:5x）。VF2: BUG-338・340・342・343 closed（SD の CRC の誤りの根を直し、10 回の起動で root が毎回 live）、339 は判断待ち（0g） | 次の割当を Q1 が決める |
| P3 | [WS291](ws291/ws.md) | GPU の描画の速さ（NVIDIA RTX の段、10.0.10.3） | q1764: p014 の続き（kernel の record の decode の短縮など） | big-1000 を 85〜91 → NVK の 136 fps へ。WS331 は完了 |
| P4 | — | — | 返却（07:2x）。WS333 p019（sessiond の熱の要求の口と権限の規則、kernel への呼びは UAPI まで仮）を main に | 午後: Pi 5 の実機 |
| P5 | [WS302](ws302/ws.md) | デスクトップのスレッドバランサ（DTB） | q1775: p001 設計（WS333 の CPU の口を使う、design-reviewer と問いまで） | WS330 の設計は済み（問いは 0f） |
| P6 | [WS310](ws310/ws.md) | ACPI の _OSC を呼び、システム全体の挙動を厳重に確かめる | q1779: p001 設計（どの _OSC を何の bit で、前後の firmware の振る舞いの差の確かめ方） | WS317（hotplugd）はユーザーの「今は取り組まない」のとおり設計もしない |
| P7 | — | — | 返却（07:4x）。WS333 p018（Dell の BIOS の熱の mode・fan・boost の provider、5330 の表で SMI の数まで確かめた）を main に | 5330 の実機（p014・p018）は電源の入れ直しの後 |
| P8 | [WS074](ws074/ws.md) | Web ブラウザ（libbrowser）: Interop 2025 で Chromium が通る試験を全部通す | 自走の 3 回目（q1727-i03）: 偶然通っていた 2 件の後退の直し → 直交の flex・grid の揃え・要素の scroll・subgrid・View Transitions の残り | 今 3,472 のうち 2,472（71.20%）。flex・grid の baseline、fieldset、CSS の論理の box の property を足した、後退 0 |

実装の担当: O-p022（Opus、[WS326](ws326/ws.md) p022 JVT の適合の試験）、S-333p008・p009・p010（Sonnet 3 人、[WS333](ws333/ws.md) の x86 の CPUID・HWP・電力の module）。p015・p017・p019・p021・p023 は cleared。

## WS ごとの進捗

### WS291 GPU の描画の速さ（GPU ごとに desktop 60 fps と重い 3D の model）

| Phase | GPU | 目標 | 今 | 状態 |
| --- | --- | --- | --- | --- |
| p001〜p006 | AMD RDNA | Linux（RADV）と同等 | 同等 | cleared（p006・p008・p009 は RX 5600 XT が抜かれて止め） |
| p010〜p012 | NVIDIA RTX 2070 | desktop 60 fps、scene-5000 で NVK（50.1 fps） | desktop 60〜68 fps、scene-5000 49.9 fps（hardware の限界で NVK と同等） | p010・p011 cleared、p012 uncleared（限界） |
| p013・p014 | NVIDIA RTX 2070 | draw・submit の多い bench を NVK 並みに（big-1000 136.2、subs-1000 75.4） | big-1000 48 → 69.7 fps、big-100 401 → 604 fps、subs-1000 11 fps（残りは命令の記録の解読約 7 ms と、pass の終わりの全画面の copy） | uncleared（2026-10-31、差の内訳を記録） |
| p020 | PowerVR（VF2） | qs40 × 13・1920x1080・direct scanout で 50 fps（RTX のカタログの FP32 の比） | 45.8 fps（direct、main に merge 62eeb340e） | in-progress（P2） |
| p021 | Raspberry Pi 5（V3D 7.1） | スペックの比の copies で 50 fps（clock を実機で確定） | 未計測（mview に要る driver の機能を host で実装済み） | in-progress（P4、実機待ち） |
| p022 | Intel i915（5330） | 5330 の Linux（ANV）で 50.6 fps の qs40 × 600 で Linux 以上 | 32.6 fps（頂点の shader 134 → 67 命令。CPU の idle を MWAIT にして GPU が 800 → 850〜900 MHz、まだ電力の上限 13 W に張り付く） | in-progress（P1） |

### WS333 /dev/thermal で統一する電力・熱の枠組み（2026-10-31 新設、WS301・WS303 を統合）

| Phase | 内容 | 状態 |
| --- | --- | --- |
| p001 | 統一の設計（CPU の idle・P-state・P/E core・熱・fan・strategy・非常の停止） | 設計済み（第 2 版、main 9946e22c0）。ユーザーの問い Q333-1〜7 待ち。実装の Phase は p003〜p024（約 70 LW） |
| p003 | kernel の口の header（cpuperf.h・thermal.h、kern_cpu_count） | cleared（main f197c8a3f、20 port の build warning 0、design-reviewer を反映） |
| p005 | 熱の core（前半） | 前半を main に（b7ac4f576、host 試験 13 場面・QEMU で hw.thermal）。後半は UAPI の review の後 |
| p008〜p010 | x86 の CPUID・HWP・電力の module（Sonnet 3 人） | cleared（試験は計 1,700 万 checks 余り） |
| p011 | x86 の CPU の driver の統合（HWP・RAPL・HFI・滞在。QEMU では MSR に触らず topology だけ） | main a058c08d7。host 試験 16 場面・QEMU 2 通り PASS。5330 は p014 |
| p018・p019 | Dell の provider（BIOS の熱の mode・fan・boost）・sessiond の熱の要求の口 | main に。5330 の実機と UAPI（p004）の後の結線が残り |
| p016・p021 | 危ない温度での非常の停止・温度で CPU の上限を下げる | cleared（QEMU で温度の上がる表から S5 の電源断まで。init の返事の口は UAPI の後） |
| p017 | ACPI WMI の層（Dell の BIOS の口の土台） | cleared（5330 の表で 4 device・14 block、SMI の書き 0） |
| p012 | 全 CPU の tick の時刻を揃える（TSC-deadline） | main 22c629663、QEMU の 2 通り PASS。5330 の電力の前後の数待ち |
| p007 | x86 の CPU の register を読む module の設計（標準廉価実装手順、header 5・46 関数・試験の道具） | cleared。AMD と ACPI の仕様は取れず範囲の外（上の 01）。次は p008〜p010 を Sonnet 3 人で並べて |
| p006 | cpuperf の枠組み（kernel、CPU の速さの制御の要求・方針の写し・上限・idle の統計） | cleared（main b5c8e8f50、host 試験 196 checks、QEMU の起動 4 つ PASS） |
| p002 | amd64 の CPU の idle を MWAIT に（5330 の idle 12.99 W 対 Linux 3.2〜3.5 W） | MWAIT C6 を main に merge（f740839e5）。idle 13.00 → 11.70 W、c600 30.6 → 32.6 fps。Linux にはまだ遠い（残りは 1 kHz の tick・P-state・ASPM・PSR の見込み、未調査） |

### WS331 driver から HAL への直の依存を除く

completed（2026-10-31、main 1035ec0a0）。driver の HAL の include と `hal_*` の呼び出しは 0。全 20 port の build、QEMU（amd64・raspi4b の login、armv7・riscv64）PASS。残り（VF2 の画面の目視、amd64 の Venus の flip）は拾える試験へ。

### WS332 tick の周波数を API で扱う

completed（2026-10-31、main 39fc879c5）。KERN_CLOCK_HZ を無くし、tick の数を前提にした残り 32 か所も時間の単位に。全 port の build と amd64 の QEMU の起動 PASS。100 Hz の port の VM の待ちは 100 → 10 ms。周期を実際に変える実装は Future Work。

### WS074 Web ブラウザ（libbrowser）

Interop 2025 で Chromium 156 が通る 3,472 件: 2,105（10-30 の始め）→ 2,386 → 2,427（69.90%）→ 2,454 → 2,472（71.20%、10-31 06:4x、main に merge）。後退は偶然通っていた scripted の 2 件だけ、Acid3 100/100。P8 が 100% まで自走（ユーザーの指示）、3 回目（i03）に入った。

### WS307 graphical installer

completed（2026-10-31、main db69ae87e）。最後の p007: 導入が UEFI の起動の項目を作り、古い zedBSD の項目を消す（QEMU の OVMF で PASS、実機は未実施）。試験は plan/tools/installer/ に。

### WS313 ODROID-C5

試験の image（text と Keiland、公式の bootloader 入り）で、2026-10-31 に HDMI に画面が出なかった（ユーザー）。serial で調べるのは VF2 の最適化の後、今日の午後。zedBSD-firmware は push 済み。

### WS326 libmedia の H.264 の software の decoder

22 Phase cleared（p022 JVT の適合: 範囲の中の 101 本 14,526 frame が全部一致、decoder の直し 0）。ユーザーの指示で昼までに終える（優先度を上げた）。p015・p017・p019・p021（全体の流れ、22 本 408 frame が ffmpeg と一致）・p023（libmedia の backend、実際の stream を h264-software で decode）は cleared ・p022・p026（共通の名前への移し）は cleared ・p024（速さ: 補間と deblocking の書き直しで host 1 thread の 1080p が 1.35〜1.94 倍、例 18.4 → 35.7 fps）は cleared → p025 規約（P1、in-progress）→ WS326 完了→ p021 全体の流れ（Opus）→ p022 適合・p023 backend → p024 速さ・p026 汎用の名前 → p025 規約。P3 の列（q1739）からは外した。

### 最近完了した WS

- WS331 driver から HAL への直の依存を除く（2026-10-31）、WS332 tick の周波数を API で（2026-10-31）、WS307 graphical installer（2026-10-31）、WS009 文書（2026-10-30）、WS327 libkeiland の thread と socket（2026-10-30）、WS315 Realtek USB 2.5G（2026-10-30）。

## open の Bug（今日見つかった物）

| Bug | 中身 | 状態 |
| --- | --- | --- |
| [BUG-373](bugs/BUG-373.md) | main の kernel の Wi-Fi の ioctl の検査の順の誤りで、古い networkd が EINVAL を受け network が上がらない | closed（VF2 の実機で確かめた） |
| [BUG-371](bugs/BUG-371.md) | Wi-Fi の初期化の失敗だけで init が有線を含む networking を飛ばす | closed（VF2 の実機で確かめた） |
| [BUG-372](bugs/BUG-372.md) | VF2 の serial の console が入力を受けない（元から未実装） | closed（VF2 の実機で確かめた） |
| [BUG-375](bugs/BUG-375.md) | direct scanout の後に window mode へ戻る時、compositor が EBUSY で終わる（VF2） | closed（P7: 原因は GPU の kernel の命令の buffer が満杯の時に待たずに失敗する事。待つ形に直し、VF2 で 8 回とも落ちず。main 2c5897f26） |
| [BUG-374](bugs/BUG-374.md) | RTX の mouse の cursor の色が他の GPU と逆 | 診断の kernel を用意済み（矢印の色で原因が分かれる）。ユーザーの目視待ち（下の「拾える試験」） |
| [BUG-370](bugs/BUG-370.md) | Photos で保存すると全面表示が閉じる | closed（QEMU で確かめた） |
| [BUG-376](bugs/BUG-376.md) | PS/2 の mouse が無くても aux の node を作り、open のたびに約 80 ms 待つ | closed（QEMUと実機 で確かめた） |
| [BUG-222](bugs/BUG-222.md) | ue0 の SCP が約 10 Mbps（10-06 の UAT） | closed（10.0.10.3 の ue0 で 2500 Mbps、scp 62〜86 MB/s） |

## ユーザーの判断を待つ事

disk の空きの件（下の 00）だけが作業を遅らせている。

### 00. host の disk の一括の片付け（2026-10-31 02:3x、急ぎ）

- 背景: host の / が満杯になり（938G 中 891G、空き 0）、P1・P4・P8・P3・P6 の build が「No space left on device」で落ちた。担当が頼んだ一時物を Q1 が消したが、新しい担当の sysroot の写し（1 つ約 1.7 GB）などで 04:3x に空きは約 3.8 GB（危ない）。担当には出力を全部 /tmp に置かせ、消せる物を挙げさせている。大きいのは古い担当の worktree（`/home/awe/zedBSD-worktrees/` に約 250 個、合わせて約 559 GB）の `build/`（試験の image・kernel・sysroot の写し）。
- Q1 がしようとした事: 今使っていない worktree（返却済み・merge 済みの担当の物）の `build/` の中身を消す（`build/review` の PNG は残す、今動いている担当の worktree と WS313 の ODROID の image は残す、git の中身と branch は消さない）。**権限の判定（取り返せない削除）で止められた**ので、回避せずユーザーに聞く。
- 決めてほしい事: この片付けをしてよいか。よければ settings に許可を足すか、ユーザーが `! ` で直に走らせる。それまで担当は大きな image を /tmp（tmpfs、空き約 69G）に作る。

### 01. AMD の CPU の仕様書の取得（2026-10-31 04:5x、権限の判定で止められた）

- 背景: WS333 p007（x86 の CPU の register を読む module の設計）で、P7 が AMD の公開の仕様書 APM Vol 3（24594、CPUID の HwPstate・CPB の bit の位置の出典）を amd.com から取ろうとした。1 回目は bot の確かめの page が返り、2 回目に browser の header を付けた取得が**権限の判定（bot の確かめの回避）で止められた**。P7 は回避せず止め、AMD の取得はしない。
- 影響: AMD の CPU の P-state の bit を仕様で裏付けられないので、p008 の範囲から外し「仕様未取得」と記録して進める。5330（Intel）には影響しない。Intel の SDM は取得済み。
- 同じ形でもう 1 件: ACPI 6.5 の第 8 章（_PSS・_PSD・_PPC、firmware が示す P-state の表の形の出典）を uefi.org から取ったら Cloudflare の challenge の page が返り、P7 は再試行せず止めた。p010 から _PSS などの解析を「仕様未取得」として外す。
- 決めてほしい事: ユーザーが AMD APM Vol 3 と ACPI 6.5 の PDF を手で置くか（置き場所は Q1 が決める、git の外）、その 2 つの部分は後回しにするか。

### 02. 共有の /tmp の試験の作業 directory の片付け（2026-10-31 05:1x、権限の判定で止められた）

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

- VF2: 今の main の kernel で HDMI に console の画面が出るか（WS331 p005 の目視の残り、BUG-372 は直った）。
- installer（BUG-360）: 大きさの欄を全部消して打ち直せるか（QEMU は disk の空きの都合で未実施）。Settings（BUG-306）: 回転を 2 回目に 90° にしても窓が残るか。
- **5330 の電源の入れ直し（起きたら）**: 入れ直した後、担当が WS299 の image で NVMe の速さを測り、WS333 p012 の前後の暇な時の電力・p011 の HWP・RAPL・p018 の Dell の mode と fan を実機で確かめる（p014）。 2026-10-31 02:2x、P1 が main の kernel で起動した後に応答しない（遠隔の電源が無い）。入れ直すと Debian に戻る。古い userland と新しい kernel の組み合わせで network が上がらないだけの見込み（BUG-373・371）。P1 はそれまで QEMU で WS299 を進める。
- **Pi 5（2026-10-31 の午後にユーザーが行う予定）**: image は出来ている（P4、main d26c657c7＋Pi 5 の GPU の branch、戻しの kernel 入り）: `/home/awe/zedBSD-worktrees/p4-ws291pi/build/ws291-p021c/rpi5-p021.img`（sha256 dcadd719…）。Q1 が SD に書く（`sudo dd … bs=4M conv=fsync`）か、ユーザーが書く。起動したら Q1 に知らせると担当が SSH で GPU の clock と fps を測る。起動しなければ config.txt の `kernel=vmunix-fallback`。
- **ODROID-C5（2026-10-31 の午後、VF2 の最適化の後）**: 試験の image で HDMI に画面が出なかった（ユーザー）。serial（今は VF2 につないでいる）を C5 につないで、起動のどこで止まるかを見る。Q1 が担当（WS313）を起動して声をかける。
- **10.0.10.3（BUG-374、RTX の cursor の色）**: 診断の kernel（白を赤・黒を青に塗る）で desktop の矢印の内側と外枠の色を見る。2026-10-31 01:44 に一度用意したがユーザーは不在になったので戻した。入れ直しの手順は [BUG-374](bugs/BUG-374.md)、Q1 が声をかける。

## 決まった事

| 日付 | WS | 決定 | 記録 |
| --- | --- | --- | --- |
| 2026-10-31 | WS309・BUG-374 | game mode（全画面の 3D）では platform に関わらず mouse の cursor を表示しない（P7 が全 backend で確かめる）。BUG-374 はユーザーが今画面を見て確かめる | [ws309/ws.md](ws309/ws.md)・[BUG-374](bugs/BUG-374.md) |
| 2026-10-31 | BUG-222 | 別の対応（WS315 の Realtek USB の vendor mode）で解決済みの見込みなので、試験だけ流して閉じる | [BUG-222](bugs/BUG-222.md) |
| 2026-10-31 | 名前 | OS 名は zedBSD に決定（仮決めでなく）。Keiland・libkeiland は残す。変える時はユーザーが改めて伝える | [master.md](master.md) |
| 2026-10-31 | 割り当て・整理 | PS/2 の aux を BUG-376 として起票し P6 へ。WS299 は 5330 が空いたら（q1744）、WS304 は P4 の Pi 5 の後（q1745）、WS306 は検討済みなので P5 が WS333 p001 の後に進める（q1746）。openXC7 の導入と LLVM の cache の upload は済み、保留の WS の整理は済み（項目を消した） | [queue.md](queue.md) |
| 2026-10-31 | 割り当て | WS307・WS331・WS332・WS326 を P3、BUG-370 と WS291（RTX の続き p014）を P6、BUG-374 を P4 | [queue.md](queue.md) |
| 2026-10-31 | WS291 PowerVR | VF2 の kernel と userland の入れ替えはすべて自動でよい。P2 は bug-analyzer の subagent を使ってよい | [ws291/ws.md](ws291/ws.md) |
| 2026-10-31 | WS291 p022・WS333 | CPU の省電力（idle の C-state・P-state）を先に直す。WS333 p002 で amd64 の idle の MWAIT を先に（P1）、p001 で統一の設計（P5） | [ws333/ws.md](ws333/ws.md) |
| 2026-10-31 | WS301・WS303 → WS333 | WS301（CPU の DVFS）と WS303（熱と fan）を廃止し、/dev/thermal で統一して設計する WS333 に統合（CPU の idle も含める） | [ws333/ws.md](ws333/ws.md) |
| 2026-10-31 | WS291 PowerVR | P2 が VF2 で SSH と sudo を使って kernel の入れ替えと計測の program の実行を続けてよい | [ws291/ws.md](ws291/ws.md) |
| 2026-10-30 | WS331 driver の HAL 依存 | HAL の atomic.h は macro だけの契約（直列化は macro の中の block で宣言して呼ぶ、cookie の型を消す、2 行の include を外す）、kernel の atomic.h は使った後に undef | [ws331/ws.md](ws331/ws.md) |
| 2026-10-30 | WS331 driver の HAL 依存 | pc98・pcat の legacy の VRAM・VGA の窓: platform driver は straight map の基底 0x80000000 を知っている前提で、基底は driver の中で定義し、HAL の定数は参照しない（handoff・API は足さない） | [ws331/ws.md](ws331/ws.md) |
| 2026-10-30 | WS331・WS332 | driver は rpi-common・s905x5m、HAL は bsp-rpi-common・bsp-s905x5m-common・bsp-odroidc5 に分ける。kernel の header は整数の型を kernel で定義し、sched・atomic・thread・vmspace は _private.h に分ける。KERN_CLOCK_HZ は obsolete なので削除し、tick を API で扱い可変にする WS332 を立てる | [ws331/ws.md](ws331/ws.md)・[ws332/ws.md](ws332/ws.md) |
| 2026-10-30 | WS331 driver の HAL 依存 | rpi4 の console: HAL の BSP の非公開の関数を呼ばない。resume に当たる処理は driver の中に同等の関数を作る（suspend は kernel_putc の約束で、要れば HAL の実装を直す） | [ws331/ws.md](ws331/ws.md) |
| 2026-10-30 | WS291 GPU の速さ | PowerVR の目標: Linux の RTX 2070 の scene-5000（qs40 × 5000、50 fps）をカタログの FP32 の比（約 392 倍、GPU 594 MHz）で割り、qs40 × 13・1920x1080 で 50 fps 以上 | [ws291/ws.md](ws291/ws.md) |
| 2026-10-30 | WS331 driver の HAL 依存 | kernel_putc は宣言を kernel の header に。kernel_notify_task は既存の sched_notify_task へ | [ws331/ws.md](ws331/ws.md) |
| 2026-10-30 | WS313 ODROID-C5 | zedBSD-firmware に Hardkernel の公式の image（無変更）と Amlogic の license、Mali の firmware を入れて push（GitHub の main 43662bd） | [ws313/ws.md](ws313/ws.md) |
| 2026-10-30 | WS307 graphical installer | p007 の G2: amd64 の hal_space_map が HAL_SPACE_SYS（firmware の窓）への実行可の map を受けるのは実装の補完で、HAL の API の変更ではない（ユーザー）。P6 が実装 | [ws307/ws.md](ws307/ws.md) |
| 2026-10-30 | WS331 driver の HAL 依存 | p001 の表 A・表 B を許可 | [ws331/ws.md](ws331/ws.md) |
| 2026-10-30 | WS309 compositor の thread の整理 | p015 ゲームモードの V2: 1 は (a)（app は変更なし、全画面と窓の切り替えは compositor の都合で app は気づかない）、2 は private な Wayland の protocol `keiland_game_surface_v1` を作る、3 は標準の型の組み合わせで画像の memory を出す、4 は同じ画像の再 acquire で画面から外れた事を知る。p015 は planned に戻り、次のセッションで実装 | [ws309/ws.md](ws309/ws.md) |
| 2026-10-30 | BUG-364 GPU の fence | (A) display はどの GPU の fence も受ける（今の動きを正とする）。Q1 が gpu.c の comment を直し、BUG-364 を閉じた（876b64d55） | [BUG-364](bugs/BUG-364.md) |
| 2026-10-30 | WS074 Web ブラウザ | Codex を解約、WS074 は Claude（Q1）の管理下に | [ws074/ws.md](ws074/ws.md) |
| 2026-10-30 | WS328 libbrowser のベクトル描画と SVG | WS074 を止めて WS328 を先に、だったが、同日の後の指示で WS074 を P8 で先に再開（WS328 は未着手） | [ws328/ws.md](ws328/ws.md) |

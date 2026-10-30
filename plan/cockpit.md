# Cockpit

ユーザーが監視する状況の一枚（2026-10-30 ユーザーの指示、[master.md](master.md) の「Cockpit の運用」）。書き手は Q1 だけ。
最終更新: 2026-10-31 01:21（Q1）

## 担当（P1〜P8）

| 担当 | WS | WS の説明 | 今している事 | 作業の予定 |
| --- | --- | --- | --- | --- |
| P1 | [WS333](ws333/ws.md)・[WS291](ws291/ws.md) | CPU の idle の省電力と、i915（5330）の描画の速さ | WS333 p002: amd64 の idle を HLT から MWAIT に替え、5330 で idle の電力と GPU の周波数を前後で測る | WS291 p022 に戻って測り直す（今 qs40 × 600 で 31.0 fps、Linux 50.6）。Notes は 5330 で起動を確かめた |
| P2 | [WS291](ws291/ws.md) | GPU の描画の速さ（PowerVR の段、VF2） | p020: GPU の memory を cache（SLC）に通す既定の回帰、表示用の copy を頂点の処理が待たない直し | qs40 × 13 を 42.35 → 50 fps 以上へ。branch は BUG-375 が直るまで main に入れない |
| P3 | — | — | 返却（WS291 p013: big-1000 48 → 69.7 fps、NVK 136 には届かず。BUG-374 は解析だけ） | — |
| P4 | [WS291](ws291/ws.md) | GPU の描画の速さ（Raspberry Pi 5 の段） | 返却。host で driver の欠けを実装済み（branch は未 merge） | ユーザーが Pi 5 の image で起動した後に: image を作り直し、Pi 4 の desktop の回帰、clock の確かめ、fps の計測 |
| P5 | [WS333](ws333/ws.md) | /dev/thermal で統一する電力・熱の枠組み | p001: 統一の設計（WS301・WS303 の設計を入力に） | design-reviewer → ユーザーの判断の一覧 |
| P6 | — | — | 空き（WS307 は p007 の残り 2 つで止め） | — |
| P7 | Bug | network と VF2 と compositor の Bug | BUG-371 の QEMU の確かめ、BUG-375（game mode の後に compositor が落ちる）の解析 | 直し → VF2（P2）と 10.0.10.3（P3）で確かめ |
| P8 | [WS074](ws074/ws.md) | Web ブラウザ（libbrowser）: Interop 2025 で Chromium が通る試験を全部通す | 自走の 2 回目。zedBSD の image で libbrowser の build と QEMU の起動を確かめる | 要素の scroll、baseline・subgrid、View Transitions の残り。今 3,472 のうち 2,386（68.72%） |

Sonnet の実装の担当（標準廉価実装手順）: 今は無し。次は [WS326](ws326/ws.md) p015・p017。

## WS ごとの進捗

### WS291 GPU の描画の速さ（GPU ごとに desktop 60 fps と重い 3D の model）

| Phase | GPU | 目標 | 今 | 状態 |
| --- | --- | --- | --- | --- |
| p001〜p006 | AMD RDNA | Linux（RADV）と同等 | 同等 | cleared（p006・p008・p009 は RX 5600 XT が抜かれて止め） |
| p010〜p012 | NVIDIA RTX 2070 | desktop 60 fps、scene-5000 で NVK（50.1 fps） | desktop 60〜68 fps、scene-5000 49.9 fps（hardware の限界で NVK と同等） | p010・p011 cleared、p012 uncleared（限界） |
| p013 | NVIDIA RTX 2070 | draw・submit の多い bench を NVK 並みに（big-1000 136.2、subs-1000 75.4） | big-1000 48 → 69.7 fps、big-100 401 → 604 fps、subs-1000 11 fps（残りは命令の記録の解読約 7 ms と、pass の終わりの全画面の copy） | uncleared（2026-10-31、差の内訳を記録） |
| p020 | PowerVR（VF2） | qs40 × 13・1920x1080・direct scanout で 50 fps（RTX のカタログの FP32 の比） | 42.35 fps（direct） | in-progress（P2） |
| p021 | Raspberry Pi 5（V3D 7.1） | スペックの比の copies で 50 fps（clock を実機で確定） | 未計測（mview に要る driver の機能を host で実装済み） | in-progress（P4、実機待ち） |
| p022 | Intel i915（5330） | 5330 の Linux（ANV）で 50.6 fps の qs40 × 600 で Linux 以上 | 31.0 fps（頂点の shader 134 → 67 命令。GPU が電力の上限で 800 MHz、WS333 p002 で CPU の idle を先に直す） | in-progress（P1） |

### WS333 /dev/thermal で統一する電力・熱の枠組み（2026-10-31 新設、WS301・WS303 を統合）

| Phase | 内容 | 状態 |
| --- | --- | --- |
| p001 | 統一の設計（CPU の idle・P-state・P/E core・熱・fan・strategy・非常の停止） | in-progress（P5） |
| p002 | amd64 の CPU の idle を MWAIT に（5330 の idle 12.99 W 対 Linux 3.2〜3.5 W） | in-progress（P1） |

### WS331 driver から HAL への直の依存を除く

規則（2026-10-30 ユーザーの決定）: driver は HAL を知らず、kernel の関数（kern_*）だけを使う。driver の HAL の include と `hal_*` の呼び出しは 0 になった（build の印 `HAL_ARCH_*` も p008 で driver から消した）。

| Phase | 内容 | 状態 |
| --- | --- | --- |
| p001〜p004 | 違反の一覧、kernel に足す関数（表 B、許可済み）、表 A の置き換え、i915 | cleared（i915 の ktest-sync の 5330 の実行は未） |
| p005 | Pi 4・Pi 5・VF2・ODROID-C5 に /dev/graphics と console（PC/AT の形、kernel_putc） | 実装済み。残り: VF2 の画面の目視、amd64 の Venus での flip の確かめ |
| p007 | HAL の early console の framebuffer の窓を kernel が外せる形に（Pi の EBUSY を解消） | cleared |
| p008〜p011 | Pi の共通化と ODROID-C5 の分離、kernel の header の _private.h、atomic.h の分離、arch の spinlock の macro | cleared |
| p006 | 規約の全文の見直し | 未作成（担当なし） |

### WS332 tick の周波数を API で扱う

| Phase | 内容 | 状態 |
| --- | --- | --- |
| p001・p002 | 設計、KERN_CLOCK_HZ の削除、Wi-Fi の UAPI を ns に | cleared |
| p003 | 率を仮定する残り（tick の数の literal、+1） | planned（担当なし） |
| p004 | 規約の見直し | planned |

### WS074 Web ブラウザ（libbrowser）

Interop 2025 で Chromium 156 が通る 3,472 件: 2,105（10-30 の始め）→ 2,215 → 2,386（68.72%、10-31）。後退 0、Acid3 100/100。P8 が 100% まで自走（ユーザーの指示）。

### WS307 graphical installer

p001〜p006・p008〜p011・p013〜p015 cleared（p012 canceled）。p007（firmware 変数の driver と efiboot）は OVMF の試験 PASS、残り: installer の導入の QEMU、古い zedBSD の entry の掃除。担当なし（P6 は返却）。

### WS313 ODROID-C5

試験の image（text と Keiland、公式の bootloader 入り）は出来ている: p1-ws313/build/odroid-c5{,-keiland}/hdd-image.img。zedBSD-firmware は push 済み。実機の結果待ち。

### WS326 libmedia の H.264 の software の decoder

20 Phase cleared。p015・p017（補間と重み、B の direct）から Sonnet で。担当なし。

### 最近完了した WS

- WS009 文書（2026-10-30）、WS327 libkeiland の thread と socket（2026-10-30）、WS315 Realtek USB 2.5G（2026-10-30）。

## open の Bug（今日見つかった物）

| Bug | 中身 | 状態 |
| --- | --- | --- |
| [BUG-373](bugs/BUG-373.md) | main の kernel の Wi-Fi の ioctl の検査の順の誤りで、古い userland の networkd が EINVAL を受け network が上がらない | 直しを main に merge（QEMU で確かめた）。実機（VF2・10.0.10.3）で確かめてから閉じる |
| [BUG-371](bugs/BUG-371.md) | Wi-Fi の初期化の失敗だけで init が有線を含む networking を飛ばす | 直しを main に merge、QEMU の確かめ中（P7） |
| [BUG-372](bugs/BUG-372.md) | VF2 の serial の console が入力を受けない（元から未実装） | 直しを main に merge、VF2 の実機の確かめ待ち |
| [BUG-375](bugs/BUG-375.md) | direct scanout の後に window mode へ戻る時、compositor が EBUSY で終わる（VF2） | 解析中（P7） |
| [BUG-374](bugs/BUG-374.md) | RTX の mouse の cursor の色が他の GPU と逆 | 解析だけ（nvrtx が受ける画像は正しい、原因は未特定。画面の目視が要る） |
| [BUG-370](bugs/BUG-370.md) | Photos で保存すると全面表示が閉じる | 担当なし |

## ユーザーの判断を待つ事

今は無し。

## ユーザーが拾える試験（頼まない、お手すきの時に）

- Pi 5: `/home/awe/zedBSD-worktrees/p4-ws291pi/build/ws291-p021/rpi5-p021.img` を SD に書いて起動し、画面に console の login が出るか（起動しなければ config.txt の `kernel=vmunix-fallback`）。起動したら Q1 に知らせると P4 が SSH で測る。
- ODROID-C5: 試験の image（上の WS313）で起動と画面。
- VF2: 新しい kernel で画面と serial の login（BUG-372）。

## 決まった事

| 日付 | WS | 決定 | 記録 |
| --- | --- | --- | --- |
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

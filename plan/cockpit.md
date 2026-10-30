# Cockpit

ユーザーが監視する状況の一枚（2026-10-30 ユーザーの指示、[master.md](master.md) の「Cockpit の運用」）。書き手は Q1 だけ。
最終更新: 2026-10-31 00:18（Q1）

## 担当（P1〜P8）

| 担当 | WS | WS の説明 | 今している事 | 作業の予定 |
| --- | --- | --- | --- | --- |
| P1 | [WS291](ws291/ws.md) | GPU の描画の速さ（Intel i915、5330 の Alder Lake の段） | 起動（q1731）。p022: main の kernel の起動と mview の確かめ、5330 の Linux（ANV）で約 50 fps の scene を決める | 今の fps → 改善（WS313 の ODROID-C5 は実機の結果待ちのまま） |
| P2 | [WS291](ws291/ws.md) | GPU の描画の速さ（PowerVR の段、VF2） | 起動（q1726）。p020: qs40 × 13・1920x1080 を 26.6 fps から 50 fps 以上へ | profile（TA と 3D の分け、解像度に比例する費用、頂点の側）→ 改善 |
| P3 | [WS291](ws291/ws.md) | GPU の描画の速さ（NVIDIA RTX の段） | 起動（q1728）。p013: big-1000（52 対 NVK 136 fps）・subs-1000（10.9 対 75.4 fps）の CPU の費用 | profile → nvrtx の record・submit の改善 |
| P4 | [WS291](ws291/ws.md) | GPU の描画の速さ（Raspberry Pi 5 の V3D 7.1 の段） | 返却。host で driver の欠け（indexed・instance・直の fetch・mip の第 1 段）を実装、build と host 試験 PASS。実機は未、branch は未 merge（Pi 4 の desktop の回帰の確かめ待ち） | Pi 5 の起動の後: image を作り直し、desktop の回帰、clock の確かめ、fps の計測 |
| P5 | — | — | 返却（WS327 完了、2026-10-30） | — |
| P6 | — | — | 返却（WS307: p007 の efiboot は QEMU で PASS、残りは install-admin の導入の QEMU と古い entry の掃除） | — |
| P7 | Bug | BUG-373（main の kernel で Wi-Fi の正規化が EINVAL、network が上がらない）・BUG-371・BUG-372 | 起動（q1732）。QEMU で再現と原因の特定 | 直し → VF2・10.0.10.3 の担当に連絡 |
| P8 | [WS074](ws074/ws.md) | Web ブラウザ（libbrowser）の改善: Interop 2025 で Chromium が通る試験を全部通す（p260） | 自走（q1727、ユーザーの指示）。今 3,472 のうち 2,215（63.80%） | 100% まで。返却のたびに Q1 が起動し直す |

Sonnet の実装の担当（標準廉価実装手順）: 今は無し。次は [WS326](ws326/ws.md) p015（補間と重み）・p017（B の direct）。

## WS ごとの進捗

### WS009 文書 — completed（2026-10-30）

/dev/gpu の UAPI の文書（DOC-54）を書き、clean tree から 4 種の image の build と 5 つの起動を build の文書どおりに再現（P7）。docs の照合の道具は plan/tools/docs-check/。

### WS327 libkeiland の thread と socket の wrapper — completed（2026-10-30）

POSIX と Win32 で同じ interface の thread・socket の層と、app の移行。QEMU の回帰で退行なし（Photos の不具合は BUG-370、WS327 の外）。試験は plan/tools/keiland-layers/。

### WS315 Realtek USB 2.5G・5G（vendor mode）— completed（2026-10-30）

5330 の RTL8156B で send 2370・recv 2373・同時 2268/2359（Linux 2353・2354・2346/2341）、10GbE も落ちない。5G（RTL8157）は資料だけ。試験は plan/tools/rtl815x/ に。F-WS315-rcvbuf は解決。

### WS307 graphical installer

| Phase | 内容 | 状態 |
| --- | --- | --- |
| p001・p003・p005・p006・p009〜p011・p013 | 仕様、build、道具、4096 sector、helper、app、通し、文書 | cleared |
| p002 | OS 名を zedBSD に、release の upload | cleared（push 後の CI は未確かめ） |
| p004 | package の開発の file、clang・libcxx・GTK4 のセルフビルド | cleared（2026-10-30 QEMU run4 PASS: 導入先で GTK4 の app と C++ を build・実行） |
| p007 | firmware 変数の kernel の driver（hal.h は変えない） | in-progress（efiboot の OVMF の試験 PASS。残り: install-admin の導入の QEMU、古い zedBSD の entry の掃除） |
| p008 | UFS からの起動の確かめ直し | cleared（2026-10-30 512 の j3 の sync 3 回 ok） |
| p012 | 実機の導入 | canceled |
| p014 | 規約の見直し | cleared（2026-10-30） |
| p015 | 画面の lock、sleep の既定 Never | cleared（2026-10-30 QEMU の 3 回目 4 項目 PASS） |

### WS291 GPU の描画の速さ（NVIDIA の段）

| Phase | 内容 | 状態 |
| --- | --- | --- |
| p001〜p006 | RDNA | cleared |
| p010 | NVIDIA desktop 60 fps | cleared（2026-10-30 P7、実機で 2 回、60〜68 fps） |
| p010a・p011 | NVIDIA の executor、scene-5000 | cleared（49.5〜49.7 fps、NVK と同等） |
| p013 | draw・submit の多い bench の CPU の費用を NVK 並みに | in-progress（P3） |
| p012 | 50 fps を明らかに超える改善 | uncleared（2026-10-30 P7: 頂点の側の固定機能の hardware の限界、NVK と同等 49.0 fps、上限 約 51.3。温度の差なし） |
| p006・p008・p009 | RDNA の残り | 止め（RX 5600 XT が抜かれて実機なし） |
| p022 | Intel i915（5330）: Linux（ANV）で約 50 fps の scene を zedBSD で Linux 以上に（ユーザーの指示で実測の Linux が基準） | in-progress（P1） |
| p021 | Raspberry Pi 5 の model 50 fps（qs40 × スペックの比） | in-progress（P4） |
| p020 | PowerVR の model 50 fps（qs40 × 13） | in-progress（26.6 fps、2026-10-30） |

### WS331 driver から HAL への直の依存を除く（P2）

規則（2026-10-30 ユーザー）: driver は HAL を知らず、kernel の関数（kern_*）だけを使う。今は driver の 38 file が HAL を include し、`hal_*` を 105 回（本体 80・試験 25）呼ぶ。

| Phase | 内容 | 状態 |
| --- | --- | --- |
| p001 | 違反の全数、置き換え先の一覧（表 A）、kernel に足す関数の案（表 B） | cleared（2026-10-30 ユーザーが表 A・B を許可） |
| p002 | kernel に表 B を足す（driver は変えない） | cleared（2026-10-30） |
| p003 | 表 A の機械的な置き換え（sparcv9 の 2 file は別の commit） | cleared（2026-10-30） |
| p004 | i915 の map（write-combine・読み出しだけ）と MSI、試験の代役 | cleared（2026-10-30、ktest-sync の 5330 は未実施） |
| p005 | Pi 4・Pi 5・VisionFive 2・ODROID-C5 に /dev/graphics と console を PC/AT の形で（GPU の最初の描画で console が framebuffer から離れる）、pc98 の 3 か所 | 実装済み（Pi 4 の QEMU で login と描き直し、VF2 の実機で起動と /dev/graphics。残り: VF2 の画面の目視、amd64 の Venus、Pi の GPU の最初の flip で framebuffer を手放せない（EBUSY）） |
| p007 | HAL の early console の framebuffer の窓を kernel が外せる形に（amd64 の fb_window を arm64・riscv64 へ） | cleared（2026-10-30、QEMU Pi 4 と VF2 の実機で窓が外れる事を確かめた） |
| p008 | Pi の共通化と ODROID-C5 の分離、残りの HAL_ARCH_* | cleared（2026-10-30） |
| p009 | kernel の header から HAL を分ける（_private.h） | cleared（2026-10-30） |
| p010 | atomic.h の HAL の分離、pc98・pcat の窓の番地 | cleared（2026-10-30） |
| p011 | arch の spinlock の取得を HAL の atomic.h の macro の契約へ | cleared（2026-10-30） |
| p006 | 規約の全文の見直し | 未作成 |

表 A（kernel に置き換え先が既にある物）:

| driver が使う HAL | kernel の置き換え先 | 違い |
| --- | --- | --- |
| hal_space_map_device・unmap_device | kern_device_map・kern_device_unmap | 戻り値が errno。属性は uncached と write-through だけ（→ B1） |
| hal_get_arch_handoff | kern_boot_handoff | 無し（driver の 13 か所は既に使う） |
| hal_rtc_read_counter | kern_rtc_read_counter | 無し |
| hal_irq_disable・enable・send_eoi、hal_irq_ack_t | kern_irq_disable・enable・send_eoi、kern_irq_ack_t | 型の名前だけ |
| hal_io_inp*・outp*、hal_mmio_* | kern_io_in*・out*、kern_mmio_* | 名前だけ |
| HAL_FATAL | KERN_FATAL | 止まる前に log と console に記録する（よりよい） |
| HAL_OK | 0（errno） | — |

表 B（kernel に足す物の案、ユーザーが決める）:

| # | 足す物 | 何に使うか | 使う driver |
| --- | --- | --- | --- |
| B1 | 定数 KERN_DEVICE_WRITECOMBINE と flag KERN_DEVICE_READONLY | write-combine の map（画面の memory を速く書く）と、読み出しだけの map | i915、ACPI |
| B2〜B5 | kern_irq_alloc_msi・attach_msi・detach_msi_sync・free_msi | MSI（PCI の message 型の割り込み）の確保と handler の付け外し。別の案は i915 を既存の kern_irq_register_msi に寄せる（関数を足さない） | i915 |
| B6 | kern_cpu_current | 今の CPU の番号 | pcat の text、dc8200、i915 |
| B7 | kern_console_early_putc | 起動の早い段の console に 1 文字（今の hal_putc と同じ動き） | serial-console、rpi4・sun4u・x68k の console |
| B8 | 型 kern_physaddr_t | 物理 address の型（HAL の名前なしで書く） | 多数（23 file） |

未決（ユーザー）: rpi4 の console の 2 関数（HAL の early console の状態を使う、P2 の推しは当面の例外）、build の macro HAL_ARCH_* を残すか、kernel の header 13 個が HAL の header を include する件を WS331 に入れるか。全文は [phase001](ws331/phase001/phase.md)。

### WS326 libmedia の H.264 の software の decoder（標準廉価実装手順）

| Phase | 内容 | 状態 |
| --- | --- | --- |
| p001〜p014、p016、p018、p020 | 設計と module 20 | cleared |
| p015・p017 | 補間と重み、B の direct | planned（次に Sonnet） |
| p019 | 再構成 | planned |
| p021〜p026 | 全体の流れ、適合、backend、速さ、規約、共通部分の汎用の名前へ | planned |

進捗: 2026-10-30 p007（参照の list）まで cleared。p021 の前に reflist の header の直し（B の POC type 0 の非存在の frame）。

### WS332 tick の周波数を API で扱い可変にする（P3）

| Phase | 内容 | 状態 |
| --- | --- | --- |
| p001 | 設計（時間の API、今の率を取る口） | cleared（2026-10-30） |
| p002 | KERN_CLOCK_HZ の削除、wlan の UAPI を ns に | cleared（2026-10-30） |
| p003 | 率を仮定する残り（tick の数の literal、+1） | planned |
| p004 | 規約の見直し | planned |

## ユーザーの判断を待つ事

今は無し。

## 決まった事

| 日付 | WS | 決定 | 記録 |
| --- | --- | --- | --- |
| 2026-10-30 | WS331 driver の HAL 依存 | HAL の atomic.h は macro だけの契約（直列化は macro の中の block で宣言して呼ぶ、cookie の型を消す、2 行の include を外す）、kernel の atomic.h は使った後に undef | [ws331/ws.md](ws331/ws.md) |
| 2026-10-30 | WS331 driver の HAL 依存 | pc98・pcat の legacy の VRAM・VGA の窓: platform driver は straight map の基底 0x80000000 を知っている前提で、基底は driver の中で定義し、HAL の定数は参照しない（handoff・API は足さない） | [ws331/ws.md](ws331/ws.md) |
| 2026-10-30 | WS331・WS332 | driver は rpi-common・s905x5m、HAL は bsp-rpi-common・bsp-s905x5m-common・bsp-odroidc5 に分ける。kernel の header は整数の型を kernel で定義し、sched・atomic・thread・vmspace は _private.h に分ける。KERN_CLOCK_HZ は obsolete なので削除し、tick を API で扱い可変にする WS332 を立てる | [ws331/ws.md](ws331/ws.md)・[ws332/ws.md](ws332/ws.md) |
| 2026-10-30 | WS331 driver の HAL 依存 | rpi4 の console: HAL の BSP の非公開の関数を呼ばない。resume に当たる処理は driver の中に同等の関数を作る（suspend は kernel_putc の約束で、要れば HAL の実装を直す） | [ws331/ws.md](ws331/ws.md) |
| 2026-10-30 | WS291 GPU の速さ | PowerVR の目標: Linux の RTX 2070 の scene-5000（qs40 × 5000、50 fps）をカタログの FP32 の比（約 392 倍、GPU 594 MHz）で割り、qs40 × 13・1920x1080 で 50 fps 以上 | [ws291/ws.md](ws291/ws.md) |
| 2026-10-30 | WS331 driver の HAL 依存 | kernel_putc は宣言を kernel の header に。kernel_notify_task は既存の sched_notify_task へ | [ws331/ws.md](ws331/ws.md) |
| 2026-10-30 | WS313 ODROID-C5 | zedBSD-firmware に Hardkernel の公式の image（無変更）と Amlogic の license、Mali の firmware を入れて push（ユーザー「WS313のpushはやってください。」、GitHub の main 43662bd） | [ws313/ws.md](ws313/ws.md) |
| 2026-10-30 | WS307 graphical installer | p007 の G2: amd64 の hal_space_map が HAL_SPACE_SYS（firmware の窓）への実行可の map を受けるのは実装の補完で、HAL の API の変更ではない（ユーザー）。P6 が実装 | [ws307/ws.md](ws307/ws.md) |
| 2026-10-30 | WS331 driver の HAL 依存 | p001 の表 A・表 B を許可 | [ws331/ws.md](ws331/ws.md) |
| 2026-10-30 | WS309 compositor の thread の整理 | p015 ゲームモードの V2: 1 は (a)（app は変更なし、全画面と窓の切り替えは compositor の都合で app は気づかない）、2 は private な Wayland の protocol `keiland_game_surface_v1` を作る、3 は標準の型の組み合わせで画像の memory を出す、4 は同じ画像の再 acquire で画面から外れた事を知る。p015 は planned に戻り、次のセッションで実装 | [ws309/ws.md](ws309/ws.md) |
| 2026-10-30 | BUG-364 GPU の fence | (A) display はどの GPU の fence も受ける（今の動きを正とする）。Q1 が gpu.c の comment を直し、BUG-364 を閉じた（876b64d55） | [BUG-364](bugs/BUG-364.md) |
| 2026-10-30 | WS074 Web ブラウザ | Codex を解約、WS074 は Claude（Q1）の管理下に | [ws074/ws.md](ws074/ws.md) |
| 2026-10-30 | WS328 libbrowser のベクトル描画と SVG | WS074（Web ブラウザ）を止めて、WS328 を先に実装する。次のセッションで p001（設計、Opus）から | [ws328/ws.md](ws328/ws.md) |

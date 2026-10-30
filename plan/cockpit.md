# Cockpit

ユーザーが監視する状況の一枚（2026-10-30 ユーザーの指示、[master.md](master.md) の「Cockpit の運用」）。書き手は Q1 だけ。
最終更新: 2026-10-30 22:30（Q1）

## 担当（P1〜P8）

| 担当 | WS | WS の説明 | 今している事 | 作業の予定 |
| --- | --- | --- | --- | --- |
| P1 | [WS313](ws313/ws.md) | ODROID-C5（Amlogic S905X5M）の BSP と Mali の GPU | 返却。zedBSD-firmware を push し branch を merge（b4e549bfc）。試験の image は p1-ws313/build/odroid-c5{,-keiland}/hdd-image.img | 実機の結果で各 Phase を直す。WS009 p028 は後で再開 |
| P2 | [WS331](ws331/ws.md) | driver から HAL への直の依存を除き、kernel の口（kern_*）だけを使う形に | p008 済み（merge fcea07e74）。watch.c の HVS を Pi の側へ移す小さな残り → p009 | p009 kernel の header の _private.h → p006 規約 |
| P3 | [WS332](ws332/ws.md) | tick の周波数を API で扱う（KERN_CLOCK_HZ の削除、取得の口だけ） | 起動（q1722）。p001 設計 | p002 KERN_CLOCK_HZ の削除（175 か所） → p003 規約 |
| P4 | — | — | — | — |
| P5 | — | — | 返却（WS327 完了、2026-10-30） | — |
| P6 | — | — | 返却（WS307: p007 の efiboot は QEMU で PASS、残りは install-admin の導入の QEMU と古い entry の掃除） | — |
| P7 | — | — | 返却（WS291: p010 cleared、p012 uncleared、PowerVR は P3 へ） | — |
| P8 | [WS074](ws074/ws.md) | Web ブラウザ（libbrowser）の改善: Interop 2025 で Chromium が通る試験を全部通す（p260） | 再起動（q1708-i02）。Debian の比較の環境は出来た、Chromium 156 の基準と dev20 の全件の測定の完了待ち | compare → gap の大きい原因から直す |

Sonnet の実装の担当（標準廉価実装手順）: 今は無し。次は [WS326](ws326/ws.md) p015（補間と重み）・p017（B の direct）。

## WS ごとの進捗

### WS009 文書（DOC-54: /dev/gpu の UAPI の文書）

| Phase | 内容 | 状態 |
| --- | --- | --- |
| p001〜p008 | 既存の文書 | cleared |
| p009 | DOC-54 一括 | canceled（p010〜p024 に分けた） |
| p010〜p021 | 節ごとの文書 12（Sonnet） | cleared |
| p022 | 要求の索引 | cleared |
| p023 | 統合・Bug の候補の一覧 | cleared（BUG-364・BUG-365 を登録） |
| p024 | 正確さの review（Opus） | cleared（12 file を直した、BUG-366 と BUG-365 の追加） |
| p025 | managed-wlan.md の書き直し（Opus） | cleared |
| p026 | build の文書・索引・Status 行（Sonnet） | cleared |
| p027 | 文書の検査の script を今の規則に直し plan/tools へ（Sonnet） | cleared |
| p028 | clean build・QEMU 起動の再現、WS001 との照合（Opus） | in-progress（照合は済み。native の image まで、boot は未判定、hybrid・uefi・bios は未） |

進捗: 2026-10-30 骨格と照合の script（P1）→ Sonnet 13 回で全文 → 統合で照合 PASS（14 file、例 12 compile）。sysctl の 4 byte の読みの不具合を Q1 が直した。

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
| p012 | 50 fps を明らかに超える改善 | uncleared（2026-10-30 P7: 頂点の側の固定機能の hardware の限界、NVK と同等 49.0 fps、上限 約 51.3。温度の差なし） |
| p006・p008・p009 | RDNA の残り | 止め（RX 5600 XT が抜かれて実機なし） |
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
| p008 | Pi の共通化と ODROID-C5 の分離、残りの HAL_ARCH_* | 実装済み（20 port の build、QEMU Pi 4、driver の HAL の grep 0）。残り: watch.c の HVS を Pi の側へ |
| p009 | kernel の header から HAL を分ける（_private.h） | planned（P2） |
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
| p001 | 設計（時間の API、可変の tick、HAL の timer） | in-progress（P3） |
| p002 | KERN_CLOCK_HZ の削除（175 か所） | planned |
| p003 | 規約の見直し（周波数を変える実装は将来、ユーザーの指示） | planned |

## ユーザーの判断を待つ事

今は無し。

## 決まった事

| 日付 | WS | 決定 | 記録 |
| --- | --- | --- | --- |
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

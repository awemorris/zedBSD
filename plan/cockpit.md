# Cockpit

ユーザーが監視する状況の一枚（2026-10-30 ユーザーの指示、[master.md](master.md) の「Cockpit の運用」）。書き手は Q1 だけ。
最終更新: 2026-10-30 18:25（Q1）

## 担当（P1〜P8）

| 担当 | WS | WS の説明 | 今している事 | 作業の予定 |
| --- | --- | --- | --- | --- |
| P1 | [WS313](ws313/ws.md) | ODROID-C5（Amlogic S905X5M）の BSP と Mali の GPU | 返却。試験の image を公式の bootloader で作り直した（text: p1-ws313/build/odroid-c5/hdd-image.img sha256 0f8f56a1…、Keiland: p1-ws313/build/odroid-c5-keiland/hdd-image.img sha256 2cf1f64d…） | zedBSD-firmware の push（ユーザー）→ Q1 が agent/p1-ws313 を merge → 実機の結果で各 Phase を直す。WS009 p028 は後で再開 |
| P2 | [WS331](ws331/ws.md) | driver から HAL への直の依存を除き、kernel の口（kern_*）だけを使う形に | 起動（phase-runner-high、q1717）。p002 kernel に表 B を足す | p003 表 A の置き換え → p004 i915 → p005（rpi4 は未決）→ p006 |
| P3 | [WS291](ws291/ws.md) | GPU の描画の速さ（PowerVR の段、VF2） | 再起動（q1715-i02）。ユーザーが VF2 の HDMI を接続。板を入れ直して framebuffer を確かめ、計測へ | 40 fps 前後の scene を決め 50 fps 以上へ |
| P4 | — | — | — | — |
| P5 | — | — | 返却（WS327 完了、2026-10-30） | — |
| P6 | — | — | 返却（WS307: p015・p004・p008 cleared。p007 はユーザーの判断待ち） | — |
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
| p007 | firmware 変数の kernel の driver（HAL を変えない） | in-progress（設計の下書き） |
| p008 | UFS からの起動の確かめ直し | cleared（2026-10-30 512 の j3 の sync 3 回 ok） |
| p012 | 実機の導入 | canceled |
| p014 | 規約の見直し | in-progress |
| p015 | 画面の lock、sleep の既定 Never | cleared（2026-10-30 QEMU の 3 回目 4 項目 PASS） |

### WS291 GPU の描画の速さ（NVIDIA の段）

| Phase | 内容 | 状態 |
| --- | --- | --- |
| p001〜p006 | RDNA | cleared |
| p010 | NVIDIA desktop 60 fps | cleared（2026-10-30 P7、実機で 2 回、60〜68 fps） |
| p010a・p011 | NVIDIA の executor、scene-5000 | cleared（49.5〜49.7 fps、NVK と同等） |
| p012 | 50 fps を明らかに超える改善 | uncleared（2026-10-30 P7: 頂点の側の固定機能の hardware の限界、NVK と同等 49.0 fps、上限 約 51.3。温度の差なし） |
| p006・p008・p009 | RDNA の残り | 止め（RX 5600 XT が抜かれて実機なし） |
| p020 | PowerVR | planned |

### WS331 driver から HAL への直の依存を除く（P2）

規則（2026-10-30 ユーザー）: driver は HAL を知らず、kernel の関数（kern_*）だけを使う。今は driver の 38 file が HAL を include し、`hal_*` を 105 回（本体 80・試験 25）呼ぶ。

| Phase | 内容 | 状態 |
| --- | --- | --- |
| p001 | 違反の全数、置き換え先の一覧（表 A）、kernel に足す関数の案（表 B） | cleared（2026-10-30 ユーザーが表 A・B を許可） |
| p002 | kernel に表 B を足す（driver は変えない） | in-progress（P2） |
| p003 | 表 A の機械的な置き換え（sparcv9 の 2 file は別の commit） | planned（P2） |
| p004 | i915 の map（write-combine・読み出しだけ）と MSI、試験の代役 | planned（P2） |
| p005 | 例外の候補（pc98 の 3 か所、rpi4 の console） | 未作成 |
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

## ユーザーの判断を待つ事

### 1. WS307（Keiland の graphical installer）p007: firmware の関数を kernel から実行する map の扱い

- 背景: installer は、導入した disk から起動できるよう UEFI の起動の項目（Boot####）を書く。UEFI の firmware は、OS の起動の後も呼べる関数（runtime services。GetVariable・SetVariable など）を持つ。ユーザーの決定で、これを呼ぶのは kernel の driver で、driver は HAL を知らず kernel の関数（kern_*）だけを使う。
- 問題: firmware の関数は code なので、kernel の memory に「実行してよい」印で map しないと呼べない。今の kernel には実行可の map を作る関数が無い。その下の HAL も、どの CPU の port でも、kernel の側の領域（HAL_SPACE_SYS）への map を拒む（user の program の領域だけを受ける）。
- 案（P6 の改訂 5、[proposed/uefi-runtime-window.md](ws307/proposed/uefi-runtime-window.md)）: kernel に新しい関数 kern_firmware_map を足し、driver はそれだけを呼ぶ。その下で amd64 の HAL の実装が、kernel の側の決まった 64 MiB の窓（firmware 専用）にだけ map を受けるようにする。hal.h（HAL の API の宣言）は変えない。
- 決めてほしい事: この HAL の変更を「hal.h を変えない実装の補完」（承認不要の扱い）と見てよいか、「HAL の契約の変更」として差分を見てから承認するか。別の道として、HAL も kernel も変えず loader が次の起動で Boot#### を書く案（C4）もあるが、起動した後の system から項目の一覧・削除ができない。
- 用語: loader = zedBSD を読み込む UEFI の program。SetVirtualAddressMap = firmware の関数を OS の仮想 address から呼べるよう配置を一度だけ知らせる UEFI の呼び出し（loader が呼ぶ、HAL とは無関係）。

### 2. WS313（ODROID-C5）: zedBSD-firmware の GitHub への push

- 背景: ユーザーの指示で、Hardkernel の公式の ODROID-C5 の image（36 MiB の .img.xz、無変更）と Amlogic の license の本文、Mali の GPU の firmware を zedBSD-firmware の repo に入れる。zedBSD の build はそこから bootloader の区画を切り出す。commit は 43662bd（branch q1-c5-firmware、親は GitHub の main の fcdfccf）。
- 問題: Q1 の push は権限の判定（Out-of-Place Publication）で止められた。回避はしていない。
- 決めてほしい事: push してよければ、端末に `! cd /home/awe/zedBSD-firmware && git push origin q1-c5-firmware:main` と打つ（このセッションで実行される）。push の後に Q1 が zedBSD の側の revision の commit を main に merge する。

### 3. WS291（GPU の描画の速さ）p020: VF2 の PowerVR の「model の 50 fps」の基準

- 背景: RDNA と NVIDIA では「Linux で約 50 fps になる重さの 3D の scene」を決め、zedBSD が同じ fps 以上を出す事を目標にした。VF2 の今の SD には PowerVR の GPU の driver を持つ Linux が無く、Linux の基準を立てられない。
- 今の扱い（Q1 の既定）: zedBSD の今の fps で約 40 fps になる scene を決め、profile を見て 50 fps 以上に上げる。P3 が実施中（copies 1 で 62 fps、10 で 31 fps）。
- 決めてほしい事: この既定でよいか、別の SD に Linux を入れて Linux の基準を立てるか。

### 4. WS331（driver から HAL への直の依存を除く）の残りの未決 3 点

- 背景: 表 A・B は 2026-10-30 に許可された。残りは次の 3 点（詳細は下の「WS331」の節と [phase001](ws331/phase001/phase.md) §4・§6）。
- (1) Raspberry Pi 4 の console の 2 関数（rpi_cons_suspend_output・resume_output）: HAL の early console（起動の早い段の画面の文字の出力）の中の状態と lock を使うので、driver にも kernel にも移せない。案 (a) 当面の例外として残す（P2 の推し）、(b) 汎用の HAL の口を足す（HAL の API の変更、承認が要る）、(c) driver が画面の文字を自分で描く作りにする。
- (2) build の macro HAL_ARCH_*・HAL_BOARD_*（6 file・16 行、どの CPU・board 向けかを compile の時に分ける印）を driver に残すか。
- (3) kernel の header 13 個が HAL の header を include していて、driver が間接に HAL の名前を見られる件を WS331 の範囲に入れるか。

## 決まった事

| 日付 | WS | 決定 | 記録 |
| --- | --- | --- | --- |
| 2026-10-30 | WS309 compositor の thread の整理 | p015 ゲームモードの V2: 1 は (a)（app は変更なし、全画面と窓の切り替えは compositor の都合で app は気づかない）、2 は private な Wayland の protocol `keiland_game_surface_v1` を作る、3 は標準の型の組み合わせで画像の memory を出す、4 は同じ画像の再 acquire で画面から外れた事を知る。p015 は planned に戻り、次のセッションで実装 | [ws309/ws.md](ws309/ws.md) |
| 2026-10-30 | BUG-364 GPU の fence | (A) display はどの GPU の fence も受ける（今の動きを正とする）。Q1 が gpu.c の comment を直し、BUG-364 を閉じた（876b64d55） | [BUG-364](bugs/BUG-364.md) |
| 2026-10-30 | WS074 Web ブラウザ | Codex を解約、WS074 は Claude（Q1）の管理下に | [ws074/ws.md](ws074/ws.md) |
| 2026-10-30 | WS328 libbrowser のベクトル描画と SVG | WS074（Web ブラウザ）を止めて、WS328 を先に実装する。次のセッションで p001（設計、Opus）から | [ws328/ws.md](ws328/ws.md) |

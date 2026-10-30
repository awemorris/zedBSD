# Cockpit

ユーザーが監視する状況の一枚（2026-10-30 ユーザーの指示、[master.md](master.md) の「Cockpit の運用」）。書き手は Q1 だけ。
最終更新: 2026-10-30 16:05（Q1）

## 担当（P1〜P8）

| 担当 | WS | WS の説明 | 今している事 | 作業の予定 |
| --- | --- | --- | --- | --- |
| P1 | [WS313](ws313/ws.md) | ODROID-C5（Amlogic S905X5M）の BSP と Mali の GPU | 返却。bootloader と Mali の firmware を menuconfig の package に（C5 の board で自動で有効）、zedBSD-firmware は local の commit | ユーザーの判断: Amlogic の bootloader の公開、license の同意（端末で）。その後 package 経由の image の build |
| P2 | [WS331](ws331/ws.md) | driver から HAL への直の依存を除き、kernel の口（kern_*）だけを使う形に | 返却。p001 の一覧（置き換え先の表 A、kernel に足す関数の表 B） | ユーザーが表 B と未決 4 点を決める → p002〜p006 |
| P3 | [WS291](ws291/ws.md) | GPU の描画の速さ（PowerVR の段、VF2） | 起動（phase-runner-high、q1715）。VF2 の画面が出ない（no boot framebuffer）の切り分けから | 40 fps 前後の scene を決め 50 fps 以上へ（基準はユーザーの回答で変える） |
| P4 | — | — | — | — |
| P5 | — | — | 返却（WS327 完了、2026-10-30） | — |
| P6 | [WS307](ws307/ws.md) | Keiland のアプリの graphical installer | 起動（phase-runner、q1711）。ws.md の Resume point から | p015 の QEMU の 3 回目 → p004 GTK4 → p008 → p014 → p007 → WS の完了 → 返却 |
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

### WS327 libkeiland の thread と socket の wrapper

| Phase | 内容 | 状態 |
| --- | --- | --- |
| p001〜p003 | 設計、thread の層、socket の層 | cleared |
| p004〜p006 | 旧い一括の Phase | canceled（標準廉価実装手順に組み直し） |
| p007 | 委譲の設計 | cleared |
| p008〜p012 | app の移行（Mailer・Music/Video・Photos・Settings・IME/picker/audio、Sonnet） | cleared |
| p013・p014 | thread-win32.c・socket-win32.c（mingw の compile まで） | cleared |
| p015 | 解説（8 章、08-network.c） | cleared |
| p016 | 受け入れ（P2） | in-progress（QEMU の回帰の途中、Video Player・Photos に未解析の FAIL） |
| p017 | 規約の見直し | planned |

進捗: 2026-10-30 Sonnet 9 回で実装。閉じた WS の試験 4 本を直した。Win32 の揺れ 17 項目は header を変えず、S7 は実装で直した。F-WS327-win32-build を登録。

### WS315 Realtek USB 2.5G・5G（vendor mode）— completed（2026-10-30）

5330 の RTL8156B で send 2370・recv 2373・同時 2268/2359（Linux 2353・2354・2346/2341）、10GbE も落ちない。5G（RTL8157）は資料だけ。試験は plan/tools/rtl815x/ に。F-WS315-rcvbuf は解決。

### WS307 graphical installer

| Phase | 内容 | 状態 |
| --- | --- | --- |
| p001・p003・p005・p006・p009〜p011・p013 | 仕様、build、道具、4096 sector、helper、app、通し、文書 | cleared |
| p002 | OS 名を zedBSD に、release の upload | cleared（push 後の CI は未確かめ） |
| p004 | package の開発の file、clang・libcxx・GTK4 のセルフビルド | in-progress |
| p007 | firmware 変数の kernel の driver（HAL を変えない） | in-progress（設計の下書き） |
| p008 | UFS からの起動の確かめ直し | in-progress |
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

### WS326 libmedia の H.264 の software の decoder（標準廉価実装手順）

| Phase | 内容 | 状態 |
| --- | --- | --- |
| p001〜p014、p016、p018、p020 | 設計と module 20 | cleared |
| p015・p017 | 補間と重み、B の direct | planned（次に Sonnet） |
| p019 | 再構成 | planned |
| p021〜p026 | 全体の流れ、適合、backend、速さ、規約、共通部分の汎用の名前へ | planned |

進捗: 2026-10-30 p007（参照の list）まで cleared。p021 の前に reflist の header の直し（B の POC type 0 の非存在の frame）。

## ユーザーの判断を待つ事

今は無し。

## 決まった事

| 日付 | WS | 決定 | 記録 |
| --- | --- | --- | --- |
| 2026-10-30 | WS309 compositor の thread の整理 | p015 ゲームモードの V2: 1 は (a)（app は変更なし、全画面と窓の切り替えは compositor の都合で app は気づかない）、2 は private な Wayland の protocol `keiland_game_surface_v1` を作る、3 は標準の型の組み合わせで画像の memory を出す、4 は同じ画像の再 acquire で画面から外れた事を知る。p015 は planned に戻り、次のセッションで実装 | [ws309/ws.md](ws309/ws.md) |
| 2026-10-30 | BUG-364 GPU の fence | (A) display はどの GPU の fence も受ける（今の動きを正とする）。Q1 が gpu.c の comment を直し、BUG-364 を閉じた（876b64d55） | [BUG-364](bugs/BUG-364.md) |
| 2026-10-30 | WS074 Web ブラウザ | Codex を解約、WS074 は Claude（Q1）の管理下に | [ws074/ws.md](ws074/ws.md) |
| 2026-10-30 | WS328 libbrowser のベクトル描画と SVG | WS074（Web ブラウザ）を止めて、WS328 を先に実装する。次のセッションで p001（設計、Opus）から | [ws328/ws.md](ws328/ws.md) |

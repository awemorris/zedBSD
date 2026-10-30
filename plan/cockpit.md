# Cockpit

ユーザーが監視する状況の一枚（2026-10-30 ユーザーの指示、[master.md](master.md) の「Cockpit の運用」）。書き手は Q1 だけ。
最終更新: 2026-10-30 11:25（Q1）

## 担当（P1〜P8）

| 担当 | WS | WS の説明 | 今している事 | 作業の予定 |
| --- | --- | --- | --- | --- |
| P1 | [WS009](ws009/ws.md) → [WS296](ws296/ws.md) | 文書（DOC-54 /dev/gpu の UAPI の文書）→ src/drivers の comment の管理情報の削除と規約 | DOC-54 は全 Phase cleared。完了の条件の不足を p025〜p028 に立て、p025（Wi-Fi の文書）を実行中 | WS009 completed → WS296 を標準廉価実装手順に組み直す設計 → Sonnet → WS297 |
| P2 | [WS327](ws327/ws.md) | libkeiland の thread と socket の薄い wrapper（POSIX と Win32） | p016 の受け入れ: 境界・build・host 試験 PASS、Win32 の揺れの一覧、QEMU の app の回帰へ | QEMU の回帰 → FreeBSD の build → p017 規約の見直し（Sonnet）→ WS の完了 |
| P3 | — | — | 返却（WS315 completed、main 834d10d88） | 次は WS221（Jumbo frame）・WS299（5330 の NVMe）（ユーザー: WS315 の後） |
| P4 | [WS307](ws307/ws.md) | Keiland のアプリの graphical installer | 返却（09:13）。次の 1 手は ws.md の Resume point | p015 の QEMU の 3 回目 → p004 clang・libcxx・GTK4 のセルフビルド → p008 → p014 → p007 firmware 変数の driver |
| P5 | [WS291](ws291/ws.md) | GPU の描画の速さ（NVIDIA RTX の段） | 返却（09:13、計測なし） | p010 desktop の測り直し → p012 以降で 3D を 50 fps 超えへ（デスクトップ機） |
| P6 | — | — | 返却（WS282・WS284 は main に merge 済み） | 未割当 |
| P7 | [WS313](ws313/ws.md) | ODROID-C5 の BSP と Mali | 返却（実機の結果待ち） | VF2 が空いたら C5 の実機の試験 |
| P8 | — | — | 未使用 | — |

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
| p025 | managed-wlan.md の書き直し（Opus） | in-progress（P1） |
| p026 | build の文書・索引・Status 行（Sonnet） | in-progress |
| p027 | 文書の検査の script を今の規則に直し plan/tools へ（Sonnet） | planned（p025・p026 の後） |
| p028 | clean build・QEMU 起動の再現、WS001 との照合（Opus） | planned |

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
| p016 | 受け入れ（P2） | in-progress |
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
| p015 | 画面の lock、sleep の既定 Never | in-progress（QEMU の 3 回目が要る） |

### WS291 GPU の描画の速さ（NVIDIA の段）

| Phase | 内容 | 状態 |
| --- | --- | --- |
| p001〜p006 | RDNA | cleared |
| p010 | NVIDIA desktop 60 fps | in-progress（WS309 の後の測り直し） |
| p010a・p011 | NVIDIA の executor、scene-5000 | cleared（49.5〜49.7 fps、NVK と同等） |
| p012〜 | 50 fps を明らかに超える改善 | planned |
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

| # | WS | 何を決めるか | 状況 |
| --- | --- | --- | --- |
| 1 | [WS309](ws309/phase015/phase.md) compositor の thread の整理 | p015 game mode の V2 の判断 1〜4（app 側の形、新しい private な Wayland の protocol を作るか、libvulkan の口、画面から外れた事の知り方） | 次のセッションの始めに聞く（ユーザーの指示） |
| 2 | [BUG-364](bugs/BUG-364.md) GPU の fence | 別の GPU の fence を承認なしに受けて良い設計か、companion の組を確かめるか | 直す担当が設計の文書で決められなければユーザーへ |
| 3 | [WS328](ws328/ws.md) libbrowser のベクトル描画と SVG | 着手の時期（WS074 の Codex のセッションと file が重なる） | 未定 |

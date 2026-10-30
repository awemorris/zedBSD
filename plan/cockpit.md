# Cockpit

ユーザーが監視する状況の一枚（2026-10-30 ユーザーの指示、[master.md](master.md) の「Cockpit の運用」）。書き手は Q1 だけ。
最終更新: 2026-10-30 13:25（Q1）

## 担当（P1〜P8）

| 担当 | WS | WS の説明 | 今している事 | 作業の予定 |
| --- | --- | --- | --- | --- |
| P1 | [WS009](ws009/ws.md) | 文書（DOC-54 /dev/gpu の UAPI の文書と、完了の条件の不足） | 返却（ラップアップ）。p028 の build・boot の再現の途中（native の image まで出来た、boot の判定は script の誤りで未判定、直した script は未実行） | 次のセッション: p028 の native の boot → hybrid・uefi・bios → WS009 completed → WS296（P6 の実装を元に組み直す）→ WS297 |
| P2 | [WS327](ws327/ws.md) | libkeiland の thread と socket の薄い wrapper（POSIX と Win32） | 返却（ラップアップ）。p016 の受け入れの途中: 境界・build・host 試験・FreeBSD は PASS、QEMU の回帰は Mail・Music・Settings まで | 次のセッション: QEMU の回帰の残り（Video Player の PAUSE・Photos の import の FAIL の解析）、BUG-367 の後に 08-network → p017 → WS の完了 |
| P3 | [BUG-367](bugs/BUG-367.md) | loopback の TCP で accept 側を先に close すると connect 側の close が戻らない | QEMU で再現と解析、今日の TCP の変更との関係を確かめて直す | BUG-367 → WS221（Jumbo frame）・WS299（5330 の NVMe） |
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

今は無し。

## 決まった事

| 日付 | WS | 決定 | 記録 |
| --- | --- | --- | --- |
| 2026-10-30 | WS309 compositor の thread の整理 | p015 ゲームモードの V2: 1 は (a)（app は変更なし、全画面と窓の切り替えは compositor の都合で app は気づかない）、2 は private な Wayland の protocol `keiland_game_surface_v1` を作る、3 は標準の型の組み合わせで画像の memory を出す、4 は同じ画像の再 acquire で画面から外れた事を知る。p015 は planned に戻り、次のセッションで実装 | [ws309/ws.md](ws309/ws.md) |
| 2026-10-30 | BUG-364 GPU の fence | (A) display はどの GPU の fence も受ける（今の動きを正とする）。Q1 が gpu.c の comment を直し、BUG-364 を閉じた（876b64d55） | [BUG-364](bugs/BUG-364.md) |
| 2026-10-30 | WS074 Web ブラウザ | Codex を解約、WS074 は Claude（Q1）の管理下に | [ws074/ws.md](ws074/ws.md) |
| 2026-10-30 | WS328 libbrowser のベクトル描画と SVG | WS074（Web ブラウザ）を止めて、WS328 を先に実装する。次のセッションで p001（設計、Opus）から | [ws328/ws.md](ws328/ws.md) |

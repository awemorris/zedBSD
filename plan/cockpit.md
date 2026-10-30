# Cockpit

ユーザーが監視する状況の一枚（2026-10-30 ユーザーの指示、[master.md](master.md) の「Cockpit の運用」）。書き手は Q1 だけ。
最終更新: 2026-10-30 13:00（Q1）

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

| # | WS | 一行の問い |
| --- | --- | --- |
| 1 | WS309 Keiland の compositor の thread の整理 | **決定済み（2026-10-30）**: 1 は (a)・切り替えは compositor の都合、2〜4 は推し。次のセッションで p015 の実装へ |
| 2 | BUG-364 GPU の fence の受け取り | display が、別の GPU が出した「描き終わり」の印を、無条件に受けてよいか |
| 3 | WS328 libbrowser のベクトル描画と SVG | いつ、誰と調整して始めるか（Codex のセッションの WS074 と file が重なる） |

### 1. WS309: ゲームモードの V2 をどう作るか

**背景**
- WS309 は、Keiland（zedBSD のデスクトップ）の compositor の thread の構成を整理する WS。p001〜p014・p016〜p019 は済み、残りはこの p015 だけ。
- **ゲームモード**は、全画面のゲームの画面を compositor が合成せず、そのまま画面に出す形。今は暫定で、compositor の画面の出力の係（T4 という thread）が、ゲームが描いた buffer を kernel の display の口（`GPU_DISPLAY_PRESENT`）で直に画面に出している。この直の口は Guardrail の例外として当面だけ許されている。
- 正式な形は「Vulkan の仕様の中」で作る（2026-10-29 のユーザーの決定 8）。2026-10-29 にユーザーは 2 つの形から **V2** を選んだ:
  - V1（copy）: compositor が、ゲームの buffer を自分の画面の画像へ毎 frame GPU で写してから出す。1080p で毎 frame 約 8 MB の写し。
  - **V2（貸し出し）**: compositor が、画面に出す画像（swapchain の image）そのものをゲームに貸し、ゲームがそこに直に描く。写しが無いので速いが、Vulkan の標準に無い部分を足す必要がある。
- 担当（P1）が V2 の作りを詰めたら、実装の前に決める事が 4 つ出た。

**用語**
- swapchain: 画面に出す画像を数枚まとめて持ち、順に描いて出すための Vulkan の仕組み。
- WSI（Window System Integration）: Vulkan が窓や画面に絵を出す部分。libvulkan（zedBSD の Vulkan の library）の中の `wsi-wayland.c` が Wayland の窓用。
- Wayland の protocol: app と compositor が話す約束事。private な protocol は zedBSD の中だけの約束事。
- acquire・present: swapchain から空いた画像を受け取る事・描いた画像を画面に出す事。
- fence: GPU の仕事が終わった事を知らせる印。
- export・import: GPU の memory を他の process に渡す事・受け取る事（fd で渡す）。
- Mesa: Linux・FreeBSD の標準の GPU の library。そこで動く app は zedBSD の libvulkan を使わない。

**決める事と推し**

1. **app 側の形** — **決定（2026-10-30 ユーザー）: (a)。全画面と窓の切り替えは compositor の都合で行い、app は気づかない。**
   - (a)【推し・決定】libvulkan の Wayland の WSI が、ゲームモードの間だけ、compositor から借りた画像を swapchain の画像として使う。ゲームの app は今の Vulkan の書き方のまま、変更なし。
   - (b) 新しい約束事を app が直に使う。mview などの app を書き直す。
   - 推しの理由: Vulkan を OS の正式な口にする決定 8 の趣旨に合い、app を書き直さずに済む。得失: Linux・FreeBSD で動く app（Mesa を使う）は (a) の恩恵を受けず、今の暫定の経路に残る。
2. **compositor と app の間に、新しい private な Wayland の protocol を作るか**（製品の判断）
   - 運ぶ物: 貸す画像の fd・形式・枚数、「この画像が空いた」の知らせ（acquire・release）、app の描き終わりの fence。名前の案は `keiland_game_surface_v1`。
   - 推し: 作る。(a) を選ぶなら、この約束事が無いと画像を貸せない。zedBSD の中だけの約束事なので、外の app には影響しない。
3. **libvulkan が画像の memory を外に出す口**
   - (a)【推し】標準の型の組み合わせで出す: swapchain を作る時に `VkExternalMemoryImageCreateInfo` を付けて受け、画像の memory を `vkGetMemoryFdKHR` で fd にする。この組み合わせは標準に書かれていないが、型は全部標準。
   - (b) libvulkan に zedBSD 独自の関数を足す。
   - 推しの理由: 独自の型を増やさない。費用: GPU の driver（venus・amdrdna・nvrtx・i915・mali・powervr）ごとに、swapchain の画像を外に出せる memory で作れるかの調べと直しが要る（他の担当の driver と重なるので Q1 が調整する）。
4. **画像が画面から外れた事の知り方**
   - 推し: 同じ画像を再び acquire できた事で知る（T4 が acquire を待つ）。Vulkan の present fence（`VK_EXT_swapchain_maintenance1`）は「present に使った資源を再び使ってよい」を示すだけで、画面から外れた事は示さないので使わない。

**決定（2026-10-30 ユーザー）**: 1 は (a)（全画面と窓の切り替えは compositor の都合で、app は気づかない）、2〜4 は推しのとおり。p015 は planned に戻り、実装に入れる。

### 2. BUG-364: display が別の GPU の fence を無条件に受けてよいか

**背景**
- 多くの板（Raspberry Pi、VisionFive 2、ODROID-C5）とノート PC（Intel の内蔵 GPU で画面、NVIDIA で描画）では、**画面を出す GPU（display）と描く GPU（render）が別の device**。zedBSD ではそれぞれ別の node（`/dev/gpu0`・`/dev/gpu1` など）になる。
- compositor は render の GPU で描き、「描き終わり」の fence を付けて display の GPU に画面を出させる。display は、その別の GPU の fence を待ってから画面を切り替える。これが「別の GPU の fence」の場面。
- kernel の source の comment は「display は、**承認した companion** の GPU の fence を受ける」と書くが、実際のコード（`gpu.c` の `gpu_fence_get`）は、fence がどの GPU の物かを一切比べず、どの GPU の fence でも受ける。WS009 の文書を書く途中（2026-10-30）で見つかった。

**用語**
- fence: GPU の仕事が終わった事を知らせる印。fd で他の process や他の GPU の node に渡せる。
- companion（ここでの意味）: display の node が「自分と組む render の GPU はこれ」と示す相手。`struct gpu_device_info` の `companion_id` の欄で示す仕組みはあるが、今は **全部の driver が 0（組の指定なし）** を入れている。header は「0 は好みが無いだけで、共有できない証明ではない」と書く。
- 承認: comment の言う「承認した」に当たる確かめは、コードのどこにも無い。

**選択肢**
- (A)【推し】どの GPU の fence も受ける今の動きを正しい設計とし、comment と header の文を直す。理由: fence は「待つ」だけで、相手の GPU の memory を読み書きしない（悪い fence で出来るのは画面の切り替えを遅らせる事くらいで、それは自分の app の画面だけ）。全部の driver が companion を 0 にしているので、(B) にすると今の Pi・VF2・C5 の画面の出し方が壊れる。ノート PC の Intel と NVIDIA の組のように、組が driver の外で決まる場合も受けられる。
- (B) companion の組だけを受ける。各 driver に正しい companion_id を入れさせ、組でない GPU の fence は断る。費用: driver ごとの組の調べと実装、試験。得: 誤った組み合わせを早く断れる。

**決まらない間**: 今の動き（どの GPU の fence も受ける）が続く。文書（docs/reference/gpu-fences.md）は今の動きを書いている。

### 3. WS328: libbrowser のベクトル描画と SVG をいつ始めるか

**背景**
- WS074 は zedBSD の Web ブラウザ（libbrowser と Browser の app）を作る WS で、**Claude ではなく、別の Codex のセッション（安い model）が main の checkout で直に作業している**。Q1 はそのセッションに指示を出せず、調整はユーザー経由になる。
- WS328 は、2026-10-30 のユーザーの指示で立てた WS: libbrowser の描画（`userland/desktop/libbrowser/paint/`）に、曲線・角丸・gradient・影のベクトル描画を Vulkan で足し、それで CSS の装飾と SVG の画像を描く。Phase は標準廉価実装手順（Opus が設計、Sonnet が実装）の形に書いてある。
- WS328 が触る file（paint/、CSS の描画、SVG の部分）は、Codex が WS074 で触っている file と重なる。同時に触ると、どちらかの作業が merge で衝突する。

**用語**
- Codex: OpenAI の coding agent。このプロジェクトでは WS074 を担当している別のセッション。
- 標準廉価実装手順: Opus が module の設計と header・試験の道具を作り、Sonnet 5.5 high が中身を実装する手順（docs/agent/spec-module-delegation.md）。

**選択肢**
- (A)【推し】設計の p001（Opus）だけ今始める。p001 は plan/ の文書と header の案だけで、Codex の file を触らない。p001 の成果に「WS328 が触る file の一覧」を入れ、それをユーザーが Codex のセッションに伝えて分担を決めてから、実装の Phase を始める。
- (B) WS074 が一区切りつくまで WS328 を始めない。衝突は無いが、SVG の対応が遅れる。
- (C) WS328 も Codex のセッションに任せる。file の衝突は起きないが、安い model に Vulkan のベクトル描画の設計まで任せる事になる。

**決まらない間**: WS328 は planning のまま動かない。

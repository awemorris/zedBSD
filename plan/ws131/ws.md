<!-- awesome-plan project=zedbsd record=ws131 -->

# WS131: libkeiland-backend の分離と libkeiui の吸収

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: p013 を cleared（後続の証拠で）。p002 は D7 の確認まで保留。2026-10-08 q910 P2 の照合: p001・p003〜p012・p014〜p023・p025〜p027 cleared（p003 の表を直した）。p002（設計、ユーザーのレビュー済み、D7 は確認中）と p013（T1-085 は流さない指示のまま、kl_ の改名は後の p021 で済み）は Q1 の判定、p024（規約と 3 OS の回帰）は規約がベータ3・Linux/FreeBSD が 10/13 以降。旧: planning（構成は決定、移行計画 [design.md](design.md) は p002 で作成、ユーザーのレビュー待ち））
Primary Milestone: MG006
Related Milestones: MG007（Linux・FreeBSD の Keiland）
Objectives: O2, O3
Parent: [Master](../master.md)
Queue: q629 / q629-i01（ws131-p002 の改訂、P3、設計のみ）
Resume point: 2026-10-03 p002 第 2 版（q629-i01）: design.md を D2 の変更（GPU の buffer も backend へ）・1 領域ずつの移行・名前の最終の規則（`KL_`・`kl_`・`KWL_`・`kwl_`）・ABI の版を上げない・review の 22 項目で改訂し、Phase を p003〜p024 の 22 個に切り直した。ユーザーのレビューと判断（D1・D3〜D13・D15・D16）の後に p003 の Queue。承認まで code は書かない。
2026-10-03 user（構成の決定）:「現状では、libkeilandは、コンポジタが使うOS抽象化機能と、クライアントが使うUIの共通化機能が、混在してしまっていますね。libkeiland-backend / libkeiland の2つに分解しましょう。libkeiland-backendはコンポジタが使うOS抽象化レイヤーの実装です。libkeilandは標準アプリが使うUIツールキットと、我々のコンポジタが持つ非標準機能をラップしたものです。電源管理、WiFi、ネットワーク、音声、PnPはlibkeiland-backendに実装して、各OSごとに、libkeiland-backend-zedbsd, libkeiland-backend-linux, libkeiland-backend-freebsdのような実装をソースツリーを分けて持ちましょう。」
→ WS131 の範囲を改訂: (1) libkeiland-backend（compositor が使う OS の抽象化層。電源管理・WiFi・network・音声・PnP、OS ごとに libkeiland-backend-zedbsd・-linux・-freebsd の別の source tree）、(2) libkeiland（標準 app の UI toolkit と、Keiland の compositor の非標準の機能の wrapper）。p001 の案 B（libkeiland = app の骨組み、libkeiui = widget）はこの分解の上で見直す。詳細の判断（app の WiFi・音声の操作の経路、compositor の画面・入力の OS の module を backend に含めるか、libkeiui の扱い）はユーザーに確認中。
2026-10-03 user:「Settingsの設定変更は、libkeilandを通じてコンポジタの拡張で通信して設定され、コンポジタが記録を行うように修正してください。」→ Settings（と他の app）の設定の変更・WiFi・network・音量・電源の操作は、libkeiland → compositor の拡張の protocol → compositor（→ libkeiland-backend）の経路にする。`~/.config/keiland/desktop.conf` への記録は compositor が行い、compositor が毎秒 stat で file を見に行く今の方式（BUG-125 の原因）は無くす。WS131 の p002 の設計に含め、compositor の拡張は WS113 の Settings → libkeiland → compositor と同じ仕組みに載せる。
2026-10-03 user（確認3）:「libkeiui は、libkeiland（UI ツールキット）に吸収します。名前は、keiland_をプレフィックスとする関数名のAPI群にします。WSに移行計画を立ててください。」→ 下の「構成と移行計画（2026-10-03）」。
<!-- awesome-plan-current:end -->

## 目標（2026-10-03 ユーザー）

「各アプリのウィンドウの作成を含めたGUI構築を共通化して、libkeilandにまとめることで、libkeilandをGUIツールキット、兼、デスクトップ機能の抽象化レイヤー、にできそうか、検討してほしいです。できそうならアプリはlibkeilandをツールキットにする構成に変更していきます。これらを1つのWSにしてください。」

## 範囲

1. **p001 検討**: 今の Keiland の app（Files・Settings・Text Editor・Image Viewer・PDF Viewer・Notes・Terminal・音楽（予定）ほか）が、窓の作成（Wayland の接続・xdg-shell・keiland_titlebar・glass・Vulkan/shm の描画・入力・clipboard・DnD・IME の text-input・menu・file chooser）をそれぞれどう書いているか、重複と差を棚卸しする。libkeiland（desktop の protocol の client、OS の module、install の path）と libkeiui（widget、WS090）の今の責務・API・ABI（KEILAND_VERSION・KUI_VERSION）を整理する。libkeiland を GUI toolkit 兼 desktop の抽象化層にする案（libkeiui を吸収するか、層を分けて libkeiland が窓と desktop を、libkeiui が widget を持つか）を、API の形・移行の手順・Linux/FreeBSD の build・既存の WS090 の移行 Phase との関係・危険とともに出し、実現性の結論と推奨をユーザーに示す。
2. p002〜: ユーザーの判断の後に、API の実装と app ごとの移行の Phase に分ける。

## 関係

[WS090](../ws090/ws.md)（libkeiui、widget の共有と app の移行）、[WS104](../ws104/ws.md)（desktop の OS の境界）、Guardrail の「compositor は libvulkan だけ」「Keiland の OS の境界」。

2026-10-03 user（確認1）:「新しい構成では、OS の抽象化は compositor 側の backend だけが持つことになります。そうすると、Settings からの操作は次の経路になります。理解は正しいです。Settings → libkeiland → compositor の拡張のプロトコル → compositor → libkeiland-backend → networkd や audiod」→ app は OS の抽象化を直接持たない。WiFi・network・音量・電源・PnP・設定の操作は compositor の拡張の protocol を通す（compositor の拡張の追加が要る）。確認2（compositor の画面・入力の OS の module を backend に含めるか）と確認3（libkeiui の扱い）は回答待ち。

2026-10-03 user（確認2）:「compositor のソースの中に、OS ごとの画面・入力の処理があります（wayland/linux/ の seat・logind・evdev、gpu-zedbsd.c など）。これも libkeiland-backend の OS ごとのツリーに移します。こうすることで、コンポジタの設計がきれいになりますね。」→ compositor の OS の module（seat・logind・evdev・KMS/GPU の gpu-zedbsd.c ほか）も libkeiland-backend-zedbsd・-linux・-freebsd へ移す。compositor の本体は OS に依存しない共通の source だけになる。Guardrail の「Keiland の OS の境界」（`<package>/zedbsd/<役割>-zedbsd.c` の配置、evdev の macro の例外、`plan/tools/keiland-os-boundary/check.sh`）と「compositor は libvulkan だけ」（gpu-zedbsd.c の macro で囲んだ ioctl は backend-zedbsd へ）は、WS131 の設計で改訂する。確認3（libkeiui の扱い）は回答待ち。


## 構成と移行計画（2026-10-03、ユーザーの決定から）

### 決まった構成

| 部品 | 使う者 | 中身 |
| --- | --- | --- |
| **libkeiland-backend**（OS ごとに別の source tree: `libkeiland-backend-zedbsd`・`-linux`・`-freebsd`、共通の interface の header） | compositor だけ | 電源管理・WiFi・network・音声・PnP（WS132）、compositor の画面・入力の OS の部分（seat・logind・evdev・KMS/GPU） |
| **libkeiland** | 標準 app | UI toolkit（今の libkeiui を吸収、API は全て `keiland_` の接頭辞）と、app の骨組み（app・窓・入力・clipboard・DnD・IME・menu・titlebar・glass）、compositor の非標準の機能の wrapper（設定・WiFi・network・音量・電源・PnP は compositor の拡張の protocol を通す） |
| **compositor**（zdesktop） | — | OS に依存しない共通の source だけ。libkeiland-backend を使い、拡張の protocol で app に機能を出す。`desktop.conf` などの設定の記録は compositor が行う |

app は OS の抽象化を直接持たない。例: Settings → libkeiland → compositor の拡張 → compositor → libkeiland-backend → networkd・audiod。

### Phase（2026-10-03 p002 第 2 版。詳細と根拠は [design.md](design.md) §0.2・§7）

第 2 版（q629-i01）の変更: ユーザーの決定で GPU の buffer も backend へ、backend を 1 領域ずつ（network → 音声 → 電源 → seat・session → 入力 → 表示 → GPU の buffer）の Phase に、名前の改名を 4 段に、ABI の版の段階を削除。design-reviewer の review の 22 項目を反映（design.md §11）。合計の目安 85〜120h。

| Phase | 目的 | Status | 依存 | 目安 |
| --- | --- | --- | --- | --- |
| [p001](phase001/phase.md) | 実現性の検討（[study.md](study.md)） | cleared | — | — |
| [p002](phase002/phase.md) | 移行計画（[design.md](design.md) 第 2 版、[rename-map.md](rename-map.md)・[rename-map-kwl.md](rename-map-kwl.md)） | in-progress（2026-10-03 ユーザーのレビュー済み、q630 で D4・D8・D9 を反映、D7 は確認中） | p001 | — |
| [p003](phase003/phase.md) | backend の土台と network | cleared（Q1 判定 2026-10-03、統合 bfeb2faf8。T1（QEMU、main d169912dd の image）: zdesktop-p013 PASS、settings-regress 8/8 PASS、C1・C2・C9 13/13 PASS。Linu…）（2026-10-08 q910 P2 の照合で phase.md に合わせた。旧: planning） | p002 の承認、P1 の network-zedbsd.c の merge（2026-10-03 満たされた）、P2 の終了 | 4〜5h |
| [p004](phase004/phase.md) | backend の音声 | cleared（q650、T2-009 の再試験 PASS） | p003 | 3〜4h |
| [p005](phase005/phase.md) | backend の電源 | cleared（q650、T1-040 C1 PASS、log の順は未確認） | p004 | 3〜4h |
| [p006](phase006/phase.md) | backend の seat・session | cleared（q650、T2-007・T2-011・T1-042） | p005 | 4〜5h |
| [p007](phase007/phase.md) | backend の入力 | cleared（q650、T1-044・T1-059） | p006 | 3〜4h |
| [p008](phase008/phase.md) | backend の表示 | cleared（q650、T2-013・T1-054） | p007 | 3〜4h |
| [p009](phase009/phase.md) | backend の GPU の buffer と境界の確定 | cleared（q659、T2-019・T1-060） | p008 | 4〜5h |
| [p010](phase010/phase.md) | 拡張の protocol `kl_system_manager_v1` と設定の記録（監視は残す） | cleared（q659、T2-020・T1-061・T1-062） | p004、P2 の BUG-125 の merge | 4〜5h |
| [p011](phase011/phase.md) | Settings を拡張へ、毎秒の監視の除去、libkeiland の OS を 0 に | cleared（q659、T2-021・022・024・T1-062） | p010、WS089・P1 の区切り | 4〜5h |
| [p012](phase012/phase.md) | libkeiui を libkeiland へ移す（名前は変えない） | cleared（q673、T2-027、main 5ec60b4） | p003 の merge、ベータ1 の app の区切り | 4h |
| [p013](phase013/phase.md) | 旧 libkeiui の名前を `kl_`・`KL_` に | cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-085 は流さず、kl_ の改名は後続の p021〜p023 と多数の回帰で確かめ済み。旧: in-progress（実装・host 試験済み、main に統合 2026-10-04。Q1 の最短の確認だけ PASS、回帰は test…） | p012 | 3〜4h |
| [p014](phase014/phase.md) | 旧 libkeiland の名前を `kl_`・`KL_` に | cleared（2026-10-05、T1-116 で FreeBSD の build と audit PASS） | p013・p011 | 3〜4h |
| [p015](phase015/phase.md) | app の骨組みの API（`kl_app`） | cleared（2026-10-05 Q1） | p014・p010 | 4〜5h |
| [p016](phase016/phase.md) | Text Editor | cleared（2026-10-07） | p015 | 3〜4h |
| [p017](phase017/phase.md) | PDF Viewer・Image Viewer | cleared（2026-10-07） | p016 | 3〜4h |
| [p018](phase018/phase.md) | Terminal・Notes | cleared（2026-10-07） | p016 | 4〜5h |
| [p019](phase019/phase.md) | Settings の窓 | cleared（2026-10-07） | p011・p016 | 4h |
| [p020](phase020/phase.md) | Files | cleared（2026-10-07） | p018 | 4〜5h |
| [p021](phase021/phase.md) | compositor の内部の名前を `kwl_`・`KWL_` に | cleared（2026-10-07） | p009・p011 | 3〜4h |
| [p022](phase022/phase.md) | compositor の log の接頭辞を `KWL ` に（試験 201 本と同時） | cleared（2026-10-07） | p021 | 3〜4h |
| [p025](phase025/phase.md) | browser の shell の窓（D7、p020 の後・p023 の前、WS074 との衝突は開始の前に Q1 がユーザーに確認） | cleared（2026-10-07、T1-311） | p020 | 4〜5h |
| [p023](phase023/phase.md) | 互換の除去・`keiland.h` 一本化・PnP の接続（WS134 の system monitor も移行の対象に含める） | cleared（2026-10-07） | p016〜p020・p025・p022（PnP は WS132）・ws134-p010 | 3〜4h |
| [p026](phase026/phase.md) | 公開の header を `userland/desktop/include/` に（`<keiland/keiland.h>` だけを app が include、`ui.h`・`compat.h` は内部、`<truetype/truetype.h>`・`<browser/browser.h>`、自前の Wayland の header は今のまま優先）（2026-10-04 user） | cleared（2026-10-07） | p013・p014 の統合（TQ-1） | 3〜4h |
| [p024](phase024/phase.md) | 全文規約と 3 OS の回帰、WS の完了 | planning | 全て | 4〜6h |

### 他の WS との関係（p002 の計画で更新、詳細は [design.md](design.md) §7.2・§8）

- [WS090](../ws090/ws.md)（libkeiui と app の移行）: libkeiui は p012・p013 で吸収される。WS090 の残りの窓の移行（p007 Settings・p010 Files）は WS131 の p019・p020 へ、描画の層（p009）と scroll（p015）と規約（p012）は WS131 の完了の後に `kl_` の名前で扱い、WS131 の間は WS090 を動かさない（D9 の決定。WS090 の表の改訂は Q1）。
- [WS113](../ws113/ws.md)（複数 display）: p005（表示の設定）を `kl_system_manager_v1` の version 2 の `get_displays` に載せる案（判断 D13）。WS113 p005 は WS131 p010 の後、WS113 p004 と WS131 p008・p009 は同時に流さない。
- [WS132](../ws132/ws.md)（PnP の通知）: backend の device 領域と `kl_system_devices_v1` の枠を p010 で作り、p023 で接続。
- [WS104](../ws104/ws.md)・Guardrail の「Keiland の OS の境界」「compositor は libvulkan だけ」: 改訂の本文は design.md §3.7、適用は段階的（p003・p009・p011 の merge、design.md §3.7、Q1）。checker の改訂は §3.8。
- WS099（BUG-125）: 原因の毎秒の `stat` を p011 で除く（Settings の移行と同じ Phase、review 1）。P2 の BUG-125 の作業の merge の後。
- WS112・WS108（package）: p012 で `libkeiui.so` が消える。backend は install しない（判断 D1）。
- WS097・WS096（GTK4・Qt6 の互換の書き下ろし）: p015 の API が複数の窓・popup・子の窓・cursor・scale を塞がない（design.md §6.4）。

### 時期（D8 の決定）

D8 の決定（2026-10-03 user）: 単独走行（N=1）で p003〜p024 を番号の順に流す。→ 改訂（2026-10-03 user「backend への移行はP1が今やりましょう。P2はシステムモニタを作っていますが、これは衝突しないように思えます。」）: P1 が他の担当（P2 の WS134 システムモニター、T1・T2）と並行で p004 から番号の順に流す。p011 の後に Q1 がユーザーに進み具合を報告する区切り。開始はユーザーの承認と P2 の終了の後に Q1 が指示する。WS090 の残りは WS131 の完了の後（D9）。

2026-10-03 user（D2 と進め方）:「はい、GPUバッファ管理もbackendに移したいです。ただし、backend化は一気に実装せず、1つずつ移行することで、正確に、確実に作業した方がいいと思います。」→ D2 は G2 をやめ、GPU の buffer の protocol（Linux・FreeBSD の zwp_linux_dmabuf_v1、zedBSD の keiland_gpu_buffer_v1）を backend へ移す（compositor は wl_buffer の寿命と OS に依らない画像の型だけ、backend は compositor が渡す小さな protocol の host の interface を使う）。backend 化は一度に行わず、領域を 1 つずつ（例: network → 音声 → 電源 → seat → 入力 → 表示 → GPU の buffer）移し、各段で 3 OS の build と回帰を確かめてから次へ進む形に Phase を切り直す（design-reviewer の review の後に P3 が design.md を改訂）。

2026-10-03 user（名前の規則）:「KUI_* については、KL_というプレフィックスに、最終的には変更しましょう。段階的な移行でいいです。Keiland関連はすべて、KL_に統一します。keiland-ui.hも、最終的には、keiland.hにまとめます。これも段階的な移行でよいです。マクロでないシンボル名は、kui_はkeiland_にマップします。」→ macro（と大文字の定数）は最終的に `KL_` に統一（`KUI_*` 166 個、`KEILAND_*` 143 個が対象。段階的）、macro でない symbol（関数・型・変数）は `kui_` → `keiland_`、公開の header は最終的に `keiland.h` 一つ（`keiland-ui.h` は段階的に統合）。design.md の §5 と rename-map を改訂する（対象の範囲の確認点は Q1 がユーザーに確認中）。

2026-10-03 user（名前の規則の更新、最新）:「シンボル名のkeiland_も、kl_にまとめる方針に変更します。…enumもKL_にします。KEILAND_BINDIR,KEILAND_LIBEXECDIRはそのままでOKです。ZWL_, zwl_はKWL_, kwl_がいいです。変更しましょう。」→ 最終の規則: macro・enum の定数は `KL_`（`KUI_*`・`KEILAND_*`。ただし install の path の `KEILAND_BINDIR`・`KEILAND_LIBEXECDIR` など paths.h の macro はそのまま）、関数・型・変数は `kl_`（`kui_*` も今の libkeiland の `keiland_*` も）、公開の header は最終的に `keiland.h` 一つ、compositor の内部の `ZWL_`・`zwl_` は `KWL_`・`kwl_`。いずれも段階的に移行（移行の間は旧名の互換）。既存の tree に `kl_`・`KL_`・`kwl_`・`KWL_` の名前は無い（Q1 が grep で確認）。注意: 試験の script 215 本が compositor の log の行の接頭辞「ZWL 」を読んでいるので、log の文字列の変更は symbol の改名と別の段にし、試験と一緒に変える。

2026-10-03 user（ABI）:「まだベータ版ですらないので、ABIは変更して問題ないです。ABIバージョンも上げなくていいです。」→ libkeiland.so の公開の名前の改名（keiland_→kl_ ほか）と libkeiui の吸収で ABI を変えてよく、KEILAND_VERSION などの ABI の版は上げない。旧名の互換は ABI のためには不要（段階的な移行の間の source の互換としてだけ必要なら置く）。

2026-10-03 user（D14 の残り）:「X server が、Wayland の規格のキーコードの定数を自分で持つようにしましょう。」「xattrは情報のパネルを名前だけを出す簡単な表示にします。」→ X server は `<uapi/input.h>` を include せず、Wayland の規格の（evdev の）key code の定数を自分で持つ（OS の依存を無くす）。Files の xattr は app 側の OS の依存として残し（タグの保存とコピーは今のまま、FreeBSD の extattr の読み替えは Files の中）、情報の panel は xattr の名前だけを出す簡単な表示にする。Terminal の pty は Terminal に残し macro の block で切り替える（前の決定）。Guardrail の app の規則はこの 3 つを除く desktop の OS の抽象化（電源・WiFi・network・音声・PnP・設定・seat・GPU）に限る。

2026-10-03 user（p002 第 2 版のレビュー）:「D4ですが、compositorはタッチIMEを実装しますので、そのUIの構築のためにlibkeilandをリンクしてOKです。不要ならリンクしなくてもOKです。」「D7は、browserのshellの窓が何のことなのかわかりません。」「D9は、この移行が完了してからでOKです。」「その他は承認します。」→ D1・D3・D5・D6・D8（単独走行で p003〜p024 を順に、p011 の後に進み具合を報告する区切り）・D10〜D13・D15・D16 は推奨のとおり承認。D4: compositor は touch IME の UI のために libkeiland を link してよい（不要なら link しない）。D9: WS090 の残りは WS131 の完了の後に扱う（WS131 の間は WS090 を動かさない。Settings・Files の窓の移行は WS131 の p019・p020 のまま）。D7 は Q1 が説明して再確認中。

2026-10-03 user（D7）:「D7,browserのshellもlibkeilandで書きましょう。」→ browser の shell（`userland/desktop/browser`）の自前の窓（約 1,800 行、libkeiui の present の写し）も WS131 で libkeiland の新しい API に移す。libbrowser（engine）は触らない。WS074 は Codex の作業中なので、Phase は app の移行の後（p020 の後、p023 の前）に置き、開始の前に Q1 が Codex の作業との衝突をユーザーに確かめる。

2026-10-03 user「FreeBSDでのテストはあとまわしにします。ホストがメモリを使い尽くしているためです。…一度適切なタイミングでホストの再起動が必要な見込みです。」→ WS131 の FreeBSD の build は未実施で進め、user の再開の指示まで FreeBSD の guest を起動しない（遅くとも p024 でまとめて）。試験の guest は担当ごとに同時に一つ。

2026-10-03 user「FreeBSDでのビルド確認は後回しで、書くだけにします。Linuxでのビルドはネイティブで行いましょう。再起動は、P4がLCDの問題を解決し、P2がTerminal改善を終了し、P1も作業を終了したあとにします。再起動後にP3を実行しましょう。」→ WS131 の FreeBSD は source を書くだけ（build・audit・guest は user の再開の指示まで行わない）。Linux は host の native の build。P3 は次の安全な区切りでラップアップし、host の再起動の後に再開する。

2026-10-03 Q1: WS134（system monitor、P2）の app を p023 の互換の除去の対象と前提に加えた（P2 の design.md §6 の依頼。足さないと B5 で FAIL）。monitor の system manager は `kl_system_manager_v1` の v3（WS113 が v2）。

2026-10-03 Q1: 設定（preferences）の部分は [WS135](../ws135/ws.md)（BUG-162、ユーザーの方針: 読み書きは libkeiland を通す、監視と通知の API、desktop.conf は compositor の内部で session の開始・終了だけ）が引き取る。p010・p011 の設定の行は WS135 の p001 の結果で書き換える（manager の枠と network・audio・power は WS131 に残る）。

2026-10-04 Q1: WS135 が completed。p010 の manager v1（capabilities）と settings、peer_uid、p011 の監視の thread と `keiland_preferences_*` の除去は WS135 で済んだ。p010 は manager に network・audio・power の interface を足す範囲、p011 は Settings の残り（network・audio の状態と feedback）と libkeiland の OS を 0 にする範囲に読み替える（依存に WS135 を足す）。

<!-- awesome-plan project=zedbsd record=ws309 -->

# WS309 Keiland の compositor の thread の構成を整理し、main loop が待たないようにする

- Master: [plan/master.md](../master.md)（2026-10-29 作成）。Primary Milestone: MG006。前身の調べ: [WS291](../ws291/ws.md) の
  [keiland-blocking.md](../ws291/keiland-blocking.md)（P2 q1661 の点検、branch `agent/p2-ws291i` にあり main に未 merge）。
- Status: incomplete（2026-10-30 q1662 P1: 設計 [design.md](design.md) 確定、p001〜p014・p016〜p019 cleared、p015 は判断待ちの uncleared）
- 目標: Keiland の compositor の main loop が frame を止める同期の待ちを持たないよう、thread を下の構成に整理する。ゲームモードの
  direct scanout は Vulkan display（`VK_KHR_display`）の仕様の中で実装する。

## ユーザーの決定（2026-10-28〜29、原文）

1. 「Keilandの直し方ですが、デバイスの検出はスレッドに追い出してください。他にもメインループにそういったブロッキング要素がないかチェックしてください。」
2. （P2 の点検の案 A〜G への答え）「すべてスレッド化しますが、いったん、全部列挙して、スレッドの数や分け方を検討しましょう。」
   （入力の検出の置き場所への答え）「先に回答したとおり、検討して整理します。アドホックにはしません。」
3. 「入力スレッドに入力デバイスの検出があるのはおかしいのでは。それだと入力が一瞬ブロックされます。I/O系はまとめていいです。むしろ入力デバイスの検出はI/O系にまとめるのがいのでは、と思ったのですが、poolということでスレッド数が明示されておらず、検討は必要と思います。」
4. 「なんでもfdのeventを多用するLinuxの設計が、POSIXの世界から逸脱していて、実用的だけど美しくないと、個人的に感じてしまいます。だから独自OSを作っているわけで。」
   「壁紙と画像、手書きの照合、の2つは、それぞれad-hocなスレッドに分離しましょう。さらに、blur、フォントの先読み、字形のラスタライズは、UXにクリティカルなので、単体の常駐スレッドにしましょう。」
5. 「確認ですが、現状でも、fence signalで非同期の通知はできているんですよね？提案のfd eventは不要ですよね？」（→ 不要と Q1 が確かめた）
   「daemonと接続するスレッドは、daemon接続ごとに単体に分離しましょう。」
6. 「手書きの照合はN文字のスレッドプールにしましょう。」
   「blurは単体のスレッドにしましょう。まずはCPUでいいです。将来GPUでぼかす実装を作って、どれくらい軽くなるのか調査しましょう。理由は、壁紙は非常に大きいので、GPU転送を考えると、高速化にならない可能性があることと、CPUで処理しても、一瞬であれば電力インパクトが小さく、排熱の問題も小さく、モバイルCPUも100ms未満であればスロットルが起こらず、しかもモバイルCPUでも1フレームのみのblurなら実用範囲の時間であると、経験的に思うからです。」
   「ゲームモードのpresentは単体の常設スレッドにしましょう。同期で待っていいです。presentの完了をpipeで知らせるのはちょっと美しくないけど、poll/selectでまとめて得られるからよさそうですか？」
7. 「(b)でよいです。コンポジタは純粋なVulkanのみに依存するという設計制約があるため、カーネルに機能追加したところで、それをVulkanの機能として使えないなら、呼び出しできないです。」
   （(b) = ゲームモードもアプリは buffer を compositor に送り、compositor が flip に積み、buffer が画面から外れたら release をアプリに返す。アプリは acquire で空きを待つ）
8. 「そうすると、ゲームモードについてはバックエンドがカーネルを呼んでもいいことにします。でも、せっかくならVulkan displayの仕様内で実装したいです。工数が多くてもかまいません。Vulkan displayをOSの正式APIに採用しているのが、このOSのウリの1つでもあるので。」

9. （2026-10-29 P1 の設計の判断の点への回答、Q1 経由）J1 game mode の Vulkan の形: ユーザーの回答（クリック）「V2 swapchain 共有 (推し)」。
   J4（Linux・FreeBSD の seat 経由の入力の open）: 推しのとおり当面は同期のまま残し、WS309 の後の Phase に置く（別の WS にしない）。J2・J3 は Q1 が「推しで OK（技術の判断）」。
   （Q1 の解釈: V2 = compositor が display の swapchain の image を external memory で export して client に渡し、client が直接描く。copy なし。libvulkan の WSI・display の部分を WS309 の範囲に含め P1 の所有に足す。）

- （Q1 の決定、2026-10-29、ユーザーの原文ではない）J5: 下の表の「spawner の process」は Q1 の解釈だったので取り下げ、io-file の thread から `posix_spawn`、pid・kill・`waitpid(WNOHANG)` は main（ユーザーには Q1 が伝え、覆れば P1 に知らせる）。
  J6: daemon の thread は main で blocking の I/O をする接続だけ（audiod・networkd・bluetoothd・phone・volumed の 5 本）。

## 決まった構成（解釈。上の原文が正）

| 種類 | thread | 持つ仕事 |
| --- | --- | --- |
| 常駐 | main | Wayland の protocol と状態だけ。待たない。完了は共通の wake pipe と poll で受ける |
| 常駐 | 描画 | GPU の queue の唯一の持ち主。取り込み・layout の遷移・shm の upload・片付けの GPU の待ちを命令として受ける |
| 常駐 | cursor | 今のまま |
| 常駐 | 画面の制御 | display の状態の変更の直列化（direct scanout の claim・release、swapchain の作り直し、modeset、回転、hotplug、backlight） |
| 常駐 | ゲームモードの present | present を同期で待つ。完了（前の buffer の release）は共通の wake pipe で main に |
| 常駐 | 入力 | evdev の read だけ（device の検出は入れない） |
| 常駐 | I/O-1 device | 入力の device の検出（scan・open・close）、電源の読み取り、`statfs`、network・電源・アカウントの処理 |
| 常駐 | I/O-2 file | 設定の読み書きと fsync（`displays.conf`・設定・`printers.conf`）、`apps.conf`・keymap・言語の file |
| 常駐 | font と字形 | 予備の font・emoji の先読み、字形の rasterize（face はこの thread だけが持つ） |
| 常駐 | blur | 壁紙のすりガラスの blur（まず CPU） |
| 常駐 | daemon ×5 | audiod、networkd、bluetoothd、bluetoothd（電話）、volumed。接続ごとに 1 本、blocking の I/O |
| pool | 手書きの照合 | N 本（N の決め方は未決）。同じ文字は最新の 1 件だけ |
| その都度 | 壁紙・画像の decode | 1 回ごと |
| process | （取り下げ、J5） | 起動は io-file の thread から posix_spawn（main は fork しない） |

- fd の event を kernel に足す案は取らない（GPU の完了は VkFence、client の完了は明示の同期の fence、present は thread が同期で待つ）。
- ゲームモード: 当面は backend（`libkeiland-backend-zedbsd/scanout-zedbsd.c`）が GPU の ioctl を直に呼んでよい（決定 8）。目標は
  Vulkan display（`VK_KHR_display` の swapchain、buffer が画面から外れた事は present fence（`VK_KHR_swapchain_maintenance1`）など
  Vulkan の機能）で実装する（工数は問わない）。複数の flip を queue に積む kernel の機能も、libvulkan が Vulkan の機能として出す形でだけ使う。

## Future Work の候補

- GPU で blur を実装し、CPU との速さ・電力を比べる（決定 6）。

## 未決

- （決定）手書きの照合の pool の N: 2026-10-29 01:3x ユーザーの回答（クリック）「CPU 数で固定 (推し)」→ 起動の時に CPU の数から決めた固定の本数（Q1 の選択肢の例: CPU の数 − 1、上限 4。具体の式は P1 の設計で）。
- P2 の q1661（`agent/p2-ws291i` 3d9306f02 の backend の入力の検出の worker、467f1cabc・1a0ead286 の記録）: アドホックにしない決定 2 により
  main に merge せず保留。この WS の I/O-1 の実装で置き換える。main には b574fbf6a（開けなかった node を覚える）が入っている。

## 設計

- [design.md](design.md)（2026-10-29 P1、thread の一覧・本数・待ちの点・queue と通知・ゲームモード・判断の点 J1〜J4）。

## Resume point

- 2026-10-30 P1（q1662-i10、branch `agent/p1-ws309i`）: p016・p017・p018 cleared（[p016](phase016/phase.md)・[p017](phase017/phase.md)・[p018](phase018/phase.md)）。
  残りは p015（ゲームモードの V2、[phase.md](phase015/phase.md) の「要る判断」1〜4）だけ。p017 の C9 の p052・p076 は WS309 の前の image でも FAIL（load 15〜23）、
  load の低い時の再試行と実機（backlight・lid・ゲームモードの SLOW_PASS）は未実施。
- 2026-10-29 P1（q1662-i10、ラップアップ、branch `agent/p1-ws309i`）: p015 は uncleared（判断待ち、[phase.md](phase015/phase.md) の「要る判断」1〜4）。
  p016 は調べと手順まで（[phase.md](phase016/phase.md) の「手順」1 から、Linux・FreeBSD の guest の試験が要る）。その後 p017（QEMU の回帰）・p018（規約の全文の見直し）。
- 2026-10-29 P1（q1662-i10）: p014 cleared（[phase.md](phase014/phase.md)。compositor は新しい libc を要る）。
- 2026-10-29 P1（q1662-i10、branch `agent/p1-ws309i`）: p013 cleared（T4 の lane `present`、claim と release は T3、[phase.md](phase013/phase.md)）。次は p014（process の起動と clipboard）、その後 p016。
- 2026-10-29 22:5x P1（q1662-i09、branch `agent/p1-ws309i`、799e52d4c の後の記録）: p012 cleared（RTX の BUG-353 の試験 PASS、[phase.md](phase012/phase.md) の「結果」）。次は p013（present の常駐の thread）。
- 2026-10-29 15:1x P1（q1662-i09、branch `agent/p1-ws309i`、WIP 5f4a72a75）: p012 は park と drain の非同期・T3 の lane・出力の移動・window mode の open・ゲームモードの入りまで（[phase.md](phase012/phase.md) の「結果」）。RTX の BUG-353 は枠待ち。
- 2026-10-29 14:3x P1（q1662-i08、branch `agent/p1-ws309h`、ラップアップ）: p012 は調べと実装の手順まで（[phase.md](phase012/phase.md)、source の変更なし）。次の起動は p012 の手順 1（非同期の park と drain）から。
- 2026-10-29 14:2x P1（q1662-i08、branch `agent/p1-ws309h`）: p011 cleared（手書きの照合の pool hand-1〜hand-N）。次は p012（display の thread）。
- 2026-10-29 13:4x P1（q1662-i08、branch `agent/p1-ws309h`）: p010 cleared（出力の大きさの変化の QEMU `plan/ws309/tests/resize-guest.sh` PASS、回転は p012 へ）。次は p011（手書きの照合の pool）。
- 2026-10-29 13:3x P1（q1662-i07、branch `agent/p1-ws309g`、b6b5b788c）: p010 は T9（blur の lane）・A1（loader は decode だけ）・landscape・`kwl_glass_resize` まで、QEMU `grave-guest.sh` PASS。次は [phase.md](phase010/phase.md) の「残り」1（回転は p012 と一緒かを Q1 に）・2。
- 2026-10-29 13:1x P1（q1662-i07、branch `agent/p1-ws309g`、4b9d7d3d7）: p009 cleared（墓へ全ての解放、client の切断の quiesce を消す、glass の waits を消す。QEMU `plan/ws309/tests/grave-guest.sh` PASS）。次は p010。
- 2026-10-29 12:3x P1（q1662-i06、branch `agent/p1-ws309f`、b5416a6fa）: p009 は遅らせた破棄の仕組みと shm の image まで。次は [phase.md](phase009/phase.md) の「残り」1 から。
- 2026-10-29 12:2x P1（q1662-i06、branch `agent/p1-ws309f`）: p008 cleared（fence 付きの task、QEMU `plan/ws309/tests/task-guest.sh` PASS、host `plan/ws309/tests/render-task/run.sh` PASS）。次は p009（遅らせた破棄、[phase.md](phase008/phase.md) の「設計からの変え方」の task の解放の待ちを置き換える）。
- 2026-10-29 04:54 P1（q1662-i05、branch `agent/p1-ws309e`、ラップアップ）: source の変更なし。p008 の「残り」1 から（[phase.md](phase008/phase.md) の再開点に build の手順と task の実装の案）。
- 2026-10-29 P1（q1662-i04、branch `agent/p1-ws309d`）: p006・p007 cleared（QEMU `daemon-guest.sh`・`font-guest.sh` PASS）。p008 は queue の lock まで（6eab69097）、次は fence 付きの task（[phase.md](phase008/phase.md) の「残り」1〜6）。
- 2026-10-29 03:28 P1（q1662-i03）: p004 cleared（io-device を全 backend で起動、電源・details・sysmon を io-device へ、machine は A2 machine-read、QEMU `plan/ws309/tests/io-device2-guest.sh` PASS）。次は p006（daemon ×5）。
- 2026-10-29 03:1x P1（q1662-i03、branch `agent/p1-ws309c`）: p005 cleared（言語・apps.conf・account・鍵・printers の open、QEMU `plan/ws309/tests/io-read-guest.sh` PASS）。p019 cleared（P2 の q1668）。次は p004。
- 2026-10-29 02:4x P1（q1662-i02、branch `agent/p1-ws309b`、d5fb0bd08・c3b274d51）: p005 は displays.conf（`saved` は書き終わりで答える）・desktop.conf の Log Out・printers.conf まで済み。
  次は p005 の残り（言語の catalog・apps.conf の読み、account・鍵の保存、[phase.md](phase005/phase.md) の「残り」）、その後 p004。QEMU は `plan/ws309/tests/io-file-guest.sh`。
- 2026-10-29 02:3x P1（q1662-i01）: p001・p002・p003 cleared、p019（shot の送りを thread へ）は QEMU で再現せず RDNA の確かめ待ち。
  次は p005（io-file: displays.conf の `saved` の答えを書き終わりで返す。displays-shell.c の `displays_pending` の保留の仕組みを広げる案）か p004（io-device の残り）。
  QEMU の image は `plan/ws309/tests/config-amd64-p1.mk`（BUILD=build/ws309/img）、試験は `plan/ws309/tests/` の io-device-guest.sh・shot-fifo.sh。

## Phase

| ID | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [ws309-p001](design.md) | 設計の確定（thread の一覧・本数・待ちの点・queue と通知・ゲームモード、敵対的レビューの反映） | cleared（判断の点 J1〜J6 は全て決定） | — |
| [ws309-p002](phase002/phase.md) | 共通の部品: thread の台帳・lane（常駐と pool）、host 試験 | cleared | p001 |
| [ws309-p003](phase003/phase.md) | io-device: 入力の device の検出と close を thread へ（zedBSD） | cleared（実機は未実施） | p002 |
| [ws309-p004](phase004/phase.md) | io-device の残り: 電源・network の詳細・sysmon（machine は A2 の machine-read、detach の ack は p016） | cleared（実機・Linux/FreeBSD は未実施） | p003 |
| [ws309-p005](phase005/phase.md) | io-file: displays.conf・desktop.conf・printers.conf の書き込み（`saved` は書き終わりで答える）、apps.conf・言語・printers.conf の読み、account・鍵の保存 | cleared（account の実行と実機は未実施） | p002 |
| [ws309-p006](phase006/phase.md) | daemon ×5 の thread（zedBSD の backend、wake pipe を backend の open の前に） | cleared（実機は未実施） | p002、J6 |
| [ws309-p007](phase007/phase.md) | font の thread: 予備の font・emoji の先読み、rasterize、color emoji の decode | cleared（実機は未実施） | p002 |
| [ws309-p008](phase008/phase.md) | render を GPU の queue の唯一の持ち主に (1): fence 付きの task と queue の lock | cleared（FreeBSD の build と実機は未実施） | p002 |
| [ws309-p009](phase009/phase.md) | 同 (2)(3): 遅らせた破棄（serial・descriptor set・二重の image）、quiesce と main の `vkDeviceWaitIdle` を消す | cleared（実機・FreeBSD の build は未実施。park の手順の wait は p012） | p008 |
| [ws309-p010](phase010/phase.md) | blur の thread とその都度の decode（壁紙・出力の大きさ） | cleared（QEMU `grave-guest.sh`・`resize-guest.sh`。回転の `kwl_glass_size_prepare` は p012 へ（2026-10-29 Q1 の判断）。実機は未実施） | p009 |
| [ws309-p011](phase011/phase.md) | 手書きの照合の pool（N = min(max(CPU−1,1),4)） | cleared（host `hand-pool/run.sh`・QEMU `hand-guest.sh`。実機は未実施） | p002 |
| [ws309-p012](phase012/phase.md) | display の thread（swapchain・modeset・回転・hotplug の列挙・scanout の可否と claim・release・backlight、seat の pause）。回転の壁紙（`kwl_glass_size_prepare`）を A1・T9 へ（p010 から、2026-10-29 Q1 の判断）。BUG-353 をこの中で直し、受け入れに RTX の再現（`plan/ws291/tests/rtx-mview-order.sh`、実機の枠は Q1、2026-10-29 Q1 の決定） | cleared（q1662-i09: 非同期の park と drain・T3・移動・window mode・ゲームモード・回転・head・列挙・backlight・lid、glass maps と層の絵を保つ。QEMU resize・grave・hand・scanout・rotate PASS、RTX `rtx-bug353-pair.sh` PASS（BUG-353）。backlight・lid の実機は未実施） | p009 |
| [ws309-p013](phase013/phase.md) | present の常駐の thread（暫定の ioctl、共通の wake） | cleared（QEMU `scanout-guest.sh`・`resize-guest.sh` PASS。実機・arm64・riscv64・FreeBSD の build は未実施） | p012 |
| [ws309-p014](phase014/phase.md) | process の起動（J5 の形）と clipboard の書き込みを poll で | cleared（libc に addclosefrom_np（Q1 の決定）、QEMU `spawn-guest.sh`・libc の `spawn-closefrom/run.sh`、host `clip-write` PASS。input method の起動と実機は未実施） | p005、J5 |
| [ws309-p015](phase015/phase.md) | game mode の V2: libvulkan の display の swapchain の image の export、compositor の受け渡し | uncleared（2026-10-29: 実装の前に判断が要る（client 側の形・新しい private の protocol・libvulkan の export の口）、phase.md の「要る判断」） | p013 |
| [ws309-p016](phase016/phase.md) | Linux・FreeBSD の seat 経由の入力の open（J4）と seat の callback の detach の ack を wake で受ける形（p004 から） | cleared（seat の let-go・paused_done の契約、logind の TakeDevice を bus で、Linux の gdm の guest PASS、FreeBSD の native の build と host-seat-freebsd PASS。let-go を待つ経路は host 試験だけ） | p003 |
| [ws309-p017](phase017/phase.md) | QEMU の回帰（Venus: 基準・GPU の境界・`KWL THREADS`・`KWL PERF pass`）と遅さの注入の host 試験 | cleared（WS309 の guest 試験 16・forge・fence PASS、C1・C2 PASS、C9 の p052・p076 は前の image でも FAIL（load）、p126 などの kei→user の追従、io-file の安定化） | p003〜p014 |
| [ws309-p018](phase018/phase.md) | 規約の全文の見直し | cleared（31 file の書き方、style-check の WS309 の行 88→9） | 全て |
| [ws309-p019](phase019/phase.md) | 試験の screen capture の画素の送りを thread へ（keiland-shot の後の SURFACE_LOST、Q1 の依頼） | cleared（RDNA の実機は P2 の q1668） | p002 |
- 2026-10-30 Q1: p015 の「要る判断」1〜4 は、ユーザー「WS309の判断は、次回セッション開始時に聞いてくれるように記録してください。」により、次のセッションの始めにユーザーに聞く（master の「次のセッションの始めにする事」）。

## p015 の判断 1（2026-10-30 ユーザー、原文）

「WS309の判断ですが、appは意識せずに、コンポジタの都合で、全画面とウィンドウを切り替えられるようにしたいです。そうすると、app側は(a)しかありえないのではないでしょうか。」
→ 解釈（Q1）: 判断 1 は (a)（libvulkan の Wayland の WSI が、game mode の間だけ compositor から借りた画像を swapchain の画像として使う。app は変更なし）。加えて、全画面と窓の切り替えは compositor が決め、app は気づかない（libvulkan の WSI が窓の経路と借りた画像の経路を内部で切り替える。swapchain の作り直しを app に求めない形を設計の目標にする）。判断 2〜4 は未回答。

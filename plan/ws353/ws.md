<!-- awesome-plan project=zedbsd record=ws353 -->

# WS353: compositor・libvulkan・kernel の待ち方の作り直し（事象は push、周期で見ない、合成と client の buffer の管理を分ける）

Status: incomplete（2026-11-13 p001 の設計をユーザーが承認、p002 から実装）
Primary Milestone: MG006
Related Milestones: MG004
Parent: [Master](../master.md)
Owner: Q1
関係: [WS352](../ws352/ws.md)（idle の短い poll を無くす。全件の報告 [poll-report.md](../ws352/poll-report.md)。p002 はこの WS の設計を待つ）、[WS309](../ws309/ws.md)（compositor の thread の構成。この WS はその上に、起きるきっかけと終わりの知らせ方を足す）。

## 目標

Keiland の compositor（`userland/desktop/wayland/`、`libkeiland-backend-zedbsd/`）と libvulkan（`userland/desktop/libvulkan/`）、要るなら kernel（`src/drivers/gpu/` など）と WSI を、次の形に作り直す設計と移行の計画を作り、実装する。

- idle の時は全部の thread が timeout 無しで block して眠り、外の事象（入力・client の要求・hotplug・daemon の知らせ・予定した時刻）でだけ起きる。
- 状態の変化は持ち主が push で知らせる。周期で読み直す code と、取りこぼしの保険の timeout を置かない。

## ユーザーの決定（2026-11-13、要点。原文ではない）

1. poll と timeout が多過ぎ、時間の制限が要る所も多過ぎるのは設計が悪いからで、根本の設計の変更をする（poll-report.md を受けて）。
2. display の hotplug（と同じ形の display の event）の fence は、libvulkan が登録された fence に、変化が起きた時に signal を送るだけの形にする。今の「問われた時に kernel に topology の番号を問い合わせて判定する」形をやめる。block するかどうかは本質ではない。これを最初に直す。
3. compositor のあるべき動きは、Q1 が示した形（下の「合意した形」）で一致。
4. render thread は、合成用の buffer の数で数える形（swapchain の image・frame の slot の数。足りなければ fence で空きを待つ）。一般の作法。
5. client の buffer の管理と、compositor の合成用の buffer の管理は別物。分けて設計する。
6. 設計は Opus 5.5 high の担当が作り、敵対的レビューは Fable の medium で行う。
7. 設計ができたらユーザーがレビューし、OK なら実装に進む（実装の Phase はユーザーの OK まで起動しない）。
8. 2026-11-13 ユーザーが設計（design.md、Fable のレビューの反映の後、main 73f38d7cb5）を承認した。Q1 の推しのとおり: 未決 1 は C（main の締め切りは timer の thread、他の thread は自分の待ちに締め切り）、未決 2 は A（main は client の socket・wake pipe に加え client の acquire の fence と backend の事象の fd 3 つも timeout 無しで待つ）、未決 3 は当面「完了（REAPED の serial）を見てから返す」を正とし、明示の同期（p013）は WS353 の完了の条件から外す。合意した形からの変更 (a) render は slot が飛んでいる間は最古の fence を待つ、(b) 表示の完了は present の fence（shown の thread）で待つ、も承認。

## 合意した形（設計の前提。設計の担当はこれを具体にする）

- main: client の socket と共通の wake pipe だけを待ち、Wayland の protocol と状態を扱う。自分では待たず、周期を持たない。
- 待つ物ごとに名前の付いた thread（WS309 の表を土台）: 入力（evdev）、hotplug（kernel が signal する fence）、display（依頼の列）、render（依頼と、slot が足りない時の fence）、present（present の完了）、io-device・io-file・font・blur・手書き（仕事の列）、daemon ×5（socket）、締め切り（timer）。終わりや変化は必ず wake で知らせ、flag を置いて覗かせない。
- 描くきっかけは画面が変わる時だけ（commit・cursor・動いている animation）。animation は動いている間だけ frame の完了（vblank の歩調）で進める。frame の callback は present の完了で送る。
- 締め切り（key repeat・消灯・ping の打ち切りなど）は、その機能が予定を持つ間だけ timer に登録する。main loop の poll の timeout に畳まない。
- 合成用の buffer: swapchain の image が数え上げにあたる（acquire は空きが無ければ block）、slot の fence は使い回す前に待つ。待つ物は一度に一つ。1 ms の見回りを無くす。cursor thread も同じ。
- client の buffer: 持ち主は client。返す条件は「新しい buffer に置き換わった」と「それを読んだ GPU の仕事が終わった」。shm は写し終えたら返す。GPU の buffer は明示の同期・暗黙の同期・完了を見てから返す、のどれか（実態を調べて設計が決める）。合成の slot に結び付けない（今は `compose.c` の `slot->held` で結び付いている）。
- libvulkan: kernel の job を持たない fence（display の event）の `vkWaitForFences` の 1 ms の問い合わせを無くす。

## 未決（設計で案を出し、Q1 がユーザーに上げる）

- timer を 1 本の thread で持つか、締め切りを持つ各 thread が自分で timed wait するか。
- main が client の socket を自分で poll するか、それも thread に出すか。
- client の buffer の GPU の同期の方式。
- （2026-11-13 P1 の案と推し、[design.md](design.md) §8。同日 q2056-i02 で Fable のレビューを受けて 3 の推しを変えた）:
  - 1 は C（main の締め切りは timer の thread、他の thread は自分の待ちに期限）。
  - 2 は A: main が listener・client の socket・wake pipe に加えて、client の acquire fence（commit に付く kernel の fence の fd）と backend の事象の fd 3 つ（seat の service・/dev/system の事象・notify の bus）を timeout 無しで poll する。合意した形（main は client の socket と wake pipe だけを待つ）との違いはこの待つ fd の種類だけで、周期と timeout は持たない。理由: acquire fence は main が commit を採る（surface の状態の変更）ので直に見るのが hop が無い、backend の 3 つは WS309 の J6 で非 blocking なので main に残すと決めた物。文字どおり（B）にするなら受けの thread を 1 本足す（p005 に足せば済む）。どちらにするかをユーザーに確かめたい。
  - 3 は「完了を見てから返す」を当面の正の形（render の REAPED の serial で返す。render が飛んでいる slot を待って完了を直ちに知らせる形になり、返しは読みの GPU の完了の直後）。明示の同期（release fence、p013）は得が読みの GPU の時間だけで前提の kernel の作業が大きいので、要る理由が測れてから。WS353 の完了の条件から p013 を外す事を推す（前の推しは明示を目標、それまで表示の完了の serial で返す、だった）。
- 設計の変更でユーザーに確かめたい点（2026-11-13 q2056-i02、design.md の先頭と §2.5・§5.6）: (a) render は「slot が足りない時だけ最古の fence を待つ」から「slot が飛んでいる間は最古の fence を待つ（無ければ依頼の cond）」に強めた（main が完了で次に進むので、見ないと互いに止まる。待つ物は一度に一つのまま、合成の完了の thread も置かない）。(b) 表示の完了の待ちは、標準の `vkWaitForPresentKHR` が swapchain の外部同期を要り別の thread から使えないので、present の fence（`VK_EXT_swapchain_maintenance1`）を新しい thread shown が待つ形にした（thread は 17 本。present の fence の標準の意味は「present が受け付けられた」で、zedBSD の libvulkan では kernel の present の返り、i915 では前の frame の latch と今の frame の flip の armed の時）。

## 設計

- [design.md](design.md)（2026-11-13 P1、q2056-i01。kernel・libvulkan・compositor・WSI の待ち方、WS352 の 91 件の対応表 §12、未決の案 §8）。
- [design-review.md](design-review.md)（2026-11-13 Fable の敵対的レビュー、重大 4・中 9・軽 12。末尾の「反映」の節に各指摘の扱い、2026-11-13 P1 q2056-i02）。

## Phase

| Phase | 中身 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計（design.md）と移行の計画（実装の Phase の表と各 phase.md）、Fable の敵対的レビューの反映 | cleared（2026-11-13 ユーザーの承認、main 73f38d7cb5） | — |
| [p002](phase002/phase.md) | kernel: display の topology の事象の fence（HOTPLUG、`GPU_DISPLAY_FENCE`、driver の push `drv_gpu_display_changed`） | cleared（2026-11-14 Q1 判定。P1 q2057 8cf85a7102（merge 879c02d83d）。latency の FAIL は試験の harness の止まりと venus の events_clear の欠けで、BUG-431（P2）で直し、同じ試験の 2 回が全 step PASS、latency 266・255 ms） | p001 |
| [p003](phase003/phase.md) | libvulkan: hotplug の fence を kernel の fence に、未提出の fence の待ち（1 ms を消す） | cleared（2026-11-14 P1 q2059、Q1 の確認待ち。branch `agent/p1-ws353c`、libvulkan f93eb33fab・c44b5ba42c。3 機種の build warning 0、QEMU の 13 step PASS（30 s の idle は poll 1 回、別の fence の 200 回の submit で起きない、300 回の競いで取りこぼし 0、16 の上限、plug ≤983 ms・unplug ≤214 ms は kernel の fence と同時、device lost で返る）。any-wait の空回り（既存）も直した） | p002 |
| [p004](phase004/phase.md) | compositor: hotplug の thread、`kwl_output_tick` の 250 ms を消し出力の追従を事象で | cleared（2026-11-14 P1 q2060、Q1 の確認待ち。branch `agent/p1-ws353d`、462c65c226・8d8c3e908c。3 機種の build warning 0、host 試験（直した `plan/ws113/tests/host-output-switch.sh`）PASS、QEMU の follow 試験 2 回 PASS（60 s の idle で知らせ・列挙 0、head の抜き差しの追従、表示中の display を抜くと移動、stop で thread の起床 = 知らせ 4＋stop 1））。game mode の QEMU は未実施（host 試験で代えた） | p003 |
| [p005](phase005/phase.md) | compositor: timer の thread、thread の結果の flag を wake に、入力の scan を事象だけに、去った client の buffer の解放の歩調を timer と事象に | cleared（2026-11-14 P1 q2061、Q1 の確認待ち。branch `agent/p1-ws353e`。WS352 の branch を b05187933a を除いて merge 3e9c426f74、fence の poll の名指し 56a43ffd37、p005 386b86fa94・c3fc188117。3 機種の build warning 0、host 試験（timer・retire の歩調、TSan）PASS、QEMU の main-wake 試験 PASS（idle 約 2 pass/s・1 timeout/s、壁紙・shot・USB の keyboard）、p002・p003 の QEMU の流し直し PASS。2-03 の 100 ms は理由（sessiond の chown）を書いて残した。media の helper の QEMU は未実施） | p004、WS352 p002 の merge（推し） |
| [p006](phase006/phase.md) | compositor: 締め切り A（session・system・protocol）を timer へ | cleared（2026-11-14 P1 q2062、Q1 の確認待ち。branch `agent/p1-ws353f`、bc7c2c6082・94ad48a837 と記録の commit。対象の 17 の期限を機能ごとの timer に（`kwl_timer_set`、`kwl_client_fail`）。3 機種の build warning 0、host 試験（timer・ws197 phone・ws309 clip-write。後の 2 つは基点で link できず直した）PASS、QEMU の `qemu-deadlines.sh` 全 step PASS（desktop の再起動 2003〜2086 ms、IME の再起動と give-up、sysmon、shot の打ち切り、`--timeout` の late 2 ms、idle は p005 と同じ）。sleep と handoff は毎 pass の tick が残り期限だけ timer（p008 へ）。session が要る期限（idle の lock・sleep・Log Out）などの QEMU は未実施） | p005 |
| [p007](phase007/phase.md) | compositor: 締め切り B（UI）を timer へ | cleared（2026-11-14 P1 q2065、Q1 の確認待ち。branch `agent/p1-ws353g`、55bfdf3cd5・9385b94210 と記録の commit。対象の 17 の期限を機能ごとの timer に（8 は handler が仕事、7 は tick が timer を arm し判定は tick、p008 へ）、`kwl_titlebar_tick` を消した、KWL PERF に timer の名前ごとの数。3 機種の build warning 0、host 試験 PASS、QEMU の `qemu-ui-deadlines.sh` 全 step PASS（notify-log 10.3 s、band 501 ms、titlebar の repeat 41、apps bar、rotate、OSK の手書き、layer の静けさ）、idle の閾値を 10 s に 2 pass・1 timeout に狭め main-wake・deadlines も PASS。client の無い idle は timer 0〜1（network-grace）と時計の分。printers の 1 s は kl_system を開く app がある間だけ残る（道が無い、design.md 2-04）。lock の card・volume などの QEMU は未実施） | p005 |
| [p008](phase008/phase.md) | compositor: animation・frame の pacing・時計、main の poll を -1 に | cleared（2026-11-14 Q1 判定。P1 q2068 4f4112c981、main に merge d795c0b9a4。main の poll は -1、client の無い idle は 60 s で timeouts 0（timer は clock と network-grace だけ）。状態を追う tick（settings・system・IME・sleep・displays・glass・desktop）は事象・timer の pass ごとに走るが、時刻の比べも自分を起こす事も無いので受け入れた（Q1 の判断）。QEMU の animation・main-wake・ui-deadlines・deadlines PASS） | p006・p007 |
| [p009](phase009/phase.md) | libvulkan: present の fence（`VK_EXT_swapchain_maintenance1` とその依存、swapchain の wake の絞り） | cleared（2026-11-14 P2 q2063、Q1 の確認待ち。branch `agent/p2-ws353a`、9f6e89c68c・05fe512c28。3 機種の build warning 0、QEMU（Venus）の present-fence 試験 11 step が 2 回 PASS（100 個の fence が順に返る、間隔は present の歩調 40 ms と同じ、unplug・destroy で全部返る、他の swapchain の acquire は poll 1 回）。Wayland の swapchain の fence と既存の回帰は未実施） | p003 |
| [p010](phase010/phase.md) | compositor: shown の thread（T17）が present の fence を待ち callback・animation をそこから、client の buffer を slot から外し REAPED の serial で返す | cleared（2026-11-14 Q1 判定。P2 q2073 f129cf3a30、main に merge。ユーザーの決定で (A): frame の callback は present の fence（表示の受け付け）で送る。Venus での遅さ（wlshm 41→68 ms/frame）は問題にせず、実機を基準にする（p016 の 5330 で測る）） | p008・p009・p011 |
| [p011](phase011/phase.md) | compositor: render は合成の buffer を slot の数で回す（飛んでいる間は最古の fence を無期限で REAPED、無ければ cond、acquire は timeout 0 と何も飛んでいない時だけ block、1 ms を消す） | cleared（2026-11-14 P2 q2067、Q1 の確認待ち。branch `agent/p2-ws353d`、c8dcf0dd41・4aae3a92f0・97697a00ac・bbdd14db9f。1 ms・50 ms・100 ms の待ちを消し、待ちは最古の fence・何も飛んでいない時の acquire・cond の 3 つだけ（全部無期限）。3 機種の build warning 0、host の `render-step` 試験 PASS（基点では 11 件 FAIL）、QEMU（Venus）の `qemu-render-wait.sh` 2 回とも全 step PASS（描画 2 種、turn、焼きの後の描画、game mode の出入り、fence の待ちは job ごとに 1 回）、基点と交互に流して描画の速さ・遅れが同じ。S4 は全 driver で present が上限を持って返る（i915 の suspend の間だけ resume まで、実害は無いと読んだ）。WS309 の guest 試験そのもの・出力の移動・実機は未実施） | p004 |
| [p012](phase012/phase.md) | compositor: cursor の thread（acquire は変化の後だけ、copy の fence をすぐ待つ） | cleared（2026-11-14 P1 q2076-i01、Q1 の確認待ち。branch `agent/p1-ws353k` 8a198aee05。build warning 0、host 試験 PASS、QEMU は回帰だけ PASS。cursor plane の実の確かめは未実施で p016 へ） | p011 |
| [p013](phase013/phase.md) | client の buffer の明示の同期（release fence、全 render の backend の `GPU_CAP_FENCE`、protocol rev 4、WSI） | planned（未決 3 の決定の後。推しは要る理由が出るまで着手しない） | p010、未決 3 |
| [p014](phase014/phase.md) | kernel・libvulkan: refresh の事象の fence、`VULKAN_SYNC_POLL_NS` を消す | cleared（2026-11-14 P2 q2064、Q1 の確認待ち。branch `agent/p2-ws353b`、66b803a706・bc77e214ab。3 機種の build warning 0、QEMU（Venus）の refresh-fence 試験 2 回 PASS（100 個が登録から 1 周期以内、間隔は周期の整数倍、1 fence に poll 1 回、登録の無い 8 s は kernel の push・待ち 0）。vblank の割り込みの push は amdrdna だけ、venus・i915・nvrtx は kernel の refresh watcher が driver の refresh op で待つ（i915・nvrtx は登録の間 kernel に 1 ms の見回りが残る。登録するのは今は試験だけ）。実機は未実施） | p003 |
| [p015](phase015/phase.md) | backend と WSI の小さな直し（network の答えの fd、WSI の 50 ms の slice） | cleared（2026-11-14 P2 q2066、Q1 の確認待ち。branch `agent/p2-ws353c`、a7f8bb3f34・3fabe10acb。3 機種の build warning 0、host の daemon-link 試験 PASS、QEMU（Venus）の `qemu-answer-wake.sh` 全 step PASS（network の link の待ちは idle 3 s で 0・3 s の join で 2、POLLERR 0、答えは 0〜1 ms で取れる。WSI は短い timeout の待ち 0、compositor を止めた間は 1 s の pace で進む）。MSG_PEEK の回避も消した。settings の画面の操作は未実施） | p001（独立） |
| [p016](phase016/phase.md) | 試験: QEMU の回帰と 5330 の idle（電力・PC2・thread ごとの起床） | planned | p002〜p015 |
| [p017](phase017/phase.md) | 規約の全文の見直し | planned | p016 |
| [p018](phase018/phase.md) | 入力の device の hotplug を sessiond に開かせる（fd の受け渡し、100 ms の settle の scan を消す） | cleared（2026-11-14 Q1 判定。P1 q2072 e8b9a14a0d、main に merge f4d831e0ea。sessiond が seat の socket で OPEN に fd を渡す、compositor の devices の thread、settle の scan を消した。QEMU の seat-hotplug 3 回目 全 PASS（挿した keyboard・mouse は sessiond の OPEN 1 回、scan 0、idle の timeout 0）。sessiond 無しの道は挿す step が 3 回とも PASS、他の step は host の負荷で時間切れ。Log Out・password の login の QEMU は未実施） | p008 |
| [p019](phase019/phase.md) | printers の 1 s の見回りを消す（printd の push、一覧は設定から、libkeiland は使う時だけ作る） | cleared（2026-11-14 Q1 判定。P1 q2074 810dba2742、main に merge 8dddb662f6。周期の check を消し、表示の時に 1 回読む。print.c の folder 作りの欠陥も直した。QEMU で Settings を開いた 30 s の printers の timer 0、追加・印刷・削除が見える） | p008 |

## ユーザーの決定（続き）

- 2026-11-14 ユーザーから次の趣旨の連絡と決定を受けた: printd は常駐でなく印刷の spool の間だけ起動する（新しい決定ではなく、前からの実装の振る舞い）。printd が起動している間は、pipe で printd に接続して push を受けるのが理想。kernel に機能（file の変化の通知など）は足さない（kernel を簡素に保ち、POSIX の中核から離れないため）。Settings は spool の中身を見る時だけ printd と通信すればよく、printer の一覧を得るのに printd と通信する必要は無い。→ 2-04（printers の 1 s）の本来の形はこの方針で後の Phase に。 同日ユーザーから、Settings は表示した時点の printer を表示すればよく、監視（変化の追従）は要らないかもしれない、という趣旨の意見を受けた（p019 は一覧を表示の時に 1 回読む形にする）。
- 2026-11-14 ユーザーから次の趣旨の決定を受けた（入力の device の hotplug と seat）: compositor は device 全般の hotplug を見張る thread を 1 本持つ（監視の対象は入力の device だけではないので、この thread は残す）。sessiond 経由で compositor が動いている時は、compositor の hotplug の thread が /dev/system を見張り、入力の device の追加を見たら sessiond に開く事を要求する（sessiond が開いて fd を渡す形。chown の追い越しの 100 ms の再試行を無くす）。sessiond 経由で起動していない時は、hotplug の thread が入力の device を自分で open する。（同日のユーザーの訂正を反映: /dev/system を見張るのは sessiond 経由の時も compositor） 2026-11-14 ユーザーが、hotplug の thread は 2 本（display の hotplug は p004 の thread、それ以外の device 全般は /dev/system を見張る 1 本）という読みを承認した（display の hotplug は Vulkan の fence で待つため fd の待ちとまとめられない）。→ 2-03 の本来の形として後の Phase に。
- 2026-11-14: 10.0.10.25 は記録（plan/tools/hw5330/README.md、plan/ws029/i915-design.md）では Dell Latitude 5330 の以前の IP（ssh の alias `solaris10-man`、hostname chaos、Debian 13）で、今の 10.0.30.3 と同じ機械。WS014 の Venus の renderer はこの 5330 の Linux で build された物。Q1 は別の server と取り違えていた（ユーザーに訂正を伝えた）。
- 2026-11-14 ユーザーの決定: p010 の frame の callback は (A)（present の fence で送る）。Venus（QEMU）で遅い事は問題にせず、実機を基準にする。
- 2026-11-14 ユーザーの決定: sessiond が入力の device を開いて SCM_RIGHTS で fd を送るので、利用者への chown は要らない（zedBSD では chown をやめる）。sessiond を通さないのは Linux と FreeBSD の backend だけで、Linux には logind がある。FreeBSD では /dev/input が利用者に chown されている前提で進める。

## Q1 の判断

- 2026-11-14（p005、P1 の問い）: 入力の device の追加の後の 100 ms の settle の scan は、kernel の順の問題ではなく（`src/drivers/generic/input.c:362〜376` は node の登録の後に ADD の事象）、sessiond が同じ ADD の事象で node を利用者へ chown する前は開けない（BUG-264）ため。p005 では 2 s の周期の scan だけ消し、settle は事象の後だけの物として残す。本来の形（sessiond が seat を与えた事を session の control の fd で push し、compositor はそれで scan）は、後の Phase として足す（p018 の候補、p005 の申し送りを見て Q1 が立てる）。

- 2026-11-14（p014 の返却）: refresh の fence は amdrdna だけが vblank の割り込みで push、i915・nvrtx は登録がある間 kernel の watcher が 1 ms で見る。今は compositor 等が refresh の fence を登録しないので idle に効かない。本来の形（i915 の vblank の push）は Future Work F-WS353-i915-vblank-push に置いた。

## Resume point

- 2026-11-14 09:4x P1（q2076-i01、branch `agent/p1-ws353k`）: p012 を実装（cursor の thread の 1 ms の再試行・5 ms の pin の見回り・present の経路の 100 ms を消し、acquire は変化の後に timeout 0、空きが無ければ `UINT64_MAX`、copy の fence は present の直後に無期限で待ち pin を外す、期限は移動の hold だけ、`KWL CURSOR_PLANE stopped` の数え）。3 機種の build warning 0、host 試験（`plan/ws353/tests/cursor-step/run.sh`）PASS、QEMU（`qemu-render-wait.sh`、回帰だけ）PASS（[phase012](phase012/phase.md) の結果）。Venus には cursor plane が無いので plane の経路は p016 の実機で（手順は phase012 の申し送り）。
- 2026-11-14 08:4x P1（q2074-i01、branch `agent/p1-ws353j`）: p019 を実装（printers の 1 s の check を消し、一覧は printers の object を作った時に 1 回読む、printd の状態は socket の push のまま、protocol を破った printd の打ち切りは `kl_backend_print_due` の timer、`print.c` が `~/.config` から folder を作る）。3 機種の build warning 0、host 試験（`plan/ws353/tests/printers/run.sh`）PASS、QEMU（新しい `plan/ws353/tests/qemu-printers.sh`）PASS（[phase019](phase019/phase.md) の結果）。
- 2026-11-14 07:4x P1（q2072-i01、branch `agent/p1-ws353i`）: p018 を実装（sessiond の seat の socket で入力の device を開かせる、compositor の devices の thread、settle の scan を消した）。3 機種の build warning 0、host 試験 PASS、QEMU の `qemu-seat-hotplug.sh` PASS（[phase018](phase018/phase.md) の結果）。Log Out の handoff の QEMU と実機は未実施。
- 2026-11-14 08:31 P2（q2073-i01、branch `agent/p2-ws353e`）: p010 を実装（T17 shown と present fence、frame の記録で callback を output の SHOWN に、buffer の `read_drawing` と返し待ち（objects.c）、animation の一歩を SHOWN に、shm の写しを image ごとに）。3 機種の build warning 0、host 試験と QEMU（新しい `plan/ws353/tests/qemu-shown.sh`、`qemu-render-wait.sh`、`qemu-animation.sh`）PASS（[phase010](phase010/phase.md) の結果）。Venus での client の速さの低下を受けるかが Q1 の判断待ち。次は p012（cursor）。
- 2026-11-14 06:2x P1（q2068-i01、branch `agent/p1-ws353h`）: p008 を実装（`kwl_due`・`due_ms` を消し poll は -1、animation は frame の完了で、期限・pacing・入力の scan・時計は timer、子の回収は SIGCHLD の後、network・phone・machine の取り込みは wake の後）。3 機種の build warning 0、host 試験 PASS、QEMU（新しい `plan/ws353/tests/qemu-animation.sh`、idle の閾値を timeout 0 にした main-wake・deadlines・ui-deadlines）PASS（[phase008](phase008/phase.md) の結果）。状態に追従する tick が毎 pass に残る点は Q1 の判断待ち。次は p010（p009・p011 の後）。
- 2026-11-14 06:5x P2（q2067-i01、branch `agent/p2-ws353d`）: p011 を実装（render の step を「依頼を先に、出来る事が無ければ最古の fence を無期限、何も飛んでいなければ acquire か cond」に、park の間は want を取らない、`KWL RENDER stopped` の数えと `KWL LAT completed`）。3 機種の build warning 0、host 試験（`plan/ws353/tests/render-step/run.sh`）と QEMU（`plan/ws353/tests/qemu-render-wait.sh`）PASS（[phase011](phase011/phase.md) の結果）。`plan/ws309/tests/render-task/` は merge の時に Q1 が消す。次は p012（cursor）、p010。
- 2026-11-14 05:03 P2（q2066-i01、branch `agent/p2-ws353c`）: p015 を実装（network の link の poll に要求と scan の答えの fd を入れ 10 ms と MSG_PEEK の回避を消す、scan の打ち切りを期限に。WSI の 50 ms の slice を消し期限まで眠る、1 s の pace は残す）。3 機種の build warning 0、host 試験と QEMU（`plan/ws353/tests/qemu-answer-wake.sh`）PASS（[phase015](phase015/phase.md) の結果）。
- 2026-11-14 P2（q2064-i01、branch `agent/p2-ws353b`）: p014 を実装（kernel の REFRESH の登録、`drv_gpu_display_refreshed`、任意の `refresh_arm` op、push の無い device の refresh watcher、amdrdna の frame 割り込みの push、libvulkan の refresh fence を kernel の fence に、`VULKAN_SYNC_POLL_NS` を消した）。3 機種の build warning 0、QEMU（`plan/ws353/tests/qemu-refresh-fence.sh`）2 回 PASS（[phase014](phase014/phase.md) の結果）。i915 の vblank の割り込みの push は Q1 が後の Phase か Bug に（要る物は phase014 の「Q1 の問いへの答え」）。
- 2026-11-14 05:2x P1（q2065-i01、branch `agent/p1-ws353g`）: p007 を実装（UI の 17 の期限を機能ごとの timer に、`KWL PERF timers` の診断）。3 機種の build は warning 0、host 試験 PASS、QEMU（`plan/ws353/tests/qemu-ui-deadlines.sh`、aat-input で駆動）5 回目で全 step PASS、idle の閾値を狭めた main-wake・deadlines も PASS（[phase007](phase007/phase.md) の結果）。printers の 1 s は「道が無い」として design.md 2-04 に。次は p008・p011。
- 2026-11-14 04:01 P2（q2063-i01、branch `agent/p2-ws353a`）: p009 を実装（宣言は固定の v1.3.269 から `maintain-external.noct` で `vulkan_external.h` に、dispatch 200 件、present の fence は job の retire で signal、swapchain の wake は chain ごと）。x86_64・arm64・riscv64 の build は warning 0、QEMU（`plan/ws353/tests/qemu-present-fence.sh`）2 回 PASS（[phase009](phase009/phase.md) の結果）。Venus では 1 present が refresh 3 つ分（40 ms）で、fence の間隔はそれと同じ。次は p010（T17）。Wayland の swapchain の fence は未実施。
- 2026-11-14 04:1x P1（q2062-i01、branch `agent/p1-ws353f`）: p006 を実装（対象の期限を機能ごとの timer に、fatal client は `kwl_client_fail`、sleep と handoff は tick を残し期限だけ timer）。3 機種の build は warning 0、host 試験 PASS、QEMU（`plan/ws353/tests/qemu-deadlines.sh`）は 3 回目で全 step PASS（[phase006](phase006/phase.md) の結果）。ws131 の host-system.sh は基点で壊れていて、別の Phase で直す事を推す。次は p007・p011。
- 2026-11-14 03:2x P1（q2061-i01、branch `agent/p1-ws353e`）: WS352 の branch を b05187933a を除いて merge（3e9c426f74）、kernel の fence の poll を名指しに（56a43ffd37）、p005 を実装（timer の thread と heap、去った client の解放の歩調、名残りの 7 つを wake・事象・timer に、zedBSD の入力の scan は事象だけ）。3 機種の build は warning 0、host 試験（`plan/ws353/tests/timer/run.sh`）と QEMU（`plan/ws353/tests/qemu-main-wake.sh`）PASS、p002・p003 の QEMU も PASS（[phase005](phase005/phase.md) の結果）。2-03 の 100 ms の settle は残し、push の形（sessiond が seat を与えた事を知らせる）は Q1 が後の Phase に足す。次は p006・p007・p011。
- 2026-11-14 02:32 P1（q2060-i01、branch `agent/p1-ws353d`）: p004 を実装（output-switch.c の hotplug の thread、`kwl_output_tick` と 250 ms を消し、出力の追従は `kwl_output_follow_later` で求めて main の take で走らせる、window mode の外・display の change・列挙の間は保留し終わりで wake）。x86_64・arm64・riscv64 の build は warning 0、host 試験（`plan/ws113/tests/host-output-switch.sh`、今の API に直した）PASS、QEMU（`plan/ws353/tests/qemu-hotplug-follow.sh`）2 回 PASS（[phase004](phase004/phase.md) の結果）。次は p005・p011。
- 2026-11-14 02:05 P1（q2059-i01、branch `agent/p1-ws353c`）: p003 を実装（libvulkan の hotplug fence は node ごとの kernel の fence の fd、未提出の fence の待ちは fence を名指す wake の token、眠る前の reap）。x86_64・arm64・riscv64 の build は warning 0、QEMU（`plan/ws353/tests/qemu-hotplug-wait.sh`、host の renderer は `build-venus-renderer.sh` で作る）は全 step PASS（[phase003](phase003/phase.md) の結果）。次は p004（compositor の hotplug の thread）。QEMU の plug の latency が 1 s に近い（kernel の fence も同時なので libvulkan より下、原因は未調査）。
- 2026-11-14 00:46 P1（q2057-i01、branch `agent/p1-ws353b`）: p002 の kernel を実装（`GPU_DISPLAY_FENCE`＝'G' 54・`GPU_DISPLAY_FENCE_CANCEL`＝55、`drv_gpu_display_changed`、venus・i915・amdrdna・ghci の push）。x86_64・arm64・riscv64 の build は warning 0、i915 の host 試験 31 本 PASS、QEMU は idle・wait・cancel・limit・race が PASS、latency は venus の driver が idle の間の head の変化を見ない既存の問題で FAIL（[phase002](phase002/phase.md) の結果）。次は Q1 の判断（venus の Bug、p002 の受け入れ）。
- 2026-11-13 23:2x P1（q2056-i02、branch `agent/p1-ws353`）: Fable の敵対的レビュー（[design-review.md](design-review.md)）の 25 件を code で確かめて design.md と phase.md に反映した（扱いの表は design-review.md の「反映」）。主な変更: render は飛んでいる間は最古の fence を待つ（S1）、表示の完了は present の fence を新しい thread shown が待つ（S2、present wait の外部同期）、client の buffer は REAPED の serial で返す、未決 3 の推しの変更、Phase の依存（p011 は p004 の後、p010 は p011 の後、p015 は独立）。source は変えていない。次はユーザーのレビュー（未決 1〜3 と上の確かめたい点）、その後 p002。
- 2026-11-13 22:5x P1（q2056-i01、branch `agent/p1-ws353`）: [design.md](design.md) と p002〜p017 の phase.md を置いた（source は変えていない）。HAL・Makefile の変更は要らない形にした。未決 3 つの案と推しは design.md §8。WS352 の branch は b05187933a を除いて merge を推す（§7）。次は Q1 の Fable の敵対的レビューの反映（p001 の残り）。
- 2026-11-13 Q1: 立てた。p001 を P1（Opus 5.5 high）に。

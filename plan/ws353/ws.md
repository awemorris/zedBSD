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
| [p005](phase005/phase.md) | compositor: timer の thread、thread の結果の flag を wake に、入力の scan を事象だけに、去った client の buffer の解放の歩調を timer と事象に | planned | p004、WS352 p002 の merge（推し） |
| [p006](phase006/phase.md) | compositor: 締め切り A（session・system・protocol）を timer へ | planned | p005 |
| [p007](phase007/phase.md) | compositor: 締め切り B（UI）を timer へ | planned | p005 |
| [p008](phase008/phase.md) | compositor: animation・frame の pacing・時計、main の poll を -1 に | planned | p006・p007 |
| [p009](phase009/phase.md) | libvulkan: present の fence（`VK_EXT_swapchain_maintenance1` とその依存、swapchain の wake の絞り） | planned | p003 |
| [p010](phase010/phase.md) | compositor: shown の thread（T17）が present の fence を待ち callback・animation をそこから、client の buffer を slot から外し REAPED の serial で返す | planned | p008・p009・p011 |
| [p011](phase011/phase.md) | compositor: render は合成の buffer を slot の数で回す（飛んでいる間は最古の fence を無期限で REAPED、無ければ cond、acquire は timeout 0 と何も飛んでいない時だけ block、1 ms を消す） | planned | p004 |
| [p012](phase012/phase.md) | compositor: cursor の thread（acquire は変化の後だけ、copy の fence をすぐ待つ） | planned | p011 |
| [p013](phase013/phase.md) | client の buffer の明示の同期（release fence、全 render の backend の `GPU_CAP_FENCE`、protocol rev 4、WSI） | planned（未決 3 の決定の後。推しは要る理由が出るまで着手しない） | p010、未決 3 |
| [p014](phase014/phase.md) | kernel・libvulkan: refresh の事象の fence、`VULKAN_SYNC_POLL_NS` を消す | planned | p003 |
| [p015](phase015/phase.md) | backend と WSI の小さな直し（network の答えの fd、WSI の 50 ms の slice） | planned | p001（独立） |
| [p016](phase016/phase.md) | 試験: QEMU の回帰と 5330 の idle（電力・PC2・thread ごとの起床） | planned | p002〜p015 |
| [p017](phase017/phase.md) | 規約の全文の見直し | planned | p016 |

## Resume point

- 2026-11-14 02:45 P1（q2060-i01、branch `agent/p1-ws353d`）: p004 を実装（output-switch.c の hotplug の thread、`kwl_output_tick` と 250 ms を消し、出力の追従は `kwl_output_follow_later` で求めて main の take で走らせる、window mode の外・display の change・列挙の間は保留し終わりで wake）。x86_64・arm64・riscv64 の build は warning 0、host 試験（`plan/ws113/tests/host-output-switch.sh`、今の API に直した）PASS、QEMU（`plan/ws353/tests/qemu-hotplug-follow.sh`）2 回 PASS（[phase004](phase004/phase.md) の結果）。次は p005・p011。
- 2026-11-14 02:05 P1（q2059-i01、branch `agent/p1-ws353c`）: p003 を実装（libvulkan の hotplug fence は node ごとの kernel の fence の fd、未提出の fence の待ちは fence を名指す wake の token、眠る前の reap）。x86_64・arm64・riscv64 の build は warning 0、QEMU（`plan/ws353/tests/qemu-hotplug-wait.sh`、host の renderer は `build-venus-renderer.sh` で作る）は全 step PASS（[phase003](phase003/phase.md) の結果）。次は p004（compositor の hotplug の thread）。QEMU の plug の latency が 1 s に近い（kernel の fence も同時なので libvulkan より下、原因は未調査）。
- 2026-11-14 00:46 P1（q2057-i01、branch `agent/p1-ws353b`）: p002 の kernel を実装（`GPU_DISPLAY_FENCE`＝'G' 54・`GPU_DISPLAY_FENCE_CANCEL`＝55、`drv_gpu_display_changed`、venus・i915・amdrdna・ghci の push）。x86_64・arm64・riscv64 の build は warning 0、i915 の host 試験 31 本 PASS、QEMU は idle・wait・cancel・limit・race が PASS、latency は venus の driver が idle の間の head の変化を見ない既存の問題で FAIL（[phase002](phase002/phase.md) の結果）。次は Q1 の判断（venus の Bug、p002 の受け入れ）。
- 2026-11-13 23:2x P1（q2056-i02、branch `agent/p1-ws353`）: Fable の敵対的レビュー（[design-review.md](design-review.md)）の 25 件を code で確かめて design.md と phase.md に反映した（扱いの表は design-review.md の「反映」）。主な変更: render は飛んでいる間は最古の fence を待つ（S1）、表示の完了は present の fence を新しい thread shown が待つ（S2、present wait の外部同期）、client の buffer は REAPED の serial で返す、未決 3 の推しの変更、Phase の依存（p011 は p004 の後、p010 は p011 の後、p015 は独立）。source は変えていない。次はユーザーのレビュー（未決 1〜3 と上の確かめたい点）、その後 p002。
- 2026-11-13 22:5x P1（q2056-i01、branch `agent/p1-ws353`）: [design.md](design.md) と p002〜p017 の phase.md を置いた（source は変えていない）。HAL・Makefile の変更は要らない形にした。未決 3 つの案と推しは design.md §8。WS352 の branch は b05187933a を除いて merge を推す（§7）。次は Q1 の Fable の敵対的レビューの反映（p001 の残り）。
- 2026-11-13 Q1: 立てた。p001 を P1（Opus 5.5 high）に。

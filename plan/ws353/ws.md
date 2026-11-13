<!-- awesome-plan project=zedbsd record=ws353 -->

# WS353: compositor・libvulkan・kernel の待ち方の作り直し（事象は push、周期で見ない、合成と client の buffer の管理を分ける）

Status: planning
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

## Phase

| Phase | 中身 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計（design.md）と移行の計画（実装の Phase の表と各 phase.md）、Fable の敵対的レビューの反映 | planned（q2056、P1） | — |

## Resume point

- 2026-11-13 Q1: 立てた。p001 を P1（Opus 5.5 high）に。

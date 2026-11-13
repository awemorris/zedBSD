<!-- awesome-plan project=zedbsd record=ws353-p001 -->

# ws353-p001: 設計と移行の計画

Status: in-progress（設計とレビューの反映済み、ユーザーのレビュー待ち。2026-11-13 q2056-i01 P1: design.md と p002〜p017 の phase.md を置いた。2026-11-13 q2056-i02 P1: Fable の敵対的レビュー（design-review.md）を code で確かめて反映した。扱いは design-review.md の「反映」の節）
Parent: [WS353](../ws.md)

## 目的

WS353 の ws.md の「ユーザーの決定」と「合意した形」を、実装できる設計（`plan/ws353/design.md`）と、移行の順の Phase の表と各 phase.md にする。source は変えない。

## 読む物

- `AGENTS.md`、`plan/agents/protocol.md`、`plan/master-design-policy.md`（§11 と設計の方針）、`plan/guardrail.md`（HAL の承認）、`plan/coding-style.md`（設計の例の code の書き方）。
- `plan/ws353/ws.md`（決定と合意した形。これが正）。
- `plan/ws352/poll-report.md`（91 件の全部、行番号は branch `agent/p1-ws352` の tree）と `plan/ws352/phase001/phase.md`・`phase002/phase.md`（branch `agent/p1-ws352` にある。main に未 merge）。
- `plan/ws309/ws.md`・`plan/ws309/design.md`（thread の構成と決定。design.md:123 の「hotplug の 250 ms の tick は main に残す」はこの WS で改める）。
- code: `userland/desktop/wayland/`（main.c・render-thread.c・cursor-thread.c・display-thread.c・output-switch.c・compose.c・lane.c・input-thread.c）、`userland/desktop/libkeiland-backend-zedbsd/`、`userland/desktop/libvulkan/`（sync.c・wsi-display-control.c・wsi-display-nodes.c・WSI の client 側）、`src/drivers/gpu/gpu.c`（display の event・GPU の job の notification）、`src/kern/poll.c`。
- branch `agent/p1-ws352` の未 merge の直し（kernel の poll を object ごとに起こす形など、`git log main..agent/p1-ws352`）。設計で採る・採らないを決める。b05187933a（libvulkan を POLLPRI で block させる形）はユーザーの決定 2 と違うので採らない。

## 範囲

1. 今の実態の調べ（設計の根拠）: display の event の fence の今の作り、GPU の job の fence の kernel の notification の仕組み、client の buffer の種類（shm・GPU）と今の返し方、kernel と libvulkan に明示の同期・暗黙の同期の口があるか、libkeiland・libvulkan の WSI の client 側の buffer の扱い。
2. 設計（design.md）:
   - kernel: display の topology の変化（と refresh の event）で登録された fence を signal する口（既存の GPU の notification の仕組みに載せる。HAL の API に触れるなら、触れずに済む形をまず探し、要る時は差分の案と理由）。
   - libvulkan: `vkRegisterDeviceEventEXT`・`vkRegisterDisplayEventEXT` の fence を kernel に登録し signal で明ける形、問い合わせの判定（`event_hotplug_poll`・`event_refresh_poll`）を消す。`vkWaitForFences` の 1 ms の問い合わせを無くす。要るなら timeline semaphore・client の buffer の同期の口。
   - compositor: thread の一覧（WS309 の表を土台に、hotplug・timer・present の役を足す）、各 thread が block して待つ物と知らせ方、main loop の形、締め切りの持ち方、render と cursor の slot の数え方、client の buffer の寿命の管理（合成の slot から外す）、frame の callback を present の完了で、animation を frame の完了で。WS352 の 91 件の各行がこの設計でどうなるか（消える・形が変わる・残る理由）の対応表。
   - WSI（libvulkan の Wayland の WSI、libkeiland）: client の buffer の返し方を変えるなら、その側の変更。
   - 未決（ws.md の 3 つ）は案と推しを書く。
3. 移行の計画: 小さく分けた実装の Phase の表（最初は hotplug の fence: kernel → libvulkan → compositor の hotplug の thread と 250 ms の tick の削除）、各 phase.md（変える file・残す振る舞い・確かめ（host・QEMU・5330 の idle の電力と PC2、入力・描画・animation の応答））、最後に規約の全文の見直し。Makefile・`mk/` の変更が要れば差分の案（当てるのはユーザーの承認の後に Q1）。
4. 敵対的レビュー: Q1 が design-reviewer を Fable（medium）で起動する。担当は design.md と phase.md を置いた所で返し、Q1 が渡すレビューの結果を次の起動で反映する。

## 受け入れ

- `plan/ws353/design.md`、実装の Phase の表（ws.md）、各 phase.md がある。
- WS352 の 91 件の対応表がある。
- Fable の敵対的レビューの指摘が全部反映されている（反映しない物は理由）。
- source は変えていない。

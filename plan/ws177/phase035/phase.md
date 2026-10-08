<!-- awesome-plan project=zedbsd record=ws177-p035 -->

# ws177-p035: 整列の枠より大きい窓と大きさの固定の窓、枠を desktop の swipe の帯の外へ

Parent: [WS177](../ws.md)
Status: test-wait（T1-475、2026-10-08 夜 Q1）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q908 / q908-i01
Origin: [backlog-p2](../backlog-p2.md) の WS181 ws181-p004 の行（client の最小の大きさが枠より大きい窓、大きさの固定の窓、枠の窓の左右の端が desktop の swipe の帯にかかる）。T1-362 の p009-calendar-arranged.png（枠より大きい窓が左の窓に重なる）。[案 U](../phasing-20261008.md)。

## 範囲と決め事

- 窓の最小・最大の大きさ（xdg_toplevel の set_min_size・set_max_size）を枠の body に当てる。最大が body より小さい窓（大きさの固定の窓など）は、その大きさで body の真ん中に置く（letterbox）。
- 最小が body より大きい窓は**整列から外す**（上限を超えた窓と同じく今の場所に floating で残る）。残りの窓で枠を計算し直して割り当て直す（詰め直し）。全部外れたら何も整列しない（`windows=0`）。どの窓を外すかは、合わない窓のうち重なりの一番下（決まった結果になるように）。重ねたまま置く（はみ出す）案は、隣の窓を隠すので採らなかった。
- 作業の領域を左右の desktop の swipe の帯（16 px、`DESKTOP_EDGE`）の外へ: 枠の左端は x=16、右端は幅−16（画面の keyboard が右の列にある時は右はそのまま）。下端は前から下端の帯の上。

## 実装（2026-10-08 P2）

- `arrange.c/h`: 純関数 `kwl_arrange_fit(body, limits[4], placed)`（最小・最大で切って真ん中、最小が入らなければ 0）。
- `arrange-shell.c`: `arrange_body` が窓の限度を当て、合わなければ 0 を返す。`arrange_misfit`、適用で合わない窓を外して作り直す（log `KWL ARRANGE too-large surface=N min=WxH slot=WxH`）。tick の「縁で大きさを変えた」の判定・glide も限度を当てた body と比べる。
- `shell.c` `kwl_glass_work_area`: 左右の帯の外。
- 試験の道具: `userland/tests/wltest` に `--min-size=WxH`（最小の大きさを宣言し、configure がそれより小さければその大きさで描く）。`acquire-fence` の呼び出しを新しい引数に合わせた。

## 確認

| 確認 | 結果 |
| --- | --- |
| host `plan/ws181/tests/run-host-arrange.sh` | PASS（checks=1220 failures=0、fit の 7 件を足した） |
| zedBSD の compositor・wltest・acquire-fence-test、Linux の Keiland（wltest を含む）の build | 成功、warning 0 |
| style-check（arrange.c・arrange-shell.c・wltest） | 新しい指摘 0（既存の分は変わらず） |
| QEMU（T1、`plan/ws177/tests/u-guest.sh` の U4） | 未実施 |

## T1-475 の FAIL の調べと直し（2026-10-09 P1）

- FAIL: `arranged-two`（`windows=3` の 3 つの slot）、`recalled`（`windows=3`）、`recalled-order`（`6;6;6`）。原因は試験の側: U1 の後の u.a の 2 つの窓の kill が `ps -A -o pid,args | grep "[w]ltest --app-id=u.a"` で、zedBSD の ps の COMMAND は argv[0] だけ（kernel の `process->command` は argv[0] の写し、`src/kern/exec.c`）のため一致せず、u.a の 2 つが残った（compositor の log で client 2・3 の `CLIENT gone` が無く、U3 の MAP の時に `RENDER surfaces=6,6,6`）。窓が 5 つで columns の上限 4 → 大きすぎる m を外して 3。整列の code（`arrange_apply` の外して作り直す loop・`arrange_recall`）は正しく動いていた。
- もう一つ: `slot_ids` が apply の行の surface の id を読むが、surface の id は client ごとの object の id で全ての client で 6。順の確かめが意味を持たなかった。
- 直し: 試験 `plan/ws177/tests/u-guest.sh` の U1 の kill を `[w]ltest`（その時に動くのは u.a の 2 つだけ）にし、`u.a-gone`（`KWL CLIENT gone` が 2）を足した。compositor `arrange-shell.c` の `arrange_apply` に apply の行の直後の行 `KWL ARRANGE clients desktop=N clients=A;B`（slot の順の client の番号）を足し、`slot_ids` がそれを読む。apply の行の形は変えない（ws181 の試験が読む）。
- 確認: `make ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/p1-ws177 build/p1-ws177/bin/wayland`（-Werror、成功、warning 0）、`sh -n u-guest.sh`。QEMU は T1 の再依頼で（未実施）。

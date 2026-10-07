<!-- awesome-plan project=zedbsd record=ws177-p001 -->

# ws177-p001: compositor の wl_surface.enter・leave（案 E）

Parent: [WS177](../ws.md)
Status: in-progress（実装済み・host PASS。T1 の QEMU の試験待ち、依頼は Q1 経由）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q882 の 1（P1、2026-10-08、承認は Q1 の dispatch「Queue（q882、承認済み）: WS177 の案 E・F・G の host の分を順に」）
Origin: [backlog-p1](../backlog-p1.md) の 42 行（WS113 ws113-p007 の窓の出力の所属）、[案](../phasing-20261008.md) の E
Purpose / goal: client が自分の surface がどの display（wl_output）に載っているかを wl_surface.enter・leave で知る。複数 display（ws113-p007 の D-ATOMIC、p015 の後）の状態に合わせる。

## 設計（2026-10-08 P1）

- **所属の定義**: 拡張では窓は 1 つの出力に属し、その出力だけが描く（ws113-p007 の D-ATOMIC）。surface の出力 = その窓の出力（`kwl_window_output`: sub-surface は親の窓、popup は toplevel の窓）。出力が表示されていない（head が消えた後の退避の前、mirror の head）窓は anchor（`compose_split` と同じ）。mirror では anchor と開いている（lost でない）全ての head。
- **載っている surface**: 画像（current）があり、cursor でなく、sub-surface の根が xdg の role を持ち、根が toplevel なら mapped、popup なら画像がある。cursor・drag の icon・input method の popup（xdg の role を持たない）・role の無い surface には送らない（制限）。
- **見えているかではなく所属**: 最小化・別の仮想 desktop の窓も出力に載ったままにする（leave しない）。scale はどの出力も 1 で、client が出力に合わせて選んだ refresh・scale を desktop の切り替えのたびに揺らさないため。spec の「scanout の領域の中」より所属を採る（sway は workspace の切り替えで leave を送る。違いは記録のみ）。
- **送り方**: surface ごとに client に伝えた出力の集合（`kwl_object.outputs_entered`、plane.h の slot の bit）を持ち、event loop の毎回の pass（`kwl_schedule` の commit の採用の後、`kwl_surface_outputs_sync`）で今の集合と比べ、差だけ送る。**得た出力を先に enter、その後に失った出力を leave**（移動の間に client が出力 0 個にならない）。各変化はその client の、その display の生きた wl_output の binding 全てに送る（binding が 2 つなら 2 回）。log は変化ごとに 1 行 `KWL SURFACE enter|leave surface=S output=N client=C`。
- **後から bind**: 新しい wl_output の binding（anchor の固定の global・head の global の両方）には、出力の初めの event の後に、その client の既にその display に載っている surface の enter を送る（`kwl_surface_outputs_bound`）。
- **display が閉じる**: `kwl_output_global_remove` が binding を inert（KWL_OUTPUT_GONE）にする前に、その display を除いて同期する（`kwl_surface_outputs_gone`）。`heads_close` は先に窓を anchor へ退避するので、anchor の enter → 閉じる head の leave の順になり、leave は binding がまだその display を名指す間に届く。
- **安全**: client が wl_output を release・destroy した後に enter が届いても、libwayland（`userland/desktop/libwayland`）は zombie の proxy を map に残し、引数を NULL にして渡す（decode は失敗しない）。dead の binding・GONE の binding・他の client の binding には送らない。fatal の client は飛ばす。
- 費用: 毎 pass に全 client の object を 1 回走査（同じ pass の commit の採用の走査と同じ程度）。event は変化した時だけ。

## 変更

- 新しい `userland/desktop/wayland/surface-outputs.c`（`kwl_surface_outputs_sync`・`_bound`・`_gone`）、`kwl.h`（`outputs_entered`、宣言）、`display.c`（`kwl_schedule` から同期）、`protocol.c`（2 つの bind の経路と global の除去）、`Makefile`・`Makefile.linux`・`Makefile.freebsd`。
- 試験: `plan/ws177/tests/host-surface-outputs.{c,sh}`（host）、`displays-p001.sh`・`config-amd64-p001.mk`（QEMU、T1）。

## 確認（host、2026-10-08）

- `sh plan/ws177/tests/host-surface-outputs.sh` → `WS177 p001 surface outputs host test PASS (plain, ASan/UBSan)`（未表示で 0 件、map で anchor に 1 回、変わらない pass は 0 件、sub-surface・popup が窓に従う、head への移動で enter が leave より先、後の binding は自分だけに enter、表示されない出力の窓は anchor、head の close で binding がある間に leave、mirror で両方・lost の head は leave、他の client の binding には送らない、unmap で leave、fatal の client は飛ばす、cursor・role の無い surface は 0）。
- build（warning 0、-Werror）: `make -j16 BUILD=build/p1-wl ZEDBSD_CONFIG=plan/ws113/tests/config-amd64-p005.mk build/p1-wl/bin/wayland`、`make -f userland/desktop/keiland-linux.mk KEILAND_LINUX_BUILD=build/p1-wl-linux all`（warning 0）。
- `python3 plan/tools/style-check.py`: 新しい 2 file は 0、`display.c`・`protocol.c` は変更の前後とも 0。`git diff --check` 0。

## 未実施

- QEMU（T1、2 出力の Venus）: `plan/ws177/tests/displays-p001.sh`（image は `config-amd64-p001.mk`）。keyboard の移動での enter→leave の順、抜去での退避、mirror での両方・拡張に戻って leave、Files が event を受けて動き続ける。
- 実機（5330）: 未実施（p008 の時に一緒でよい）。FreeBSD の build: 未実施（Makefile.freebsd に足しただけ）。
- client の側で enter を使う所（libkeiland が出力ごとの refresh を選ぶ等）は範囲外。

## Event

2026-10-08 / q882-i01（P1）: 実装と host の確認。T1 の依頼を Q1 へ。

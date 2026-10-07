<!-- awesome-plan project=zedbsd record=ws177-p009 -->

# ws177-p009: 手書きの頑健さ（案 H）

Parent: [WS177](../ws.md)
Status: cleared（2026-10-08 Q1 判定、host の範囲。実機の UAT は未実施）
Disposition: normal
Primary Milestone: MG006（WS から継承）
Queue / attempts: q886 の 1（P1、2026-10-08、Q1 の dispatch「Queue（q886、承認済み）」）
Origin: [backlog-p2](../backlog-p2.md) の 55〜60（WS165 ws165-p002・p003）、[案](../phasing-20261008.md) の H

## 設計と変更（2026-10-08 P1）

| 行 | 形 |
| --- | --- |
| 55 1 点・極端に少ない線 | `hand_ink_scant`（hand-cloud.h、公開）: 点が 1 つ・線が動かない・面（frame）の高さの 2%（`HAND_INK_LEAST`）より小さい ink は「形が無い」。`recognize_strokes` は候補 0、`keyboard-hand.c` は note「Too small to read」を出す。行の言うとおり tap は認識しない（`.` は 2% より大きく書けば読む: 300 px の面で 6 px。tap を `.` にする案は採らなかった。UAT で不便なら変える） |
| 56 templates の file が無い・壊れている | `hand_templates_parse` は template が 0 個なら ENOENT。`templates_line` は strtok をやめて自分で語を切る。`5,` のように点が途中で切れた語、余計な文字（`9x`）、`/` の後の文字を EINVAL にする（前は `5,` を y=0 として通していた）。`kwl_hand_load` は 1 MiB を越える file を EFBIG にする（前は黙って切っていた）。読み誤りは EIO。どれでも認識の時に note「No handwriting data」。読みが失敗した後に手書きの面を出した時は、その場で note を出す（`kwl_hand_preload`） |
| 57 Hershey の重ね線 | `convert.py`: ほかの長い線から全長が 1.5 単位以内にある線（太字の重ね線）を除く（`without_overstrikes`、線の上を 0.5 単位ごとに見る）。線は 1224 本から 656 本になった。濁点・半濁点（50 字）の印は残る（確認済み） |
| 57 で見つけた不具合 | `convert.py` が occidental（hersh.oc）と oriental（hersh.or）の番号の衝突（509・511・617・619・622・703・715）で oriental を取っていたので、**I・K・q・s・v・3・? の手本が漢字だった**（実の手書きのこれらは読めなかった）。番号が両方にある時は occidental を取る |
| 58 濁点・半濁点の位置 | 右上の印が無い時、`strokes_mark_anywhere` が位置を問わず印を探す（小さな輪は半濁点、小さな 2 本が近ければ（中心が ink の 20% 以内）濁点）。採るのは、印を除いた残りに印を取る仮名の候補があり、その距離が ink 全体の最良の距離の 0.8 倍（`HAND_MARK_LOOSE_SHARE`）より小さい時だけ（シ・ツの点は濁点にしない）。右上の判定は `strokes_measure`・`stroke_in_corner`・`stroke_is_ring` に分けて規約の形に書き直した（動きは同じ） |
| 59 templates の読みを別の thread で | `kwl_hand_preload`: 手書きの面を初めて出した時（`keyboard_hand_toggle`）に thread を作って読む。認識は `hand_join` で待つ（普通は読み終えている）。thread を作れない時は、従来どおり最初の認識で読む。templates と状態は thread が書き、event loop は join の後にだけ読む |
| 60 ink が 8,192 点を越える | `hand_ink_input`: 越えた時は各線の点を step ごと（先頭と末尾は必ず）に間引いて、全部の線を使う（step は点の数を (8192 − 2×64) で割って切り上げた値）。最大は 64 線 × 512 点 = 32,768 点 |

- 変更: `userland/desktop/wayland/hand-cloud.c`・`hand-cloud.h`・`keyboard-hand.c`・`keyboard.h`・`keyboard.c`（hand on で preload）、`userland/packages/fonts/hand-hershey/convert.py`。
- 試験: `plan/ws177/tests/host-hand-robust.{c,sh}`（新）。認識率は WS165 の `plan/ws165/tests/run-host-hand.sh`・`run-host-hand-keyboard.sh`。

## 確認（host・build、2026-10-08）

- `sh plan/ws165/tests/run-host-hand.sh 20`（228 字 × 20）: 前は top-1 90.6%（形が同じ組を 1 つに数えて 91.4%）・top-4 98.4%・平均 5.32 ms。57 の後は 93.5%・99.1%。58 の後は **93.6%（94.4%）・99.1%、平均 4.52 ms** で PASS。外す字（20 中 10 以上）は前の K・O・v・2・×・は・ほ・れ・ぽ から O・l・× になった。じ（12 回外す）は 57 の後に出て、58 で直った。
- `sh plan/ws165/tests/run-host-hand-keyboard.sh` → PASS（7 の ok、`.`・`·` の点も読む）。
- `sh plan/ws177/tests/host-hand-robust.sh` → PASS（ASan/UBSan）: tap・ずれた tap・離れた tap は note、線は読む。印の位置（右上・本体の中・左上）は 49・45・45 / 50（前の認識器では 49・0・0）。8,314 点の ink（が、印が最後）は が。壊れた file 6 種と大きすぎる file は失敗して note、本物の file は読み直せる。thread の読みで あ を認識する。
- build（warning 0、-Werror）: `make -j16 BUILD=build/p1-d ZEDBSD_CONFIG=plan/ws113/tests/config-amd64-p006.mk build/p1-d/bin/wayland`。`python3 plan/tools/style-check.py hand-cloud.c keyboard-hand.c` は 0 件。

## 見つけたこと・残り

- l は 20 回とも I になる: 手本の I（57 で漢字から直った）が l と同じ縦の 1 本で、形では分けられない。WS165 の `host-hand.c` の形が同じ組（`{ 'l', '|', '1' }`）に I を足すのが妥当（WS165 の試験なので Q1 に提案、ここでは変えない）。
- 本体の中・左上の印で外す 5 字: ジ・ゾ・ヅ・ボ・ポ（片仮名の点・輪がシ・ソ・ツ・ホの本体に近い）。
- 未実施: QEMU（手書きの面の log は WS165 の T1 の試験がある。この Phase の変更は host で確かめられる範囲）。実機の UAT（5330 の touch で tap・本体の中の濁点・最初の認識の遅れ）。Linux・FreeBSD の compositor の build。

## Event

2026-10-08 / q886-i01（P1）: 実装と host・build の確認。途中で T1-407・T1-411 の試験の直しのために区切った（90159c20e）。

## Q1 の判定（2026-10-08）

merge e3d2b08a2。(1) tap を認識しない（Too small to read）は採る。(2) の提案に従い Q1 が plan/ws165/tests/host-hand.c の形が同じ組に I を足した（run-host-hand.sh PASS、top-1 93.6%・同じ形を含め 94.9%・top-4 99.1%）。host の範囲で cleared、実機の touch の UAT は未実施。

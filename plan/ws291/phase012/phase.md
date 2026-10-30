<!-- awesome-plan project=zedbsd record=ws291-p012 -->

# ws291-p012 NVIDIA（RTX 2070）: scene-5000 を 50 fps を明らかに超える所へ

- 親: [WS291](../ws.md)。前: [p011](../phase011/phase.md)（scene-5000 は 49.5〜49.7 fps で NVK 50.1 と揺れの内で同等、GPU の run 約 19.4 ms、
  頂点の program の命令を半分にしても GPU の時間は不変）。
- 状態: in-progress（2026-10-30 14:12 P7、q1712-i01、worktree p7-ws291、基点 main 5aca73738）。
- 承認: ユーザー（2026-10-30 原文）「WS291をP7で実行します。P7はこのWSが終わったら返却してください。」。範囲（Q1 の依頼）: p012 以降で
  scene-5000 を 50 fps を明らかに超える改善（今は 49.5〜49.7 fps で NVK と同等）。

## 受け入れ（測る前に決める）

- scene-5000（qs40 ×5000 instanced、game、mailbox、4 枚、12 s の spin、display の解像度の fullscreen、direct scanout）を同じ起動の条件で 3 回以上、
  全部が 50.5 fps 以上かつ平均が NVK の 50.1 を揺れ（p011 の run の幅 約 0.3 fps）より大きく超える。copies 1 の shot が Linux の参照と従来どおり
  （max 76、16 以上の差 2 画素程度）、hang の回復が従来どおり。amd64 の vmunix・変えた userland が warning 0。
- 既定を変える物は、ユーザーの決定（RDNA の UCLK の件: 「負荷に応じて上げ下げさせる形」）と食い違わない形にする。食い違うなら Q1 へ。

## 手順

1. 内訳: 同じ kernel で scene-5000、cull-5000（`--cull=all`、三角形を全部捨てる: 頂点の側だけ）、copies 1 を測り、GPU の時間が頂点の側か画素の側かを見る。
   GSP の性能の state（`performance: performance state Pn`）と `nvrtx.boost=1`（GSP に最高の state を保たせる、WS241 の boot の語）の差も同じ枠で。
2. 見立てに沿って直し、同じ script で前後を測る。

## 道具

- `plan/ws291/tests/rtx-scene-main.sh OUT VMUNIX`（`FILES`・`RESTORE_CKSUM`・`SETS`・`REPS`・`RUNS`）: NVMe の kernel を読み戻し、main の desktop の
  program を入れ（rtx-files.sh）、kernel を入れ、SETS の boot の設定ごとに RUNS の run を REPS 回、終わりに全部を戻して照合、Debian へ。

## 記録

### 内訳 s1（2026-10-30 14:11〜14:22、kernel = main 5aca73738 の build cksum 2016763217、`rtx-scene-main.sh`、SETS='base nvrtx.boost=1'、build/ws291/rtx/q1712-s1）

| set | scene-5000 fps（3 回） | cull-5000（--cull=all）fps | copies 1 fps | STAGES の例（frame・record・submit・fence ms） |
| --- | --- | --- | --- | --- |
| base（GSP の state P0） | 49.42・50.39・49.09 | 48.98・49.06・48.47 | 1773・1757・1729 | 19.81〜20.34・0.07〜0.09・0.12〜0.13・19.42〜19.94 |
| nvrtx.boost=1（P0 のまま） | 49.87・49.77・49.38 | 49.41・49.23・49.35 | 1827・1787・1754 | 20.02〜20.22・0.09〜0.11・0.12〜0.14・19.62〜19.82 |

- 読み: (1) 三角形を全部 cull しても（画素の仕事 0）時間は同じ → GPU の 19.4 ms は全部が頂点の側（index・頂点の fetch・VS・primitive の組み立てと cull）。
  画素の側（ZCULL・early-Z・ROP）を詰めても効かない。p011 の「VS の命令を半分にしても不変」と合わせ、SM の演算でなく固定機能の front-end が律速。
  (2) GSP は負荷の間 P0 で、nvrtx.boost=1 も fps を変えない（clock は既に上限）。boost では frame_ms_max の時々の 68〜95 ms の跳ねが 27〜36 ms に
  収まった（1 回ずつの観察、未確かめ）。(3) frame と GPU の差（acquire・record・submit・present・起床）は約 0.4〜0.8 ms: 全部消しても約 51.5 fps が上限。
- 機械の状態: 終わりに file と kernel を戻して照合（vmunix 1463007375、perf の行 0）、Debian、RTX は vfio-pci。

### 文書の無い 3D の method の試し（2026-10-30 14:24〜14:31、診断の kernel cksum 4254886907、build/ws291/rtx/q1712-knob）

- 診断の patch `plan/ws291/tests/nvrtx-knob-diag.patch`（commit しない source の変更、vk-execute.c）: 新しい executor（session）ごとに順に 1 つの値を、
  run の頭（class の初期 state の後）に書く。値の組（7 個の巡回）: 無し、SET_VPC_PERF_KNOB（0x0f14）= 0・0xffffffff・0x20202020、
  SET_VTG_WARP_WATERMARKS（0x0f98）= 0x00200020・0x00080004、SET_VTG_REGISTER_WATERMARKS（0x1284）= 0x00ff0080。どれも Mesa（NVK・nvc0）は
  書かない method で、既定は GSP の golden context の値（読めない）。`DIAG='DIAG|…' rtx-scene-main.sh`（REPS=14、RUNS=scene）で run ごとに dmesg の印を取り、
  run k が session k+3 に当たる（dmesg の並び）。

| 値 | scene-5000 fps（2 回） |
| --- | --- |
| 無し（既定） | 48.61・48.38 |
| VPC_PERF_KNOB 0 | 48.47・48.70 |
| VPC_PERF_KNOB 0x20202020 | 19.09・19.06 |
| VPC_PERF_KNOB 0xffffffff | 2.98・3.01 |
| VTG_WARP_WATERMARKS 0x00200020 | 49.34・48.73 |
| VTG_WARP_WATERMARKS 0x00080004 | 48.85・48.63 |
| VTG_REGISTER_WATERMARKS 0x00ff0080 | 48.76・48.84 |

- 読み: VPC の knob は大きいほど遅い（VPC が primitive を流す速さの絞り）、0 は既定と同じ → 既定は既に最も速い側。VTG の watermark は揺れの内。
  → この 3 つで front-end は速くならない。hang・fault は無し（dmesg は起動の時の BAR1 の既知の行だけ）。
- 別の観察: この起動では既定でも 48.4〜48.8 fps（s1 の 49.1〜50.4 より低い）。s1 も 1 つの起動の中で run を重ねると下がる（base 49.4・50.4・49.1 → cull 49.0・49.1・48.5）。
  長い負荷で clock が下がる（温度か電力の上限）見込み、未確かめ（温度・clock を読む道は nvrtx に無い）。
- 機械: 終わりに戻して照合（vmunix 1463007375、perf の行 0）、Debian、RTX は vfio-pci。source の診断の変更は build の後に戻した（git の差分 0）。

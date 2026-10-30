<!-- awesome-plan project=zedbsd record=ws291-p013 -->

# ws291-p013: NVIDIA: draw と submit の多い bench の CPU の側の費用を Linux（NVK）並みに

Status: uncleared（2026-10-31 Q1。NVK と同等には届かず。差の内訳を記録。次の手は新しい transport で record の段を無くす事など）
Parent: [WS291](../ws.md)
Queue: q1728（P3）

## 目的

2026-10-30 ユーザーから、WS291 の RTX の改善を P3 で行う、という趣旨の指示を受けた。p011 の最後の計測（kernel k5）で、scene は NVK と同等だが、draw と submit の多い bench が大きく負けている:

| bench | zedBSD | NVK |
| --- | --- | --- |
| big-1000 | 52 fps（record 約 8 ms・submit 約 10.6 ms、decode 約 6 ms、draw 1 回 約 0.6 µs） | 136.2 fps（1 frame 7.3 ms） |
| subs-1000 | 10.9 fps | 75.4 fps |

## 範囲

1. big-N（record・decode・executor の draw ごとの費用）と subs-N（submit 1 回の費用）の profile。
2. 効く所から nvrtx の Vulkan の backend と kernel の driver を直し、NVK に近づける（目標は NVK と同等、少なくとも差の原因を全部説明できる所まで）。scene-1500・scene-5000 と desktop の 60 fps を後退させない。
3. 計測は 1 本の script（plan/ws291/tests/rtx-bench.sh ほか）、要約だけを読む。HAL の API は変えない。driver は hal_* を使わない。

## q1728-i01（2026-10-30〜31 P3、worktree p3-ws291r、基点 main 9a5a7fc22）

- 承認: Queue q1728-i01（2026-10-30 ユーザーの指示: WS291 の RTX の改善を P3 で行う、の趣旨）。範囲は上の「範囲」のとおり。
- 実機: 10.0.10.3（RTX 2070）、lock /tmp/zedbsd-rdna.lock を 2026-10-30 23:23 から保持。

### 道具（新）

- `plan/ws291/tests/nvrtx-draws-bench.sh OUT [TREE] [COPIES] [FRAMES] [SUBMITS]`（`nvrtx-draws-bench.c`、vk-d3 の host 試験から作った）: mview の
  `--draws=each` の frame（1 copy に 16 group、pipeline 2 種・set 2 種・push constants・indexed draw）を libvulkan の wire で nvrtx の executor の source に流し、
  Begin（reset）・record（decode）・submit（executor）の CPU の時間を host で測る。偽の GPU は push buffer を 3D class の state の模型（method の値、
  constant buffer への load、group の binding）に通し、draw ごとの state の hash を OUT/snapshots.txt に書く。TREE に別の tree（`git archive` で出した基点）を
  渡して 2 つの executor の hash を比べる（一致＝各 draw が GPU で見る state が同じ）。`BENCH_DUMP=n`・`BENCH_TRACE=n` で draw n の state・method を出す。
- `plan/ws291/tests/rtx-p013.sh OUT NAME=VMUNIX...`（`FILES`・`RESTORE_CKSUM`・`LIST`・`ROUNDS`・`SHOT`・`PROF`）: 1 window で kernel を順に（往復）入れ替えて
  bench（rtx-ab.list）、SHOT=1 で 100 copies の still を 3 通り（one: group ごとに 1 instanced draw、each: copy と group ごとの draw を 1 submit、subs: 1 copy を
  1 submit で 100 回）撮って one と比べ、PROF=1 で最後の round に nvrtx.vk.perf=1 の big-1000・subs-100。始めに NVMe の kernel を読み戻して照合、FILES を入れ（rtx-files.sh）、
  終わりに全部戻して cksum を照合し Debian へ。`rtx-each-shot.sh` は shot だけの単独版。

### 実機の前提の問題（main の kernel と NVMe の古い userland）

- main 9a5a7fc22 の kernel を NVMe（userland は 10-28 の build）に入れると userland まで起動するが ue0 が up にならず（nicwatch が 3 回 reboot、4 回目は応答なし）。
  23:48 に SwitchBot（AC276E42B252）で入れ直し、Debian から NVMe の vmunix を va1（1463007375）に戻し nicwatch.count を 0 に。
- 原因（Q1 の見立て、BUG-371）: WS332 の b3b05b2be で WLAN の ABI の版が 2 に上がり、古い networkd が EINVAL で networking 全体が飛ぶ。p4 の log は kernel の行だけで
  networkd の行は無く、log では確かめられない。NVMe の networkd・wifi・libc・libkeiland・wayland・files・settings・terminal・keiland-ime・sessiond を main の build に
  入れ替える（FILES、元は *.p7old）と素の main の kernel で ue0 が上がった。以後の計測は全部この形。

### 直し（commit d7ad304a2、src/drivers/gpu/nvrtx）

- executor の draw（vk-execute.c）: 同じ push buffer で直前の app の draw の後（push buffer が同じで、その draw の終わりから何も書かれていない）なら、
  違う state だけを書く: viewport・scissor（値）、culling・depth・blend・vertex と pixel の program・vertex input・uniform（pipeline の値を部分ごとに比べる。
  program だけ違えば `nvrtx_push_draw_program`）、vertex buffer・index buffer、draw の base・instance（`nvrtx_exec_draw_held`）。比べは語の loop
  （kernel の kern_memcmp は byte ずつ）。
- constant buffer 0（push constants と texture handle）: session に 1 つ（slot 0 の EXECUTE_ROOT_AT、作る時に 0、以後 CPU は書かない）。push buffer の中で最初の
  draw が select・全 group に bind・全体を load、後の draw は違う語だけを LOAD_CONSTANT_BUFFER（`nvrtx_exec_constants_load`、1INC の header）で load
  （NVK の cb0 と root descriptor と同じ形）。uniform buffer の bind が selector を替えた後は select し直す。
  最初の形（e4: 各 push buffer の CPU が書く slot に GPU が load）は subs の still が one と 2989 画素（全 copy の靴）ずれた。CPU が書く memory に GPU が書く形を
  やめた e5 で 0。
- texture の pool の handle: (view, sampler) の組を 32 の表で push buffer の中で再利用（前は直前の 1 つだけ。mview は group ごとに texture が替わる）。
- dispatch（vk-dispatch.c）: record の route を先頭に（frame の command のほとんどが record で、他の route は CMD を持たない）。
- host 試験: nvrtx-vk 94/0、nvrtx-vk-d3 218/0（stub の gpu_run を 1INC・immediate の header と constant buffer への load に追従、quad の constants の位置を追従）。
  bench: 1000 copies 1 submit・100 copies 100 submit の両方で draw ごとの state の hash が基点と全一致。host の submit（executor）10.4 → 2.8 ms、record 4.9 → 3.8 ms。
- kernel: `make ZEDBSD_CONFIG=plan/ws291/tests/config.mk BUILD=build/ws291/k build/ws291/k/vmunix` warning 0・amd64 vmunix check PASS。

### 実機（10.0.10.3 RTX 2070、main の userland、`rtx-p013.sh`、fps、括弧は big-1000 の record・submit ms）

| kernel | big-100 | big-1000 | subs-100 | subs-1000 | scene-1500 | scene-5000 | shot each・subs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| base（main 9a5a7fc22、3184984784） | 420.4・400.7 | 48.0・48.4（8.5・11.9） | 104.4・103.5 | 11.0・10.9 | 161.0・160.2 | 50.0・49.7 | 0・0 |
| e4（3 の形、1132680407） | 408.1・413.3 | 54.8・65.4（6.9〜8.7・7.3〜8.4） | 107.0・106.2 | 10.9・10.9 | 159.1・158.9 | 49.6・49.7 | 0・2989 画素 |
| e5（d7ad304a2、3510553775） | 408.5・412.1 | 65.1・66.2（6.9・7.3） | 106.2・105.2 | 11.0・11.0 | 160.0・158.9 | 50.0・49.7 | 0・0 |
| NVK（p010） | — | 136.2 | — | 75.4 | 165.6 | 50.1 | — |

- 証拠: build/ws291/measure/w1（base・e4、00:02〜00:16）・w2（e5、00:16〜00:25）の summary.txt。各 window の終わりに file と kernel を戻して照合（全一致）、
  Debian、RTX は vfio-pci（4 function）。

### 2 回目の直し（commit 8e3ec7b65）と計測

- e6（record の hold を recording ごとに 1 回にして reset の全 record の走査を消す）: host の reset 0.36 → 0 ms、実機の big-1000 は e5 と揺れの内
  （64.0・66.8 対 63.9・66.8）→ 入れない。差分は `plan/ws291/tests/nvrtx-record-holds.patch` に残す。
- perf の行（w3、nvrtx.vk.perf=1）で、big-1000 の push buffer が 1 frame に約 9 本（1 本の GPU 0.72 ms、frame の GPU 約 6.4 ms）と分かった: texture の
  handle の表の場所を address の下位 bit で決めていたため、mview の 14 の view が同じ場所に重なり、draw ごとに pool の entry（header・sampler の作成と
  書き込み）を作って pool が満ちていた（host の bench は view が 2 つで見えなかった。bench を 14 view に）。e7: 場所を黄金比の乗算の上位 bit で決め、
  隣の 2 つの場所を 1 組に → 1 frame に push buffer 約 3 本。
- host: bench（14 view）で draw ごとの state の hash が基点と全一致（1000 copies・100 submits）、submit 12.8 → 3.0 ms。nvrtx-vk 94/0・vk-d3 218/0。

| kernel（w3 00:28〜00:50・w4 00:50〜01:13） | big-100 | big-1000（record・submit・fence ms） | subs-100 | subs-1000 | scene-1500 | scene-5000 | shot each・subs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| e5（3510553775） | 395.7・403.3 | 63.9・66.8（6.7〜7.1・7.2〜7.4・0.9） | 105.5・105.1 | 11.0・11.0 | 160.2・159.2 | 49.7・49.7 | 0・0 |
| e6（962897288、入れない） | 403.6・400.7 | 64.0・66.8 | 104.4・106.1 | 11.0・10.9 | 159.4・159.8 | 49.7・49.6 | 0・0 |
| e7（8e3ec7b65、3526316689） | 603.6・566.8 | 69.7・69.3（6.7・2.3・5.2） | 104.6・104.7 | 11.0・11.0 | 159.6・159.1 | 50.0・49.6 | 0・0 |
| e7 + cursor の診断（8400716） | 578.3・582.3 | 69.0・67.0 | 104.4・104.0 | 11.0・10.9 | 159.6・154.4 | 49.8・49.6 | 0・0 |

### 差の内訳（e7、big-1000、perf 有り 1 frame 約 14.4〜16.2 ms）

- frame = record 6.7〜7.5 ms（CPU だけ、GPU は空き）+ submit 2.3〜2.7 ms（executor、GPU は最初の push buffer から走る）+ fence 5.1〜5.9 ms（GPU の残り）。
  GPU は 1 frame に push buffer 約 3 本・計 約 6.4 ms（runs 927・gpu avg 2151 µs / 5 s）。
- record: 48000 record（1 draw に sets・push・draw の 3 つ）の decode。libvulkan の wire の record を 1 MiB の batch ごとに kernel が record の配列へ
  読み直す。host の bench で 1 record 約 80 ns、実機 約 0.11〜0.14 µs（P5 の計時）。NVK は vkCmd* が push buffer を直に書くので、この段が無い。
- GPU: 6.4 ms のうち 約 3.9 ms は三角形（scene-5000 の 19.4 ms × 1000/5000、p012 の固定機能の律速）。残り 約 2.5 ms が 16000 draw の分（1 draw 約
  0.16 µs: texture handle と push の constant buffer 0 への load、pipeline の切り替え（cull と pixel program）、draw）。
- NVK の 7.3 ms に届くには、record の段を無くす（libvulkan が push buffer に近い形で書き kernel は検査と写しだけにする等、transport の作り直し）と、
  GPU の draw ごとの費用の内訳を NVK の push と比べる段が要る。どちらも q1728 の残りの時間では未着手。
- subs-1000（11.0 対 NVK 75.4）: GPU が 1 run 約 89 µs（perf: gpu avg 89 µs、1 run に 16 draw の実の描画は数 µs）で、pass の終わりの全画面の shadow→target の
  copy が律速（p011 の q1693-i01 の (B)、WSI の image を block-linear にする件、WS291 の範囲外）。CPU の 1 submit（libvulkan の reserve 8 µs・execute 53〜60 µs）
  は GPU の陰に隠れる。copy を消しても 1 submit の CPU 約 60〜90 µs が次の上限（NVK は 1 submit 約 13 µs）。

### desktop（後退の確かめ）

- `rtx-desk-main.sh build/ws291/measure/d7 build/ws291/vmunix-e7`（RUNS=1、main の desktop の program、01:13〜01:19）: launch 63・type 61・output 68・drag 60・
  home 64・wiseview 65 fps（scroll・hover・keys は入力の間隔で決まる段）。p010 の q1712-i01（63/64・61/62・68/65・61/60・63/64・65/60）と同じ。後退なし。

### BUG-374（cursor の色、Q1 が q1728 に追加）

- 調べは [BUG-374](../../bugs/BUG-374.md)。nvrtx に届く画像は正しく、display の cursor の語も NVIDIA の clc57d.h と一致。原因は未特定、直しは未実施
  （画面を見ないと plane の cursor か合成の cursor かを分けられない）。

### 機械の状態と再開点（2026-10-31 01:19 P3）

- 機械: Debian、RTX は vfio-pci（4 function）、NVMe の kernel は vmunix-va1（1463007375）、NVMe の userland は 10-28 の file に戻して cksum を照合
  （main の build の入れ替えの元 *.p7old は残る）。lock は 01:19 に返した。
- branch agent/p3-ws291r: d7ad304a2（e5）・8e3ec7b65（e7）・dee5a42dc（main 1233c07a5 の merge）・e95feb227（BUG-374 の記録）。main に入れてよいのは
  executor の 2 つ（各々実機で効果あり、shot の画素差 0、scene と desktop の後退なし）。
- 到達: big-1000 48.0〜48.4 → 67.0〜69.7 fps（NVK 136.2）、big-100 401〜420 → 567〜604、subs は 11.0 のまま（NVK 75.4）。NVK と同等には届かない。差の原因は上の
  「差の内訳」（record の decode 約 7 ms、GPU の draw ごとの費用 約 2.5 ms、subs は pass の終わりの全画面の copy）。
- 次の候補: (1) record の段を無くす transport（libvulkan が kernel の push に近い形で書く、または kernel が wire から直に push を書く）、(2) GPU の draw ごとの費用を
  NVK の push と method の単位で比べる（IMMD の header、constant buffer 0 の load の量、program の切り替え）、(3) subs は (B)（WS309 p015 の候補）の後に 1 submit の CPU。

<!-- awesome-plan project=zedbsd record=ws291-p026 -->

# ws291-p026: NVIDIA（nvrtx）: 記録の時に push buffer まで組み、submit は ring に載せるだけにする設計

Status: in-progress（2026-11-01 P1 q1838-i01: design.md 第 3 版と敵対的レビュー 2 回は済み。ユーザーへの問い Q1・Q2（Q3 は Q2 の答え次第）の答え待ち）
Parent: [WS291](../ws.md)
依存: [p024](../phase024/phase.md)（G1 の名指しと (b) の説明、NVK との段ごとの比べ）
Queue: q1832（P2、i01）、q1838（P1、i01）

## 目的

2026-11-01 ユーザーから、p024 の残りの差 G1（submit の時に呼び手の thread で push buffer を組む、subs-1000 の 1 submit で約 8 µs）は
(b)「記録の時に push buffer まで作る」形で、設計の Phase から始める、という趣旨の答えを受けた（数日の規模、HAL と UAPI は変えない見込み、
設計と敵対的レビューの後に実装）。この Phase はその設計（code は書かない）。

目標の数（p024 の NVK）: subs-1000 75.4 fps（今 58.5〜58.7）、big-1000 136.2 fps（今 118 前後）。

## 範囲（q1832-i01・q1838-i01）

1. 設計の文書 [design.md](design.md): 今の流れの事実、submit の時にしか決まらない物の扱い、command buffer の reset・再記録・同時の使用・secondary、
   組んだ push buffer の memory の持ち方と解放、GPU の fault・CANCEL・OOM（BUG-381）、kernel と userland の境（security）、他の driver と libvulkan
   への影響、HAL・UAPI を変えない根拠、選ばなかった案、試験の計画、実装の Phase の案と受け入れ、危険と戻し方。
2. design-reviewer の敵対的レビュー（[design-review.md](design-review.md)）と反映。
3. ユーザーへの問い（下）。
4. 範囲の外: code、big-1000 の client の encode の短縮（下の「範囲の外の残り」）。q1838 では見積もりの根拠を確かめるための実機の計測を 1 本だけ
   行った（Q1 の許可、下の q1838-i01）。

## 設計の要点（design.md 第 3 版）

- (b) は 2 つの仕組みの組: **B-K**（nvrtx の kernel が短い command buffer の push buffer を End の時に組み置き、submit は ring に載せるだけ。
  texture の header と sampler は session に 1 つの表に object ごとに 1 回書く）と **B-U**（libvulkan が fence も semaphore も無い vkQueueSubmit を
  recording の thread に渡し、返事を待たずに返す）。
- subs-1000 の伸びは B-U からしか生まれない（今の mview は「記録 → End → submit → 次の記録」で、submit が kernel を待つ限り組み立てをどこへ
  動かしてもアプリの thread は縮まない）。10.0.10.3 の実測で、今の GPU の 1 part は約 9〜10 µs（timing 有り）、B-U の後の律速は thread 側の約 10 µs で、
  NVK の 1 part 13.3 µs より短い。見込み 75〜90 fps（未計測の見積もり）。
- B-K は今の 6 つの bench の fps を動かさない。記録し直さない bench でも GPU が律速に近く、fps の伸びは 1 割に届かない見込みで、効き目は submit の
  CPU の時間（kernel の時間が半分以下）に出る。
- big-1000（inflight 1）は (b) では縮まない。B-K は draw 256 以下の command buffer だけを組む。
- push buffer は kernel が組むまま（GPU の address space が全 session で 1 つなので、user が書く形は隔離されない）。
- HAL は変えない（差分の案は無い）。`include/uapi/` も P-A・P-B では変えない。P-C だけが native の capset の word に 1 bit を足し、その約束を記す
  `include/uapi/gpu-op.h` の comment を書き足す。

## 実装の Phase の案（design.md §13）

| 案 | 中身 | 受け入れの要点 |
| --- | --- | --- |
| P-A（libvulkan、1.5〜2 日） | 0. GPU の 1 run を timing 無しで測り直す。1. B-U（hand-off、入口の drain、back-pressure、失敗の latch、fork、環境変数の切り）。2. host 試験 H3a・H3b（libvulkan の結合、TSan の 3 thread） | subs-1000 ≥ 75.4 fps、subs-100 +20 %、他の 4 bench は幅の内、画素の差 0、desktop・game mode の後退なし、Venus PASS、hand-off が入った数が 0 でない |
| P-B（nvrtx、約 3 日） | B-K（heap、chunk と回収、End の組み置きと適格、ring の list、set の世代、組み置きの run の GPU 時間の数え、`nvrtx.vk.prebuild=1`）、mview の `--record=once`・`--submit-all`、host 試験 H1（意味の状態の比べ）・H1b（ring の list） | `--record=once` の subs-1000 で 1 submit の kernel の時間が切の半分以下、fps が切以上、GPU の使用率 85 % 以上、`--submit-all` の画素の差 0、6 bench の後退なし、part の 99 % 以上が組み置き |
| P-C（libvulkan、約 1 日、任意） | End を先に送る（eager End）と引き戻し、capset の bit | `--submit-all` で frame −15 %、desktop の後退なし |
| P-D（任意、別の設計） | build を node の lock の外へ、quad の record の組み置き、長い command buffer の保持、読み取りだけの写し | — |

## ユーザーへの問い

前置き: WS291 は「GPU の driver の描画の速さ（desktop 60 fps と重い 3D、AMD RDNA → NVIDIA → PowerVR）」の WS で、今は incomplete。この p026 は
その NVIDIA（Debian の機械 10.0.10.3 に挿した RTX 2070、amd64、driver nvrtx）の、多数の小さな submit を出す bench（subs-1000: 1 frame に 1000 回の
vkQueueSubmit）を Linux の NVK（75.4 fps）並みにするための設計の Phase。今 58.5 fps。2026-11-01 にユーザーが「記録の時に push buffer まで作る」形
（(b)）を設計から進めると決めた。設計とレビューで、(b) の kernel の部分（B-K）だけでは subs の fps は伸びず、伸びは libvulkan が submit を待たずに
返す部分（B-U）から来ると分かった。今の状態: 設計（design.md 第 3 版）と敵対的レビュー 2 回は済み、code はまだ無い。決めてほしいのは下の 2 点
（Q3 は Q2 で P-C を選んだ時だけ）。

用語:
- **push buffer**: GPU が読む命令の列。今は vkQueueSubmit の時に kernel が組む。
- **recording の thread**: libvulkan の中の別の thread。今は長い記録を送るのに使っている。
- **hand-off**: vkQueueSubmit の仕事を recording の thread に渡し、kernel の返事を待たずにアプリへ戻ること。
- **DEVICE_LOST**: Vulkan の「GPU を失った」失敗。アプリは普通 device を作り直すか終わる。**OOM** は「memory が足りない」失敗で、アプリが立ち直れる
  ことがある。
- **組み置き（B-K）**: vkEndCommandBuffer の時に kernel が push buffer を組んでおき、submit は ring（GPU の受け口）に載せるだけにする形。

### Q1: fence の無い vkQueueSubmit を、kernel の返事を待たずに返してよいか（B-U）

- 背景: subs-1000 の伸びは、submit が kernel を待たずに返る事からしか生まれない（design.md §3.2）。10.0.10.3 で測ると今の GPU の 1 part は約 9〜10 µs で、
  待たずに返せば律速は libvulkan の thread の約 10 µs になり、NVK の 13.3 µs/part（75.4 fps）より速い見込み。返事を待たないと、その submit の失敗は後の
  呼び出しで DEVICE_LOST として見える（OOM も DEVICE_LOST になる）。非同期になるのはその submit の kernel の仕事の全部で、(b) の kernel の部分（B-K）が
  あってもこの点は変わらない。普段の frame では kernel の中で memory を取らないので、遅れて見える失敗は実際にはほぼ GPU を失った時と、最初の frame の
  確保の失敗だけ（推測）。fence・semaphore のある submit・present・WaitIdle は今どおり待つ。対象は nvrtx だけ（他の GPU の driver の道は変わらない）。
- 選択肢:
  - A（推し）: 認める。nvrtx だけ、2 CPU 以上の時だけ、環境変数 `VULKAN_ZEDBSD_SUBMIT_HANDOFF=0` で切れる。得: subs の伸び（見積もり 75〜90 fps、
    未計測）。失: その submit の OOM が後で DEVICE_LOST になる。
  - B: 認めない。B-K（kernel）だけを作る。得: 失敗の意味が今のまま。失: subs-1000 は伸びない（同じ command buffer を記録し直さずに何度も出すアプリで
    submit の CPU の時間が減るだけ）。
  - C: 作るが既定は切り（環境変数で入れる）。得: 既定の意味は今のまま。失: 普段の使い方では速くならない。

### Q2: 実装の Phase の順と範囲

- 背景: P-A（libvulkan、B-U）に subs の伸びがある。P-B（kernel、B-K、約 3 日）は (b) の本体だが、この機械では fps の伸びは 1 割未満の見込みで、
  効き目は記録し直さない command buffer の submit の CPU の時間（半分以下）と、P-C の土台。P-C は複数の command buffer を記録してから 1 回出すアプリ向け
  で、`include/uapi/gpu-op.h` の comment に約束を書き足す（Q3）。
- 選択肢:
  - A（推し）: P-A → P-B。P-C は Future Work。P-A の数が出たら Q1（Claude）が報告し、P-B はその上で測る。得: 早く subs の数が出て、(b) の kernel の形も
    作る。失: P-B の数日は fps にはほとんど出ない。
  - B: P-A → P-B → P-C（Q3 も今決める）。得: 複数の command buffer をまとめて出すアプリでも記録と組み立てが重なる。失: 1 日と uapi の文書の変更が増える。
  - C: P-A だけ。P-B・P-C は Future Work。得: 短い。失: (b) の kernel の形（出し直しの速さ、ring だけの submit）は作らない。

### Q3（Q2 で B を選んだ時だけ）: native の capset の word に 1 bit を足し、`include/uapi/gpu-op.h` の comment に書き足してよいか

- 背景: kernel が「End の時に組む」かを libvulkan に知らせる取り決め。値は nvrtx の `runtime.c` と libvulkan の `context.c` が持つが、約束は
  `include/uapi/gpu-op.h` の comment に書かれている（今は bit 0 の H.264 だけ。bit 1 の同期の command は書き漏れ）。構造体・ioctl・opcode は変えない。
- 選択肢:
  - A（推し）: 足す（bit 2）。comment に bit 1・2 を書き足す。得: 組まない kernel では libvulkan が今の動きのまま。
  - B: 足さない。eager End は環境変数で入れる（既定は切）。得: uapi の文書は今のまま。失: 既定で効かない。

### Q1（Claude）が決めてよい技術の判断（ユーザーには結果だけを知らせる案）

- mview に bench の mode を 2 つ足す（`--record=once`・`--submit-all`）と、NVK の基準を同じ mode で Debian の window で 1 回取る（P-B の受け入れに要る。
  推し: 足す）。第 1 版の Q4 をここへ移した。

## 範囲の外の残り（記録）

- big-1000 の NVK との差（client の encode 3.55 ms 対 3.2〜3.4 ms と GPU の時間）は (b) では縮まない（design.md §3.3）。別の Phase の候補。
- 共有の GPU の address space（全 session が 1 つの VA）は既にある危険で、session ごとの VA と、組み置きと heap を GPU から読み取りだけで写す形は
  Future Work の候補（design.md §8）。

## q1832-i01（2026-11-01 P2、worktree p2-ws291g1、main d4b741303）

- 読んだ物と事実は design.md §1・§2。第 1 版を書いた（2026-11-01 09:24〜）。design-reviewer の 1 回目のレビュー（blocker 2・major 7・minor 12）を受けた。
  反映の前にラップアップで返した（branch に commit は無く、design.md・design-review.md・phase.md は main の 348ec391e4 などに入っている）。

## q1838-i01（2026-11-01 P1、worktree p1-ws291p026、main 3b5614897 の時点）

- design.md を code（HEAD 272e6c2260）で確かめ直し、1 回目の指摘を反映して第 2 版（commit c74cd26650）。design-reviewer の 2 回目のレビュー
  （blocker 1・major 7・minor 11、design-review.md の末尾）を受け、反映して第 3 版。1 回目・2 回目の指摘ごとの反映は design-review.md の表。
- 2 回目のレビューで、第 1 版からの見積もり（GPU の 1 part 5.6 µs）の根拠が誤り（subs の 1 run は 160 でなく約 16 draw）と分かったので、10.0.10.3 で
  1 本だけ測った（Q1 の許可、20:56〜21:03、lock の中）:
  - 実行: `build/tmp/p026-diag/run.sh`（`plan/ws291/tests/rtx-runcost.sh` を KERNEL=p024 i3a（2764259344、ac の腕、P3 の build の写し）、FILES=p024 の base の
    13 file と libvulkan lv9（509112954）、TIMING=on、RUNS=6、`--draws=each --copies=1000 --submits=1000`）。要約は
    build/tmp/p026-diag/subs1000/summary.txt（git の外）。
  - 結果: 56.4〜58.5 fps、段は record 5.6〜5.8・submit 11.2〜11.8・fence 0.00〜0.05 ms。`nvrtx.vk.perf` の GPU の 1 run（timed の標本）は最初の窓
    avg 29 µs・max 1090 µs、後の 10 窓 avg 9〜10 µs。runs 1 窓 21〜30 万。
  - 戻し: 14 file と NVMe の kernel（1463007375）が cksum で元と一致、Debian（RTX 4 function vfio-pci）、lock は free と書いた。
  - これは診断で、受け入れには使わない（P-A の 0 番で、その Phase の build で timing 無しも測り直す）。
- 確かめていない事: timing 無しの GPU の 1 part、標本の偏り（timed は slot が替わる時の run だけ）、SET_TEX_HEADER_POOL を流れの途中で替えた時の
  hardware の振る舞い（設計はそれに頼らない形にした）、NVK・nouveau の source（読んでいない）。
- 消してほしい物（Q1 へ）: build/tmp/p026-in/（計測のために P3 の build から写した kernel と 14 file。計測は済んだ）。

### 再開点（2026-11-01 P1、q1838-i01 を返す）

- 設計の Phase の成果（design.md 第 3 版、design-review.md の 2 回分と反映の表、この phase.md）は commit 済み。残りはユーザーの Q1・Q2（・Q3）の答え。
- 答えの後: Q1 が A なら P-A の Queue（libvulkan、phase-runner-high、10.0.10.3 の割当と Venus の回帰の window が要る）。cleared か uncleared かは Q1 の判定
  （設計の範囲は済んでいる。問いの答えを design.md に反映するのは小さな直しで、次の Queue の頭でよい）。

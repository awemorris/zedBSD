<!-- awesome-plan project=zedbsd record=ws291-p026 -->

# ws291-p026: NVIDIA（nvrtx）: 記録の時に push buffer まで組み、submit は ring に載せるだけにする設計

Status: in-progress（2026-11-01 09:24 P2 q1832-i01 で着手、設計の Phase）
Parent: [WS291](../ws.md)
依存: [p024](../phase024/phase.md)（G1 の名指しと (b) の説明、NVK との段ごとの比べ）
Queue: q1832（P2）

## 目的

2026-11-01 ユーザーから、p024 の残りの差 G1（submit の時に呼び手の thread で push buffer を組む、subs-1000 の 1 submit で約 8 µs）は
(b)「記録の時に push buffer まで作る」形で、設計の Phase から始める、という趣旨の答えを受けた（数日の規模、HAL と UAPI は変えない見込み、
設計と敵対的レビューの後に実装）。この Phase はその設計（code は書かない）。

目標の数（p024 の NVK）: subs-1000 75.4 fps（今 58.5〜58.7）、big-1000 136.2 fps（今 118 前後）。

## 範囲（q1832-i01）

1. 設計の文書 [design.md](design.md): 今の流れの事実、submit の時にしか決まらない物の扱い、command buffer の reset・再記録・同時の使用・secondary、
   組んだ push buffer の memory の持ち方と解放、GPU の fault・CANCEL・OOM（BUG-381）、kernel と userland の境（security）、他の driver と libvulkan
   への影響、HAL・UAPI を変えない根拠、選ばなかった案、試験の計画、実装の Phase の案と受け入れ、危険と戻し方。
2. design-reviewer の敵対的レビュー（[design-review.md](design-review.md)）と反映。
3. ユーザーへの問い（下）。
4. 範囲の外: code、実機・QEMU、big-1000 の client の encode の短縮（下の「範囲の外の残り」）。

## 設計の要点（詳しくは design.md）

- (b) は 2 つの仕組みの組で subs-1000 に効く: **B-K**（nvrtx の kernel が短い command buffer の push buffer を End の時に command buffer の持つ
  memory に組み、submit はそれを ring に 1 回で載せる）と **B-U**（libvulkan が fence も semaphore も無い vkQueueSubmit を、その記録と一緒に
  recording の thread に渡し、返事を待たずに返す）。
- 今の mview の subs は「記録 → End → submit → 次の記録」で、submit が組み立てを待つ限り、組み立てを End に動かしても別の thread に置いても
  アプリの thread の 1 part は縮まない。p024 の見積もり（70 fps 台）が数えた重なりは B-U があって初めて生まれる。B-U だけでも伸び、B-K だけでは
  伸びない（design.md §3.2）。
- big-1000（inflight=1）は今も GPU が submit の時の組み立てと重なって走るので、(b) では frame がほぼ縮まない。長い command buffer を End で組むと
  後退するので、B-K は組まない（§3.3）。
- push buffer は kernel が組むまま（GPU の address space が全 session で 1 つなので、user が書く形は隔離されない、§8）。
- HAL・`include/uapi/` は変えない。P-C だけ renderer の capset の native の flag に 1 bit（§10）。

## 実装の Phase の案（design.md §13）

| 案 | 中身 | 受け入れの要点 |
| --- | --- | --- |
| P-A（libvulkan、1〜1.5 日） | B-U（hand-off、drain、back-pressure、失敗の latch、環境変数の切り） | subs-1000 ≥ 75.4 fps、subs-100 +20 %、他の 4 bench は幅の内、画素の差 0、desktop・game mode の後退なし、Venus PASS |
| P-B（nvrtx、2〜3 日） | B-K（granule、End の組み立て、適格と fallback、多 segment の ring、set の世代、`nvrtx.vk.prebuild=1`） | 新しい `--record=once` の subs-1000 で入が切の 1.5 倍、6 bench の後退なし、画素の差 0 |
| P-C（libvulkan、1 日、任意） | End を先に送る（eager End）と引き戻し | 新しい `--submit-all` で frame −15 %、desktop の後退なし |
| P-D（任意、別の設計） | build を node の lock の外へ、長い command buffer の retain | — |

## ユーザーへの問い

前置き: WS291 は「GPU の driver の描画の速さ（desktop 60 fps と重い 3D、AMD RDNA → NVIDIA → PowerVR）」の WS で、今は incomplete。この p026 は
その NVIDIA（RTX 2070、driver nvrtx）の、多数の小さな submit を出す bench（subs-1000: 1 frame に 1000 回 vkQueueSubmit）を Linux の NVK
（75.4 fps）並みにするための設計の Phase（今 58.5 fps）。決まっていないのは下の 4 点。

用語:
- **push buffer**: GPU が読む命令の列。今は vkQueueSubmit の時に kernel が組む。
- **recording の thread**: libvulkan の中の別の thread。今は長い記録を送るのに使っている。
- **hand-off**: vkQueueSubmit の仕事を recording の thread に渡し、kernel の返事を待たずにアプリへ戻ること。
- **DEVICE_LOST**: Vulkan の「GPU を失った」失敗。アプリは普通 device を作り直すか終わる。**OOM** は「memory が足りない」失敗で、アプリが
  立ち直れることがある。
- **fallback**: 組み置きができない command buffer を、今の作り方（submit の時に組む）で走らせること。

### Q1: fence の無い vkQueueSubmit を、返事を待たずに返してよいか（B-U）

- 背景: subs-1000 の伸びは、submit が kernel を待たずに返る事からしか生まれない（design.md §3.2）。返事を待たないと、その submit の失敗は
  後の呼び出しで DEVICE_LOST として見える（OOM も DEVICE_LOST になる）。fence・semaphore のある submit・present・WaitIdle は今どおり待つ。
  対象は nvrtx だけ（他の driver の道は変わらない）。
- 選択肢:
  - A（推し）: 認める。nvrtx だけ、2 CPU 以上の時だけ、環境変数 `VULKAN_ZEDBSD_SUBMIT_HANDOFF=0` で切れる。得: subs の伸び（見積もり 75〜90 fps）。
    失: その submit の OOM が後で DEVICE_LOST になる（B-K の後は非同期の部分が ring だけになり、実際に起きるのはほぼ GPU を失った時だけ）。
  - B: 認めない。B-K（kernel）だけを作る。得: 失敗の意味が今のまま。失: subs-1000 は伸びない（同じか少し下がる見込み）。同じ command buffer を
    記録し直さず何度も出すアプリでだけ速くなる。
  - C: 作るが既定は切り（環境変数で入れる）。得: 既定の意味は今のまま。失: 普段の使い方では速くならない。

### Q2: 実装の Phase の順と範囲

- 背景: B-U（P-A、libvulkan）に subs の伸びがあり、B-K（P-B、kernel、数日）は同じ command buffer の出し直しと、組み立ての置き場所を変える (b) の
  本体。P-C は複数の command buffer を記録してから 1 回出すアプリ向け。
- 選択肢:
  - A（推し）: P-A → P-B → P-C。得: 早く subs の数が出る。P-B は P-A の上で測る。
  - B: P-B → P-A → P-C。得: (b) の kernel の形を先に。失: P-B だけの時点では subs の数が動かない。
  - C: P-A だけ。目標に届いたら P-B・P-C は Future Work に。得: 短い。失: (b) の kernel の形（出し直しの速さ、ring だけの submit）は作らない。

### Q3: renderer の capset の native の flag に 1 bit を足してよいか（P-C だけ）

- 背景: kernel が「End の時に組む」かを libvulkan に知らせる取り決め。`include/uapi/` の外（nvrtx の `runtime.c` と libvulkan の `context.c` が
  同じ値を持つ、今 bit 0 が H.264、bit 1 が同期の command）だが、kernel と userland の間の約束なので確かめる。
- 選択肢:
  - A（推し）: 足す（bit 2）。得: 組まない kernel では libvulkan が今の動きのまま。
  - B: 足さない。eager End は環境変数で入れる（既定は切）。得: 取り決めは今のまま。失: 既定で効かない。

### Q4: mview に bench の mode を 2 つ足し、NVK の基準も同じ mode で取るか（P-B・P-C の受け入れ用）

- 背景: 今の bench は毎 frame 記録し直すので、B-K の効き目（出し直しと先の組み立て）が見えない。
- 選択肢:
  - A（推し）: 足す（`--record=once`、`--submit-all`）。NVK の基準は Q1 が割り当てた window で 1 回。
  - B: 足さない。P-B・P-C は「後退なし」だけで受け入れる。

## 範囲の外の残り（記録）

- big-1000 の NVK との差（client の encode 3.55 ms 対 3.2〜3.4 ms と GPU の時間）は (b) では縮まない（design.md §3.3）。別の Phase の候補。
- 共有の GPU の address space（全 session が 1 つの VA）は既にある危険で、session ごとの VA は Future Work の候補（design.md §8）。

## q1832-i01（2026-11-01 P2、worktree p2-ws291g1、main d4b741303）

- 読んだ物と事実は design.md §1・§2。第 1 版を書いた（2026-11-01 09:24〜）。

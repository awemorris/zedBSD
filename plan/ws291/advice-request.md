# GPU 描画の速さについての技術相談（自作 OS の Vulkan 実装、NVIDIA RTX 2070 と Raspberry Pi 5）

（自分用の参照: WS291 phase024 と phase021。以下は外部の方に単独で読んでもらう文書で、リポジトリには触れられない前提で書く。）

## 0. 相談の要点

自作 OS の GPU driver と自作の Vulkan 実装で、次の 2 つの描画の速さを詰めている。助言をもらいたい。

- **A（主）**: NVIDIA RTX 2070（Turing）。「小さな vkQueueSubmit を 1 フレームに 1000 回」の benchmark が約 43 fps、同じ機械の Linux（Mesa の NVK）は 75.4 fps。「1 回の submit に draw が 16000 本」の benchmark は約 119 fps 対 NVK 136.2 fps。1 回の submit の CPU 費用が約 23 µs（record 込み）で、NVK は約 11 µs（record 込み、フレーム全体では約 13 µs）。この差の詰め方と優先順位を知りたい。
- **B（従）**: Raspberry Pi 5 の V3D 7.1。1920x1080 で約 270 万三角形のシーンが約 48〜50 fps。GPU の中で bin（頂点側）と render（画素側）が直列に走っている。次のフレームの bin を今のフレームの render と安全に重ねる方法と、bin の費用を下げる他の手を知りたい。

質問は各節の末尾と、第 5 節の回答形式にまとめた。数値の詳細表は付録にある。数値と動作の記述は、手元の計測記録とソースから取った。記録に無い事は「未確認」と書いた。

## 1. 背景

- **zedBSD** は、kernel・GPU driver・窓システム・Vulkan 実装を全部自作している OS。Linux のコードは複写していない。
- **libvulkan** は自作の Vulkan のユーザー空間ライブラリ。GPU ごとの backend を持つ。Mesa は使っていない。
- **GPU core** は kernel 内の共通層。全 GPU driver が共有する ioctl の ABI を提供する。「job」（監督付きの submit。予約、期限、commit、完了台帳を持つ）を管理する。監督用の thread が期限と停止を見る。
- **nvrtx** は NVIDIA の driver。GPU の初期化と channel の作成は NVIDIA の GSP ファームウェアに RPC で頼む（nouveau の GSP 利用に近い形）。
- 設計上の特徴は、**Vulkan の command の解釈と GPU 向けの push buffer（GPU が読む method の列）の生成を kernel の中で行う**こと。libvulkan は vkCmd* を独自の wire 形式（opcode と引数の列）に符号化して ioctl で kernel に渡す。kernel は shader の compiler（SM75 向け）も持つ。
- **Keiland** は自作の compositor（窓システム）。**game mode** は、全画面の client に swapchain の image を貸し、合成せずに画面へ直接出す（direct scanout）モード。ここでの計測は全部この game mode。
- **mview** は自作の 3D model viewer で、benchmark の本体。モデル qs40（37000 三角形）を複製して描く。解像度は 1920x1080、present は mailbox、swapchain の image は 4 枚、1 回 8 秒の計測。
- 速さの基準は、同じ機械（RTX 2070 を挿した Debian 13、Linux 6.12、Mesa 25.0.7、nouveau の GSP 有効）上の NVK。同じ mview の Wayland client を weston で動かして取った。weston は NVK では合成を GPU でやる（direct scanout にならない）ので、NVK の数値は合成 1 枚分を含む。この条件の差が結果に与える影響の大きさは未確認。
- 機械の CPU の型と core 数は記録に無い（未確認）。

### benchmark の定義

- `scene-N`: 頂点・index を device buffer に置き、instancing で N 体を描く。1 フレーム 16 draw。GPU の頂点処理が支配的。
- `big-N`: 1 回の submit に 16×N 本の draw を入れる。1 draw は、pipeline と descriptor set の bind（変わる時だけ）、push constants 128 byte、indexed draw の 3〜5 命令。big-1000 は 16000 draw、命令は約 5 万〜8 万。
- `subs-N`: 1 コピー（16 draw）の小さな command buffer を N 回 submit する。subs-1000 は 1 フレーム 1000 submit。各 submit の command buffer は毎フレーム記録し直す。

scene は NVK と同等（49.8 fps 対 50.1 fps）で、GPU の頂点側の固定機能が律速と見ており、ここでは扱わない。問題は CPU 側の submit 費用を測る big と subs。

## 2. 対象 A: NVIDIA RTX 2070

### 2.1 現状の数値（NVK との比較）

| benchmark | zedBSD 現在 | NVK | 備考 |
| --- | --- | --- | --- |
| subs-1000 | 約 43 fps（窓ごとに 39〜44） | 75.4 fps | 差の主題 |
| big-1000 | 約 119 fps（118〜119） | 136.2 fps | 次の主題 |
| scene-5000 | 49.8 fps | 50.1 fps | 同等 |
| desktop（compositor の操作） | 60 fps 以上 | — | 基準達成済み |

### 2.2 現在の submit の経路

1 回の vkQueueSubmit（subs の場合）が通る段を順に書く。

1. **ユーザー空間（libvulkan）**
   - vkCmd* の記録は、command buffer ごとに wire 形式へ符号化する。1 命令あたり約 45 ns。小さな記録は単独では送られず、次の submit の transaction の頭に同梱される（ソースの読みによる。kernel への ioctl は submit の 1 回に収まる）。長い記録（big）は 256 KiB ごとに別 thread が kernel へ送る。
   - vkQueueSubmit は、fence も signal の semaphore も無い場合にも、**libvulkan 自身が「私用の fence」を付ける**。完了の追跡と、台帳（ledger）への登録のため。ledger の entry を calloc で作る。
   - 外部 fence の準備を済ませ、transaction（記録の同梱分と queue submit の op）を作る。
2. **ioctl `GPU_JOB_SUBMIT`（1 submit に 1 回）** — GPU core の中
   - 要求構造体の copyin と検証。stream 本体の copyin（open ごとの固定 buffer へ。16 KiB まで）。
   - job の予約（reserve）: 完了 slot、fence の bind、期限（10 秒）の設定。
   - driver の command 関数を呼ぶ（nvrtx では呼び手の thread でそのまま実行）。
   - 成功なら commit（予約 → COMMITTED）、拒否なら cancel。outcome を copyout。
   - 監督 thread の起床は、期限が早まる時などに限って 1 回（以前は 1 submit に 4〜5 回あった）。
3. **nvrtx の kernel 内の処理（呼び手の thread）**
   - stream を復号し、vkCmd の record（約 120 byte の構造体、command buffer ごとの配列）として保持する。vkResetCommandBuffer で record の参照を外す。
   - queue submit 時に **executor** が record を順に走らせ、push buffer の method 列に変換する。push buffer は session ごとに最大 3 slot（各 2 MiB。push buffer 1 MiB、定数 buffer、texture header の pool など）。同じ push buffer 内の直前の draw との差分だけを書く。定数 buffer 0（push constants と texture handle）は session で 1 つ持ち、GPU が書き、CPU は書かない。
   - 区切り（最初は 256 draw、以後 2048 draw ごと）で push buffer を ring に積む。GPU は executor の書き終わりを待たず走り始める。
4. **ring への投入**
   - ring の 1 submit 分の entry（8 byte ずつ）を書き、**USERD（channel の GET/PUT 領域）の put を 1 回だけ書く**。put は BAR1 の window に写した USERD への 1 回の store。その後 doorbell を MMIO register への 1 回の書きで鳴らす。
5. **完了の扱い**
   - run（push buffer 1 つの GPU 実行）の終わりに、GPU が fence 値を semaphore に書く。run どうしは同じ channel で直列、各 run の先頭で前の run の終了を待つ（WFI）。
   - 「完了 marker」を kernel の completer thread が順に処理する。完了の検出は semaphore 値の読みで、期待終了時刻の手前までは眠り、以後は短く待つ。割り込みを主に使うかは未確認。
   - ユーザー側の台帳は、終わった job をおよそ 7 submit ごとに `GPU_JOB_RETIRE`（最大 64 job をまとめて消費する ioctl）で回収する。

### 2.3 測った内訳（subs-1000、最新の構成）

1 submit あたり約 23 µs（フレーム 23.3 ms ÷ 1000）。

| 段 | 時間 |
| --- | --- |
| 記録（vkCmd 約 50〜80 命令、begin/end 込み） | 約 5.6 µs |
| vkQueueSubmit 全体（計測の水増し込み） | 約 17.2 µs |
| そのうち ioctl `GPU_JOB_SUBMIT` の kernel 内 | 約 15.8 µs |
| 　要求の copyin と検証 | 1.0 |
| 　stream の copyin | 0.7 |
| 　job の reserve（copyout 込み） | 1.4 |
| 　driver 内 | 10.5 |
| 　　executor（begin 0.1、16 draw 3.3、ring への投入 1.0、他 約 1.6 は内訳不明） | 6.0 |
| 　　stream の復号と record 化 | 約 1.7 |
| 　　record の参照外し（RESET） | 0.8 |
| 　　BEGIN | 0.1 |
| 　finish（commit） | 0.9 |
| 　後始末（pin 外し、outcome の copyout） | 1.2 |
| 私用 fence の取得 | 約 1 µs |
| 台帳の回収（7 submit に 1 回、1 回 約 7 µs） | 約 1 µs / submit |

- 計測は kernel 内の時計の読み 1 対が約 45 ns かかるので、細かい段ほど水増しされている。
- system call の入口そのものは軽い（getppid 190 ns、GPU_GET_INFO 1150 ns）。ioctl の 1 回あたりの copyin/copyout は約 0.7〜1.0 µs。1 submit あたりユーザー空間との copy が 4 回（約 3 µs）。
- NVK（同じ benchmark、1 フレームの各段を 1000 で割った値）: record 約 6.1 µs、submit 約 4.6 µs、fence 待ち 約 2.5 µs、フレーム全体で 13.25 µs。
- executor（6.0 µs、うち 16 draw が 3.3）を除けば、1 つの段で 3.5 µs を超える物は無い。薄く広く散らばっている。

**GPU 側の 1 run の費用**は、CPU 側がまだ遅かった時期の計測で、1 run（16 draw）約 20 µs、空の run（draw 無し）約 4 µs。この時期は GPU の空きの方が大きかったので律速でなかった。今の構成で再測定していない（未確認）。CPU 側の費用が NVK 並みの 13 µs 以下に落ちた時に、**GPU 側の 1 run 約 20 µs が新しい律速になる**かもしれない、と懸念している。NVK は 1000 run を 13 ms 強のフレームで走らせているので、1 run は 13 µs 以下のはず（推定）。

### 2.4 big-1000 の内訳

| 段 | zedBSD（最新） | NVK |
| --- | --- | --- |
| record | 約 3.55 ms | 3.15 ms |
| submit | 約 3.3 ms（GPU は実行中） | 0.03 ms |
| fence 待ち | 約 1.0 ms | 4.09 ms |
| フレーム | 約 8.4 ms（残りは present など） | 7.34 ms |

- NVK は vkCmd が push buffer を直接書くので、record の後は submit がほぼ空で、GPU が約 4.1 ms 走る。
- zedBSD は record（client の符号化と kernel の復号が重なる）の後、submit の中で executor が push buffer を書きながら GPU が走る。GPU は約 4.3〜4.4 ms（1 draw 約 0.22 µs。NVK の約 0.256 µs と同等かやや速い）。
- 7.35 ms に届くには、record を約 2.6〜3.0 ms に縮めるか、GPU の実行を record と重ねる必要がある。後者は Vulkan の意味（submit まで GPU は動かせない）の外だと見ている。
- vkCmd 1 命令あたりの費用は client 側で約 45 ns、kernel の復号が約 0.08〜0.14 µs/record（短い道の後は約 160 命令）。user 空間の profiler が target 上に無く、encode の内訳は未計測。

### 2.5 試した事とその結果

subs-1000 の fps（同じ窓の比べ）は 10.9 → 約 43 まで来た。経過の要点を、効いた物と効かなかった物に分ける。

**効いた物**

- 画面に出る image を block-linear（NVIDIA の tile 配置）で共有し、pass の終わりごとの全画面 copy を無くした: subs-1000 10.9 → 約 20。
- register の読みは 1 回約 2 µs かかる。ring の USERD put を 1 submit に 1 回だけ、BAR1 の store に: 19.9 → 25.5。GR の trap register の読みを 1 ms に 1 回までに。
- texture header と sampler の words を object に覚え、image の VA が同じ間は作り直さない。`nvrtx_put32` を word の store に: 25.5 → 約 29。
- `GPU_JOB_SUBMIT`（reserve・command・commit を 1 本の ioctl に）と `GPU_JOB_RETIRE`（回収をまとめる）: 約 28.7 → 32.6（+17%）。subs-100 258 → 300。
- 監督 thread の起床を、期限が眠る期限より早まる時だけに: 約 32 → 34.5。
- executor の state を session に持ち、submit ごとの kern_calloc を無くす。stream の写し buffer を open ごとの固定に。libvulkan の writer を最初から必要な大きさで取る（倍々の realloc が 1 submit に 4 回あった）: 約 35 → 約 43（+23%）、vkQueueSubmit 22.4 → 17.2 µs。
- big の側: executor が直前の draw との差分だけを書く、定数 buffer 0 を session で永続、texture handle の表の再利用で 48 → 約 70 fps。libvulkan の送り thread で長い記録を kernel に非同期に送る: 約 70 → 85〜91。kernel の record の短い道（draw 5 命令を直に読む）: 85〜88 → 92〜100。push buffer を 256 draw、以後 2048 draw で区切って GPU を早く走らせる: 〜116。recording の batch を 1 MiB → 256 KiB: 114〜115 → 118〜119。

**効かなかった物（再提案は不要）**

- run ごとの GPU 側の固定の段（shader cache の invalidate、barrier の WFI、run 終わりの WFI の fence、fence の membar と non-stall 割り込み）を 1 つずつ、または全部外す診断: subs-1000 は 19.8〜20.6 fps から動かず。その時点で CPU の道が律速だった。draw 無しの空の run でも約 34 µs の CPU が残った。
- ユーザー空間の ledger の回収の閾値を 8 → 24 に上げる: subs-1000 +6〜11% だが big-100 が −3% で、採らなかった。
- blob の stream の写しを session に持つ、executor の slot を 3 → 8、pipeline 間の program 共有、dirty state の追跡、record の hold を 1 回にする、compiler の FFMA 化: 効果は揺れの内。
- 512 KiB・128 KiB の batch: 256 KiB と同等。
- 送り thread の導入時に、渡す大きさを 1 batch の 1/4 にする試し: big-100 が 6〜7% 遅くなった（その後、batch 自体を 256 KiB にした形では効いた。上記）。
- 画素は、変更ごとに 100 コピーの still を取り、基点と差 0 を確かめている。

### 2.6 残る 3 つの道とトレードオフ

(a) **kernel の VM の小さな copyin/copyout の速い道**
- 現在の `copyin`/`copyout` は、現 address space の範囲を VM の lock の下で確かめ、ページを pin してから複写する（pin は copy ごとに作る）。1 submit で 4 回、合計約 3 µs。
- 小さい（数十〜数百 byte）copy だけ、pin を作らず、現在の address space で直に読む/書く道（fault を例外で受ける）が作れれば、見積で 1.5〜2 µs の短縮（未測定）。
- 危険: fault と address space の切り替え、他 thread からの unmap との競合。

(b) **command buffer の記録の時に push buffer（GPU の method 列）まで作る。submit は ring に積むだけ**
- 見積では、submit 時の復号（1.7 µs）、record の参照外し（0.8 µs）、executor の一部が消えるが、work 自体は record 側へ移るだけで、記録と submit が同じ thread で直列なら、subs の総 CPU は 2.5 µs 程度しか減らない（見積）。big では、今は executor が GPU の実行と重なって隠れているので、record に移すと逆に遅くなるかもしれない。kernel が batch 到着時に変換して client の符号化と別 core で並列にできれば、record の長さは max(client の符号化、kernel の変換) になり、損をしない見込み。
- 難所: push buffer の中身が submit 時の状態に依存する。texture pool の handle の割り当て、descriptor set の内容（record 後・submit 前に更新され得る）、定数 buffer 0 の差分書き込み（直前の draw が前提）、memory の再 bind、secondary command buffer と再 submit（同じ buffer を複数回 submit）。NVK は record 時に push buffer を書くが、descriptor を GPU が読む形（bindless 風）で状態依存を避けていると理解している（未検証）。
- 設計の作り直しが大きい。

(c) **fence も signal の semaphore も無い submit に、私用 fence と job を付けない**
- 見積では、job の reserve（1.4）、finish（0.9）、後始末（1.2）、私用 fence（1）、台帳の calloc と回収（約 1）で 5〜6 µs 減る（未測定）。3 案で最大。
- 代償: 「全ての native な submit は kernel の監督（期限、hang の検出と回復）の下にある」という設計決定を変える。監督無しの submit が hang した時の検出を、同じ queue の後続の job（fence 付き）に任せる形が要る。
- 製品上の判断で、まだ決めていない。助言があれば、安全に成立する形を知りたい。

3 案の見積の合計で 9〜10 µs。現在と NVK の差（約 10 µs）にほぼ見合う。ただし (b) を除く場合は届かない。

### 2.7 助言してほしい点

1. NVK、nouveau、NVIDIA 公式 driver は、1 submit の kernel 側の費用を 5 µs 前後に保つために何をしているか。1 submit あたり ioctl は何本か、fence の扱い（syncobj、timeline）、kernel 側の scheduler、ring の書き込みと doorbell の方式を、動作の記述として教えてほしい（コードの貼り付けは不要）。
2. (a)(b)(c) のどれが一番効くか、どの順で進めるか。この見積に誤りが無いか。
3. 内訳の中で、他に不自然に見える物は何か。
4. 他の手法（例えば、submit を共有 memory の ring に積み、ioctl を省く方式。fence の割り込みをまとめる方式。submit 前提の作業の別 core への分散）は現実的か。
5. NVK との差を段ごとに profile する方法。記録・submit・fence・GPU の 1 run を NVK 側で分けて計る手順（Mesa や nouveau のどの trace や perf の仕組みを使えるか）。
6. GPU 側の 1 run の固定費（上記の約 20 µs、空 run 約 4 µs）は Turing で妥当か。CPU 側が NVK 並みになった時に、これが新しい壁になる懸念をどう検証するか。
7. big-1000 で 136 fps に届くための現実的な方法。

## 3. 対象 B: Raspberry Pi 5（V3D 7.1）

### 3.1 条件と現状

- GPU は V3D 7.1.10、12 QPU、1.15 GHz。1920x1080 で、qs40 を 74 コピー（約 274 万三角形、PTB が binned と数える数も 2.74M）描く。目標は 50 fps（RTX 2070 での 50 fps の scene を FP32 性能の比で割って決めた）。
- 現状は約 48〜50 fps（offscreen と画面で同程度）。GPU の bin 約 9.2 ms、render 約 8.4 ms。**この 2 つは直列**に走る（フレーム 20 ms 前後）。以前の計測では bin 12.8 ms、render 8.6 ms だった（coordinate shader の入力削減の前、性能 counter の値からの換算）。
- desktop（compositor の操作）は約 60 fps だが、窓の生成時に hitch（短い引っかかり）が出る。原因は未調査。
- bin の性能 counter（以前の計測）: QPU が動いている割合は 12 QPU に対し約 11%、stall は少ない。つまり bin は shader の計算でなく、前段（頂点の取り込み、PTB（tile binning）のリスト書き込み）で詰まっていると見ている。

### 3.2 今の job の流し方

- driver は 1 つの worker が 1 job ずつ処理する。bin の command list を起動し、完了の IRQ を待つ。その後に render 側の cache を無効化して render を起動し、IRQ を待つ。bin と render の依存は IRQ によるソフトウェアの待ちで作り、ハードウェアの semaphore の packet は使わない。
- tile list のメモリ（pool）は job ごとに指定し、bin が足りなくなった時の overflow 用に 256 KiB 単位の pool を最大 4 つ（それぞれ最大 16 MiB）渡す。足りなくなると IRQ で次の pool を worker が渡す。
- bin に依存する render は、bin の完了を確かめるまで hardware の FIFO に入れていない。worker は 1 つで、job は 1 度に 1 つしか進まない。

### 3.3 やったこと

- TFU（V3D の texture 変換器）による texture の raster → UIF（V3D 固有の tile 配置）変換と mip tree を実装した。7.1 の変換の self-test が通れば TFU を使い、通らなければ CPU で変換する。
- coordinate shader（bin 用の頂点 shader）が、位置に必要な attribute だけを読む（12 語 → 7 語）。属性の fetch は VCD（頂点の取り込み器）が user の buffer を GPU アドレスで直接読む。
- 32 bit の index が全て 16 bit に収まる時は、16 bit の写しを作って bin に読ませる（instance ごとの読み直しで index の読みが半分）。
- bin の overflow pool を、pass の頂点数 × instance × 2 byte から決める（以前は 256 KiB × 4 の固定で足りず ENOMEM だった）。
- VCM の batch 数を VPM の大きさから決める（4.2 の式を 7.1 にも）。uniform の stream を使う。

### 3.4 助言してほしい点

1. **bin（フレーム N+1）と render（フレーム N）を安全に重ねる方法**。Mesa の v3d の kernel driver と V3D のハードウェアは、bin 用と render 用の job queue を分けて、render を bin の完了 fence に依存させているはずと理解している（未検証）。我々が気をつけるべき点を挙げてほしい: tile list と tile state の pool の二重化、overflow pool の扱い（bin 中に伸びる）、cache の flush と invalidate の順（bin の出力を render が読む前）、frame 間で共有する buffer（uniform、instance data）の書き込みの競合、performance counter、reset 時の復旧、hardware の FIFO の深さ、7.1 での違い（semaphore の packet や、job の fence の作り方）。
2. その重ねでの理論上の上限（max(bin, render) ≒ 9 ms）に対し、実際に期待できる値。
3. bin の費用を下げる他の手。例えば、primitive の数・頂点の取り込みの形・tile の大きさ・MSAA・PTB の設定・VPM の配分。clip の対処、cull を bin 側に入れるか。
4. 窓生成時の hitch の典型的な原因（pool の確保、TFU の変換の待ち、IRQ の取りこぼし）。

## 4. 守ってほしい制約

- kernel のプラットフォーム抽象層（HAL）の API は変更しない。GPU driver は HAL を直接呼ばない。
- **Linux・Mesa・NVIDIA のコードを複写しない**（ライセンス上）。動作・設計・手順・register や field の意味の記述は歓迎。コードの貼り付けは不要で、貼られても使えない。
- 土台の kernel は Linux ではない。Linux の機構（DRM の scheduler、dma-fence、syncobj など）は、そのまま使えない。我々の仕組みで何に当たるかを書いてもらえると助かる。

## 5. 回答の形式

- **順位付けの提案**を列挙してほしい。各提案に次を付ける。
  1. 何をするか（動作の記述、数行）。
  2. 期待できる効果（µs か fps。根拠、または見積の幅）。
  3. 検証の方法（どの計測で、どの数が動けば成功か）。
  4. 危険や副作用（正しさ、他 benchmark への悪影響）。
- 上の質問ごとに番号を付けて答えてほしい。分からない所は「分からない」でよい。
- 我々の見積に誤りがあれば指摘してほしい（特に (a)(b)(c) の見積と、B の bin/render の重ねの安全性）。

---

## 付録 A: 数値の詳細（RTX 2070）

以下の fps は、実機で同じ build の同じ窓の比べ。boot ごとの揺れが大きい（subs-1000 は同じ build で 39.9〜43.7）ので、窓をまたぐ比べは参考。

### A.1 subs-1000 の経過

| 構成 | subs-1000 fps | subs-100 fps | big-1000 fps | big-100 fps |
| --- | --- | --- | --- | --- |
| 基点（block-linear 共有の後） | 17.0〜20.0 | 176〜188 | 110 | 532〜541 |
| USERD の put を 1 回、BAR1 の store | 25.5 | 238 | 111 | 583 |
| texture header・sampler の object への保持 | 29.2〜29.4 | 256〜259 | 110〜114 | 569〜582 |
| word の store | 28.7 | 258.5 | 113.3 | 586 |
| GPU_JOB_SUBMIT・RETIRE の導入後 | 32.6 | 299.5 | 115.7 | 568 |
| 起床の抑制（capacity） | 34.3〜34.7 | 298〜301 | 揺れの内 | 揺れの内 |
| 起床の抑制の後 → executor の state の持ち越し、stream buffer、writer の後 | 34.8・35.1 → 42.4・43.7 | 304 → 347・365 | 113.3・115.4 → 117.4・116.1 | 549・555 → 555・555 |
| recording の batch 256 KiB | 41〜43 | 354〜362 | 118〜119 | 596〜610 |
| NVK | 75.4 | 測定なし | 136.2 | 測定なし |

### A.2 1 run の固定の費用の診断（GPU 側の段を外す変種、subs-1000、timing 無し）

| 変種 | fps | 備考 |
| --- | --- | --- |
| そのまま | 19.8〜20.6 | submit 42〜43.5 ms/フレーム |
| shader cache の invalidate なし | 同上 | |
| barrier の WFI なし | 同上 | |
| run 終わりの fence の WFI なし | 21.7 | |
| fence の membar と non-stall 割り込みなし | 同上 | |
| record を 1 つも実行しない（空の run） | 23.7〜24.4 | 1 submit 約 34 µs が残った |
| 上の 4 つを全部外した組み合わせ | 19.8〜20.6 の内 | |

subs-100 の GPU の 1 run は約 20 µs、空の run は約 4 µs。executor が GPU を待った時間は、5 秒の窓で 0.1〜0.2 秒（2〜4%）。

### A.3 ioctl 1 本ごとの kernel 内の時間（以前の構成、1 submit あたり）

| ioctl | kernel 内 |
| --- | --- |
| 予約（reserve） | 4 µs |
| command（stream の復号と executor） | 21 µs（うち復号と executor 15.6、executor 11.5、ring 0.8） |
| commit | 2 µs |
| 台帳の回収（WAIT） | 平均 1 本 2 µs |
| ioctl の間のユーザー空間 | 約 11 µs |
| 監督 thread の起床を外した診断 | subs-1000 +9% |

この構成の合計は約 40 µs で、上の 2.3 の最新構成（約 17 µs）はこれを縮めた結果。

### A.4 big-1000 の経過

| 構成 | fps | record / submit / fence (ms) |
| --- | --- | --- |
| 基点（初期） | 48.0 | 8.5 / 11.9 |
| draw ごとの差分書き込み、定数 buffer 0、texture 表 | 63〜70 | 6.7 / 2.3 / 5.2 |
| libvulkan の送り thread | 85〜91 | 4.8〜5.0 / 2.3〜2.6 / 3.8〜3.9 |
| kernel の record の短い道 | 92〜100 | 3.8〜4.1 / 2.2〜2.5 / 4.0 |
| push buffer を 256/2048 draw で区切る | 116 | 3.8 / 3.3 / 1.0 |
| recording の batch 256 KiB | 118〜119 | 約 3.55 |
| NVK | 136.2 | 3.15 / 0.03 / 4.09 |

### A.5 NVK の基準

| benchmark | fps | frame ms | record | submit | fence |
| --- | --- | --- | --- | --- | --- |
| scene-1500 | 165.6 | 6.04 | 0.06 | 0.04 | 5.80 |
| scene-5000 | 50.1（単独の run 49.1） | 19.90 | 0.08 | 0.05 | 19.36 |
| big-1000 | 136.2 | 7.34 | 3.15 | 0.03 | 4.09 |
| subs-1000 | 75.4 | 13.25 | 6.13 | 4.63 | 2.45 |

### A.6 最新構成の各 benchmark（2 回の平均的な値）

| benchmark | zedBSD | NVK |
| --- | --- | --- |
| big-100 | 約 590〜610 fps | 測定なし |
| big-1000 | 約 119 fps | 136.2 |
| subs-100 | 約 355〜365 fps | 測定なし |
| subs-1000 | 約 41〜44 fps | 75.4 |
| scene-1500 | 約 157 fps | 165.6 |
| scene-5000 | 約 49.8 fps | 50.1 |

### A.7 big-1000 の GPU 側

- 1 フレーム約 16000 draw、GPU の実行は submit の始めから約 4.3〜4.5 ms。1 draw 約 0.22 µs（時計込みの executor 側は約 140 ns/draw）。
- 三角形の頂点処理の下限は scene-5000 の GPU 19.4 ms の 1/5（約 3.9 ms）。draw ごとの費用との内訳は、最新の構成では分けて測っていない（未確認）。

## 付録 B: 数値の詳細（Raspberry Pi 5）

| 項目 | 値 |
| --- | --- |
| GPU | V3D 7.1.10、12 QPU、1.15 GHz（起動時は 500 MHz、上げる）、VPM 64 KiB |
| 画面 | 1920x1080、qs40（37000 三角形）× copies |
| copies 1 / 21 / 62 / 74 の GPU 時間 | 2.8〜3.0 ms / 9.4 ms / 19.2 ms / 21.6〜22.9 ms（offscreen、coordinate shader の入力削減の前） |
| c74 の bin / render（以前の計測、88 run の定常） | 14.7 M cycle（12.8 ms）/ 9.9 M cycle（8.6 ms） |
| c74 の最新 | bin 約 9.2 ms、render 約 8.4 ms、約 48〜50 fps |
| bin の QPU active | 19.3 M（12 QPU で 11%）、stall は少ない |
| render の QPU 使用率 | 約 9% |
| binned 三角形 / reversed | 2.74 M / 2.11 M |
| TFU による texture 変換を使わない場合（c21） | 15.95 ms（使う場合 9.43 ms） |

- 同じ bin と render の 2 つを直列に流すことが、c74 の 20 ms 前後のフレームの主な理由。
- 最新の値（bin 9.2 ms、render 8.4 ms、画面での 48〜50 fps、desktop の hitch）は、整理された記録に載る前の最新の計測による。それ以外の表は整理済みの記録から取った。

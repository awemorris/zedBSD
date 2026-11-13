<!-- awesome-plan project=zedbsd record=ws353-p004 -->

# ws353-p004: compositor の hotplug の thread（250 ms の tick を消す）

Status: cleared（2026-11-14 02:32 P1、q2060-i01。build 3 機種 warning 0、host 試験 PASS、QEMU の follow 試験 2 回 PASS（下の「結果」）。Q1 の確認待ち）
Parent: [WS353](../ws.md)。設計 [design.md](../design.md) §5.3・§5.4。

## 目的

hotplug の fence を名前の付いた thread が無期限に待ち、signal で main に知らせる。main の `kwl_output_tick` の 250 ms の look（output-switch.c:46・145〜213）を消し、出力の追従を事象で行う。ユーザーの決定 2 の「最初に直す」の仕上げ。

## 変える file

- `userland/desktop/wayland/output-switch.c`: hotplug の thread（設計 §5.3 の流れ、停止の fence）、`kwl_output_follow`（今の tick の後半）、`follow_pending`。`OUTPUT_HOTPLUG_MS`・`hotplug_checked_ms` を消す。
- `compose.h`・`kwl.h`: hotplug の thread の状態と出口、`follow_pending`。
- `display.c`: `kwl_schedule` から `kwl_output_tick` を外す。
- `compose.c`・`heads.c`・`output-switch.c`: `output_lost`・`heads_stale`・head の `lost` を立てる所（compose.c:552・4665・4892・4906、heads.c:580・695・766）で `kwl_output_follow` を求める。`kwl_display_take` の完了と `output_listed` の終わり、window mode に入った時（`enter_window_mode` の後）で `follow_pending` を見る。window mode でない間（起動の直後、game mode）・display が busy・列挙の途中に来た FIRST・HOTPLUG・lost は `follow_pending` に覚えるだけ（今の tick の条件、output-switch.c:158〜163。設計 §5.4、レビュー M5）。
- `displays-shell.c`: `kwl_displays_pending_tick` を今の期限の仕組みのまま `kwl_displays_tick` 側へ（timer は p006）。
- `main.c`: wake の後の take に hotplug の出口を足す。終了で停止の fence の empty submit（queue の lock の下）と join（`threads.c` の台帳、`KWL THREADS` に `hotplug`）。submit が `VK_ERROR_DEVICE_LOST` で失敗しても join してよい（thread の待ちは device の error で返る、p003 の (7)。設計 §5.3、レビュー M3）。

## 残す振る舞い

- 「新しい fence を先に登録し、列挙はその後」の不変条件、OUTPUT_LOOK_FIRST・SIGNAL・HEADS・RECOVER の意味、limited・move_failed の消し方、出力の移動と head の作り直しの結果。
- Makefile は変えない（新しい file を作らない）。

## 確かめ

- build: compositor（x86_64・arm64・riscv64）warning 0。
- QEMU（Venus、VENUS_DISPLAY=dbus）: 1 本の script で、head の抜き差し（venus-head.sh）→ 出力の移動・head の追従（`KWL OUTPUT` の行を guest の log の file から）、表示中の display を抜いた時の移動、抜き差しの無い 60 s の hotplug の look の回数が 0（main の 10 ms の pass が残る間は `timeouts` では区別できないので数えない。レビュー L12）、game mode の間の抜き差しは window mode に戻った時に追う（M5）、終了で hotplug の thread が join される。
- 回帰: WS113 の出力の移動の試験（plan/tools/guest/ の該当、Q1 の割当）。

## 受け入れ

- 上の判定が PASS。250 ms の look が code に無い。結果・SHA・未実施を書く。

## 依存

- p003。

## 結果（2026-11-14 P1、q2060-i01、branch `agent/p1-ws353d`、基点 main e948b78708）

### commit

- 462c65c226: compositor（`userland/desktop/wayland/` の output-switch.c・compose.h・compose.c・kwl.h・display.c・display-thread.c・displays-shell.c・heads.c・main.c）と host 試験（`plan/ws113/tests/host-output-switch.c`・`.sh`）。
- 8d8c3e908c: QEMU の試験（`plan/ws353/tests/qemu-hotplug-follow.sh`・`config-amd64-follow.mk`）。
- この記録（phase.md・ws.md）の commit は報告に書く。

### 実装

- hotplug の thread（output-switch.c、`KWL THREADS` の名前 `hotplug`）: `kwl_compose_open` の終わり（display の thread の後）で `kwl_output_hotplug_start`。thread は F を登録して FIRST を知らせ、`vkWaitForFences(device, 2, {F, stop}, VK_FALSE, UINT64_MAX)`。F の signal で G を登録してから F を destroy して SIGNAL、stop で終わる、登録の失敗・待ちの失敗（device lost）は UNAVAILABLE を知らせて終わる。thread は compositor の状態に触れない（device の fence と自分の notes だけ）。知らせは lock の下の bit（FIRST・SIGNAL・UNAVAILABLE）と、空から非空の時だけ wake pipe に 1 byte（`main_woken`）。
- 停止: `kwl_compose_close` の最初で `kwl_output_hotplug_stop`（queue の lock の下で `vkQueueSubmit(queue, 0, NULL, stop)`、失敗は log して join。device lost では待ちが `VK_ERROR_DEVICE_LOST` で返る）。終わりに `KWL OUTPUT hotplug stopped wakes=N looks=M`（thread の起床の数と列挙の数、試験が読む）。
- 出力の追従: `kwl_output_tick` と `OUTPUT_HOTPLUG_MS`（250 ms）・`hotplug_checked_ms`・`compose->hotplug` を消した。`kwl_output_follow`（今の tick の後半: SIGNAL・FIRST の列挙、lost の出力の移動、head の作り直し）、`kwl_output_follow_later`（事象から求める: `follow_pending` を立て、0→1 の時だけ wake）、`kwl_output_follow_wake`（保留を止めていた物の終わりに wake）。main は wake の後の take（`kwl_display_take` の後）で `kwl_output_hotplug_take` を呼び、notes を取ってから保留の follow を走らせる。
- 求める所: compose.c の `output_lost` を立てる 3 か所と出力の open の `heads_stale`、heads.c の `heads_stale`（全 head を閉じた時）と head の `lost` の 2 か所、output-switch.c の head の作り直しが EBUSY の時。保留: window mode の外（`!windowed`・`os_paused`）、display が busy、列挙の途中は `follow_pending` を保つだけ。起こす所: `window_mode_opened`（display.c、`windowed = 1` の後）、`kwl_display_end`（display-thread.c）、`output_listed` の始め（列挙の終わり）。
- `kwl_displays_pending_tick` は今の期限の仕組みのまま `kwl_displays_tick` の始めから呼ぶ（timer は p006）。

### 設計からの変更（理由）

1. 事象の所は `kwl_output_follow` を直に呼ばず `kwl_output_follow_later` で求め、走らせるのは main の take の一か所にした。lost を立てる所は render の message の処理や frame の組み立ての途中で、そこから列挙や出力の移動（`kwl_display_begin` → park）を始めると入れ子になるため。wake は「求めが 0→1 になった時」と「保留を止めていた物の終わり」だけで、保留の間に自分で起き続けない（host 試験で確かめた）。
2. hotplug の thread の出口は lane の FIFO ではなく bit（FIRST・SIGNAL・UNAVAILABLE）にした。複数の SIGNAL が main の take の前に重なっても、次の fence は先に登録済みなので列挙 1 回で足りる。FIRST と SIGNAL が重なれば SIGNAL の列挙（limited・move_failed を忘れる以外は FIRST と同じ）。
3. thread が無い時（stop の fence・lock・thread の作成の失敗）と最初の登録の失敗は、今と同じく window mode で列挙を 1 回だけ行う（FIRST）。今の code の「登録の失敗は 250 ms ごとに登録と列挙をやり直す」は周期の見回りなのでやめた（以後 hotplug は追わず、出力・head の lost の追従は続く）。
4. 停止は phase.md の main.c ではなく `kwl_compose_close` の最初に置いた（compose を閉じる全ての経路で device の破棄の前に止まる）。
5. 列挙の record の calloc の失敗は、follow を保留に戻す（次の事象の pass で再び試す。wake はしない: OOM で回り続けないため）。head の作り直しの EBUSY 以外の失敗（ENOMEM）も同じく次の事象まで。

### 確かめ（実行したコマンドと結果）

- build（package の規則は -Wall -Wextra -Werror）: `make -j4 BUILD_JOBS=4 build/x86_64/packages/wayland/.stamp`、`make -j4 BUILD_JOBS=4 CONFIGMK=config/config.rpi4.mk build/rpi4/packages/wayland/.stamp`（arm64）、`... CONFIGMK=config/config.vf2.mk build/vf2/packages/wayland/.stamp`（riscv64）。最後の source（462c65c226）で 3 機種とも rc 0・warning 0（output-switch.c を compile し直したのを log で確かめた）。QEMU の image の build（`config-amd64-follow.mk` の userland と disk）も noct の外の warning 0。
- host 試験 `sh plan/ws113/tests/host-output-switch.sh`: 直す前は main（e948b78708）で compile しない（`kwl_output_use_external` の引数、`kwl_heads_sync`・`kwl_compose_output_open` など今は無い口。p003 までの API の変更に追従していなかった）。今の API と本物の hotplug の thread（threads.c と pthread、Vulkan の fence・待ち・submit は stand-in）で書き直し、plain と ASan/UBSan で PASS。調べる事: 最初の fence の知らせで列挙 1 回、事象の無い間は wake も列挙も無い、hotplug で次の fence を先に登録・thread の起床 1 回・limited を忘れる、出力の移動・拒否・内蔵の判定（前の試験の項目）、lost の出力の移動・待ち・開き直し、読めない一覧、window mode の外の hotplug は保留して window mode で列挙（M5）、display の change の間の lost は保留して終わりで追う（その間は自分で起きない）、head の作り直しの EBUSY は保留、BUG-266 の戻り、stop で thread が終わり fence が全部壊れる、最初の登録の失敗でも列挙 1 回、device lost で thread の待ちが返り join できる（M3）。登録と destroy の順を逆にした変異で 9 件 FAIL（試験が順を見ている事を確かめた。source は戻した）。
- QEMU（Venus、max_outputs=2、`-display dbus,gl=on`、runtime `build/ws353-p004-run`）: `sh plan/ws353/tests/qemu-hotplug-follow.sh build/ws353-p004-qemu`（image の build から）と `IMAGE=build/x86_64/hdd-image.img sh plan/ws353/tests/qemu-hotplug-follow.sh build/ws353-p004-qemu2`。compositor は serial から `--testing` で起動、log は guest の `/tmp/p4/kwl.log`、判定は serial の `grep -c` の数と status（console の log で判定しない）。2 回とも全 step PASS:
  - start: window mode、`KWL OUTPUT hotplug first`、`displays count=2`、`KWL THREADS` に hotplug（`names=io-file,io-device,present,display,hotplug`）。
  - idle-60（抜き差しの無い 60 s）: hotplug の知らせ 0→0、列挙 1→1（look の回数 0）。
  - unplug-1・plug-1: head 1 の head が閉じ、開き直る。unplug-0（表示中の display 0）: 出力が display 1 へ移る（`switch name=Venus virtual display 1`）。plug-0: 出力は display 1 に残り display 0 が head。host の変化から知らせの行が見えるまで（serial の確かめの粒度を含む上限）2 回目 245・1355・214・304 ms。
  - idle-30（変化の後の 30 s）: 知らせ 4→4、列挙 11→11（1 回目 12→12）。
  - stop: SIGTERM で `KWL OUTPUT hotplug stopped wakes=5 looks=11`（4 回の hotplug と stop、余計な起床なし）と `KWL EXIT`、20 s 以内に終わる。KWL FAILED 無し。
  - 1 回目の summary の「told within」の ms は script の変数の名前の重なりで壊れていた（判定には使っていない）。直して 2 回目を流した。
- 証拠: `build/ws353-p004-qemu/`・`build/ws353-p004-qemu2/`（summary.txt、kwl.log は serial で読んだ写しで記録用）。

### 未実施

- game mode（direct scanout）の間の抜き差しの QEMU での確かめ（fullscreen の scanout の client が要る）。host 試験の「window mode の外の hotplug は保留し window mode で列挙」で代えた。
- WS113 の出力の移動の QEMU の試験（phase.md の回帰）: plan/tools/guest/ にも plan/ws113/tests/ にも今は無い（`output-switch-p004a.sh` は残っていない）。host 試験と上の QEMU の試験（出力の移動・残る・head の追従）で代えた。
- device lost の後の終了の QEMU での確かめ（p003 の lost の試験が libvulkan の側を、host 試験が compositor の側を見た）。
- arm64・riscv64 は build だけ。実機（5330 など）は p016。
- `KWL PERF` の `timeouts` は見ていない（main の 10 ms が残るので区別できない、レビュー L12）。

### 見つけた事

- Venus では head 1 の抜き差しで表示中の display 0 も別の generation になり（`lost operation=present|acquire` → `reopen name=Venus virtual display 0`）、開き直しが走る。今までと同じ振る舞い（追従の速さが変わっただけ）。
- head 0 を抜いた時は、hotplug の知らせより先に frame の acquire の SURFACE_LOST で出力が移り始める事がある（lost は `kwl_output_follow_later` で同じ pass の take で追う）。

### p005 への申し送り

- 求めの wake（`kwl_output_follow_later`・`_wake`）は main が自分の wake pipe に書く。p005 で wake pipe の扱いを変えるならこの 3 つの呼び手（follow_later・follow_wake と hotplug の thread の note）も合わせる。
- `kwl_output_hotplug_take` は毎 pass に呼ばれるが、notes が無く `follow_pending` が 0 なら lock を 1 回取るだけで何もしない（周期の見回りではない）。p008 で poll を -1 にしても、保留の follow は wake で起きる形になっている。
- `kwl_displays_pending_tick` は `kwl_displays_tick`（毎 pass）に残した（3 s の期限、p006 で timer へ）。

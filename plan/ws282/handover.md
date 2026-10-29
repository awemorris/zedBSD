<!-- awesome-plan project=zedbsd record=ws282-handover -->

# WS282 の引き継ぎ（camera の枠組みと uvc、試験から続ける）

- 書いた日: 2026-10-29 23:23、P6（q1667-i05）。宛先: 別の tree の session（試験から続ける人）。
- 2026-10-29 ユーザー（Q1 経由）「別なツリーのセッションで再開したときに試験から継続できるように、P6からハンドオーバーしてください。」
- **source の状態**: p002〜p011 の実装は main に入っている。Q1 が `212cbdd4a` を merge して `7be3822e6` になった。
  WS282 の source は次の物。
  - `include/uapi/camera.h`、`include/drivers/camera/cam.h`、`src/drivers/camera/cam.c`
  - `include/drivers/usb/usb-uvc*.h`、`src/drivers/usb/usb-uvc*.c`
  - `userland/tests/camcap`、`camcheck`、`camcommon`、`camvk`
  - `docs/reference/camera.md`
  - 他の WS の file の WS282 の hunk: xHCI の high-bandwidth、GPU の allocation の backing、system event の CAMERA、sessiond の seat
- **どの Phase も「実装済み・試験未実施」**。host の試験と build（warning 0）しかしていない。実機・QEMU の試験は 1 回も無い（QEMU に UVC の model は無い）。cleared の Phase は無い。
- 詳細は各 `phaseNNN/phase.md` の「実装した事」と「未実施」、design は [design.md](design.md)（§7.3 が実機の試験の表）。

## 0. 共通の準備

### host の試験（どの tree でもまず流す。全部 PASS の筈）

```
sh plan/ws282/tests/run-cam-host-test.sh        # 枠組み（import の case を含む）
sh plan/ws282/tests/run-uvc-host-test.sh        # descriptor・PROBE・alternate・payload・control
sh plan/ws282/tests/run-camcheck-host-test.sh   # camcheck の pattern の読み
sh plan/ws282/tests/run-xhci-hb-host-test.sh    # xHCI の HS の high-bandwidth の encoder と TBC/TLBPC
```

- 4 本とも通常と ASan/UBSan で流す。最後の行が `…: PASS` なら合格。
- 出力は `build/tmp/ws282-*` の新しい directory に置かれる。

### build（試験の前の確認。warning 0 が条件）

```
make -j16 ZEDBSD_CONFIG=config/ci/config-<c>.mk BUILD=build/ws282/<c> build/ws282/<c>/vmunix     # c = amd64 rpi4 rpi5 visionfive2 pcat
make -j16 ZEDBSD_CONFIG=plan/ws282/tests/config-rpi5.mk BUILD=build/ws282/rpi5 build/ws282/rpi5/bin/camcap build/ws282/rpi5/bin/camcheck build/ws282/rpi5/bin/camvk
```

### 試験の image（AGENTS §9: その WS の config での build と、tree の中の file の複写だけで作る）

- **5330（amd64、bare metal）**: 実機の xHCI が要るので、passthrough の QEMU ではなく bare metal で起動する。
  - 手順: 5330 の回帰の config に camcap・camcheck を足した config で image を作り、`plan/tools/hw5330/5330-run.sh --image IMG NAME` で 1 回起動する。この script は `/tmp/i915-hw.lock` を自分で取る。
  - `plan/ws282/tests/config.mk` は CI の amd64 に camcap・camcheck を足しただけで、keiland-shot が無い。5330-run.sh の screenshot まで要るなら、`include plan/tools/hw5330/config-5330.mk` + `ZEDBSD_USER_PROGRAMS += camcap camcheck` の config を `plan/ws282/tests/` に足す（未作成）。
  - build: `make ZEDBSD_CONFIG=<その config> BUILD=build/ws282/img-5330 disk-image`
  - guest の操作は SSH（`plan/tools/guest/guest.sh`、5330-run.sh の SSH の待ちを流用）で `camcap` などを root で流す。
- **Pi 5・Pi 4・VF2**: `plan/ws282/tests/config-rpi5.mk`・`config-rpi4.mk`・`config-visionfive2.mk` で `disk-image` を作る。
  - rpi5 の config には camvk も入る。VF2 は `plan/tools/vf2/run.sh`。
  - 機械と IP は `plan/tools.md` の「実機」の節にある。IP は DHCP で変わるので、使う前に `uname -a` で機種を確かめる。
- 撮れた絵・PGM・PNG は `build/review/ws282/` に置く（Q1 がユーザーに見せる）。
- 実機の割り当ては Q1 が行う（一度に一人）。電源の入れ直しも原則 Q1。

### 前提（揃っていなければその Phase を止めて Q1 へ）

| 物 | 要る Phase | 状態（2026-10-29） |
| --- | --- | --- |
| HDMI capture（手元、UVC 1.0/1.1、MJPEG と YUYV） | p003〜p007 | ある（ユーザー） |
| webcam（D12: USB 2.0、UVC 1.1、YUYV と MJPEG、P ≤ 1024 の小さな YUYV の mode） | p004b（YUYV）、p010（control） | 未購入か未確認（ユーザーに聞く） |
| pattern を全画面で出す PC（D11、ユーザーの PC の browser） | p006 | ユーザーが居る時間だけ。試験の一覧に置く |
| WS197 p007a2 の L2（BT SCO の回帰の基準）が PASS して main にある | p005 の受け入れ | **未達**（WS197 は保留） |
| Pi 5 の GPU（WS279 の V3D）と Vulkan の export（`GPU_CAP_ALLOCATION_SHARE`） | p008 | 未確認。CI の config は `CONFIG_DRIVER_BCM2712_GPU` が n なので、rpi5 の試験の config で y にする要がある見込み（未確認） |

### 決まっていない事

- **login 画面の account に camera を渡さない**（p009）。推しの暫定で、**ユーザーの確認待ち**。`seat.c` は鍵・smart card と同じ扱いにしてある。
- D11（pattern は PC の browser）と D12（買う webcam）はユーザーに提示中（design §6.2）。

## 1. Phase ごと

各 Phase は「状態 → 試験（機械、コマンド、合格の基準）→ 前提 → 再開点」の順に書く。

### p002 枠組み（cam.c）

- 状態: 実装済み。host の試験は PASS。実機は無い。
- 試験: 実機の受け入れは p003 以降の試験に含まれる（`/dev/cam0` の open・所有・buffer・poll）。
- 再開点: 無し（p003 の試験で一緒に見る）。

### p003 bind・descriptor・PROBE/COMMIT・登録

- 状態: 実装済み・試験未実施。
- 試験: 5330 + HDMI capture（+ webcam）で次を確かめる。
  - `dmesg | grep usb-uvc:` の dump（版・format・frame・interval・alternate の表、`host accepts up to`、p010 で足した processing unit・camera terminal の行）
  - `ls -l /dev/cam0` が `crw------- root`
  - `camcap -l`（format・mode・interval の一覧）
  - `camcap -f yuyv -w 1280 -h 720` が `ENOSPC`（exit 2）
- 合格の基準:
  - 表が device の descriptor と合う。
  - descriptor の byte 列（`lsusb -v` 相当か dmesg の hex）を `plan/ws282/tests/descriptors/` に置き、`uvc-host-test.c` に実機の case を足す（design §7.1。MJPEG の format を必ず含む）。
- 再開点: dump を取り、descriptor の case を host の試験に足す。

### p004a xHCI の HS isoch IN の実地

- 状態: 実装済み・試験未実施。**xHCI の HS isoch IN は一度も実地で動いていない**。
- 試験: 5330 + capture か webcam の P ≤ 1024 の mode で次を流す。
  - `camcap -f mjpeg -w <低い mode> --probe-only 1200`
  - 続けて counters と `sysctl` の `hw.usb` の isoch の数
- 合格の基準: 300 本の URB で MISSED/ERROR < 1%、FID が交互、retained・halt が 0。
- 失敗した時: xHCI の不具合は BUG の ticket にして WS197 へ（Q1）。
- 再開点: 最初の実機の試験はこれ。

### p004b streaming・frame の組み立て・camcap

- 状態: 実装済み・試験未実施。bulk の経路は未実地（bulk の device が無い）。
- 試験: 5330 + capture（MJPEG の低い mode）か webcam（YUYV 320x240/640x480）で次を流す。
  - `camcap -f mjpeg -w … -h … -o f.jpg`（`.jpg` は DHT を補う）、`camcap -f yuyv -w 320 -h 240 -o f.ppm`
  - `camcap -n 300 -o /dev/null`
- 合格の基準:
  - 絵が PC の画面（ユーザーの目視、PNG を `build/review/ws282/` に）。
  - 300 frame で sequence の飛び 0・ERROR 0。
  - 720p YUYV は `ENOSPC`。
- 再開点: p004a の後。

### p005 xHCI の HS の high-bandwidth（3072 byte/interval）

- 状態: 実装済み・試験未実施。
- 直した所と理由:
  - `include/drivers/pci/pci-xhci-control.h` の encoder（HS の mult を Max Burst に、Max ESIT・Average TRB を packet × (mult + 1) に）。
  - `pci-xhci-isoch.h` の TBC/TLBPC。
  - `pci-xhci.c` の HCIVERSION の保存と enqueue。
  - uvc の alternate の選択。
  - 理由: HDMI capture の YUYV と 1080p は 1024 byte/interval を超える（design §4.7）。
- **他の WS への影響**: `plan/ws197/tests/xhci-isoch-test.c` の「high bandwidth refused」の 1 件はこの変更で FAIL になる（契約を変えたので期待どおり）。直し方の案は phase005.md にある。WS197 の file なので直していない（Q1 に伝えた）。
- 試験: 5330 + capture で次を流す。
  - `camcap -f yuyv -w 1280 -h 720 -n 300`、`camcap -f mjpeg -w 1920 -h 1080 -n 300`
  - 前後で `bt-sco-loopback`（WS197 の L2 の道具）と USB の keyboard・mouse（HID）
- 合格の基準: 3072 の alternate が選ばれ（dmesg）、720p YUYV ≥ 10 fps、1080p MJPEG 30 fps。L2 の結果が前後で同じ、HID が動く。
- 前提: **WS197 p007a2 の L2 の PASS が main にある事**（未達）。L2 の基準が無い間は、camera の部分だけ流して「回帰の比較は未実施」と書く。
- 再開点: p004b の後。

### p006 画素の試験（camcheck、pattern.html）

- 状態: 実装済み・試験未実施。pattern.html を browser で出した事も無い。
- 試験: 5330 + capture。PC が `plan/ws282/tests/pattern/pattern.html?mode=yuyv`（と `?mode=mjpeg`）を全画面で出す。
  - `camcheck -f yuyv -n 100`、`camcheck -f mjpeg -n 100`
  - `camcheck --cycles 100`（stop/start の leak）
  - `camcheck --second-open`
  - option の全部は `camcheck` の usage。最後の行が `camcheck: PASS` か `FAIL (<理由>)`。
- 合格の基準:
  - 色が許容内（YUYV 8、MJPEG 16）。
  - counter が単調非減少。
  - drop（sequence の飛び）0。
  - io_stats の alloc − free が前後で同じ。
  - 2 つ目の open が EBUSY。
- stream 中の抜去と再挿入はユーザーの試験の一覧へ。
- 再開点: p004b（高い mode は p005）の後、ユーザーが居る時間に。

### p007 Pi 5・Pi 4・VF2

- 状態: Linux の quirk の表（phase007.md）と 3 枚の試験の config だけ。IRQ の中の複写の時間の計り方は案だけ（`io-stats.h` の版の上げが要るので Q1 の判断）。
- 試験: 各板の config の image で、capture か webcam を挿して次を流す。
  - `camcap -n 1 -o f.ppm`
  - `camcheck -n 30`（PC の pattern が要る。無ければ camcap の 300 frame で飛び・ERROR だけ）
- 合格の基準: 3 枚で同じ結果。non-coherent の板（Pi 4・VF2）で画素が壊れない。
- 再開点: p006 の後。

### p008 GPU の memory に取り込む（CAM_MEMORY_IMPORT、camvk）

- 状態: 実装済み・試験未実施。
- 直した所と理由:
  - `cam.c` の import の経路（uncached の view、`EXDEV` の条件）。
  - `gpu.c` の `drv_gpu_shared_backing`。
  - bcm2711 の `share.c`・`render-device.c` の allocation の backing。
  - `gpu-share.h` の ops の新しい field。このため、位置で初期化している amdrdna・ghci・i915・nvrtx・venus に `NULL` を 1 つずつ足した。
  - 理由: design §3.6・D9。
- 試験: Pi 5 + capture か webcam、`config-rpi5.mk` の image（GPU を有効にした config が要る見込み）で次を流す。
  - `camvk -w 640 -h 480 -n 100 -o /root/camvk`
  - 同じ mode の `camcap -f yuyv -o /root/cap.ppm`
- 合格の基準:
  - 最後の行 `camvk: PASS`（全 frame で GPU の gray と CPU の Y が一致、sequence の飛び 0）。
  - PGM が camcap の絵と同じ。
  - fps と uvc の counters（uncached の書きの速さ）を記録する。
- 失敗の読み方:
  - exit 4 は import ができない（`CAM_CAP_IMPORT_GPU` 無し）。
  - QUEUE の `EXDEV` は、Vulkan の memory が連続・uncached でない。
  - frame の比較の不一致は、GPU が古い frame を読んでいる。
- 再開点: p007 の Pi 5 の後、WS279 の GPU が要る。

### p009 文書・system event・seat

- 状態: 実装済み・試験未実施。
  - `docs/reference/camera.md`
  - `system.h` の `KERN_SYSTEM_EVENT_CAMERA`（0x100、mask 0x1ff）
  - `cam.c` の ADD/REMOVE の post
  - `seat.c` の `/dev/cam0`〜`cam3`（session の user に 0600、login 画面と restore は root）
  - `systemevents` の `camera`
- 試験: 5330 の graphical login の image で次を確かめる。
  - login の前後で `ls -l /dev/cam0` の owner（session 中は user、logout で root）
  - 挿した時に `systemevents camera` が ADD/REMOVE を出す
- 前提・判断: login 画面の account に渡さない件はユーザーの確認待ち。
- 道具の登録: `plan/tools.md` への登録の文案は phase009.md にある（Q1 が merge の時に入れる）。
- 再開点: p006 の後。

### p010 UVC の control

- 状態: 実装済み・試験未実施。
  - selector・bit・offset は UVC 1.5 の仕様の PDF と照合済み（phase010.md）。
  - power line は 1.5 の device で 0〜3。
- 試験: 5330 + webcam（capture は control を持たない見込み）で次を流す。
  - `camcap --controls`
  - `camcap -f yuyv -w 320 -h 240 --control 32=<min と max> -o a.ppm` で明るさが変わる
  - `--control 49=0` の後に `--control 48=<値>`（manual の露光）
  - `49=1` の間の 48 は `EACCES`
- 合格の基準: 範囲・既定が device の dump と合い、値が効く。
- 再開点: webcam が要る。

### p011 規約の全文の見直し・license の監査

- 状態:
  - 規約の見直しは済み（`212cbdd4a`、書き方だけ）。
  - **license の監査は未実施**（design §8 の出典の block、Linux の uvc・xHCI を手順の参照に留めている事の確認）。
  - 実機の試験で code を直したら、その差分の見直しをもう一度。
- 再開点: 試験が一通り済んでから。

### p012 SuperSpeed の isoch

- planning のまま。USB 3 の device を買ってから設計する。

## 2. 再開点（まとめ）

1. 新しい tree で main（`7be3822e6` 以降）から host の試験 4 本と build を流す（§0）。
2. Q1 に 5330 と HDMI capture の割り当てを頼む。5330 の camera の image の config（config-5330.mk + camcap camcheck）を `plan/ws282/tests/` に作り、p003 → p004a → p004b の順に流す。xHCI の不具合が出たら BUG の ticket にして WS197 へ。
3. ユーザーに webcam（D12）と pattern の PC（D11）の都合、login 画面の件を聞く（Q1 経由）。
4. p006 → p005（WS197 の L2 の後）→ p007 → p008（Pi 5 の GPU）→ p009 → p010 → p011 の license の監査。

## 5330 の webcam の最初の結果（2026-10-30、WS315 の P3 が試験の途中で取った）

- 機械: 5330（Dell Latitude 5330）の内蔵の webcam「Integrated_Webcam_HD」（0c45:6d1d、UVC 1.00、isoch の endpoint 0x81、alternate 1:128x1 … 6:1024x3、host は 3072 まで）。uvc の attach は正常で cam0 ができた。
- kernel: WS315 の tree（main の WS282 を含む）＋ RTL815x の TX queue の変更、userland は WS315 の img1 の image（camcap 入り）。
- 結果: `sudo camcap -n 10 -t 5000 -o /tmp/ws315-cam.jpg` が rc=3、`CAM_DEQUEUE_BUFFER: Input/output error`。
  - info: capabilities=0x0000004a max_buffers=8 format_count=2 clock_hz=15000000 host_payload_max=3072
  - format: mjpeg 640x480 interval 333333、image 614989、payload 3072、buffers 4
  - counters: frames=0、resubmit_failures=1、他は全部 0（dropped・invalid_header・missed_packets・sequence_gaps・halt_timeouts・no_signal）
  - 2 回（kernel だけの差し替えと image）とも同じ。
- ユーザー（2026-10-30）「webcamは、私の認識だと、まだ実機テストしたことがなくて、今回初めて行ったように思います。つまり、UVCのドライバが動いた実績がなかったので、根本的に、動くようにするところからスタートだと思います。」→ WS282 の再開は、UVC の driver を実機で動かすところから（どの段が動いていないかの切り分けから）。
- 見立て（未解析）: isoch の最初の resubmit が失敗して stream が止まった。xHCI の isoch の送り直しの道（WS197 の isoch・WS221 p014 の xHCI の queue の変更との関係も）から調べる。
- 証拠の file（P3 の worktree `/home/awe/zedBSD-worktrees/p3-ws315` の中、git の外）: `build/hw5330/logs/ws315-k5-dmesg.txt`・`ws315-img1-dmesg.txt`（uvc の行は 249〜279 付近）、`build/ws315-hw/k5/extra.txt`・`img1/extra.txt`。

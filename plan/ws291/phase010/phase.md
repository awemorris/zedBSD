<!-- awesome-plan project=zedbsd record=ws291-p010 -->

# ws291-p010 NVIDIA（RTX 2070）: nvrtx の起動・desktop の計測と 60 fps、Linux の NVK の基準

- 親: [WS291](../ws.md)。
- 状態: in-progress（2026-10-29 06:20 P2、q1670-i01、worktree p2-ws291m、基点 main 42da8d889。12:55 q1670-i06: driver 側の直し（cursor・window_lock）の後、全段 60 fps 以上か入力の間隔で決まる値。受け入れの判定は WS309 の後に測り直して）。
- 承認: ユーザーの回答（クリック、2026-10-29）「WS291 の NVIDIA の段 (推し)」（問い: RX 5600 XT を外している間、P2 は何をするか）。
  機械の変更（原文）「再起動後、RTL8127と、RTX 2070が接続されています。RX 5600 XTはPCIスロットの高さの関係で、RTL8127と排他だからです。」
- Linux の基準の driver: ユーザーの回答（クリック、2026-10-29、Q1 経由）「NVK（nouveau） (推し)」→ Mesa の NVK、設定の file は変えず、計測の時だけ sysfs で
  vfio-pci から外して nouveau（GSP、`config=NvGspRm=1`）を読み込み、終わったら vfio-pci に戻す。
- BAR1 の window の直し: Q1 の判断（2026-10-29）「WS291 の NVIDIA の段の目標（desktop 60 fps・model 50 fps）を塞ぐので WS291 の中で直して」、
  形は「host-visible で要る物だけ map、device-local は map しない／要る時だけ」、直しの前後で window の使用量を数えて記録。vk-node.c の変更は WS250 の
  design.md の前提と食い違わない形に（食い違うなら Q1 へ）。

## 範囲

1. plan/ws291/tests/config.mk の `CONFIG_DRIVER_PCI_NVRTX := n` を戻す（nvrtx と GSP の firmware は CI の config から）。
2. RTX 2070 で今の nvrtx が起動・表示するかを確かめる（NVMe の zedBSD に kernel、BootNext）。
3. desktop の frame-ops の段（RDNA の p001 と同じ表）を 60 fps に。妨げる driver の不具合を直す。
4. Linux（Debian、NVK）で約 50 fps になる scene を決める（model の段は p011）。

## 受け入れ

- frame-ops の各段が 60 fps（p001 と同じ形の表）。amd64 の vmunix・変えた userland が warning 0。実機の証拠と未実施を分けて記録。

## 記録

### 起動（2026-10-29 06:20〜06:30、実機 RTX 2070、kernel = 1746f7c18 の build、cksum 2886737663）

- 1746f7c18: config.mk の NVRTX を戻した。vmunix は link でき（3412200 byte、AMD64_KERNEL_MAX_BYTES の内、P3 の packet pool の直しが main にある）warning 0。
- `rdna-nvme.sh kernel` で NVMe の p4 の vmunix に（前の物は vmunix.old）、`measure.sh zedbsd` で BootNext 0004。
  dmesg（SSH）: identify TU106 → layout → FRTS → booter → GSP-RM up（963 ms）→ static-info → vram-window（BAR1 256 MiB）→ copy-channel → golden-context →
  work-channel → display（HDMI の SOR 2、1920x1080）→ takeover → gpu-device 登録 → lease 1（compositor）。/dev/gpu0 があり wayland・sessiond が走る。
- 道具: plan/tools/gpu-perf/drivers/nvrtx.conf（status は無し、timing は `nvrtx.vk.perf=1`）と machines/rtx.conf（rdna.conf と同じ機械、driver nvrtx）。
  `GPU_PERF_MACHINE=rtx` で measure.sh を使う。担当の worktree の plan/tmp/guest/id_ed25519 は mode 600 が要る（664 だと measure.sh が zedBSD を見つけない）。

### desktop（build/ws291/rtx/desk1、frame-ops）

| 段 | frames | fps | 備考 |
| --- | --- | --- | --- |
| launch | 46 | 59 | terminal は開かない（下） |
| type | 3 | 77 | 空振り |
| keys | 11 | 71 | 空振り |
| output | 4 | 35 | 空振り |
| scroll | 4 | 20 | 空振り |
| hover | 2 | 48 | |
| home | 68 | 62 | |
| wiseview | 53 | 60 | work max 148 ms が 1 回 |

- compositor の frame: KWL PERF の submit+present 9.3 ms、draw 2.2 ms（RDNA より重い、次の調べ）。
- **不具合**: compositor 以外の client が全部 VK_ERROR_OUT_OF_DEVICE_MEMORY（errno ENOSPC = 8）。terminal は `ZTERM FAILED operation=vkAllocateMemory
  result=-2 errno=8`、mview は `vkCreateSwapchainKHR result=-2 errno=8`、ZFILES の swapchain も、keiland-shot は `ERROR buffer`。
  見立て（code を読んだ、未確認）: nvrtx の executor の GPU memory（vk-node.c `nvrtx_vk_gpu_alloc`）と blob は VRAM を必ず BAR1 の CPU の window
  （256 MiB、console 8.6 MiB を含む）に map する（`nvrtx_bo_map`、満杯で ENOSPC）。libvulkan は kernel の ENOSPC を OUT_OF_DEVICE_MEMORY にする
  （context.c `vulkan_kernel_error`）。compositor の画・import（1920x1036 の 7.6 MiB 等）で window が埋まる。

### Linux の NVK の基準（2026-10-29 06:30〜06:38、Debian 6.12.111、Mesa 25.0.7、nouveau NvGspRm=1、build/ws291/rtx/nvk1・nvk2・nvk3）

- 手順: `echo 0000:01:00.0 > /sys/bus/pci/drivers/vfio-pci/unbind`、driver_override に nouveau、`modprobe nouveau modeset=1 config=NvGspRm=1`
  （vulkaninfo: NVIDIA GeForce RTX 2070 (NVK TU106)）→ `GPU_PERF_MACHINE=rtx LINUX_MVIEW=… measure.sh bench linux` → 戻し: weston を止め、nouveau から
  unbind、vfio-pci に bind。`modprobe -r nouveau` は softdep で vfio_pci も外すので、その後 `modprobe vfio-pci`（4 function とも vfio-pci に戻ったのを確かめた）。
  次から modprobe -r はしない。

| run | fps | frame ms | record | submit | fence |
| --- | --- | --- | --- | --- | --- |
| scene-1 | 9086 | 0.11 | 0.01 | 0.01 | 0.07 |
| scene-1500 | 165.6 | 6.04 | 0.06 | 0.04 | 5.80 |
| scene-4500 | 54.5 | 18.30 | 0.07 | 0.04 | 17.85 |
| **scene-5000** | **50.1**（単独の run 49.1） | 19.90 | 0.08 | 0.05 | 19.36 |
| scene-5500 | 45.4 | 21.99 | 0.10 | 0.06 | 18.40 |
| scene-6500 | 38.4・39.2 | 25.5〜26.0 | 0.10 | 0.06 | 21.4〜25.2 |
| big-1000 | 136.2 | 7.34 | 3.15 | 0.03 | 4.09 |
| subs-1000 | 75.4 | 13.25 | 6.13 | 4.63 | 2.45 |

- → NVIDIA の model の scene は qs40 ×5000 instanced（Linux NVK 50.1 fps）。
- 条件の差: weston は NVK では全 repaint が「Using render-only state composition」（`[state] no overlay plane support`、dmabuf の modifier は
  NVIDIA_BLOCK_LINEAR_2D）で、RDNA の RADV のような plane-only（direct scanout）にならない。Linux 側は合成の分（1080p の 1 枚）を含む。

### BAR1 の window の直し（06cdbf381、印 cf48046e5）

- 06cdbf381: blob を作る時に window に map しない（VRAM は PRAMIN で 0 に clear、`nvrtx_bo_clear`）。CPU が初めて触る時に map（`nvrtx_node_backing_cpu`:
  resource の read・write・map、transport の stream・reply の blob、cursor の画、executor の CPU の copy は `nvrtx_vk_memory_cpu`）、その後は VRAM を返すまで
  map のまま。`nvrtx_bo_map` は二重 map を防ぐため確かめを lock の下に。window の使用・peak を nvrtx.vk.perf=1 の `perf: window` の行に、満杯は最初の
  8 回を `window full` の印に。ABI・HAL・libvulkan は不変。Q1 の形の案（host-visible の type だけ map）でなく「要る時だけ map」にした理由: nvrtx の
  memory type は 1 つ（DEVICE_LOCAL・HOST_VISIBLE・COHERENT）で、libvulkan は host-visible の type の blob に必ず MAPPABLE を付けるので、flag では分けられない。
- cf48046e5: VRAM の allocator の満杯（`vram full`）と work channel の VA の満杯（`work space full`）の印（各 8 回まで）。
- 確かめ（host）: vmunix（ws291 の config）warning 0、plan/tools/nvrtx/nvrtx-vk-host-test.sh PASS 60 checks（`nvrtx_vk_memory_cpu` の stub を足した）。

### 実機の A/B（2026-10-29 07:04〜07:19、plan/ws291/tests/rtx-window-ab.sh、build/ws291/rtx/ab1、desk-l2）

- eager（全 blob を作る時に map、nvrtx-eager-map.patch、vmunix-eager1 cksum 997937608）と lazy（06cdbf381、vmunix-lazy1 827899503）を nvrtx.vk.perf=1 で。

| | eager | lazy |
| --- | --- | --- |
| window の peak（frame-ops と probe の後） | 245936 / 253504 KiB、`window full` 2 回（16384・8100 KiB の要求） | 106984 / 253504 KiB、満杯 0 |
| terminal | `vkAllocateMemory result=-2 errno=8` | 開く |
| frame-ops | terminal が無く type〜drag は空振り | 全段が動く |

- lazy の desktop（counts 無し、ab1/lazy-nocount-desk と desk-l2）: keys 143〜167、output 65〜69、type 60、home 61〜62、wiseview 61〜64、launch 48〜49
  （terminal の起動の 1 回 78〜88 ms）、**scroll 19〜20、hover 21〜23、drag 32〜35 fps**（RDNA の p001 は全段 60）。
- 次の壁 (1): desktop の後の mview の swapchain が `work space full: 8100 KiB asked, 18688 KiB free in 8 runs`（vmunix-lazy2 = cf48046e5 の build、
  cksum 204755418）。work channel の GPU の VA の users の部分は 0x28100000〜0x40000000（約 383 MiB、`NVRTX_WORK_VA_BYTES` = PD の 1 entry の 512 MiB）。
- 次の壁 (2): 起動の直後（client 無し）の mview は `api=depth format result=-11`（VK_ERROR_FORMAT_NOT_SUPPORTED）。nvrtx の executor に depth の形式が無い。
- keiland-shot は eager・lazy とも撮れた（6222418 byte、shot の ERROR buffer は最初の起動の時だけ、未解析）。
- 手順の注意: `pkill -f rtx-window-ab.sh` は同じ語を含む自分の shell も止める（tools.md の注意と同じ、exit 144）。

### work channel の VA の拡張（01478d325、実機は未実施）

- NVRTX_WORK_VA_BYTES 512 MiB → 3.5 GiB（0x20000000〜0x100000000、first-level の entry 1〜7、resource manager の 4 GiB の range の手前）。
  mmu.c: 512 MiB ごとの double-entry directory（最初の物は今の 0x3000、2 つ目からは page table の後ろ）にし、page table は 1 本の列のまま
  （`nvrtx_mmu_gr_pte_offset` は不変、1 entry の range の image は前と同じ byte）。work.c の arena: tables 0〜0x70a000、instance 0x800000、
  USERD 0x801000、own buffers 0xa00000（arena 32 MiB は不変）。
- 確かめ: vmunix warning 0（build/ws291/vmunix-va1 cksum 1463007375）、plan/tools/nvrtx/nvrtx-sm75-host-test.sh 287 checks 0 failures、
  page table の walk の host の確かめ（scratchpad、7 つの VA で dir0・page table・PTE の位置が一致、PASS）。実機は次の枠。

### 再開点（2026-10-29 07:20 P2）

- 実機: Debian、RTX は vfio-pci、lock は返した。NVMe の zedBSD の kernel は vmunix-lazy2（cf48046e5 の build、cksum 204755418、前の物は vmunix.old）、
  cfg の nvrtx.vk.perf の行は外した。
- 次（Q1 に聞いた）: (1) work channel の VA を複数の 512 MiB の directory に広げる、(2) executor に depth・index の draw・mip（mview が動くまで、新しい
  Phase の案 p010a）、desktop の scroll・hover・drag の 20〜35 fps の調べ。案 A・B（RDNA）は host の harness の runner が tree に無く後回し。

### checkpoint（2026-10-29 08:12 P2、q1670-i01 の返却、Q1 の指示「checkpoint を返して終えて」）

- tip ddf4ff288（この節の commit がその後）、未 commit 無し。
- 実機: 08:10 からユーザーが RTL8127 の移設のため電源を切っている。最後の P2 の状態は Debian（RTX は vfio-pci）、NVMe の zedBSD の kernel は
  build/ws291/vmunix-lazy2（cksum 204755418）、cfg に nvrtx.vk.perf の行は無し。P1 の枠の script が kernel を一時的に替えて戻す予定だった（Q1）。
- 再開: (1) `sh plan/ws291/tests/rtx-va-check.sh build/ws291/rtx/va1 build/ws291/vmunix-va1`（01478d325 の build、cksum 1463007375。build/ は
  worktree p2-ws291m の物。新しい worktree なら `make ZEDBSD_CONFIG=plan/ws291/tests/config.mk BUILD=build/ws291/k build/ws291/k/vmunix` で作り直す）:
  desktop の後の mview が `work space full` を出さず `api=depth format` まで進めば VA の拡張は効いている。(2) [p010a](../phase010a/phase.md) の実装。
  (3) desktop の scroll・hover・drag の 20〜35 fps の調べ。案 A・B（RDNA）は host の harness の runner（plan/ws014/tests の gpu-job.c の build の手順）が
  tree に無く後回し（kernel と host の header の型の衝突で今の tree では compile できない: tid_t・sigset_t）。

### work channel の VA の拡張の実機（2026-10-29 08:49〜08:56 P2、q1670-i02、build/ws291/rtx/va1、worktree p2-ws291n）

- `sh plan/ws291/tests/rtx-va-check.sh build/ws291/rtx/va1 build/ws291/vmunix-va1`（01478d325 の build、cksum 1463007375）: desktop の後の mview 3 回とも
  swapchain を越えて `api=depth format result=-11`（[p010a](../phase010a/phase.md) で足す物）まで進み、kernel の行に `work space full`・`window full` は無い → VA の拡張は効いている。
- desktop（counts 無し）: keys 167、output 67、type 61、home 63、wiseview 62、launch 49、scroll 20、hover 23、drag 36 fps（前の lazy と同じ）。
- 終わり: Debian、RTX は vfio-pci、NVMe の zedBSD の kernel は vmunix-va1（前の物は vmunix.old）。

## q1670-i06（2026-10-29 12:19〜12:55 P2、worktree p2-ws291r、基点 main 2dcea9df9）

- 範囲（Q1）: p010 の残り、RTX の desktop の操作の fps を 60 に。まず p011 の後の値と遅い段の 1 frame の内訳。
- ユーザー（原文、Q1 経由）「WS291は、Keilandのスレッドの修正を適用後でないと、正確にならないかもしれないです。」→ Q1: desktop の fps の受け入れは
  WS309（Keiland の thread の整理）の後に測り直して判定する。今は driver の側の直しまで、compositor の側の調整はしない。
- 道具: `plan/ws291/tests/rtx-desk-profile.sh OUT RESTORE VMUNIX [perf]`（kernel を入れ、起動直後の session で frame-ops、表と launch・hover・drag の
  frame ごとの値（frames.txt）、perf を付けると nvrtx.vk.perf=1 でもう 1 回と kernel の perf の行、最後に RESTORE に戻して Debian）。

### 段の読み方（p001 と同じ）

- scroll（wheel 50 ms 間隔）・hover（move 15 ms 間隔と aat-input の起動）・keys（200 ms 間隔）の fps は試験の入力の間隔で決まる（RDNA の p001 も
  scroll 20・hover 23）。この 3 段は fps でなく work（draw→submit）と lag で見る。60 fps に届かない本物は drag と launch だった。

### 計測 1（dp1、今の main の build、cksum 3566687130、build/ws291/rtx/dp1）

| 段 | fps | work p50/max | next p50/max | 備考 |
| --- | --- | --- | --- | --- |
| launch | 51・54 | 1/2・0/100 | 16/78・16/80 | terminal の起動の 3 frame で next 76〜78 ms |
| hover | 22 | 2/18 | 32/79 | 入力の間隔 |
| drag | 31・38 | 1/19 | 32/40 | period 33（2 vblank） |
| home・wiseview・type・output | 59〜69 | 0〜1（wiseview max 87〜90） | 16〜17 | |

- p011 の非同期の submit で work は全段 0〜2 ms（va1 の 6〜10 ms から）。drag の 1 frame: work 1、shown 1、next 32 ms。
- 原因（session の log と code）: 毎 frame の acquire の直前に cursor の plane の present（Keiland の cursor-thread、queue_lock を持ったまま
  vkQueuePresentKHR）が約 30 ms（KWL PERF main: cursor presents=189、present 合計 2665 ms、max 46 ms）。libvulkan は cursor の present ごとに
  GPU_DISPLAY_CURSOR_SET と MOVE を呼ぶ。nvrtx の cursor_set は毎回 64x64 を blob（VRAM）から UC の BAR1 越しに byte 単位で読み（16384 回）、
  node->lock の下で slot に書いて core の update を送り、前の core の update が vblank まで残る間は EBUSY → libvulkan が 1 refresh 眠って再試行。

### 直し 1: cursor（c72afc267）

- display/cursor.c: cursor_set は image を CPU の写し（node->cursor_pixels、slot ごと）に読み、今の slot の写しと同じなら hardware に触らず 0
  （EBUSY にしない）。違う image だけ前と同じく core の settle を見て slot に書き core の update。読みは offset・stride が 4 の倍数なら word 単位
  （4096 回）。node.c: perf の行 `perf: cursor sets N same N avg Nus, moves N avg Nus`（node->lock を持つ時間）。HAL・ABI・libvulkan は不変。
- 確かめ（host）: vmunix warning 0、amd64 vmunix check PASS、`plan/tools/nvrtx/nvrtx-vk-host-test.sh` PASS 86/0。

| 段（dp2、cksum 3859311083） | fps（perf 無し・有り） | work max | next p50/max |
| --- | --- | --- | --- |
| drag | **60・61**（182 frames） | 5・6 | 16/22・16/21 |
| hover | 23・23 | 5（前 18） | 28/75 |
| launch | 59・50 | 100・78 | 16/78（terminal の起動の frame が残る） |

- cursor の perf: 動く間 `sets 197 same 198 avg 4959us`。同じ image の set が 1 回約 5 ms node->lock を持つ（16 KiB を UC で読む、1 word 約 1.1 µs）。
  drag は 60 になったが、node->lock を 5 ms 握るのは残りの候補（読みを node->lock の外へ、か比べる量を減らす）。

### 直し 2: launch の window_lock（5daead373）

- launch の遅い frame: terminal の起動で next 76〜78 ms が 3 回、submit が 57〜76 ms の frame も。見立て（code）: blob を作る時の VRAM の clear
  （nvrtx_bo_clear → nvrtx_vram_fill、PRAMIN の register window、約 4 MB の swapchain の image ごと）が 64 KiB ごとに window_lock を取り直すが、
  kernel の mutex の release は待ち手に手渡さない（mutex_unlock は FREE にして 1 人を起こすだけ）ので、clear の thread がすぐ取り直し、window を使う
  他の道（display の flip の notifier の読み・reset、work の submit の USERD の PUT の書き込み、TLB の invalidate）が clear の終わりまで待つ。
- 形（Q1 の指示「固定の sleep より、塊ごとに外して待ち手が取れる形か、window を使わない道」）: channel.c で fill は 16 KiB ごとに window_lock を外し、
  その間「他の利用者が待っている間だけ」CPU を譲る（channel_window_take: fill 以外の利用者は gpu->window_wanted を atomic に数えてから lock、
  channel_fill_aside: window_wanted が 0 になるまで sched_yield、飢えの防ぎに 2 ms の上限）。待ち手がいなければ休まない。
  window を使わない道を採らなかった理由: GPU の copy engine の fill は blob の VRAM を copy の channel の VA に map する仕組みが無い（copy の channel
  は自分の buffer だけを map）。BAR1 の WC の map は kernel の PCI の BAR の map に WC の flag が無い（DRV_PCI_MAP_PREFETCHABLE は定義だけで未使用）、
  BAR1 の window も消費する。どちらも範囲が広がるので後回し（terminal の起動そのものの速さ、clear の 76 ms は残る）。
- 確かめ（host）: vmunix warning 0、vmunix check PASS、host test PASS 86/0（cksum 1245428584）。

| 段（dp3、cksum 1245428584、perf 無し） | fps | work p50/max | period p50/max | next p50/max |
| --- | --- | --- | --- | --- |
| launch | **64**（60 frames、terminal は開いた） | 1/2 | 17/19 | 16/18 |
| type | 61 | 0/1 | 17/17 | 17/17 |
| keys | 36（200 ms 間隔） | 0/1 | — | — |
| output | 65 | 0/1 | 15/17 | 14/17 |
| scroll | 19（50 ms 間隔） | 1/1 | 52/53 | 51/53 |
| hover | 23（15 ms 間隔） | 4/5 | 32/80 | 27/75 |
| drag | **61**（182 frames） | 1/5 | 17/22 | 16/21 |
| home | 63 | 0/1 | 17/24 | 16/23 |
| wiseview | 62 | 1/90 | 17/51 | 16/24 |

- 証拠: worktree p2-ws291r の build/ws291/rtx/dp1・dp2・dp3（summary.txt、desk/frames.txt、dp1・dp2 は perf/kernel.txt）。実機の結果だけ、QEMU は無し。
- 終わり（12:53）: Debian（6.12.111）、RTX は vfio-pci（lspci）、NVMe の zedBSD の kernel は vmunix-va1（cksum 1463007375）、cfg の nvrtx.vk.perf の行 0。
  lock は 12:53 に返した。

### 残り・再開点（12:55 P2）

- 受け入れの判定は WS309 の後に同じ script で測り直す（`sh plan/ws291/tests/rtx-desk-profile.sh OUT RESTORE VMUNIX`）。
- wiseview の work max 87〜144 ms（compositor の cpu＝draw→recorded の frame、area が画面より大きい capture の frame）。compositor の側なので今は触らない。
- cursor の同じ image の set の 5 ms（上）。terminal の起動の blob の clear 自体（4 MB で約 76 ms、起動する client だけが待つ）。
- 1 回ずつの計測（dp3 は 1 回）。揺れの確かめは WS309 の後の測り直しで。


## q1700-i01（2026-10-30 09:10〜09:13 P5、worktree p5-ws291n、基点 main ef35deaf9。利用の上限で Q1 がラップアップ、計測は未実施）

- 承認: ユーザー（2026-10-30 原文）「WS291はNVIDIAですか？それなら今実行してOKです。」。範囲（Q1）: (1) この p010 の desktop の 60 fps を測り直す
  （WS309 の後の約束。WS309 は p015 以外 cleared）、(2) p012 以降の予備で scene-5000 を 50 fps を明らかに超えるところへ。
- 済み: worktree の準備だけ（plan/tmp/guest/id_ed25519 を mode 600、build/ に共有の toolchain への symlink、build/ws291/ に linux-mview・measure/ref・
  usb/bin/vkhang・vmunix-va1（cksum 1463007375）を p2-ws291l・p5-ws291 から写した）。main の kernel を build: `make ZEDBSD_CONFIG=plan/ws291/tests/config.mk
  BUILD=build/ws291/k build/ws291/k/vmunix` warning 0、build/ws291/vmunix-m0（cksum 1774294323）。
- 未実施: 実機の計測は 1 つもしていない（deploy もしていない）。機械は Debian、RTX は vfio-pci（09:13 に確かめた）、NVMe の kernel は触っていない。lock は 09:13 に返した。
- 次の 1 手（再開の手順）:
  1. NVMe の zedBSD の userland は WS309 の Keiland より古い見込み（未確かめ）。測り直しの前に main の `/bin/wayland`・`/lib/libkeiland.so`・
     `/lib/libvulkan.so`（WS309 p014 で wayland は新しい libc の `posix_spawn_file_actions_addclosefrom_np` を要る → libc.so も）を build し、
     plan/ws309/tests/rtx-bug353.sh と同じ形（元を *.old に残し、終わりに戻す）で入れる。入れる前に zedBSD の今の file の cksum を取って比べる。
  2. `sh plan/ws291/tests/rtx-desk-profile.sh build/ws291/rtx/q1700-desk build/ws291/vmunix-va1 build/ws291/vmunix-m0`（2 回、揺れを見る）。表は上の dp3 と同じ形で記録。
  3. p012（新設、phase.md は未作成）: scene-5000 の GPU 19.5 ms の内訳を mview の `--cull=all`（頂点だけ）と `--size=`（画素の量）で zedBSD と NVK の両方で取り、
     頂点・画素のどちらが NVK より重いかを見てから直す（vertex の program の質・属性の fetch・ZCULL など）。

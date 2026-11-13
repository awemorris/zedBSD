<!-- awesome-plan project=zedbsd record=ws347-p009 -->

# ws347-p009: host が local memory に GOP の VRAM 以外を書かない形に

Status: in-progress（2026-11-13 Q1、q2043-i01 P4）
Parent: [WS347](../ws.md)

## 目的

2026-11-13 ユーザーの訂正: host が窓（BAR の local memory の窓）から GPU の local memory に書いてよいのは GOP の VRAM（firmware が残した boot の framebuffer）だけ。ring・vertex・fence など host と GPU が分け合う物は全部 host memory に置き、GPU が DMA で読み書きする（台帳 B35）。

## 範囲

1. `src/drivers/gpu/ghci/display.c` の `copy_by_processor`（blitter も copy の program も使えない時に CPU が local の窓を通して frame に書く予備の道）を削除する。GOP を残して使う事はまず無い（ユーザー）。予備の道が無くなった時の振る舞い（executor の無い GPU・program の拒否の時に copy の present を断る等）を本来の形で決める。
2. driver の全体で、host が local の窓を通して local memory を読み書きする所を点検し、GOP の VRAM の引き継ぎ以外を無くす（無くせない物は根拠を書いて Q1 へ）。fence・ring・descriptor・PSO など host が読む・書く物が host memory にある事を確かめる。
3. host 試験（WS290 の道具）を追従させ全 PASS、vf2・x86_64 の kernel warning 0。

## 点検（2026-11-13 P4）

driver の全体（`src/drivers/gpu/ghci/` の core と `vk/`）で、processor が local memory に触れる所と、host と GPU が分け合う物の置き場所を読んだ。

| 物 | 置き場所・道 | 結果 |
| --- | --- | --- |
| ring、fence の page（flip の record も） | `cp.c` の `ghci_memory_alloc`（host） | host memory、GPU が DMA |
| CP_HEAP（page table） | `cp.c` の `alloc_heap`（host、HEAP_END の下） | host memory |
| Vulkan の heap（PSO・code・descriptor・event・query・chunk） | `vk-memory.c` の `heap_grow`（host） | host memory |
| driver の copy の program（PSO・code・resource table） | `vk-meta.c` の `rows_place`（host） | host memory |
| idle frame | `display-scan.c` の `idle_frame_take`（host） | host memory |
| client の resource・blob の read・write・map | `ghci.c` の `gpu_resource_read`・`write`・`map`、`vk-stream.c` | local は ENOTSUP（元から） |
| device-local の VkDeviceMemory の clear | `vk-memory.c` の `local_clear`（GPU の FILL_BUFFER か copy の program） | GPU だけ |
| copied present の frame | `display.c` の `copy_by_processor`（窓から CPU が写す） | **削除**（下の設計） |
| cursor の surface | `display-scan.c` の `ghci_scan_alloc`（discrete では local）＋ `ghci_scan_write`（窓から CPU が書く） | **host memory に移した** |
| cursor の image の読み | `display.c` の `cursor_rows`（local の storage を窓から読む） | **local は ENOTSUP** |
| driver の窓の access の道具 | `memory.c` の `ghci_memory_window_hold`・`unhold`・`local_read`・`local_write`・`local_span`、local の allocation の `view`（窓の中の位置） | **削除**（local の `view` は NULL） |
| BAR2 の kernel の view | `pci-attach.c` が最大 1 GiB を map し `device->window` に | **持たない**: claim と先頭の 1 page の map だけ（物理の位置を知るため。console の終わりに BAR の範囲を渡す `boot_end` が要る）。`device->window`・`window_physical`・`window_bytes` を外した |
| GOP の boot framebuffer | 窓の開閉（`window_users_change`、boot の範囲の予約と解放）、panic の時に host が boot の image を灯す（registers だけ） | 残す（許された唯一の使い方） |

無くせない物: 無い。残る窓の使い方は GOP の引き継ぎ（boot console が自分の mapping で書く間 WINDOW_CTRL を開けておく事）だけ。

### 根拠の確かめ（Q1 の依頼）

- **discrete の GPU の表示が host memory を scan できるか**: ghci.md（fe6431b1）§6.0 の address の分類は、`CMD_FLIP` の `base` と `FB_BASE`・`FB_NEXT` を「local VRAM（≥ LOCAL_BASE）か host DRAM の aperture（< HOST_LIMIT）」の 32-bit の物理 address とする。cursor も `CMD_FLIP` の `PLANE_CURSOR` で同じ `base` から `CURSOR_BASE` に入る（§7.3 の CMD_FLIP）。§11.3 の MEM の HOST_LIMIT は「LOCAL の範囲の外の物理 address は全て host の PCIe の DMA へ」と二値の routing を定める。RTL も `rtl/gpu/mc/atg_mc.v` の冒頭の説明のとおり、DC を含む MC の client の要求を LOCAL の範囲で local（AXI ID 0）と host（ID 1）に分ける。だから scan できない構成は仕様にも RTL にも無い（driver は前から discrete で host の idle frame と shared の blob を scan している）。flat（walker 無し）の discrete（KC705 の minimal）は driver が local memory を使わず全部 host。
  - 仕様の文の食い違い: §13.2 の「Physical Address Space」は「discrete MEM がある時は local VRAM の address」、§11.3 の比較の表は discrete の Framebuffer を「Local memory」と書き、host の scan-out に触れない。§6.0・§11.3 と RTL は host を許す。台帳に足すかは Q1 の判断（下の「Q1 への案」）。
- **BAR2 の view を消しても boot の console が壊れないか**: boot の console は ghci の BAR2 の mapping を使わない。x86_64 は loader の framebuffer の窓（`src/hal/x86_64/space.c` の `fb_window_hold`・`fb_window_give`）、`src/drivers/generic/boot-fb.c` は `kern_device_map` で自分の mapping を作る。ghci の driver が console に渡すのは `kern_text_end` の BAR の物理の範囲（`display-scan.c` の `boot_end`、`device->bars`）だけで、これは 1 page の map の `physical_address` と BAR の大きさから今までどおり作る。host 試験の「display boot end」の場面（BAR の範囲 2 つを渡す）は変えずに PASS。

## 設計（2026-11-13 P4）

### 何が写すか

| GPU | frame | 写す物 |
| --- | --- | --- |
| blitter 有り | discrete は local、他は host | blitter（host の frame も。p008 の「Q1 への案」をここで採った: CPU の写しが無くなったので） |
| blitter 無し、copy の program が使える | 同上 | driver の copy の program（session 0 の dispatch） |
| blitter 無し、executor 無し（shader unit 無し）か program の拒否 | frame を取らない | 無し: copied の道を出さない |

- **copied の道を出さない形**: scan-out の constraints（`scanout_constraints`）が `GPU_SCANOUT_COPY` を外し `GPU_SCANOUT_SHARED` だけを返す。libvulkan（`wsi-display.c`）は COPY が無ければ shared の道（blob をそのまま scan）を選ぶ。それでも来た copied の present は何も置かず ENOTSUP。frame（2 つ）は取らない（使わない memory を持たない）。
- **program の拒否を先に知る**: executor の表に `copy_ready` を足した（`ghci_executor_ops`、driver の中の表で HAL ではない）。program を compile して session 0 に置き、拒否（compiler か pipeline state）なら ENOTSUP を返す。constraints の問い合わせと claim（`buffers_take`）が呼ぶので、libvulkan が swapchain の道を選ぶ前に拒否が分かる。拒否は GPU ごとに覚えるので 2 回目からはすぐ答える。ENOMEM・EIO（今は置けない）は constraints・claim の誤りとして返す。
- **cursor**: surface（2 つ、64×64×4 byte）は CPU が書くので host memory に置き、表示の controller が DMA で読む（`ghci_scan_alloc` に `local` を足し、frame だけ local）。image は CPU が読むので host memory の storage だけを受け、local memory の storage は ENOTSUP（client が local に描いた cursor は host に写してから渡すか、software の cursor）。
- **窓**: driver は local memory の view を持たない。窓を開けるのは GOP の boot framebuffer を boot console が書く間だけ（前と同じ）。

## 実装（2026-11-13 P4、ab8fd68b7e）

- `display.c`: `copy_by_processor` を削除。`copy_engine` → `copy_route`（`COPY_NONE`・`COPY_BLITTER`・`COPY_PROGRAM`、copy_ready を呼ぶ）。`buffers_take`（engine が無ければ frame を取らない、cursor の surface は host）、`present_copied`（engine が無ければ ENOTSUP、program の ENOTSUP の後の CPU の写しを削除）、`scanout_constraints`（COPY の有無）、`cursor_rows`（local は ENOTSUP、window の読みを削除）。file の頭の説明。
- `display-scan.c`・`.h`: `ghci_scan_alloc` に `local`、`ghci_scan_write` は host だけ（local は ENOTSUP）。
- `memory.c`: `ghci_memory_window_hold`・`unhold`・`local_read`・`local_write`・`local_span` を削除、`ghci_memory_alloc_local` は view を作らない。file の頭の説明（processor は local memory に触れない、窓は boot framebuffer のため）。
- `pci-attach.c`: BAR2 は claim と先頭 1 page の map だけ（`WINDOW_PLACE_BYTES`、`window_claimed`）。
- `ghci.h`: `device->window`・`window_physical`・`window_bytes` と上の宣言を削除、`ghci_executor_ops.copy_ready`、allocation の `view` の説明。
- `vk/vk-meta.c`・`vk.h`・`vk-node.c`: `ghci_vk_copy_ready`（`rows_place` を 1 度）。
- 試験: `plan/ws290/tests/ghci-driver-host-test.c`、`ghci-vk-host-test.c`（下）。

## 試験（2026-11-13 P4）

- host 試験 `sh plan/ws290/tests/ghci-host-test.sh`（worktree、log は worktree の `build/p009-host-test.log`）: words 151・ref 56・model 280・driver 1079・vk 517・pipeline 176・execute 291・meta 98（GSI1 の model で 22 dispatch）、gsi1 の PASS の行、全部 0 failed、`PASS ghci290-host-test`。
  件数の変化（p008 の後の driver 1086・vk 522 から）は窓の道具の場面を消し、新しい振る舞いの場面を足した差。
  - driver: 窓の hold・local_read・local_write の場面と「窓の無い device」の場面を削除し、「local の run に view が無い・窓は閉じたまま」に。`start_device_window`（窓の view を渡す起動）を消し `start_device` に。「blitter 無し・executor 無し」の場面を新しい振る舞いに: frame 無し、constraints は SHARED だけ、copied の present は ENOTSUP で command が 1 つも増えず sequence 0・窓は閉じたまま、shared の present は表示される。blitter の場面に「両方の道」を足した。kc705-uma の最初の present は blitter の COPY_BUFFER 1 つで host の frame に（前は CPU）。cursor に discrete の場面: frame は local、surface 2 つは host、image を設定して flip され `CURSOR_BASE` が host の surface、local の image は ENOTSUP。
  - vk: discrete の場面の「driver が窓から読み書き」を「local の allocation に view が無い」に、「窓の無い discrete」の場面（client の拒否の重複）を削除。`test_window` を削除。表示: blitter 無しの 3 つの profile で constraints が両方の道、kc705-uma（blitter）は host の frame を session 0 に map して COPY_BUFFER 1 つ、program の拒否（claim の前に印）は frame 無し・rows を置かない・constraints は SHARED だけ・copied の present は ENOTSUP で何も置かない。
- kernel（worktree の build/、main の build/ は使わない）: `make PLATFORM=vf2 CONFIGMK=plan/ws347/tests/config-vf2-ghci.mk llvm-toolchain-cached` → `sysroot BUILD_JOBS=4` → `build/vf2/vmunix -j4`、`make PLATFORM=x86_64 CONFIGMK=plan/ws347/tests/config-amd64-ghci.mk` の同じ 3 つ。全部 rc 0、vmunix の log の warning 0、GHCI の 42 file を compile（log は worktree の `build/p009-vf2-*.log`・`build/p009-x86-*.log`）。

## 未実施・残り

- 実機（VF2＋KC705）・QEMU は無い（p004）。discrete の GPU で host memory の cursor の surface を表示が実際に読む事、blitter が host の frame へ写す事、constraints の問い合わせで copy の program を compile する時間は p004 で見る。
- `ghci_scan_write` の引数 `device` は使わなくなった（`(void)device`）。呼ぶ所は 1 つなので、規約の見直し（p005）で引数を外すかを決めてよい。
- WS290 の記録 `plan/ws290/design.md`（p011 の実装の段）と `plan/ws290/phase011/phase.md` は `ghci_memory_window_hold`・`local_read`・`write` と「driver が BAR2 を map している間」を今も書く（他の WS の記録なので直していない）。
- Q1 への案: ghci.md の §13.2「Physical Address Space」と §11.3 の比較の表は discrete の scan-out を local だけのように書き、§6.0・§11.3 の HOST_LIMIT・RTL は host を許す。driver は host の scan-out（idle frame、shared の blob、今回の cursor の surface）に頼るので、台帳の B35 の仕様の追記（「窓は GOP の VRAM のため」）と一緒に「discrete でも scan-out は host memory を読める」と書く事を仕様の側に頼むかを決めてほしい。
- 規約の全文の見直しは p005。

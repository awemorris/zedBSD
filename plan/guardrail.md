<!-- awesome-plan project=zedbsd record=guardrail -->

# Guardrail

zedBSD の貢献の規則と標準の索引。Queue・backlog・実行許可ではない。より新しく具体的なユーザーの決定が優先する。
規則の本文（エージェントの守ること）はリポジトリ直下の [AGENTS.md](../AGENTS.md) にもある（§4 担当の共通の規則・§7 禁止と承認・§9 検証）。

## 範囲と構造

- 作業は承認された Phase の範囲の中で行う。kernel（`src/`・`include/`）、bootloader、libc（`src/libc/`・`include/libc/`）、
  userland、platform、build/config、tools は所有が違う。file の追加や module の境界の変更の前に、現行のコードと Phase を確かめる。
- 2026-09-26の旧secondary queueは削除済み（当時の作業中データ/成果は無い）。2026-10-02ユーザー指示による新しいサブエージェント別Queueは別設計であり、[運用契約](agents/protocol.md)に従う。mainだけが共有Board/cacheとmergeを所有し、agent別の承認済みQueueを並列に実行できる。
- HAL: **API の変更（`include/hal/hal.h` の宣言・契約・HAL の責務）は具体的な差分ごとの事前承認**が要る。`src/hal/` の実装の変更
  （既存宣言の実装の修正・補完・最適化、arch 内部の header と struct）は承認なしで行ってよい
  （2026-09-25 ユーザー「HALの実装は勝手に修正してください。APIの変更のみ許可が必要です」。2026-09-12 の「HALの改変には許可が必要です」を
  この範囲に狭めた）。実装の変更は Phase の記録に差分の所在と検証を書く。hal.h に触れる差分はレビューできる形で plan に置き、承認まで適用しない。
  承認済みの差分は下の表。
- driver の配置と `drv_` の global symbol の方針を保つ。検証済みの refactor を古い試験の前提より信頼し、試験の側を直す。
- RTL8822B の `.inc` はライセンスを分けるために独立させており、**別の file のまま保つ**。
- kernel と libc（2026-09-23 ユーザー明確化）: kernel と libc はモノリシック。kernel・driver・HAL が include してよい libc の header は
  `libc/vulkan/*` だけ。ioctl・errno などの ABI は UAPI に分ける。kernel は標準 C の header 名を暗黙に読まず、libc の object を link
  しない（kcrt を使う）。`userland/desktop/libvulkan` は必須の構成要素。SPIR-V の compile は kernel 空間の driver が行う。この構成は変えない。
- **compositor は libvulkan だけを使う（2026-09-30 ユーザー）**: Keiland の compositor（zdesktop、`userland/desktop/wayland/`）は、GPU と表示を
  libvulkan（Vulkan の API と拡張）だけで扱い、GPU の UAPI（`include/uapi/gpu*.h`）を ioctl で直接呼ばない。入力の device の evdev の ioctl は
  対象の外。今残る直の ioctl（起動時の表示の問い合わせ、buffer の import の確かめ、fence の問い合わせ、表示の claim・release、Vulkan の無い
  予備の表示）は [WS103](ws103/ws.md) で移した（2026-10-01 完了。OS 固有の部分は macro でなく OS ごとの module `gpu-zedbsd.c` に閉じた。2026-10-04 WS131 p009 の改訂: OS 固有の部分は `libkeiland-backend-zedbsd/` に閉じる（`gpu-zedbsd.c` が kernel の GPU の型を読む唯一の file）（V1 の改訂、2026-09-30 夜 ユーザー承認）。確かめは `plan/tools/gpu-boundary/v1-check.sh`）。ユーザーの問い「私はKeilandコンポジターがlibvulkanのみを使用していると思っていたのですが、
  ioctlを使ってしまっているのですか？」への Q1 の説明の後、「規則にして今移す」を選んだ。
  同日の補い（ユーザー）:「どうしても最適化に必要なところは、opt-outできるようにマクロで囲めますか？必須機能では使っていない気がします。」→
  必須の機能は libvulkan だけで動かす。最適化のためにどうしても要る直の ioctl だけは、compositor の build の macro（例 `ZWL_GPU_DIRECT`、既定は有効）で
  囲み、macro を無効にした build でも全ての機能が動くようにする（WS103 の V1）。
  同日の決め（ユーザー）:「では、まずioctlを可能な限りやめて、Vulkan APIでlibvulkanで行うようにします。移行できない部分は、マクロで囲って、
  zedBSDでのみ行うようにします。evdevはLinuxにもあるので、ひとまずノータッチでよいです。」→ 直の GPU の ioctl はできる限り Vulkan の API（zedBSD では
  libvulkan）へ移す。移せない物は zedBSD の時だけ build される macro で囲む（Linux・FreeBSD の build では入らない）。evdev の ioctl は今は変えない。
- **配置**（2026-10-03 ユーザーの決定、WS131）: desktop の OS の抽象化（電源・network・音声・PnP・設定・seat・入力・表示・GPU の buffer）の OS に固有の code は libkeiland-backend にだけ置く。interface と複数の OS が共有する仕組みは `userland/desktop/libkeiland-backend/`、OS ごとの実装は `libkeiland-backend-zedbsd/`・`-linux/`・`-freebsd/`。libkeiland-backend を使うのは compositor だけ。compositor（`userland/desktop/wayland/`）と libkeiland は OS の header（`<uapi/…>`・`<linux/…>`・`<dev/…>`）・`"userland/base/…"`・`ioctl()`・OS の macro の block を持たない。macro の block は `libkeiland-backend/keiland-backend-evdev.h` の evdev の header の選択だけ。backend の OS の tree は自分の OS の header だけを include し、compositor の内部と `<keiland.h>` を include しない。**app の自分の機能のための OS の依存はこの規則の対象外**（D14、2026-10-03 user）: Terminal の pty（`TIOCSWINSZ` は 3 OS で同じ、`openpty` の header が `<pty.h>`／FreeBSD の `<libutil.h>` で違うので macro の block で切り替える）と Files の xattr（`info.c`・`task.c`、FreeBSD の extattr の読み替えは Files の中。`tags.c` は 2026-10-05 ws127-p012 で Tags の機能とともに削除）。X server は対象外にせず、Wayland の規格（evdev）の key code の定数を自分の header に持って `<uapi/input.h>` の include を無くす。対象外の一覧は checker の許可の表に置く。
  （2026-10-04 Q1: WS131 p009 の merge で、下の旧い規則（2026-10-01 の WS104・WS105 の OS ごとの `<package>/<os>/` の配置）をこの段落で置き換えた。checker は `plan/tools/keiland-os-boundary/check.sh` の C1〜C5・L1〜L7・M1・X1・B1・B3・S1、X server の key code は `xserver/keycodes.h`。旧い規則の文は git の履歴と WS131 の design.md §3.7。）
- **app と設定**（WS131・WS135、2026-10-04、WS131 の design §3.7 を p011 の merge で適用）: app は WiFi・network・音量・電源・PnP を libkeiland の `kl_system_*`（拡張 `kl_system_manager_v1`）でだけ扱い、desktop の設定を `kl_settings_*` でだけ扱う。libkeiland は daemon に接続せず、system の file を読まない（例外は app 自身の設定の file `~/.config/keiland/<app>.conf`）。`desktop.conf` と WiFi の鍵の store を書くのは compositor だけ（鍵は compositor の thread が保存する）。compositor が libkeiland を link する時は描画の層と motion・scroller・gesture・version だけを使い、Wayland の client の部分を使わない（D4）。確かめは `plan/tools/keiland-os-boundary/check.sh` の B2（compositor の `nm -u`）と B3（backend を使うのは compositor だけ）。
- **Bluetooth と Display も compositor 経由（2026-10-08 ユーザー）**:「SettingsのBluetooth関連の操作は、libkeilandで抽象化して、zedBSD/Linux/FreeBSDで同じインタフェースで使えるようにします。libkeiland --> compositor --> コンポジタのlibkeiland-backend --> プラットフォームのデバイス操作 、です。コンポジタを通しているのは、libkeilandをプラットフォーム独立にするためです。サウンドやWiFiも同様です。libkeilandにプラットフォーム固有の操作がもし入っていれば、それはコンポジタ経由に移したいです。同様に、Settingsのディスプレイ関連の操作も、libkeilandで抽象化します。」→ 上の「app と設定」の規則（kl_system_* は compositor の拡張を通す）を Bluetooth と Display に広げる。app（Settings を含む）は Bluetooth・Display を libkeiland の口だけで扱い、libkeiland は compositor の拡張を呼び、compositor は libkeiland-backend の OS ごとの実装（zedBSD は bluetoothd・GPU、Linux は BlueZ・KMS、FreeBSD は各々）を使う。libkeiland に残っている OS 固有の操作は compositor 経由に移す（2026-10-08 Q1 が監査中）。
- **browser / libbrowser（2026-10-01 ユーザーレビュー）**: engine の source/private header/table/shader/生成器は `userland/desktop/libbrowser/` が所有し、
  `userland/desktop/browser/` は libbrowser.so のコンポーネントを window/tab に包む main/shell/app data を所有する。
  libbrowser は標準 Vulkan を使用可、Wayland の header/API/protocol と直接の link は public/private とも使用不可。
  shell が Wayland events を抽象化した public input interface に変換する。全文の正本は [browser component](standards/browser-component.md)。
  新しい mandatory rule と配置の置換であり、C coding-style の例外ではない。[WS107](ws107/ws.md) で移行し、最終source/header/include/link/実clientを検証済み（2026-10-02）。
- **test app の配置（2026-10-01 ユーザーレビュー）**: base/desktop の対象30件（mview/gpudemo を含む）を `userland/tests/` へ移す計画は [WS106](ws106/ws.md)。
  対象表と package/config/install の契約を確定してから移動する。POSIX test utility は base に残す。
- kernel の実装を userland の build の依存へ写さない。`mkfs` などの tool は単独で使える形を保つ。
- base system の実装とライセンスの境界: [設計方針](master-design-policy.md)。
- 外部 package（`userland/packages/`）はソースツリーへ取り込まず、tarball を取得・検証して patch する。ライセンスは
  [provenance](ws032/provenance.md) と `plan/tools/packages/audit-licenses.sh` で監査する。GTK4/Qt6の先行upstream移植から後のzlib独自実装へ学ぶ順序とsource境界は[WS114–116方針](standards/ws114-gtk-qt-learning.md)。
- WS は一つの具体的な到達目標を持つ（2026-09-12 ユーザー指示）。完了・終了した WS を再利用して別の目標を足さない。
  WS の終了時は子 Phase を全件照合し、未完了は完了にせず、指定の保留先か別の WS へ引き継いで元の Phase を閉じる。

## 標準と検証

- C のコーディング規約: [coding-style.md](coding-style.md)（全文が正本。簡約版は置いていない）。新しいコードには全文を適用する。
  規約の変更で評価順・所有・振る舞いを変えない。tool の対応: [standards/automation.md](standards/automation.md)。
- 集約の `make check` は走らせない。Phase に意味のある絞った確認を行う。
- build の関門: 選んだ platform の構成で `make -j16`（warning 0）。
- 起動の確認は `plan/tools/boot-test.sh`（画面の login prompt）。QEMU の console log・serial log を読んで判定しない。
  WS105 の Debian 13 Linux guest は SSH の疎通と QMP の `screendump` の PNG を用いる（2026-10-01 ユーザー許可、[WS105 D25](ws105/ws.md)）。SSH はホストの loopback から QEMU の転送を経て guest に接続する。zedBSD の image は `boot-test.sh` を使う。
  機能の回帰は guest を起動しない host の試験で行い、guest の操作はシリアル（`plan/tools/guest/serial.py`）か SSH
  （`plan/tools/guest/guest.sh`）で対話する。不具合の解析は QEMU の gdbstub・monitor・QMP で行う。詳細は [tools.md](tools.md) の「QEMU の解析」。
- 回帰の範囲は Phase の性質で決める。コードの意味を変えない refactor は build（warning 0）と最後の boot test だけ。
- 表示の build（2026-09-29 ユーザー）: demo・実機の image は既定（logo を出し `kmsg=quiet`）。GPU の driver を直す Phase では logo を無効にし
  kernel の message を画面に残す（`ZEDBSD_GRAPHICAL_BOOT=n "ZEDBSD_BOOT_EXTRA_LINES=display=edp login=graphical"`）。
- ユーザーの受け入れの範囲を守る。取り下げられた網羅的な異常系試験・繰り返しの実機起動・免除された実機関門を戻さない。
  QEMU の証拠と実機の証拠を分けて書く。

## 承認済みの HAL 差分

| 日付 | 範囲 | 出典 |
| --- | --- | --- |
| 2026-09-12 | amd64 の MMIO read/write accessor 8 個（hal.h・責務は不変） | WS014 p003（q306） |
| 2026-09-13 | amd64 の device mapping の補完（patch SHA256 `e6ec9e6c2deda41b840fa6f10846438d091f3a20ce782b9251b7979ac7591c8d`） | WS030 p002（q308）、[承認記録](https://github.com/awemorris/zedBSD/issues/390#issuecomment-5647471812) |
| 2026-09-23 | HAL 配下の `#include` path の変更と、kernel/HAL 共通の compile flag（`-nostdlibinc`・`-fno-builtin`）。宣言・実装・責務は不変 | WS035 p004・p023・p034・p035 |
| 2026-09-23 | `HAL_TIMER_FREQUENCY` を arch ごとに（`include/hal/arch/<arch>.h`）、pc98 の PIT 入力 clock を BIOS `0:0501` bit 7 で選ぶ | WS040 p001・p005 |
| 2026-09-23 | i386 の `hal_mmio_read8`/`hal_mmio_write8`（既存宣言の補完） | WS036 |
| 2026-09-24 | rpi4 の framebuffer の console の font を PC/AT の 8x16 の複製へ（`src/hal/arm64/bsp-rpi4/` への font の追加と描画の置換。hal.h は不変） | WS044 p001 |
| 2026-09-24 | rpi4 の framebuffer に描いた後（文字の枠・カーソル・初回のクリア）に data cache をメモリへ書き出す（`src/hal/arm64/bsp-rpi4/framebuffer.c`、`hal_dcache_clean_range` を使う。hal.h は不変）。実機で画面に出ない不具合の修正。差分 `plan/ws044/proposed/rpi4-framebuffer-cache.diff`、承認「承認、すぐ進める」 | WS044 p006 |
| 2026-10-23 | ARMv7-A（ZYBO）の HAL の事前承認の 3 点: include/hal/arch.h に armv7 の節、新しい include/hal/arch/armv7.h、受け渡しの名前 `boot.command-line`・`armv7.fdt`・`armv7.initrd`（hal.h は不変）。案 plan/ws286/proposed/hal-armv7.md、承認 ユーザー「U項目は読みましたが、全部推しでOKでした。」（U1） | WS286 |
| 2026-09-24 | rpi4 の framebuffer の大きさを firmware の設定（`TAG_GET_PHYSICAL`、config.txt の 1920x1080）から取り、cache の書き出しの後に `dsb sy`（`src/hal/arm64/bsp-rpi4/framebuffer.c`。hal.h は不変）。差分 `plan/ws044/proposed/rpi4-framebuffer-size.diff`、承認「承認、すぐ進める」 | WS044 p007 |
| 2026-09-24 | rpi4 の起動の診断: ACT LED（GPIO42）の点滅で段階を示す `led.c`/`led.h`、framebuffer の 3 秒のテスト模様と値の表示、`locore.S` で `SCTLR_EL1` を確定した値にする（MMU・cache 無効、little-endian）。hal.h は不変。差分 `plan/ws044/proposed/rpi4-boot-diagnostics.diff`、承認「承認、すぐ進める」 | WS044 p008 |
| 2026-09-24 | arm64 の命令 cache の同期: user の page を実行可能に対応付けるとき（`hal_space_map`）と実行可能に変えるとき（`hal_space_prot`）に `hal_sync_instruction_stream`、MMU の有効化で EL0 に UCT・DZE・UCI・nTWI・nTWE（`src/hal/arm64/space.c`・`locore.S`。hal.h は不変）。実機の init の SIGILL の修正。差分 `plan/ws044/proposed/arm64-icache-sync.diff`、承認「承認、すぐ進める」 | WS044 p009 |
| 2026-09-25 | amd64 の `amd64_percpu_current()` を `rdmsr IA32_GS_BASE` から `%gs:0` の `self` の load に（`src/hal/amd64/percpu.c`、`percpu.h` に `self` が先頭の field である `_Static_assert`。hal.h は不変）。lock と `thread_current()` のたびの rdmsr が kernel の時間の約 24% だった。差分 `plan/ws046/hal-percpu-gs.md`、承認「承認待ちのHALの変更を許可します」 | WS046 p009 |
| 2026-10-28 | amd64 の user の map で `HAL_SPACE_DEVICE|HAL_SPACE_WC` を WC の device page（PAT index 4）として受け、device の WC|NOCACHE は INVALID、RAM への WC は UNSUPPORTED（`src/hal/amd64/space.c` の leaf_flags・user_page_allowed、hal.h は不変。kernel の map_device の WC|DEVICE は今までどおり INVALID）。差分 P2 779020ee6 | WS291 q1635、ユーザー「P2 の WC の変更は承認します。」 |

| 2026-09-25 | amd64 の page table の owner PTE に子 table の present entry の数を bit 52〜62 に持ち、`hal_space_unmap` の空 table の切り離しを全 table の走査（`detach_empty_tables`）から O(1) の判定に（`src/hal/amd64/space.c`。hal.h は不変）。差分 `plan/ws061/phase002/hal-amd64-table-counts.diff`。2026-09-25 の規則の変更（実装は承認不要）により適用 | WS061 p002 |
| 2026-09-27 | arm64 の `hal_pmem_map_uncached()`・`hal_pmem_unmap_uncached()`（hal.h に宣言を追加、arm64 だけ実装: MAIR の entry 3 を Normal non-cacheable、`0xffff_0080_0000_0000` の uncached の窓、`src/kern/uncached.c`）。PCIe の DMA が cache を snoop しない Pi 4 の xHCI のため（`plan/ws048/proposed/hal-pmem-uncached.diff`） | WS048 p004。ユーザー「HAL approvalsは3つとも承認します。」 |
| 2026-09-27 | amd64 の `hal_get_arch_handoff("acpi.rsdp")`: HAL の ACPI の発見が受け入れた RSDP の物理 address を返す（hal.h は不変、HAL の責務の追加。`plan/ws049/proposed/hal-acpi-rsdp.diff`） | WS049 p006。同上 |
| 2026-09-27 | aarch64 の `include/hal/arch/aarch64.h` に `hal_gpregs`・`hal_fpregs`・`hal_vregs` と `HAL_DEBUG_*`（amd64 と同じ形）、`src/hal/arm64/debug.c`（single step、hardware breakpoint・watchpoint、context switch ごとの debug state）。ptrace のため（commit 27831f19） | WS044 p010。同上 |
| 2026-09-28 | amd64 pcat の `src/hal/amd64/bsp-pcat/cons.c`: `kmsg=quiet` のとき framebuffer を消さず、右上の 136x40 の進捗の枠を logo の背景で塗り、`console_suspended=1` にする（HAL の責務の変更。hal.h は不変） | ユーザー「HALのdiffは承認します。」（2026-09-28） | `plan/ws035/proposed/hal-quiet-console.diff`（SHA256 3f63a8411d5b3684c8bb790e86e049a0d70122626c6f1aca579e5413cbb8566b） |
| 2026-10-05 | `hal_irq_set_mode(int irq, int trigger, int polarity)` と `HAL_IRQ_TRIGGER_EDGE/LEVEL`・`HAL_IRQ_POLARITY_HIGH/LOW`（IRQ を mask した間に trigger と極性を設定、表せなければ `HAL_ERR_UNSUPPORTED`）。**ユーザー自身が hal.h に追加**（commit 6cc1bea、「hal_irq_set_mode()を追加しました。HALのインタフェースの追加は私が行いました。実装はサブエージェントに任せましょう。」）。P1 の案 `hal_irq_set_trigger` は不採用。各 architecture の実装は subagent（WS159 p006） | WS159 p006 |
| 2026-10-05 | WS052 S0i3 の HAL v2（専門家のレビューと Fable の再レビューの結論）: H1v2 `hal_cpu_idle_suspend_supported()`・`hal_cpu_idle_suspend()`、H3v2 `hal_cpu_notify()`・`hal_cpu_notify_mask()` の契約の明記、H4v2 `hal_irq_set_wake(int irq, bool enable)`・`hal_irq_suspend()`・`hal_irq_resume()`（wake の IRQ は handler を呼ぶ、latch しない）、H5 `hal_rtc_read_counter()` の counter が全ての idle をまたいで進む契約。差分 `plan/ws052/proposed/` の 4 file（SHA256 は README-v2 §1）を Q1 が hal.h に当てた（commit 29f954f5）。ユーザー「ではレビュー結果の内容で、HALのインタフェースの変更を許可します。」「hal.hの追加をレビューしました。OKです。…今回の変更は承認します。HAL実装に進むことを承認します。」 | WS052 p006 |
| 2026-10-14 | amd64: user の GS base の設定・取得の 2 関数の宣言と `struct hal_gpregs` の `gs_base` の意味の変更（A1、`include/hal/arch/amd64.h`）、HAL が user の GS base を task の状態に持ち kernel の入口・出口で swapgs する（A2）。差分の案は [ws080/proposed/hal-gs-base.md](ws080/proposed/hal-gs-base.md)（ユーザーのクリック「A1・A2 を承認」） | WS080 p002（注記 2026-10-14: A1 の宣言は hal.h の `hal_task_t` の typedef より先に読まれる header にあるため `void *task` と書いた。型は同じで定義は `hal_task_t`、P1 a06d5d75d） |
| 2026-10-18 | riscv64（WS256、VisionFive 2）: (1) `include/hal/arch.h` に `HAL_ARCH_RISCV64` の節、(2) `include/hal/arch/riscv64.h` を新規（aarch64.h と同じ節: tick 1000 Hz・signal frame・atomic・debug point なし・checks）、(3) riscv64 の HAL が `hal_get_arch_handoff` で名前付きの object `riscv64.fdt`・`riscv64.initrd`・`riscv64.boot-hart` を返す（hal.h は不変、HAL の責務の追加）。差分は [ws256/proposed/hal-riscv64.md](ws256/proposed/hal-riscv64.md)。案から変わる時は改めて差分を示す | ユーザー（クリック）「3 件とも承認」 |
| 2026-10-18 | 新しい port（WS263 riscv32・WS264 ppc32・WS265 ppc64・WS266 m68k の virt）の事前の承認（形を限る）: hal.h は変えない。`include/hal/arch.h` に arch の 1 節、新しい `include/hal/arch/<arch>.h`（aarch64.h・riscv64.h と同じ節の並び）、`hal_get_arch_handoff` の名前付きの値。それ以外の HAL の API の変更は本来どおり差分ごとの承認を待つ。入れた差分は Q1 が朝に一覧で報告する | ユーザー（クリック）「形を限って事前に承認」 |
| 2026-10-19 | 新しい port の WS271 mipsel・WS272 mips64el（QEMU malta）にも同じ形の事前の承認（ユーザーのクリック「同じ形で事前承認」）: hal.h は変えない。`include/hal/arch.h` に arch の 2 節、新しい `include/hal/arch/mips32.h`・`mips64.h`、名前付きの受け渡し `boot.command-line`・`mips.initrd`。案は [plan/ws271/proposed/hal-mips.md](ws271/proposed/hal-mips.md)。形を外れる変更は差分ごとに承認 | WS271・WS272 |
| 2026-10-19 | WS273 の `include/hal/arch/mips32.h` の `HAL_TIMER_FREQUENCY` の comment だけの差分（ユーザーのクリック「全部推奨どおり」の D8）: [plan/ws273/proposed/mips32-h-timer-comment.diff](ws273/proposed/mips32-h-timer-comment.diff)。意味は変えない。FR=0 の `hal_fpregs` の案（mips32-h-fpregs-fr0.diff）は承認していない | WS273 |
| 2026-10-19 | WS273 の実機の検証の例外（ユーザーのクリック「承認する」の D7）: 起動の確認は boot-test.sh だけの規則の例外として、ユーザーが RG350 の実機の画面・LED を目で見て写真で返す結果を証拠にする。試験は今すぐ頼まずユーザーが拾える一覧に置き、結果が届くまで Phase は uncleared | WS273 |
| 2026-10-19 | WS256 VisionFive 2 の framebuffer（ユーザーのクリック #29「両方を承認」）: hal.h は変えない。(1) VF2 の HAL が loader の handoff 版 2 の framebuffer を受けた時だけ `hal_get_arch_handoff` で `riscv64.framebuffer.phys`・`.size`・`.width`・`.height`・`.pitch`（byte）・`.format`（`KERN_BOOT_FRAMEBUFFER_*`）を返す（64 bit の word への pointer、無い時 NULL）。(2) VF2 の HAL が同じ framebuffer に早期の 80x25 の console を描く（bsp-virt の console を riscv64 の共通に移す）。案は [plan/ws256/proposed/hal-riscv64-framebuffer.md](ws256/proposed/hal-riscv64-framebuffer.md) | WS256 |
| 2026-10-19 | WS275 sun4u の firmware framebuffer（ユーザーのクリック「承認する」、改訂 3 の文面で「改訂 3 で承認」2026-10-19）: hal.h は変えない。include/kern/boot.h に `KERN_BOOT_FRAMEBUFFER_XRGB8888`（3、byte が X,R,G,B の順）と `KERN_BOOT_FRAMEBUFFER_RGB332`（4、loader が palette を設定済みの 8 bpp）を足し定義文を直す（各名前は address の小さい順の byte、XRGB8888 は同じ名前の DRM の format と逆で DRM の XRGB8888 は BGRX8888、RGB332 の bit 7-5・4-2・1-0 と ramp）。bsp-sun4u が名前付きの値 `sun4u.framebuffer.phys`・`.size`（linebytes×height）・`.width`・`.height`・`.pitch`・`.format`・`.pci.vendor`・`.pci.device`、`sun4u.i8042.data`・`.command` を返す（意味は「HAL が表示している framebuffer」、Bochs の fallback の時も返し、NULL は画面を出せなかった時だけ。i8042 は loader の受け渡しから、D4 (a)）。案は [plan/ws275/proposed/hal-sun4u-names.md](ws275/proposed/hal-sun4u-names.md)。再レビューで文面が変われば改めて示す | WS275 |
| 2026-10-19 | 例外: sun4u の kernel が pcat の `src/drivers/platform/pcat/graphics/vgafont.c`（`drv_pcat_vgafont16`）をそのまま compile する（WS275、design-review-1 の M4。boot-test.py が path で読むので移さない）。generic への移動は F-111 で boot-test.py の追従と一緒に | WS275 |
| 2026-10-30 | hal.h の RTC の説明の comment に残る KERN_CLOCK_HZ（WS332 で削除）の語を、意味を変えずに今の言葉に直す。宣言・型・約束は変えない（ユーザーの承認） | WS332 p002 |
| 2026-10-30 | include/hal/atomic.h: 契約を macro だけにする（ENTER の直列化は macro の中の block で enter・leave を宣言して呼ぶ、hal_atomic_cookie_t を消す、arch.h・types.h の include を外し、単体の include を認める comment）。hal.h の宣言は変えない（ユーザーの指示） | WS331 p010 |
| 2026-10-30 | arch の header の hal_atomic_uint_try_acquire・hal_atomic_relax を include/hal/arch/atomic/<arch>.h の macro に移し、hal/atomic.h が -DHAL_ARCH_* で選んで include する（arch の header から消す）。hal.h の宣言は変えない（ユーザーの指示） | WS331 p011 |

2026-09-25 以降、hal.h を変えない `src/hal/` の実装の変更は承認を要しない（上の規則）。hal.h の変更はこの表の承認が要る。

## HAL と kernel・boot loader と HAL の受け渡し（2026-10-18 ユーザー）

ユーザー:「HALからカーネルへは、汎用の構造体しか渡せない規約です。固有の情報は、名前文字列で取得するAPIがありますので、それを使ってください。ブートローダからHALへは、固有の構造体を使えますが、それはHAL内に定義を置きます。また、boot.hのPC98やRPiの構造体は、削除します。この削除対応を、WSを作って別なサブエージェントで進めてください。」
- HAL → kernel は汎用の構造体（`include/kern/boot.h` の `kern_boot_handoff` など）だけを渡す。platform・architecture に固有の情報は、名前の文字列で取る API（`hal_get_arch_handoff(name)`）で渡す。
- boot loader → HAL は固有の構造体を使ってよい。その定義は HAL の中か `bootloader/` の header に置く（`include/kern/` に置かない）。**`bootloader/` の header を include してよいのは HAL だけ**（2026-10-18 ユーザー「bootloader/のヘッダを見ていいのは、HALだけにします。HALからカーネルへは、名前ごとの値でアクセスします」）。kernel・driver は HAL から名前ごとの値（例 `pcat.framebuffer.phys`）で受ける。
- `include/kern/boot.h` の PC-98・Raspberry Pi の固有の構造体は削除する（WS261）。

## driver は HAL を知らない（2026-10-30 ユーザー）

ユーザー（2026-10-30）「ドライバから見て、HALは使えません。存在を知りません。カーネルの機能を使ってマッピングするのみです。」「ドライバがHALをincludeしているところがあれば、規約違反ですので、driversをinclude/srcともにgrepしてみてください。」「了解です。ガードレールに記載してください。これを修正するWSを立ててください。P2で実行してください。HALのマップ関数には、カーネル側に相当する関数があるはずです。まずは違反利用された関数が、どの関数に置き換えられるか、一覧を作成してください。そして、ラッパーがない場合、カーネルに関数追加しますので、その一覧をください。」

- driver（`src/drivers/`・`include/drivers/`）は HAL の header（`hal/hal.h`・`hal/<arch>/…`）を include せず、`hal_*` を呼ばない。memory の map・割り込み・I/O・時刻・boot の情報などは kernel の機能（`kern_*` の口、`include/kern/`）だけを使う。kernel に相当の口が無ければ kernel に関数を足す（HAL の API は変えない）。
- 2026-10-30 の grep で、既存の違反は `src/drivers/` の 38 file・41 か所の include（`include/drivers/` は 0）。直すのは [WS331](ws331/ws.md)。直るまでの既存の違反は残してよいが、新しい code で増やさない。

## WS141（Raspberry Pi 4 の GPU）の GPL の参照の扱い（2026-10-04 ユーザー）

Linux の vc4・v3d（GPL-2.0）から作る作業の文書は `plan/ws141/temp/` に置き commit しない（書き写し可）。zedBSD の code を書く前に定数を全て一括で独自の名前に変え、WS の最後に license と字面・設計の類似を監査する。BLOB は `userland/firmware/` へ移して file から load。register の定義と packet の形は MIT の Mesa（`src/broadcom/`）・Broadcom の公開の文書・device tree の binding から取り、file ごとに license を監査する。**改訂（2026-10-04 ユーザー、クリックの回答「事実として使う」）**: V3D・HVS・pixelvalve・HDMI の register の offset と bit は GPL の header にしか無いので、値はハードウェアの事実として使ってよい。名前は一括で独自に改名し、配置・comment・構造は写さず自分で書き、WS の最後の類似の監査で確かめる。zedBSD の code は Zlib。詳細は [WS141](ws141/ws.md) の「ライセンスの扱い」。範囲は WS141 と [WS037](ws037/ws.md)（nvrtx、2026-10-04 ユーザー「やりかたはVC4と同じです」。GSP の firmware は `userland/firmware/`）と [WS214](ws214/ws.md)（Pi 4 の HVS の plane、2026-10-12 ユーザー（クリック）「WS214 にも広げる」: 公開の文書の無い HVS5 の scaled の plane の値も同じ規則で使う、値ごとの出典は `plan/ws214/hvs-values.md`）と [WS039](ws039/ws.md)（AMD RDNA の amdrdna、2026-10-15 ユーザー「値は事実として使い、名前・構造・comment は写さない、をこの WS にも広げます。」: Linux の amdgpu の GPL の file の値も同じ規則で使う、MIT の file・Mesa の RADV・ACO は file ごとに license を監査して出典として使う）だけ（他の WS の GPL の参照には適用しない）。

- 2026-10-30 ユーザーから、WS291 p021 の P4 に plan/ws141/temp/ の利用（Mesa（MIT）の照合用の file を plan/ws141/temp/mesa/ に置き、V3D の host 試験を compile・実行する）を許可する、という趣旨の指示を受けた。temp は git に入れない。

## MIT・GPL の出典のデータの引用（2026-10-15 ユーザー）

ユーザー:「レイテンシの表は、データなので、引用で済むように思います。引用元を明示すればいいと思います。」（nvrtx の compiler の `sm75-isa.h`・`sm75-schedule.c` の Collabora の MIT の表示について）
- MIT の source（Mesa の NAK など）から取った値・表（命令の field の位置、opcode、latency の表など）はデータとして引用し、**block ごとに引用元**（file の path・版・関数か表の名前・分かれば行）を comment で示す。code を写さない限り、出典の著作権表示と許諾の文は付けない。
- **code・comment・構造は、MIT であっても写さない**（2026-10-15 ユーザー「code・コメント・構造:については、MITであっても写してはいけません。」）。
- MIT の出典の扱いも全部の WS に当てる。
- **出典は該当箇所に block 単位で書き、file の上部には書かない**（2026-10-15 ユーザー「ガードレイルに、ファイル上部には参考にしたことや引用したことを一切書かず、該当箇所にブロック単位で記載することを明記してください。あとでドライバ全体をこれに従って監査します。」）: file の先頭の comment には、参考にした source・引用したデータ・派生の宣言・出典の一覧（SHA-256 を含む）を一切書かない。引用したデータは「Value referenced: <版> <path> (<表・define・関数の名前>)」（値が複数なら「Values referenced: …」）（2026-10-15 ユーザー「Procedure referenced: / Value(s) referenced: にしましょう。」）、参考にした手順は「Procedure referenced: <版> <path> (<関数の名前>)」の形で、その block の直前の comment に書く。comment は複数行の形（`/*` の行、` * ` の行を折り返して、` */` の行）にし、1 行の `/* … */` にしない（2026-10-15 ユーザー「referenced:は複数行スタイルのコメントにしてください。でないと、機械的につけたように見えてしまい、機械翻訳だと思われかねません。」）。driver 全体を後でこの規則で監査する（未対応の例: nvrtx の sm75-isa.h の先頭の出典の一覧、sm75-schedule.c の Collabora の表示、i915 の display の「Derived from the Linux kernel」）。
- **GPL にも広げる**（2026-10-15 ユーザー）:「GPL の file から取った値にも、定数やテーブルはデータであり、引用元の明示で使ってよい、というルールにします。」→ GPL の file（Linux の nouveau・amdgpu・vc4・v3d など）の定数・表もデータとして、block ごとに引用元を示して使ってよい。code・comment・構造は写さない（上の WS141 の節の規則は code について続く）。範囲は全部の WS（2026-10-15 ユーザー「範囲を全部の WS に広げます。」。上の WS141 の節の「他の WS の GPL の参照には適用しない」を置き換える）。

- **WS039（AMD RDNA、amdrdna）での適用**（2026-10-15 夕 ユーザー）:「もう1つサブエージェントを立てます。AMD RDNAドライバを書く既存WSの作業をします。このWSにも参照元などに関するガードレールをしっかり適用していきます。値の引用は明示すればOK、コード・シンボル名の引用は不可、ハードウェア操作手順の引用は明示すればOK、GPLなコードを引用しても、引用ならライセンスは影響しない、です。」→ WS039 にこの節の規則をそのまま厳格に当てる: 値（register の offset・bit・表・定数）は block ごとに「Value(s) referenced:」で出典を明示して使ってよい。hardware の操作の手順は「Procedure referenced:」で明示して参考にしてよい。code と symbol の名前（関数・型・macro・enum・register の名前）は写さない（独自の名前にする）。GPL の file からの引用でも、明示した引用なら zedBSD の code の license（Zlib）に影響しない。 補い（2026-10-15 夕 ユーザー、クリック「mnemonic は使ってよい」）AMD が公開する RDNA1 ISA の文書の命令名（s_mov_b32・v_add_f32・s_waitcnt など）は hardware の事実として code の名前・comment に使ってよい。Mesa（ACO）の source の symbol（aco_opcode の enum 名・関数名・型名）は写さない。

## source の中の開発の管理情報（2026-10-15 ユーザー）

ユーザー:「開発管理はすべてのソースから削除しますが、今は作業対象になったときに削除するだけでいいです。WS248でも消してほしかったですが、また今度でいいです。独立したWSを作って、あとでソースツリー全体を見直しましょう。WS249では削除を行いましょう。」
- source（comment・log の文字列）に開発の管理情報（WS・Phase・Queue・Bug・試験の回の番号、plan/ への参照、日付つきの決定の記録、開発の段の符号）を書かない。
- 2026-10-17 ユーザー:「…"The test build run under QEMU leaves the display hardware alone"みたいな話はplan/に書けばいいのであって、よそのプロジェクトでも流用されるソースコードから削除するべきと思います。…」→ このプロジェクトの開発・試験の進め方の話（試験の build・QEMU・試験機での振る舞い、どの段で何を入れたか、STAGE の類）も source に書かない。要るなら plan/ に書く。hardware と code の振る舞いの説明は残す。GPU ドライバ（src/drivers/gpu/）は WS251 p002 で先に消す。
- 既存の物は、その file が作業の対象になった時に、その作業の中で削除する（log の文字列を変える時は照合する試験・道具を追従させる）。tree 全体の見直しは [WS251](ws251/ws.md)。

## i915 の display の書き換えは 5330 で動く段ごとに（2026-10-15 夜 ユーザー）

ユーザー:「待ってください、書き換えが一度に大幅すぎます。すべての作業を即時中断してください。」「まず、Intel GPUのディスプレイコントローラについては、ゴールとしてはSkylakeからRaptor Lake Refreshまですべてで動くようにします。ただし、各ステップで5330で動くかどうかをきちんとリグレッションテストしながら、ステップで書き換えていきます。そうでないと破綻します。」「まず、5330で動作するところまで作業してください。5330で動作しないものをコミットしないでください。」
- i915 の display の書き換え（WS245）は小さな段に分け、各段を 5330 で回帰の試験（起動・内蔵 LCD の表示・desktop）に通してから main に入れる。5330 で動くと確かめていない物は main に commit・merge しない。
- 既に main に入った WS245 の物（p002 の 9e78e62f8・3fdaf59dc・a9925a4db、p004 の 2d5b167aa）は、まず今の main が 5330 で動くかを確かめ、動かなければ revert する。
- 「内蔵 LCD の表示」の確かめ（2026-10-15 夜 ユーザー、クリック「register などの証拠で代える」）: 各段では panel の電源（PP_STATUS）・backlight の PWM・frame counter・表示中の buffer（SURFLIVE）・pipe の CRC などを ssh で読み段の前後で比べる事で満たす。目視は節目だけユーザーの一覧に。外部 monitor は常設しない（「常設できない」）、段の kernel で止まった時はユーザーが電源を入れ直す（「その都度自分で入れ直す」）。5330 の確かめは素の起動で行う（passthrough は VBT が無く panel が点かない、2026-10-15 ユーザー「ベアメタルにしましょう」）。
- **全部を実装してから試験（2026-10-16 夕 ユーザー、下の「まとめて試験」を置き換え）**:「i915は、実装だけ先に終わらせ、全部マージしたものをテストして、動かなければ二分探索で半分マージしてテスト、だめならさらに半分、で進めます。まずはぜんぶ実装してください。」→ 残りの段を全部 branch に積んで実装（段ごとに host 試験・build の warning 0・記録）、全部を積んだ tip で T1 が 5330 の回帰。動かなければ半分の段までの物で試験、だめならさらに半分、と二分探索で当たりの段を探す。main に入れるのは 5330 で合格した所まで。
- **まとめて試験（2026-10-16 夕 ユーザー）**:「i915は、実装だけ先に終わらせて、10 phase分くらいまとめてマージしてテスト、という流れで加速しましょう。」→ 段は小さいまま、段ごとの 5330 の回帰はやめ、約 10 段を branch に積んでから 1 回まとめて 5330 の回帰（T1）に通し、合格したら main に入れる（5330 で動くと確かめていない物を main に入れない規則は保つ）。FAIL の時は段を半分に割って当たりを探す。host 試験・build の warning 0 は段ごと。

## 決定の出典

ユーザーの指示（HAL の制限と rollback の review、RTL8822B のライセンスの例外、refactor と試験の信頼、WS025 の規約の柔軟性、
インストーラの受け入れの範囲、QEMU だけの UAS の受け入れ）。新しい規則はこの索引、該当する規約の全文、tool の対応、影響する計画を
更新する。合意した構造や範囲を黙って置き換えない。

- WS106 の純粋なsource移動には [既存style維持の限定例外](standards/ws106-relocation.md)を適用（2026-10-01 ユーザー回答）。新実装/意味変更は対象外、全文reviewとhash/diff/build/install/bootは必要。WS106完了時に適用を終了。

WS107 の限定例外（2026-10-02ユーザー承認）: [全文](standards/ws107-relocation.md)。内容不変の移動styleのみ保持、component品質修正/新試験/既知14候補はC全文適用、完了時失効。

WS108（2026-10-02 user）：Debian13/Ubuntu26.04のQEMU guest作成・native build/dpkg導入/GUI検証、loopback SSH/QMP PNGで起動確認を許可。WS105の既存SSH/PNG方式をこの2OSへ適用、serial/console log判定禁止、host画面/入力/optを変更しない。CIのmake targets/release filesを作成、pushと実remote publishは行わない。

WS108 release/native検証の再利用tool: [driver/inputs/手順](../tools/release/keiland-linux-deb/README.md)、[検証coverage](standards/automation.md#ws108-packaging-coverage-2026-10-02)。2026-10-02指示を実装・検証、C標準/既存OS/ABI方針は不変。

## WS109 native FreeBSD の承認（2026-10-02）

ユーザー「drm-kmodを利用OKです。FreeBSDにも例外を適用します。FreeBSD実機は用意しておくので、作業を進めておいてください。」（2026-10-02 JST、このchat）。D1:既存FreeBSD drm-kmod利用可、GPL-free systemstack条件をこの範囲で置換。Keiland sourceの寛容license/外部実装を取り込まない境界は維持。D2:WS109専用FreeBSD QEMU guestのloopback SSH/QMP PNG検証を承認。D3:実機はユーザーが準備、入手前にnative build/backend実装を進める。実GPU/WiFi結果は将来の実機関門に残し、mock/QEMUbuildで代替しない。

適用の全文: [WS109 native scope](standards/ws109-native.md)。C規約の例外ではない。実機のdevice名/driver/検証結果は到着後に記録し、WS acceptance F1/F3/F4/F5の残る部分を検証する。

WS109 q557 native input classification: Linux/FreeBSD share the evdev discovery/read and native
EVIOCGABS/EVIOCGNAME/EVIOCGID/EVIOCGBIT/EVIOCSCLOCKID mechanism in wayland/evdev, with native
record/ioctl constants chosen only by the approved tiny zwl-evdev.h selector. Seat/VT/device
authority stays in OS modules; dma-buf reservation export ioctl also stays in OS modules.
This realizes the existing shared-mechanism rule for the authorized FreeBSD port; public
contracts/device operations and zedBSD GPU/ioctl constraints are unchanged. Boundary checker
recognizes freebsd roots and only those existing evdev request families in that one source.

## 2026-10-02 / ws109-20261002-qemu-venus-acceptance

Current user, this chat async reply: 「実機検証は不要です。qemuでVenusが使えればclearとします。」
This explicitly replaces the earlier user-provided-machine gate. Physical GPU/display/WiFi
acceptance is waived for WS109; do not request hardware or reintroduce those gates. Required
replacement evidence is actual FreeBSD QEMU Venus usage; mere host support, headless lavapipe or
Linux/zedBSD Venus does not establish that evidence. Native backend/build/standards and affected
regression obligations remain. p004 hardware radio operations become waived, not falsely tested;
native audio/wired and native radio ABI/refusal/WPA wire evidence remains classified accurately.
p003/F3 and p005/F5 replace physical display/main-app checks with owned FreeBSD QEMU Venus-backed
checks. If the native guest stack lacks a required Venus driver, investigate a bounded actual
capability chain and expose the remaining platform/scope choice; kernel/driver port is still outside
WS109's agreed scope. Current q566 standards/regression/docs subset stays authorized; Venus
configuration/implementation is selected separately after q566. No automatic WS/Phase clearance.
Origin user decision reconciled to WS/all changed Phase own criteria, Guardrail/scoped standard,
Queue supplement and docs; remote decision/structural events pending publication.

## 2026-10-02 / ws109-20261002-user-i915-passthrough

Current user chat reply: 「awe@10.0.10.25 でi915をPCIパススルーして、FreeBSDゲストを実行してみましょう。」
This authorizes SSH to that specified host and an owned FreeBSD QEMU guest with existing
IrisXe0000:00:02.0 passthrough. The prior loopback-only rule has a scoped host-control exception;
guest SSH remains via remote127.0.0.1 forward, QMP PNG and real native observations, no serial
logs. Physical WiFi/user-supplied-machine request remains waived. Try this explicit native i915
GPU route to address q567 Venus prerequisite; boot alone does not clear F3/F5 or claim Venus.
Remote survey: chaos/Linux6.19.13/QEMU10.0.11/7.4GiBmemory; GPU8086:46a8 alreadyvfio-pci,
IOMMUgroup0 GPUalone, no other QEMU. Use4GiB guest, owned disk copy/overlay/endpoint. No host
GPU unbinding/reboot/kernel/library replacement or unrelated VM/process changes. Existing
native i915/drm-kmod may be installed in own guest under prior permission. New driver port
still excluded. Actual native Vulkan/DRM/provider/render/lease checks required where possible.

## 2026-10-02 / ws109-physical-build-install

最新ユーザー: awe@10.0.30.3 ~/zedBSD実機の全操作を事前承認、make keiland-freebsd / sudo make keiland-freebsd-install / /opt/keiland直接起動を希望。最後のGUI受け入れはユーザーの実操作確認。先の実機waiverをこの受け入れについて置換、q572 evidenceは歴史として保存。SSH hostkey変更はユーザーが新ED25519指紋を確認済み、task専用known_hostsで接続。WIP commitに続く開発host pushとFreeBSD pullも追加指示で今回承認（従前push禁止のscope例外）。非force pushのみ、remote人間変更を保つ。native compiler/base libcと既存packages、seatd/video設定を利用、make toolchain不要。FreeBSD GDMはportのWayland制限説明後ユーザーが撤回、対象外。全newsourceの全文規約確認をp007で実行。Issue/Project/comment公開承認とは区別しoutboxを保つ。

## Linux 5種類のpackage計画（2026-10-02）

Event ws112-package-plan-20261002: [方針全文](standards/ws112-linux-packages.md)、[WS112](ws112/ws.md)。指定deb3/rpm1/Arch1をCIで作成して全5種類releaseへ添付、ad hoc生成可、CI runtime確認不要。RPi OS arm64はuser回答で確定。FreeBSDはsource install前提でpackage無し。
既存WS108の歴史とQEMU native build指示は保持し、今後のCI runtime必須方針を置換する。最終全文規約/package形式・metadata/CI artifact整合は必要、C例外は無し。今回は計画のみ、Queue/実装/push/公開承認ではない。

2026-10-02 / ws112-rpi-build-only-20261002: WS112 RPiはuserの追加指示でbuild/deb生成のみの受け入れ、QEMU GPU制約を理由としてGPU/GUI/実機関門は不要。正本はstandards/ws112-linux-packages.md。

## WS113 複数displayの新しい契約（2026-10-02）

Event ws113-multidisplay-plan-20261002: current userのi915接続通知/Vulkan Display拡張→compositor、Settings→libkeiland→compositor拡張、全拡張/全mirror、配置drag/窓全体移動の指示。3追加回答（pointerが隣画面へ入った時に切替、単一出力は拡張時、zedBSD i915をまず受け入れ）を[全文](standards/ws113-display.md)・[WS113](ws113/ws.md)に記録。GPU UAPIのcompositor直接ioctl禁止、OS module境界、HAL承認/C全文規約は維持。計画のみ、実装/Queue許可は無し。

## GTK4/Qt6 upstream移植と独自実装（2026-10-02）

Event ws114-gtk-qt-port-projections-20261002: user指定の順序は[全文](standards/ws114-gtk-qt-learning.md)。Linux標準GTK4の実測・機能表のユーザーレビュー、採用範囲のXDG-shell/portal改善、zedBSD upstream GTK4/Qt6移植で知見を得てから独自書き下ろしWS097/WS096へ渡す。外部source/license境界とC全文規約は不変。WS034 p029/p030は新WSへ移管、p028/p034/p038は維持。採否未決の機能実装、Qt6詳細実装は選定しない。

## C定数テーブルの先行宣言 / 2026-10-02

ユーザー「必要な関数宣言だけ先に置く例外を認める」。[C全文§2](coding-style.md)に追加: 初期化子が参照する関数の一行forward宣言だけを定数file-scope table前へ置ける。それ以外は変数後の通常宣言block、comment/definition/所有/寿命の規則は維持。WS074 p172のcompile上の矛盾を解消する限定規則。簡約版なし、全文を直接使用。

## WiFi の制御の権限（2026-10-02）

2026-10-02 user（明示の承認）:「WiFiの制御は、networkグループに入っているユーザには許可する、でどうですか？」 → networkd は `network` group の利用者に WiFi の policy の操作（on/off・join・key）を許可する。group 外は従来どおり。秘密の鍵は利用者の store に残す（networkd に鍵を渡さない境界は不変）。[ws005-p019](ws005/ws.md)。

## i386 の build と試験（2026-10-02）

ユーザー「i386はしばらくテストもビルドもしなくていいです。」→ pcat・pc98 などの i386 の build と試験を当面行わない（受け入れの関門から外す）。i386 の sysroot は作り直さない。

## libc の追加（2026-10-02）

ユーザー「alloca(),getc_unlocked()は実装できるなら実装してほしいです。」「<alloca.h> に #define alloca(size) __builtin_alloca(size) を置くだけでいいです。」→ Q1 が `include/libc/alloca.h` と stdio の `getc_unlocked`・`getchar_unlocked`・`putc_unlocked`・`putchar_unlocked`（POSIX、lock は再帰なので locked 版を呼ぶ）を追加。

## 移植で見つかった API の取り込み（2026-10-02）

ユーザー「getc_unlocked()は追加します。このように、GNUソフトウェアの移植で判明した、POSIX標準でないが重要なAPIは、積極的に取り込みます。」→ 外部 package の移植で libc に無いと分かった API（POSIX でない GNU・BSD の広く使われるものを含む）は、package ごとの patch で済ませず、libc に積極的に取り込む。取り込みは ABI 互換と名前空間（WS001 の POSIX の観点）を確かめ、全文規約・build・試験を通す。最初のまとめは [ws034-p058](ws034/phase058/phase.md)。

## 5330 の passthrough で iGPU と AX211 を同時に渡さない（2026-10-03）

ユーザー「iGPUとAX211は、同時にvfioを有効にした実績がゼロです。おそらく同時には有効にできないのです。」→ 今後は iGPU（00:02.0）と AX211（00:14.3）を同じ QEMU に同時に passthrough しない。WiFi の試験は AX211 だけ（std VGA）、GPU の試験は iGPU だけ。記録上は 2026-10-02 の q590（run 2・3）と q599-i03 で同時の passthrough が起動しているが、2026-10-03 00:41 の同時の passthrough の直後に host が hard hang した（BUG-145 の記録）。間欠的に host を止める危険として扱う。

## iGPU の passthrough を使う場面（2026-10-03）

ユーザー「iGPUを使うのはグラフィックドライバ改善とデスクトップ描画パフォーマンス改善のPhaseのみにしてください。恒常的にiGPUをパススルーする必要性を私は感じません。Venusで十分ですし、LLVMpipeで十分です。」→ 5330 の iGPU（00:02.0）の passthrough は i915 の driver の改善の Phase だけで使う。追記（同日 user）:「パフォーマンス改善でも、5330でVenusを使えば実質i915が使えますので、そうしてください。」→ desktop の描画の性能の改善は 5330 の host の i915 を使う QEMU の Venus で行い、iGPU の passthrough は使わない。それ以外（WiFi・desktop の UI・app・GTK/Qt など）の試験は QEMU の Venus か llvmpipe で行う。

## Keiland の OS の境界の改訂予定（2026-10-03）

ユーザーの決定で、desktop の OS の抽象化を libkeiland-backend（compositor が使う、OS ごとに libkeiland-backend-zedbsd・-linux・-freebsd の別の source tree）に集め、compositor の OS の module（seat・logind・evdev・KMS/GPU）も移す。libkeiland は標準 app の UI toolkit と compositor の非標準の機能の wrapper になり、app は OS の抽象化を直接持たない（WiFi・network・音量・電源・PnP・設定は compositor の拡張の protocol を通す）。上の「Keiland の OS の境界」の配置の規則と checker は [WS131](ws131/ws.md) の設計で改訂する（それまでは今の規則のまま）。

## 5330 の AX211 の passthrough の再開（2026-10-04）

user「AX211のpassthruは、きちんとLinuxドライバをblacklistして再起動すれば動作します。その手順が抜けています。ホストを使ってオーケーです。sudo はパスワードレスで使えるので、設定は変えてオーケーです。」→ 5330（10.0.30.3）の host で iwlwifi（AX211 の Linux の driver）を blacklist して再起動し、AX211（00:14.3）を vfio-pci で QEMU に passthrough してよい。host の設定（modprobe.d の blacklist、vfio-pci の起動時の割り当て、再起動）を変えてよい。2026-10-03 の「iGPU と AX211 を同じ QEMU に同時に passthrough しない」は維持（今回は AX211 だけ）。手順は `plan/tools/` か `plan/ws004/tests/` の script の注記に残す。

追記（2026-10-04 user）:「5330のリセットは何回でも対応するので、気兼ねなくパススルーを試してください。また、i915と同時にパススルーに成功した事例はまだないので避けてください。」→ AX211 の passthrough で host が落ちても試験を続けてよい（担当は落ちたら Q1 に知らせ、Q1 がユーザーに 5330 の電源の再投入を頼む）。iGPU（i915）と AX211 の同時の passthrough はしない。

## GPU の driver の scanout の規則（2026-10-04 ユーザー）

ユーザー「GPUドライバはGOPの出力先以外に、scanoutを開始しない.というルールを覚えておいてください。」（WS051 の決定「ドライバは出力先を変更しない。…GOPから引き継ぐときに、GOPの出力先を、ドライバの出力先とする。…GOPがUSB-Cに出力されていない限り、ドライバはUSB-Cに出力しない。」と、hotplug は Keiland が Vulkan の Display の拡張で受けて mirror・拡張・出力 off を決める、の一般化）→ 全ての GPU の driver（i915・WS037 nvrtx・WS141 bcm2711（GOP に当たるのは firmware の framebuffer の出力先）・今後の物）は、boot の時に firmware（GOP など）が出していた出力先（port・pipe）以外に、自分の判断で scanout を始めない。それ以外の出力先は、graphical session の Keiland の明示の指示（libvulkan の Display の拡張の経路）があった時だけ。今の i915 の外部 display の優先の挙動はこの規則に反するので WS051 で直す。
補い（2026-10-04 ユーザー）:「GPUドライバは、GOPの出力先であれば、scanoutできるようにどのインタフェースでも初期化を試みる、もまた真です。HDMIにせよDPにせよeDPにせよ。」→ 逆も規則: GOP の出力先がどの interface（HDMI・DP・eDP・USB-C の DP-alt など）でも、driver はその出力先で scanout できるように初期化を試みる（対応していない interface なら、その旨を log に出して firmware の画面を保つ）。

## 例外（2026-10-05 追加）

| 例外 | 範囲 | 理由・決定 | 期限 |
| --- | --- | --- | --- |
| base の `/usr/libexec/passkey-fido2` と `userland/base/libpasskey`（WS172）が package の OpenSSL の libcrypto を使う（`/sbin/passkey` は libc の crypt だけ、2026-10-05 ユーザーの P1 の決定で範囲を変更） | FIDO2 の暗号（SHA-256・HMAC・AES・P-256）だけ。2026-10-06 夜 ユーザー（クリック「広げる」）で、同じ libpasskey を使う base の道具 `fidoctl`（WS161 p004）も範囲に含める | ユーザー 2026-10-05「passkeyはbaseに起きます。OpenSSLはリリースまでに独自実装に置き換える予定なので、問題ないです。」（master-design-policy §2.1 の例外） | リリースの前（WS172 p005） |
| compositor の game mode の直の scanout（BUG-223、ws122-p005b）: libkeiland-backend-zedbsd に display の直接の口（old の fullscreen mode の GPU_DISPLAY_CLAIM・PRESENT・RELEASE などの ioctl）を置き、compositor はその backend の口を通して client の buffer を出す | app が明示に頼む全画面（game mode）の時だけ。普通の全画面と全ての合成は libvulkan だけのまま。ioctl は backend の zedBSD の tree の中だけで、compositor の source には入れない | 2026-10-06 ユーザー（クリックの回答「B backend に direct の口」、P2 の A/B/C の案から）。「compositor は libvulkan だけを使う」（2026-09-30）の限定の例外 | 無し（設計の変更の時に見直す） |

## 寸法の単位（2026-10-06 ユーザー）

「Pixel単位で話していますが、あとでDPIスケーリングを導入したいので、デフォルトDPIでの論理Pixelだと理解してください。」→ ユーザーと計画が言う px は既定の DPI での論理 px。compositor・libkeiland・app は寸法を論理 px の定数で持ち、将来の DPI scaling の倍率を掛ける所を散らさない。

## 画像の処理は GPU で（2026-10-12 ユーザー）

ユーザー:「PI4のような低消費電力のモバイルSoCでは、画像をCPUで触ると、体験を損ないます。すべてGPUで処理する必要があります。5330のCPUは強力なので、CPUでも問題なかったんだと思います。スマホのゲーム移植ではよくあることです。」→ compositor・libkeiland・desktop の app・GPU の driver（kernel）は、描画の毎 frame の経路で画素を CPU で触らない（縮小・合成・blur・色の変換・tiling の並べ替え（raster→UIF など）・写し）。GPU の command（描画の pass・vkCmdBlitImage・vkCmdCopyImage・TLB copy・TFU など）で行う。CPU で画素を触るのは、file の decode（jail の中）と、起動の時・内容が変わった時の一度だけの用意に限り、その時も結果を GPU の側で使い回す。5330 で速くても Pi 4 で確かめる。既存の CPU の経路は WS141 p013 と以後の Phase で順に GPU へ移す。

補い（2026-10-12 ユーザー）:「GPUでglassをやってほしかったのですが、以前あなたがCPUでやりたいと言ってそういう実装にしたのです。GPUでできるならそれがいいです。」→ glass（backdrop の blur・合成）は GPU だけで行う（画面外の texture に描く → fragment shader で縮小と blur の段 → blur した texture を sample して板を合成）。CPU への書き戻しはしない。Pi 4 では WS141 の案 (A)（GPU の描画の出力を UIF で持つ）がその前提。

## userland/base の program の有無（2026-10-19 ユーザー）

「userland/base/はよほどのことがない限りアーキごとに存在する・しないで分けたくないです。32-bitでNICがないアーキテクチャは、ESP32とかPIC32MXとかでないかぎり、たぶんないのでは。」
- userland/base の program は、arch・platform ごとに「有る・無い」を分けない。新しい port も、userland を build する段では base の全部を build して root に入れる。link が通らない時は runtime・libc の側で直す。
- 例外（NIC の無い microcontroller 級の target など）はユーザーの判断で決める。kernel だけの段（VFS の段まで）の port は userland を build しないので対象外。

<!-- awesome-plan project=zedbsd record=ws035 -->

# WS035: デスクトップ環境とアプリケーションの導入

<!-- awesome-plan-current:start -->
Status: incomplete
Primary Milestone: MG006
Related Milestones: MG001, MG005
Objectives: O1, O2
Parent: [Master](../master.md)
Queue: なし（q465 finished）
Resume point: fg010: p052・p053 cleared（2 つのモードの核、`wl_shm` と cursor）。p054 cleared（acquire fence）。p059 cleared（look and feel の疎通確認: 浮いたタイトルバー、すりガラス、`zwl --glass`）→ p011 → p055 … → p058。元の続き: タスクバー・タイトル描画
<!-- awesome-plan-current:end -->

## 単一目標

zedBSDへ実際のアプリケーションを導入する。そのためのデスクトップ環境（zdesktop）、
音声、フォント描画を作り、最後にChromiumを動かす。**アプリケーションの導入が主目的**である。

導入の過程で見つかったシステムの設計・実装の誤りは、原因側で直してブラッシュアップする。
新しく実装したばかりのGPUには直すべき点が出てくるはずだが、GPUの安定化は主目的ではなく、
システムのブラッシュアップは副次的な成果である（2026-09-23ユーザー明確化）。

到達点: zdesktop上でWaylandクライアントとX11クライアントを表示・操作できる。
ウィンドウ管理・装飾・タスクバー（WiFi・音量）・タイル一覧が動く。audiod経由で音が出る。
Chromiumが起動してページを表示する。導入中に見つかったシステムの問題は、修正済み、
担当WSへ移管済み、またはBugに登録済みである。

2026-09-23 ユーザー指示「もう1つwsを追加します。これはデスクトップ環境の構築と、
それを通じたGPUドライバの安定化を目標とします。」
同日の明確化「WS035は、実際のアプリケーションを導入することが本当の目的です。…GPUの安定化は
主目的ではないです。アプリの実装が主目的で、システムの安定化は副次的なものです。」

## 範囲（ユーザー指定）

1. **refactor（最初に行う）**
   - `include/drivers/` を `src/drivers/` と同じ階層に揃える。
   - `libc/` を `src/libc/` へ移し、`include/libc/*` を `include/` へ移す。
   - `include/kern/*` を `include/kern/` へ移す。rpi4とsun4uのboot定義
     （現在の `include/kern/boot.h`、`include/kern/boot.h`）も同じ場所へ移す。
2. **`/dev/graphics` の移行**
   - `/dev/graphics` はレガシーな2Dフレームバッファと、レトロPCのVRAMを扱う。
   - GPUドライバがロードされたら、`/dev/graphics` に通知する。
   - 通知を受けた `/dev/graphics` は、GPUのscanoutバッファを使うように切り替える。
   - これにより、ファームウェアのフレームバッファからGPU初期化後の画面へ継ぎ目なく切り替える。
3. **audioドライバ**
   - FreeBSDのOSS（`/dev/dsp0`）とおおむね同じAPIにする（完全互換は求めない）。
   - まずaudioドライバのフレームワークを作る。
   - 次にQEMUのIntel HDAエミュレーションを使ってhdaドライバを開発する。
   - 最後にPCIパススルーで実機のHDAを使い、再生できるようにする。
4. **audiod**: `/sbin/audiod` を作り、PulseAudioのごく基本的な再生機能を提供する。
   Chromiumが動くように互換libpulseを作る。最初は再生と録音だけで、転送等は不要（2026-09-23追加指示）。
5. **libtruetype**
   - Unicodeのttfフォントだけに対応する、ごく単純なフォントレンダリングライブラリ。
   - FreeTypeのライセンスが特殊なため、Zlibライセンスで独自に実装する。
   - zdesktopと関連アプリから使う。
6. **zdesktop**
   - `zwl` を `/bin/zdesktop` へ改名する。
   - 簡単なウィンドウ管理を実装する。
   - KWinのように、コンポジタ側でウィンドウタイトルとフレームを描く。
   - タイトルのフォントは、後でFreeType等を使う予定。当面は起動時に `/dev/graphics` から
     ASCII文字のglyphを全部取得して使う（既存の `KERN_GRAPHICS_GET_GLYPH` を使う）。
   - X11プロトコルの基本部分だけを実装し、X11サーバとしても振る舞う。既存Xzedのコードを
     コピーして使ってよい（Xzedは自作のZlibコードである）。
   - コンポジタに簡単なタスクバーを実装する。WiFiと音量の状態表示と操作を持つ。
   - コンポジタに簡単なウィンドウ一覧のタイル表示を実装する（Windows+Tab）。
7. **Chromium（最後）**: `userland/packages/network/chromium` に追加する。
   全ての基盤が揃わないと着手できないため、最後に行う。

## 調査で分かった現状（2026-09-23）

- `include/drivers/` はフラット（`pci-xhci.h`、`usb-storage.h`、`gpu.h` 等）で、
  `graphics/` と `hid/` だけがサブディレクトリになっている。`src/drivers/` は
  `usb/ pci/ gpu/ wifi/ ethernet/ fs/ disklabel/ isa/ generic/ platform/<機種>/` に分かれている。
- libcは2箇所ある。`libc/`（`libc.mk`、`include/`、`ctype.c` 等）と
  `userland/base/libc/`（`posix.c` 等）である。今回の移動対象は前者。
  `include/libc/` には標準ヘッダのほか、`X11/`、`vulkan/`、`wayland*`、`linux/`、`machine/` がある。
- `include/` には `boot/ drivers/ hal/ kern/ uapi/` がある。`include/kern/boot.h` という
  ファイルが既にあり、新しいディレクトリ `include/kern/` と名前が並ぶ。
- boot関係のヘッダは `bootloader/include/`、`bootloader/sparcv9/handoff.h`、
  `src/hal/x86/boot-parameters.h` などにも散っている。今回の移動対象はユーザー指定の範囲に限る。
- `/dev/graphics` は機種ごとの実装（`src/drivers/platform/pcat/graphics/pcat-graphics.c`、
  `.../pc98/graphics/pc98-graphics.c`）だけで、共通層もGPU連携も無い。UAPIは
  `include/uapi/graphics.h`（ioctl 1–11。glyph取得の10を含む）。
- GPUは、共通層 `src/drivers/gpu/gpu.c` と、`venus/`・`i915/`（WS031がdisplay/KMSを実装中）。
- audio関連のコードは無い（ヒットしたのはi915 displayのaudio power制御だけ）。
- `zwl` は `userland/base/zwl/`。Xzedは `userland/X11/xzed/`（全ファイル `SPDX: Zlib`）。
  X11側には既存の `zwm`・`zshell`・`zterm` がある。
- WiFiは `/sbin/wifi` と `include/uapi/wlan.h`。networkdの状態をデスクトップへpushする経路は無い（後述）。

## 実行開始条件（2026-09-23ユーザー指示）

このWSの実行は、別エージェントが作業中のWS031とWS032が終わるまで始めない。
計画はそれ以前に進めてよい。両WSが終わったら、このWSのrefactor（p002–p004）を最優先で行い、
その後にこのWSの残りと WS034 を進める。

2026-09-23 ユーザー連絡: WS031のGPU作業は一区切りとして停止した。WS031は未完了のまま
（p001〜p014 cleared、p015〜p018 planning・後回し、統合回帰は未実行）で、成果と残課題Phaseは
`plan/ws031/ws.md` に記載済み。したがってWS031側の待ちは解消し、残る待ちはWS032（q315 active）である。

2026-09-23: WS032はcompleted（q315の10件すべてcleared）。upstreamをマージし、待ちは解消した。
以後はこのエージェントが唯一の実行者である（Master「実行体制とQueue運用方針」）。

## i915の試験環境: QEMU＋VFIO（2026-09-23ユーザー決定）

GPUに関わる確認はi915で行う。エージェントが自律ループを回せるように、実機のIGDをQEMUへ
VFIOでパススルーして試験する。正本はWS031の引き継ぎ（`plan/ws031/ws.md`「引き継ぎ
（2026-09-23、次のエージェントへ）」）で、このWSはそれをそのまま使う。以下はその要約である。

**ホスト**

| 役割 | 場所 |
| --- | --- |
| 作業・build | centris（`ssh awe@10.0.10.2`）、tree `~/zedBSD-gpu`、passwordless sudo |
| 試験機（KVM host、LCDも試験対象） | Latitude 5330（hostname `chaos`、`awe@10.0.10.25`）、IGD `8086:46a8`（subsystem `1028:0b02`） |
| LCD撮影 | Windows workstationの `capture_lcd.ps1`（写しは `plan/ws031/tests/host/capture_lcd.ps1`） |

**iGPUの割当て**: 起動時の既定はvfio-pci（`/etc/modprobe.d/vfio-igd.conf`、
`/etc/modules-load.d/vfio-igd.conf`）。実行時は `~/bigbang/igpu-mode.sh host|vfio|show`
（写しは `plan/ws031/tests/host/igpu-mode.sh`）で切り替える。QEMUが動いている間は切替えを拒否する。
`host` はVenus（host i915上のvirtio-gpu）用である。

**QEMUのコマンドライン**（5330の `~/bigbang/run-parity-vk.sh`、写しは
`plan/ws031/tests/host/run-parity-vk.sh`。直接は起動せず、centrisから `vkloop-hw.sh` を使う）:

```
sudo -n timeout 360 qemu-system-x86_64 \
  -machine q35,accel=kvm,memory-backend=mem -cpu host,host-phys-bits-limit=39 -m 4096 -smp 4 \
  -object memory-backend-memfd,id=mem,size=4G,share=on \
  -device vfio-pci,host=0000:00:02.0,x-igd-opregion=on,rombar=0 \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd \
  -drive if=pflash,format=raw,file=/home/awe/bigbang/vg-parity.fd \
  -drive file=/home/awe/bigbang/guest-parity.img,format=raw,if=none,id=zd0 -device nvme,drive=zd0,serial=zedbsd0 \
  -vga std -display none -monitor none -serial file:/home/awe/bigbang/run-parity-serial.log -nic none \
  -debugcon file:/home/awe/bigbang/run-parity.log -no-reboot
```

WS031で実測した失敗に基づく要点:

- `host-phys-bits-limit=39` は必須。IOMMUのMGAWが39で、無いとVFIOのDMA mapが-22になる（E-23/E-42/E-43）。
- `memory-backend-memfd,share=on` と `memory-backend=mem` を組にする。`x-igd-opregion=on,rombar=0`。
- `-vga std` はUEFIローダのGOP用に要る（WS029 boot-002）。
- guestからはOpRegionが見えない（ASLS=0）。試験buildは `I915_TEST_VBT=y` で
  `vendor/intel-vbt/` の5330のVBTを取り込む（`vkloop-hw.sh` が常に指定する）。
- `QMP=1` でQMP socketと `qemu-xhci`＋`usb-tablet`＋`usb-kbd` が付く（画面取得と入力用）。
  PS/2 keyboardのキーはzwlに届かない（原因は未調査で、WS031 p015の項目）。
- 実機を使うrunは `flock /tmp/i915-hw.lock ...` で1つずつ行う。1 runはbuild・転送・起動込みで3〜5分。
  Venusも同じ5330で動くので、iGPUの切替えはQEMUが止まっている時だけ行う。
- LLVM toolchainはzedbsd6（commit "lldb works" 以降）。

**画面の照合**: `CAPTURE=<vkdemo|wayland|mview> vkloop-hw.sh ...` でguest RAMから画像を取り、
`/tmp/capture-last/`（`result.json`、PPM、`sheet.png`）に出す。このため、自律ループでも画像で
照合できる。LCDの実表示はWindows側のカメラで撮る（人が起動する）。

**ファームウェアFBからの継ぎ目のない切替え（p005）**: VFIO下では、GOP FBは `-vga std` 側にあり、
IGDのパネルとは別の出力になる。QEMUで確かめられるのは、通知・切替えの手順と、画面内容の
引継ぎ（バッファの一致）までである。同じパネル上で継ぎ目が無いことは、ベアメタル起動でしか
確かめられない。

HDA（p008）も同じ5330でVFIOを使う。PCHのHDAのIOMMU groupと、IGDとの同時パススルーの可否を
p001で確かめる。

## 前提・依存・衝突

- **他エージェントとの衝突（最重要）**: p002–p004の一括移動は、`~/zedBSD/` で進行中の
  WS031（`src/drivers/gpu/i915/`）とWS032（libc・`platform/amd64/`）の未コミット作業に
  ぶつかる。移動は両WSの完了後に行う（上記の実行開始条件）。
- **GPU**: 主対象はi915（QEMU＋VFIO、上記）。WS031の成果（display/KMS、native Vulkan、
  zwlのseat/pointer/keyboard、mview）の上に作る。WS031の残課題Phase（p015 準正常系・異常系と小修正、
  p016 executor、p017 compiler、p018 性能の構造改善）はWS031に残っている。このWSのp015で見つかった
  GPUの問題がそれらの範囲に入る場合は、WS031で直す（下記の決定事項5）。
  `src/drivers/gpu/i915-old/` は専門家レビュー用の参照なので触らない。
  virtio-gpu（Venus＋host Lavapipe）は、GPUに依存しない部分を素早く試す補助経路として使ってよい。
- **WiFi表示**: networkdに状態のpush通知は無い（下記の調査）。このWSのp018で実装する。
  WS005 p016（同じ目的のplanning Phase）はこのWSへ移管し、canceledとした。
- **Chromium**: WS032のclang/libc++（p005/p008）、WS034の基盤（libc是正）、本WSの
  audiod・libtruetype・zdesktop（Wayland）・GPUに依存する。
- **HDA実機**: WS029と同じVFIOループを想定する（Latitude 5330）。ただしPCHのHDA
  （`00:1f.3`）は、LPC/SMBus等と同じIOMMU groupに入っている可能性がある。単独で
  パススルーできるかをp001で確かめる。

## 制約（Guardrail）

- HAL（`include/hal/hal.h`、`src/hal/`）の変更は、具体的な差分ごとにユーザーの事前承認を得る。
  refactorでHAL配下のinclude文が変わる場合も同じ扱いにする（機械的な置換だけの差分として提示する）。
- UAPIの追加（`/dev/dsp`、graphics通知、audiod protocol等）は、該当Phaseの設計として提示・合意してから実装する。
- RTL8822Bの `.inc` は分けたまま保つ。`drv_` のglobal symbol方針を維持する。
- aggregate `make check` は使わない。git add/commit/pushはユーザーが行う。
- 全文規約 [coding-style.md](../coding-style.md) を適用する。Chromiumへのパッチはupstreamの書式に合わせる。

## Phase一覧

近い順に具体化し、遠い項目は目的と主要リスクだけを書く。見積はQueue作成時に確定する。
2026-09-23に、1 Queueのスロットで終わる大きさへ分解した（分割した元のPhaseのIDは、範囲を縮めて残す）。
設計Phase（p020〜p022）は、デバイスドライバには設計Phaseを入れる方針による。Opus 5 Highで自動実行し、
敵対的レビューも自動で行う。

| Combined ID | Phase | Status | 依存 | 主なファイル範囲 |
| --- | --- | --- | --- | --- |
| [ws035-p001](phase001/phase.md) | 設計固め: refactorの移動対応表（ファイル単位）と影響範囲、VFIOホスト状態の再確認、HDAのIOMMU group | cleared（q316-i01） | — | 文書 |
| [ws035-p002](phase002/phase.md) | refactor: `include/drivers/` を `src/drivers/` の階層に揃える。PCIドライバの公開ヘッダは `include/drivers/pci/pci-<名前>.h`（`pci-i915.h`、`pci-venus.h`）とし、登録関数を `drv_pci_i915_driver_register`・`drv_pci_venus_driver_register` へ改名する | cleared（q317-i01） | p001 | include、全driver、Makefile |
| [ws035-p033](phase033/phase.md) | 設計: kernelからlibcを切り離す（kernel用Cランタイム、型と定数の出どころ、compilerが生成する `memcpy` 等の扱い、検査）。敵対的レビュー付き | cleared（q317-i02） | p001 | 文書 |
| [ws035-p034](phase034/phase.md) | kernel用Cランタイム（kcrt）: `include/kern/kcrt.h` と `src/kern/kcrt.c` に標準C APIの代替 `kern_*()` を実装し、heap allocatorの実装を `src/kern/heap.c` へ移し、vmunixへのlibcのlinkをやめる | cleared（q318-i01） | p002, p033 | src/kern、include/kern、platformのvmunix.mk |
| [ws035-p035](phase035/phase.md) | kernelのinclude整理: libcのヘッダ（`stdio.h`、`string.h`、`sys/*.h` 等）をkernel・driver・HALから除き、`include/uapi/`・`include/kern/`・compilerのfreestandingヘッダだけにする。検査で0件を保証 | cleared（q318-i02） | p034 | src/kern、src/drivers、src/hal（include行）、vmunix.mk |
| [ws035-p003](phase003/phase.md) | refactor: `libc/` のsourceを `src/libc/` へ移す。`src/crt/` を `src/libc/crt/` へ移し、`crt0.S` を `crt0-i386.S` へ改名（2026-09-23ユーザー指示） | cleared（q319-i01） | p035 | libc、Makefile、platform |
| [ws035-p023](phase023/phase.md) | refactor: `include/libc/*` を `include/libc/` へ移す（2026-09-23ユーザー決定。`include/libc/linux/` は同日に削除済みで対象外）。sysroot・rootfsへは `/usr/include/` 直下にコピーする。Vulkanのヘッダは `include/vulkan/` | cleared（q320-i01） | p003 | include、全体のinclude経路 |
| [ws035-p036](phase036/phase.md) | refactor: softfloatを `src/libc/` の直下へ（`zed-` 接頭辞の除去、sparcv9ディレクトリの廃止）。2026-09-23ユーザー指示 | cleared（q321-i01） | p023 | `src/libc`、platformのmk |
| [ws035-p004](phase004/phase.md) | refactor: `include/kern/*` を `include/kern/` へ移す。`include/kern/boot.h` → `include/kern/boot.h`、`include/kern/boot.h` → `include/kern/boot.h`、`include/kern/boot.h` → `include/kern/boot.h`、`include/kern/boot.h` も `include/kern/` へ（2026-09-23ユーザー決定）。amd64以外はbuildせず編集だけ | cleared（q322-i01） | p023 | include/boot、include/kern、bootloader、HAL（include行） |
| [ws035-p020](phase020/phase.md) | 設計: `/dev/graphics` の共通層とGPU scanoutへの引き継ぎ（kernel内部API、通知、戻し） | cleared（q323-i01） | p001 | 文書 |
| ws035-p005 | `/dev/graphics` の共通層: ファームウェアFB・機種VRAMをbackend化（pcat、pc98を移行） | planning | p004, p020 | graphics、platform |
| ws035-p024 | `/dev/graphics` のGPU scanoutへの切替えと戻し（i915、補助にvirtio-gpu） | planning | p005 | graphics、gpu core、i915 display |
| [ws035-p011](phase011/phase.md) | zdesktop: `zwl` を `/bin/zdesktop` へ改名、基本のウィンドウ管理（focus、移動、リサイズ、z-order、最小化・最大化） | **uncleared**（q323-i04。合成の仕組みが無いことが判明。設計Phaseが先に要る） | p004、p051（合成の設計）、**p052〜p054（承認後）** | `userland/base/zwl` |
| ws035-p025 | zdesktop: コンポジタでのタイトル・フレーム描画（`/dev/graphics` のASCII glyph） | planning | p011 | zdesktop |
| [ws035-p018](phase018/phase.md) | networkdの状態push通知（購読）（WS005-p016から移管） | cleared（q325-i01。q323-i07 は uncleared） | p004 | networkd、net |
| ws035-p012 | zdesktop: X11サーバ機能（Xzedから移植） | planning | p011 | zdesktop、X11 |
| ws035-p013 | zdesktop: タスクバーとWiFiの表示・操作。**WiFi の状態は libzdesktop 経由で取る**（p042、networkd と直接話さない） | planning | p025, p018, p042 | zdesktop、libzdesktop |
| ws035-p014 | zdesktop: ウィンドウ一覧のタイル表示（Windows+Tab） | planning | p011 | zdesktop |
| [ws035-p021](phase021/phase.md) | 設計: audioフレームワークと `/dev/dsp`（OSS互換寄りのAPI、driver ops、DMAリング、録音） | cleared（q323-i05） | p001 | 文書 |
| [ws035-p022](phase022/phase.md) | 設計: hdaドライバ（codec列挙、stream DMA、再生・録音、QEMUとVFIO実機） | cleared（q341-i01） | p021 | 文書（[hda-design.md](hda-design.md)） |
| [ws035-p006](phase006/phase.md) | audioフレームワークと `/dev/dsp`・mixer | cleared（q340-i01） | p004, p021 | `src/drivers/audio`、`include/drivers/audio`、`include/uapi/audio.h` |
| [ws035-p007](phase007/phase.md) | hdaドライバ（QEMU intel-hda、再生・録音） | cleared（q342-i01） | p006, p022 | `src/drivers/pci/pci-hda.c` |
| [ws035-p009](phase009/phase.md) | `/sbin/audiod`（ミックス、録音の配布。interface は p050 の設計に従う） | cleared（q364-i01。QEMU で bit 一致・mix・音量・rate 変換・録音・SIGBUS 回復・device 無し） | p006, p049, p050 | audiod（新規） |
| ws035-p019 | 互換libpulse（再生・録音、遅延・時刻情報、一時停止・flush、音量・ミュート） | planning | p009 | libpulse（新規） |
| ws035-p026 | zdesktop: タスクバーの音量表示・操作。**音量も libzdesktop 経由**（p042） | planning | p013, p019, p042 | zdesktop、libzdesktop |
| ws035-p008 | hdaドライバの実機確認（人間が行う。エージェントは確認用のimageと手順を用意する。HDA は passthrough できないので、ユーザーが後で USB boot のベアメタルで試す。2026-09-24） | planning | p007 | hda |
| [ws035-p010](phase010/phase.md) | libtruetype（`cmap`・`glyf`・`hmtx`、anti-aliasの描画、`/lib/libtruetype.so`） | cleared（q323-i06） | p004 | libtruetype（新規） |
| [ws035-p040](phase040/phase.md) | 互換libz（`userland/base/libz-compat`、`/lib/libz-compat.so`）: deflate/inflate を素直に実装する。zlib の全機能は要らない。最適化より読みやすさ。baseのプログラムはこれに依存する | planning | p004。**zdesktop が要るときに入れる** | `userland/base/libz-compat`、`include/libc/compat/zlib.h` |
| [ws035-p041](phase041/phase.md) | 互換libpng（`userland/base/libpng-compat`、`/lib/libpng-compat.so`）: decode と encode。encode は filter にこだわらない。decode は試験用のRGBA32 PNGが読める正常系まで | planning | p040。**zdesktop が要るときに入れる** | `userland/base/libpng-compat`、`include/libc/compat/png.h` |
| [ws035-p042](phase042/phase.md) | libzdesktop（`userland/base/libzdesktop`、`/lib/libzdesktop.so`）: **Vulkan 以外の OS 依存をここに閉じ込める**。zdesktop は networkd・audiod などと直接話さず、このライブラリを通す。**まずは空の枠だけ** | cleared（q325-i02） | p004 | `userland/base/libzdesktop`、`include/libc/zdesktop.h` |
| ws035-p027 | zdesktop: タイトル等の文字描画をlibtruetypeへ移す | planning | p010, p025 | zdesktop |
| ws035-p028 | zdesktop: toolkit（GTK・Qt）が要るWaylandの対応範囲（xdg-shellの残り、keymap、clipboard、subsurface、wl_output、cursor、xdg-decoration） | planning | p025 | zdesktop、libwayland |
| ws035-p029 | Chromium: 依存とbuild環境（gn・ninja、NSS等の依存、クロスbuildの設定） | planning | p019, p028, WS034の依存ライブラリ | packages/network/chromium |
| ws035-p030 | Chromium: 最初のbuild（content_shell、headless） | planning | p029 | 同上 |
| ws035-p031 | Chromium: Ozone Waylandの表示と入力 | planning | p030 | 同上 |
| ws035-p032 | Chromium: 音声（libpulse）・フォント・ネットワークの統合 | planning | p031 | 同上 |
| ws035-p016 | Chromium: ブラウザとしての受入（起動、ページ表示、操作） | planning | p032 | 同上 |
| [ws035-p037](phase037/phase.md) | シリアルコンソールの受信: COM1 から届いた文字を console の入力にし、キー入力なしでゲストを操作できるようにする | cleared（q324-i01） | p004 | `src/drivers/platform/pcat/serial-mirror.c`、`src/kern/tty.c`、config |
| [ws035-p043](phase043/phase.md) | 端末の line/character mode の切替で先行入力が失われる（`TCSETS` が溜まった入力を移していなかった）。シリアル操作で1文字抜けて shell が終わる現象の原因 | cleared（q332-i01） | p037 | `src/kern/tty.c` |
| [ws035-p044](phase044/phase.md) | UHCI の短い packet で転送を終える（SPD）。64 byte の倍数の frame を取りこぼす（p039 で発見） | cleared（q346-i01。UHCI の ECM で ping 0/3→5/5） | p039 | `src/drivers/pci/pci-uhci.c` |
| [ws035-p045](phase045/phase.md) | USB CDC-ECM の送信の列と受信の取りこぼし（送信 URB 1 本で使用中は捨てる。ws034-p046 で発見） | cleared（q345-i01。TX dropped 3697→0。NCM は実機が要るので残した） | p039 | `src/drivers/usb/usb-cdc-ecm.c` |
| [ws035-p046](phase046/phase.md) | USB hub driver（hub の先の device の列挙・hot-plug・切り離し。QEMU の UHCI で 3 つ目の device が見えない、実機の hub・dock も同じ。2026-09-24 ユーザー指示） | cleared（q365-i01。xHCI・UHCI の hub の下の列挙・storage の読み・hot-plug・2 段・子から先の切り離し。USB 3 hub と EHCI の TT は後回し） | — | `src/drivers/usb/usb-hub.c`（新規）、USB core |
| [ws035-p047](phase047/phase.md) | networkd: `net dhcp` の直後に DHCP をやり直し、その数秒の `connect()` が失敗する（実機 NCM で観察。2026-09-24 ユーザー指示） | cleared（q357-i01。kernel の到着の事象 `RTM_IFINFO_ARRIVAL`、手の設定を方針に記録。差し直した adapter が設定されない不具合も直した） | p038 | `userland/base/networkd` |
| [ws035-p048](phase048/phase.md) | `CONFIG_DRIVER_PCI_HDA` の既定を ON にする（2026-09-24 ユーザー決定） | cleared（q350-i01。amd64 だけ既定 ON） | p007 | Makefile、`config/drivers/pci.drivers` |
| [ws035-p049](phase049/phase.md) | `/dev/dspN` の OSS の mmap interface（ring を mmap し、hardware の位置を問い合わせる。今はコピーありでよい。ゼロコピーの DMA buffer は後で別に目指す） | cleared（q356-i01。shadow ring を割込みごとに copy。QEMU で mmap 再生 96000 frame が bit 一致） | p006, p050 | audio framework、`include/uapi/audio.h` |
| [ws035-p050](phase050/phase.md) | 設計: audiod の unix socket interface（共有メモリで受け渡し、streaming の interface は持たない。`shm_open` 直後に `shm_unlink` した匿名の fd を SCM_RIGHTS で渡す。libpulse の `pa_stream_write()` はその buffer に書く。`/dev/dsp` が mmap 対応なら audiod はそれを使い、非対応なら write する） | cleared（q355-i01。[audiod-design.md](audiod-design.md)） | p006 | 文書 |
| ws035-p056 | `/dev/dspN` の mmap のゼロコピー: DMA の ring そのものを `vm_device` で user に mmap する（p049 のコピーありの後。interface は同じ） | planning | p049 | audio framework |
| [ws035-p051](phase051/phase.md) | 設計: デスクトップの合成（compositing）。ユーザーが微調整して承認する（2026-09-24 ユーザー指示） | cleared（q354-i01。[compositing-design.md](compositing-design.md) を提出。同日のレビュー 2 回（2 つのモード、swapchain、`wl_shm` は補助として持ち GPU の経路を最適化）を反映、**承認待ち**） | p004 | 文書 |
| [ws035-p052](phase052/phase.md) | （2026-09-25 承認）2 つのモードの核: 全画面モード（swapchain を破棄してそのまま scanout）とウィンドウモード（背景→窓を下から順に Vulkan で合成、`VK_KHR_display` の swapchain）の切替、quad の pipeline、GPU 画像の OPAQUE_FD import（buffer ごとに 1 回）、sampling の fence と release、効果を載せる枠 | cleared（q457-i01。ウィンドウモードの Vulkan の合成と全画面モードの直接 scanout、切替。画面の読み取りで窓 2 つ・全画面・戻りが一致、import は buffer ごとに 1 回。前の試行 sq001-i01 は uncleared） | p051 | `userland/base/zwl` |
| [ws035-p053](phase053/phase.md) | （2026-09-25 承認）`wl_shm`（補助の経路、damage の範囲の CPU の copy）と cursor（zdesktop の cursor 画像、ウィンドウモードで合成、`set_cursor` の shm・GPU の surface） | cleared（q458-i01。`wl_shm`（damage の行の copy、即 release）、矢印と client の cursor、frame の予定。画面の読み取り 4 段と GPU の窓の frame の測定） | p052 | 同上 |
| [ws035-p054](phase054/phase.md) | （2026-09-25 承認）acquire fence の request（`zed_gpu_buffer_v1` の拡張） | cleared（q459-i01、2026-09-26。受け入れ 3 はユーザーの判断で読み替え: present は元から完了を待たず、速くならないことを測って記録） | p052 | 同上、libwayland の WSI |
| [ws035-p059](phase059/phase.md) | （2026-09-26 ユーザー指示）look and feel の疎通確認: 本体から離れて浮いたタイトルバー（実装）、すりガラスの shader、上部のバー（ハリボテ）、壁紙、角丸と影（`zwl --glass`） | cleared（q460-i01、2026-09-26。drag・最大化・閉じる・hover を QMP で確認。背後の窓のぼかしは p057、最小化とバーの操作は p011・p013） | p052〜p054 | 同上 |
| [ws035-p060](phase060/phase.md) | （2026-09-26 ユーザー指示）Vulkan で描く client（mview）を glass の窓で: mview に `--windowed`・`--size` | cleared（q461-i01、2026-09-26。model が窓に描かれ、drag で回り、最大化で描き直す。Lavapipe で 8.8 fps） | p059 | `userland/base/mview` |
| [ws035-p061](phase061/phase.md) | （2026-09-26 ユーザー指示）壁紙の画像（`--wallpaper`、ユーザーの絵を抽象化、git 外）と、すりガラスで透ける窓（`--window-opacity`） | cleared（q462-i01、2026-09-26。絵の壁紙、透ける窓 10 %・60 % の画面。壁紙は git 外） | p059 | `userland/base/zwl` |
| [ws035-p062](phase062/phase.md) | （2026-09-26 ユーザー指示）タイトルバーのドッキング: 最大化で題名が上部のバーへ吸着（ダブルクリック・上への drag）、バーから下への drag で解除、遷移、仮想デスクトップのハリボテ | cleared（q463-i01、2026-09-26。ダブルクリック・上への drag・button でドッキング、バーの題名のダブルクリック・⧉・下への pull で解除、220 ms の遷移） | p059 | `userland/base/zwl` |
| [ws035-p063](phase063/phase.md) | （2026-09-26 ユーザー設計 [wiseman-design.md](wiseman-design.md)）Wiseview（ウィンドウ一覧）の疎通: 下端からの drag、グリッドのタイル、選択・閉じる | cleared（q464-i01、2026-09-26。下端からの drag で開き、グリッド・札・選択・閉じる。4 窓で確認） | p062 | `userland/base/zwl` |
| ws035-p064 | （同）物理的に追従するドッキングの解除（引くと縮み、閾値で snap、届かなければバネで戻る） | planned | p062 | 同上 |
| ws035-p065 | （同）仮想デスクトップの実体と左右の端のスワイプ（画面が追従して横へ） | planned | p063 | 同上 |
| [ws035-p066](phase066/phase.md) | （2026-09-26 ユーザー指示）zdesktop（Wiseman Mode）を Intel GPU（5330 の i915 ネイティブ実行器、VFIO）で疎通 | cleared（q465-i01、2026-09-26。i915 実機の GPU で Wiseman Mode・ドッキング・Wiseview を capture で確認（wl_shm の窓）。GPU の client の窓は F-022、実行器の不足は F-023） | p063、WS031 | `userland/base/zwl`、`plan/ws031/tests` |
| ws035-p055 | （2026-09-25 承認）damage（buffer age と scissor） | planned（sq001） | p011 | 同上 |
| ws035-p057 | （2026-09-25 承認）効果: すりガラス（背後のぼかし）と影 | planned（sq001） | p055 | 同上 |
| ws035-p058 | zdesktop（secondary queue で変えた全 source）の規約の全文との照合と回帰（sq001 の締め） | planned（sq001） | sq001 の他の Phase | sq001 で変えた source |
| [ws035-p038](phase038/phase.md) | SSHハーネス: ゲストへ SSH で入り、コマンド実行・ファイル転送・ゲスト内 lldb・QEMU gdbstub でのデバッグを行う道具を仕上げる（`plan/tools/guest/` は着手済みで未完成） | cleared（q347-i01。networkd が USB の interface を UP にしないため設定されなかった → RAISE を追加） | p037、USB CDC-ECM が上がること（p039） | `plan/tools/guest/` |
| [ws035-p039](phase039/phase.md) | USB CDC-ECM の実機確認: 実績のないまま入っている ECM driver が QEMU で実際に link し address を得るかを、シリアルコンソールで観察しながら確かめる。**ECM は USB 2.0 の device なので EHCI と xHCI の両方で確かめる**（ws004-p019 の記録との食い違いの照合を含む）。USB storage と同居したときの挙動も切り分ける | cleared（q343-i01。xHCI の IMAN の競合を直した。UHCI は p044、TCP は ws034-p046 へ） | p037 | `src/drivers/usb/usb-cdc-ecm.c`、試験 |
| ws035-p015 | システム是正の受け皿（アプリ導入で見つかったGPU・カーネル・libc等の問題。GPU基盤の問題はWS031へ） | planning | p004 | 随時 |
| ws035-p017 | 全文規約確認・回帰・制限整理（必須の最終確認） | planning | 全Phase | 全体 |

p015は一度では閉じない。後続Phaseで見つかった問題はp015へ戻して直す。ただし、Intel GPUドライバの基盤
（WS031の範囲: executor、compiler、同期・性能の構造、準正常系・異常系）に属する問題はWS031へ移して直す。

### 依存関係の解析（2026-09-23）

- **refactorは直列**: p002 → p034 → p035 → p003 → p023 → p004（p033の設計はp002と並行できる）。kernelからlibcを
  切り離してから（p034・p035）libcを動かす（p003・p023）ので、移動後の `include/` にlibcのヘッダがあっても、
  kernelは読まないことを検査で保証できる。どれもMakefile、include文、sysroot生成に触れるので、
  同じ作業ツリーで並行すると衝突する。p001（移動の対応表）が前提。
- **全WSのコードPhaseはrefactorの後**: WS031のp022以降、WS034のコードPhase、このWSのp005以降は、
  refactor（p004まで）の完了を前提にする。
- **文書だけのPhaseはrefactorと並行できる**: p001、p020〜p022、WS034-p001は文書だけを作るので、
  refactorのPhaseと同じQueueに入れられる。
- **Queue内の並行条件**: 同じQueueの2つのPhaseは、「主なファイル範囲」が重ならない組み合わせにする。
  重なる場合は直列にする。
- **zwlを触るWS031のPhase**（p027 present mode、p028 入力）は、zdesktopへの改名（p011）より前に済ませると衝突しない。

## Phaseの要点とリスク

- **p001**: 移動前後の対応表（ファイル単位）を作り、include文・Makefile・sysroot生成・
  noct宣言生成・試験のパス参照の影響範囲を洗い出す。受入環境（QEMU amd64、GPUとHDAはVFIO）、
  HDAの実機host facts（IOMMU group、host側の
  snd_hda_intelのunbind/restore手順）を確定する。
- **p002**: 例: `include/drivers/pci-xhci.h` → `include/drivers/usb/xhci/...`、`gpu.h` →
  `include/drivers/gpu/...`。規則は `src/drivers/` のディレクトリと同じにする。機械的な
  移動とinclude置換に留め、中身は変えない。全platformのbuildで確かめる。
- **p003**: 最大のリスク。`include/` へlibcの公開ヘッダ（`stdio.h`、`sys/` 等）が入ると、
  kernelのbuildで `-Iinclude` からuserland用ヘッダを誤って拾う恐れがある。kernel/HALの
  include経路にlibcヘッダが入らないことを、build設定か検査で保証する。sysrootへの
  install先は変えない。`userland/base/libc/` の扱い（移動しない想定）もp001で確定する。
- **p004**: `include/kern/boot.h` と `include/kern/` の関係（そのまま並べるか、
  中へ移すか）を決める。bootloader側のコピーや参照も追う。
- **p005**: 共通の `/dev/graphics` 層を作り、ファームウェアFB（GOP等）と機種VRAMを
  backendにする。GPUドライバがscanoutを確立したら、kernel内部APIで `/dev/graphics` へ
  通知し、画面内容を引き継いだうえでGPU scanoutへ切り替える。GPU側が故障・解放したら
  元に戻す。console（`kern_text_register`）との関係も整理する。QEMU（virtio-gpu）で
  切り替えの前後を画像で照合する。i915はWS031の成果が揃ってから行う。
- **p006**: `/dev/dsp0` を中心に、`SNDCTL_DSP_*`（形式・チャネル・レート・fragment・
  GETOSPACE・SYNC等）と `/dev/mixer` 相当の音量を、必要な範囲で提供する。driver ops構造体、
  DMAリング、underrun処理を作る。
- **p007**: QEMUの `-device intel-hda -device hda-output`（またはhda-duplex）で、
  codec列挙、stream DMA（BDL）、再生を作る。QEMU側でwav captureして独立に照合する。
- **p008**: WS029と同様のVFIOループで実機HDAを使う。host側の音声停止と復旧を、
  ユーザーの許可範囲で行う。
- **p009**: `/sbin/audiod` が `/dev/dsp` を占有し、複数クライアントの再生をミックスし、
  録音を配る。クライアントとの通信は独自protocolでよい（互換性はp019のlibpulseで取る）。
  最初は再生と録音だけにする。転送（network sink等）やmodule機構は作らない。
- **p019 互換libpulse**: Chromiumが動くことを目的に、`libpulse.so` をAPI互換で提供する
  （2026-09-23ユーザー決定）。Chromiumは非同期API（`pa_threaded_mainloop`、`pa_context`、
  `pa_stream`、introspect）を使うので、`pa_simple`だけでは足りない。最初は再生と録音の
  基本機能に絞り、Chromiumなどで足りない機能に気づいたら、後からPhaseを作って足す。
  ただし「基本機能」には、最初から遅延・時刻情報の問い合わせ（正確な値）、一時停止・再開、
  flush、ストリームの音量・ミュートを含める。動画系アプリ（Chromium、Firefox、VLC）が
  映像と音声を合わせるのに使い、無いと音ズレや再生・停止の不具合になるため（2026-09-23合意）。
  ネットワーク再生（native protocolのTCP、トンネル、RTP等）はサーバ側の構成機能で、アプリは
  libpulse経由で使うだけなので、範囲外のままでよい。
  libpulseはLGPLなので、ヘッダも実装もupstreamから転記せず独自に書く（API名と型の互換だけを取る）。
- **p010**: `cmap`（format 4/12）、`glyf`（2次ベジエ）、`loca`/`hmtx`/`hhea` を読む。
  anti-aliasのスキャンライン描画を行う。hintingとCFF/OpenTypeレイアウトは範囲外。
  FreeType由来のコードは参照・転記しない。`/lib/libtruetype.so` として提供する。
- **p011**: `userland/base/zwl/` を改名する（ディレクトリ名もzdesktopにするかはp001で決める）。
  focus、移動・リサイズ、z-order、最小化・最大化、server-side decoration。
- **p012**: Xzedの既存コード（Zlib）を取り込み、基本requestとXWayland相当の合成を行う。
  既存 `userland/X11/`（zwm、zshell、zterm）との関係はp001で決める。
- **p013/p014**: タスクバーはWiFi（networkdの通知）と音量（audiod）を表示・操作する。
  タイル表示はGPU合成を使う。
- **p016**: 最大のPhaseで、着手時に分割する前提。GN/ninjaによるcross build、Ozone
  （Wayland）backend、Vulkan/ANGLE、PulseAudio経路、フォント（Chromium同梱のFreeType/
  fontconfig）を扱う。FreeBSD/OpenBSDのport差分を参考にする。

## WiFi状態通知の調査（2026-09-23）

networkdには、状態をpushで通知する経路が**無い**。

- 制御は `/run/networkd.sock`（`AF_UNIX`、`SOCK_STREAM`、mode 0660）の要求・応答だけで、
  opcodeは `SHOW`、`UP/DOWN`、`DHCP`、`STATIC`、`WIFI_ENABLE/DISABLE/LIST/CONNECT/DISCONNECT`、
  `LAN_ENABLE/DISABLE` 等である（`userland/base/net/protocol.h`）。購読やイベント配信の
  opcodeは無い。
- networkd自身はkernelのroute socket（`PF_ROUTE`、`RTM_IFINFO` の carrier up/down・removal・
  overflow）を受けている。これはリンクとassociationの変化だけで、SSID・接続中の状態・IPの
  取得などは含まない。
- WS005 p016（planning）がこの通知を計画していたが、未実装である。

したがって、ユーザー指示どおりこのWSのp018で実装する。networkdに購読用のopcode
（接続すると現在のsnapshotを送り、以後は変化を送る）を加え、遅いクライアントが
networkdを止めないよう、有限のキューと取りこぼし時の再同期を持たせる。
WS005 p016の受け入れ条件（秘密情報を含まない、閲覧から制御権限を得ない、daemon再起動後に
一致する）を引き継ぐ。

## 決定事項（2026-09-23ユーザー回答）

1. 実行はWS031・WS032の完了後。refactor（p002–p004）を最優先で行う。
2. GPUに関わる確認はi915で行い、エージェントの自律ループのためQEMU＋VFIOで試験する（上記の参照条件）。
3. audiodは、Chromiumが動くように互換libpulseを作っていく（p019）。最初は再生と録音だけ。
   転送等は不要。足りない機能は気づいた時点でPhaseを追加する。
4. WiFi状態通知は既存に無いため、このWS（p018）で実装する。WS005 p016は移管済み。

5. WS031は残課題があり、このセッションで続きを実行する。WS031はIntel GPUドライバの基盤の完成と
   ブラッシュアップを目指す。このWSで見つかったGPU基盤の問題はWS031で直す（2026-09-23ユーザー明確化）。

画面の照合は、WS031の `CAPTURE=` によるguest RAMからの取得を使う。人が確かめるのはLCDの実表示だけとする。

## p001の結果と、refactor前に決めること（2026-09-23）

[ws035-p001の結果](phase001/results.md)。移動対応表は [refactor-map.md](refactor-map.md)、参照一覧は
`refactor-refs/`（`plan/ws035/tests/refactor-refs.py` で再生成できる）。

- **移動の規模**: 248ファイル（p002 38、p003 56、p023 148、p004 6）。書き換えが要る行（計画の記録を除く）は
  p002 231、p003 116、p023 137、p004 37。名前の衝突は無い。
- **HAL配下の書換え**: p004の6行（6ファイル）の `#include` パスだけ。`include/hal/` は変わらない。p023では、HALが
  読むlibcヘッダ23件の置き場所が変わるが、HALのソースは変わらない。
- **kernelのinclude経路**: kernelは今すでにlibcの公開ヘッダを読んでいる（`src/kern`・`src/drivers` で32件、
  i915 buildで35件、HALで4〜7件、kernel内libcで37件）。ws.md p003の「kernelがuserland用ヘッダを誤って拾う」
  という心配は現状と合わない。保証は「移動前後で、kernel・HALが読むlibcヘッダの集合と内容が変わらず、
  増えない」とし、`plan/ws035/tests/kernel-include-audit.py --compare` で確かめる。
- **refactor前のbuild**: buildできるのはamd64とi915-amd64だけ（どちらもwarning 0）。pcat・pc98・rpi4・sun4u・
  x68kは移動前から失敗している（未定義 `kern_ptrace`・`vm_device_*`、`rtld.c:1796` の未使用関数、存在しない
  `src/hal/pmem-constraints.c`、GCCの `__UINT32_C_SUFFIX__` 等）。refactorの確認は「amd64がbuildでき、他は
  移動前と同じ失敗」になる。
- **toolchain**: このcheckoutには `build/llvm` 等が無い。p001は `~/zedBSD/build` の私的コピー
  （LLVM 23.1.0-zedbsd6、Noct 2.0.1-zedbsd12）を使った。以後のbuild Phaseも同じ扱いか、`make toolchain` が要る。
  → 2026-09-23: このcheckoutで `make llvm-host-archive` によりLLVM（23.1.0、patch zedbsd6）をsourceから再buildし、
  `build/llvm` ができた。アーカイブ（120,818,454 byte、SHA-256 `7dab5e3c8dc7202174320abd5470e17117a7c24ff1575999f3d413dba4152b4c`）を
  GitHub Release `rev-0` の `zedbsd-llvm-23.1.0-x86_64-linux.tar.gz` に上書きし、ダウンロードして一致を確かめた。
  `toolchain/llvm/version.mk` の `ZEDBSD_LLVM_CACHE_SHA256` も更新した（ユーザー指示、Phaseにしない作業）。
- **HDA**: `00:1f.3`（`8086:51c8`、`snd_hda_intel`、codec Realtek ALC3254）はIOMMU group 15に、eSPI `00:1f.0`・
  SMBus `00:1f.4`・SPI `00:1f.5` と一緒に入っている。今のホスト構成では**単独でパススルーできない**。

## 決定事項（2026-09-24ユーザー回答）

1. **audiod** は unix socket の interface として設計する（p050）。音は共有メモリで受け渡し、streaming の
   interface は持たない（負荷を下げるため）。共有メモリの fd は `shm_open` の直後に `shm_unlink` して匿名にし
   （memfd は無い）、SCM_RIGHTS で渡す。client 側は libpulse の `pa_stream_write()` がその buffer に書く。
   `/dev/dspN` に OSS の mmap interface を持たせ（p049）、対応なら audiod は mmap、非対応なら write する。
   ゼロコピーの DMA buffer は今は要らない。概念上の interface と、コピーありでも動く実装を最低限とし、
   ゼロコピーは後で別に目指す。
2. **合成（compositing）**: 設計を出す（p051）。ユーザーが微調整して承認する。
3. **HDA の既定は ON**（p048）。実機の確認は、HDA が passthrough できないので、ユーザーが後で USB boot の
   ベアメタルで行う（p008）。
4. **USB hub driver** を作る（p046）。networkd の二重 DHCP を直す（p047）。

## 未決事項

1. ~~HDA実機（p008）~~ → 決定済み（2026-09-23）: 開発はQEMUのintel-hdaエミュレーションだけで行う。実機での動作確認は人間が行う。
2. ~~HAL承認~~ → **承認済み（2026-09-23ユーザー「HALのinclude path変更は承認します。」）**。範囲はHAL配下の `#include` の
   パス変更だけ（p004の6行と案Bの4行、p023でHALが読むlibcヘッダの置き場所が変わること、p035でHALのinclude行を
   libcのヘッダからuapi・kern・freestandingヘッダへ置き換えること）。HALの宣言・実装・責務の変更は含まない。
   当初の記載: p004の6行の `#include` パス置換（`src/hal/arm64/bsp-rpi4/boot.c`、`src/hal/i386/bsp-pc98/boot.c`、
   `src/hal/sparcv9/bsp.h`、`src/hal/sparcv9/cmain.c`、`src/hal/x86/boot-parameters.c`、`src/hal/x86/boot-parameters.h`）。
   p023でHALが読むlibcヘッダの置き場所が変わること（HALのソースは変わらない）も承認の範囲に含めるか。
3. p002（2026-09-23一部決定）: i915とVenusの公開ヘッダは `include/drivers/pci/pci-i915.h`・`pci-venus.h` とし、
   `gpu/i915.h` は作らない。登録関数は `drv_pci_i915_driver_register`・`drv_pci_venus_driver_register` へ改名する。
   xHCIのヘッダはsourceの場所（`src/drivers/pci/`）に合わせ、`hid`・`graphics` は実装の隣へ置く（エージェント案）。
   buildされない `i915-old/` は書き換えない（エージェント案）。
4. ~~p004の `include/kern/boot.h`~~ → 決定済み（2026-09-23）: `include/kern/` へ移す（案B）。
7. ~~（p033）kernel・HAL共通のcompile flagの変更~~ → 承認（2026-09-23）。: `-nostdinc -isystem sysroot` を `-nostdlibinc` に、`-fno-builtin` を加える。
   HALのsourceは変えないが、HALのcompile flagが変わる。HALの `.text` は `-fno-builtin` で変わらないことを確認済み。承認範囲に入るか。
8. ~~（p033）Vulkanの宣言を `include/uapi/vulkan/` に置く~~ → **却下（2026-09-23）**: VulkanはUAPIではなくlibcの一部。`include/libc/vulkan/` に置き、
   driverは `<libc/vulkan/vulkan.h>` をincludeする（同日の `include/vulkan/` 案を置き換え）。
   **kernelとlibcの関係（2026-09-23ユーザー明確化）**: このOSはkernelとlibcが完全にモノリシックである。driverがlibcを
  参照することは許される。Vulkanのヘッダはlibcの一部で、`include/libc/vulkan/` に置く。driverは `<libc/vulkan/vulkan.h>` を
  includeしてよい。`userland/base/libvulkan` はoptionのpackageではなく、必須の構成要素がbuild単位に分かれているだけである。
  kernelで除くのは、接頭辞なしの標準Cヘッダ名（`<stdio.h>`・`<string.h>` 等）による暗黙の読込みと、libcのobjectのlinkである。: i915のnative Vulkan実行器がVulkanの型をwire formatとして
   decodeするため、uapiとして扱う。`include/libc/vulkan/*.h` はwrapperとして残す。
9. ~~（p033）host fixtureへの委任~~ → 承認（2026-09-23）。: `include/uapi/hosted.h` を作り、zedBSD以外のhost buildでは標準名の定義をhostのlibcに任せる。
   `-Iinclude/libc` を使うfixture 111ファイル（`plan/*/tests`）に `-DKERN_UAPI_NATIVE` を機械的に足す。
10. ~~（p033）明示的な呼出しの改名~~ → 承認（2026-09-23）。（ユーザー決定に沿うがdiffが大きい）。
    compilerが暗黙に出す `memcpy`・`memset` は `kcrt.c` に標準名で定義する（実測で `memmove`・`memcmp` は出なかった）。
11. ~~（p033）`locale-record.c` の削除~~ → 承認（2026-09-23）。（libcのlocale.cをlinkするためだけに存在）と、kernel側の `hal_memset` 4箇所の置換。
12. ~~（p033で新規発見）p023との衝突~~ → 解決（2026-09-23）: libcのヘッダは `include/libc/` に置く（下の(c)）。sysrootとrootfsへは `/usr/include/` 直下にコピーする。: p023でlibcのヘッダを `include/` 直下へ移すと、kernelの `-Iinclude` がcompilerの `stdint.h` 等より
    先にlibcの `stdint.h` を見つけてしまい、kernelが再びlibcのヘッダを読む（p035の検査が失敗する）。host fixtureも同様に壊れる。
    対策の候補: (a) kernelのcompileでcompilerのfreestandingヘッダのdirを `-I` で `-Iinclude` より前に置き、`include/` 直下の
    libcヘッダを読んだら検査で失敗させる、(b) kernel用のヘッダ見取り図（`kern/`・`hal/`・`drivers/`・`uapi/` だけを並べたdir）を
    生成してkernelはそれだけを見る（refactor-mapの案2）、(c) libcのヘッダを `include/` 直下ではなく別の場所（例 `include/libc/`）に置く。
13. **（p033）** `uapi-abi-layout-check`・`posix-header-check` にruleが無い件はWS026（テスト資産）へ、arm64・GCCのplatformの
    `-DKERN_UAPI_NATIVE` はWS036へ回す（エージェント判断）。
#### p034の結果（2026-09-23、[results.md](phase034/results.md)）

kcrt（`include/kern/kcrt.h`、`src/kern/kcrt.c`、16の `kern_mem*`/`kern_str*` と `kern_snprintf`/`kern_vsnprintf`）と
`src/kern/heap.c`（`kern_heap_*`）を実装し、vmunixからlibcのobjectを外した。232ファイル・2,662箇所を置換scriptで
`kern_*` に直し、`locale-record.c` を削除した。amd64・i915-amd64がwarning 0でbuildでき、libc symbolは0
（compilerが要求する `memcpy`・`memset` の2つだけkcrtが定義。実測でこの2つ以外は出ない）。host試験（kcrt 105検査、
heap 55/77検査、通常＋ASan/UBSan）、書式scan、fixture 46件の回帰がPASS。

未達だった2件は、どちらもp034の原因ではなかった（p002のkernelでも同じ結果）:

- **QEMU smoke**: 2026-09-21の変更でPC/ATのテキストconsoleがCOM1にだけ出るようになり、`-serial none` で
  debugconしか読まない `plan/ws001/tests/qemu-base-utility-smoke.sh` が、どのkernelでもtimeoutしていた。
  rootがこのscriptをCOM1から読むよう修正し、p034のkernelで **PASS** を確認した（2026-09-23）。
- **ws018 `run-legacy-bootfs-removal-host-test.sh`**: 参照するパスと名前が古く、移動前から失敗している。
  設計の前提（`entry.c` をcompileする）も誤りだった。WS026（テスト資産の整理）へ回す。

判明した事項: i386で `kcrt.c` は `__udivdi3`（64 bit除算）を要る（以前はlibcの `int64.c` から得ていた）。
他platformの `vmunix.mk` は `locale-record.c` を参照したまま。どちらもWS036で直す。
書式の挙動が1箇所変わった（負値＋幅が ` -5` になる。C準拠）。

#### PC/ATコンソールとトップレベルbuildの修正（2026-09-23ユーザー指示、Phase外の作業）

ユーザー指示「PC/ATのテキストコンソールは、きちんとフレームバッファに出るべきです。COM1は、オプションを指定して
ビルドしたときに、ミラーリングする必要があります。出力しか対応していません。入力はqemuコンソールでsend-keyして
ください。」および「トップレベルでmakeが通って、hdd-image.imgが生成できない」の修正。

- **COM1ミラーをoptionにした**: `CONFIG_PCAT_SERIAL_MIRROR`（menuconfigのbool、既定n）を追加し、
  `src/drivers/platform/pcat/serial-mirror.c` の条件を `HAL_PCAT_DEBUGCON` から `PCAT_SERIAL_MIRROR` へ変えた。
  以前はdebugconの指定に相乗りして常に有効だった。テキストconsoleは従来どおりframebufferへ描画する。
  ミラーは出力だけで、入力はkeyboard（QEMUではmonitorの `sendkey`）から取る。
  option無し・有りの両方でamd64 kernelがwarning 0でbuildでき、symbolの有無も確認した。
- **起動確認をboot testへ置き換えた（2026-09-23ユーザー指示）**: `plan/ws001/tests/qemu-base-utility-smoke.sh` は
  **削除した**。IDE・BIOS起動で古く（IDEはDMA未実装で極端に遅い）、実機はUEFIのUSB起動しか使っていないため。
  代わりに `plan/tools/boot-test.sh`（と `boot-test.py`）を置いた。OVMFでUSB stickから起動し、QMPの `screendump` で
  framebufferを撮り、kernelと同じconsole font（`vgafont.c`）で各8x16 cellを照合して文字を読み、`login:` が出たら
  PNGを保存する。実行して **PASS**（`plan/tmp/boot-test/login.png`、`login.txt`）。COM1ミラーは要らない。
- **トップレベルbuildの修正2件**:
  - `build/NoctLang` がp034の後片付けで消えていたため、sourceを取り直しhost用 `noct` をbuildした。
    サブエージェントの定義に「共有の `build/` を消さない」を追加した。
  - `userland/packages/editors/remacs/Makefile`: 辞書 `SKK-JISYO.remacs` にruleが無く、`build/sources` が無いtreeで
    `No rule to make target` で止まっていた。cloneのstampに結び付けるruleを足した。
  - 以上で `make disk-image`（ユーザーのconfig.mk、clang・openssl・openssh入り）がEXIT 0で通り、
    `hdd-image.img` 796,917,760 byte、`vmunix` と `vmunix.map` が生成された。

#### mapファイル（2026-09-23ユーザー指示）

`-g` は付けず、link時にmapファイルを作る方針にした。amd64の `$(BUILD)/vmunix` のlinkに `-Map $@.map` を足し、
`vmunix.map`（約950 KiB、section・object・symbolのアドレス）が出ることと、buildがwarning 0で通ることを確認した。
他platformへの追加はWS036で行う。

#### Vulkanヘッダとcommand protocolの確認（2026-09-23ユーザー訂正）

- **commandの中身がVulkanの構造体であるのは意図した設計**である。Venusのcommandを採用しているためで、
  i915 driverが `<libc/vulkan/vulkan.h>` をincludeするのが正しい。UAPI（`gpu*.h`）はbuffer管理とcommand実行の
  入口を定義し、commandの中身はVenus由来のVulkan構造体である。
- **`I915_STREAM_MAGIC`（native stream）はUAPIに出したままでよい**。これは `gpu-i915-test` のための経路で、
  GPUフレームワークが未完成でVulkan実装が繋がっていなかった時期に、i915と直接疎通させるために使った
  試験用の仕組みである。既に使われていない。試験用なのでUAPIにあってよい。
- したがって、kernelがincludeしてよい `libc/` のヘッダは **`libc/vulkan/*` だけ**である（`vulkan.h`・`vulkan_core.h` 等、
  必要なものを直接includeしてよい）。
  それ以外のlibcヘッダ（`<stdio.h>`・`<string.h>`・`<sys/*.h>` 等、接頭辞なしの標準Cヘッダ名を含む）は
  kernel・driver・HALからincludeしない。ioctl・errno等のABIはUAPIに分離したままにする。
- エージェントが一時「Vulkanヘッダへの依存はkernelがVulkanの意味を持ちすぎている兆候で、実行器をuserlandへ
  移すべき」と解析したのは**誤り**だった。zedBSDはMesa流のuser modeドライバを持たず、userlandにあるのは
  `/dev/gpu0` へioctlを出す薄い `libvulkan` だけである。SPIR-Vのcompileはkernel空間のdriverが行い、i915の
  native streamはdriver内部に閉じる。この構成は変更しない。

14. ~~`include/libc/linux/`~~ → **削除済み（2026-09-23ユーザー指示）**: `<uapi/input.h>` を読むだけのLinux互換ラッパー
    （`input.h`、`input-event-codes.h`）だった。evdevは振る舞いはLinux互換だが、ヘッダまで互換にしない。参照していた
    `docs/reference/evdev.md` とWS006の `evdev-layout-test.c` を直した。FreeBSD互換の `include/libc/dev/evdev/` は残した。
5. ~~移動前から失敗しているplatformのbuild~~ → 決定済み（2026-09-23）: 新しい[WS036](../ws036/ws.md)で扱う。refactorの間は、
   amd64以外のbuildが壊れてもよい（amd64以外は後でbuildを通す）。refactorの確認はamd64（とi915-amd64）で行う。
6. **kernelからlibcを切り離す**（2026-09-23ユーザー指示「カーネルがstdio.hを含む標準Cヘッダを読み込むのはおかしな話です。
   それは除去するべきです。」）: p033〜p035を追加した。調査の結果、vmunixは今libcのsource一式（heap、stdio、locale、regex、
   ndbm、realpath等）をlinkしているが、kernelが実際に使うlibcの関数は24個だけだった（heap allocator 9、`memchr`・`memcmp`・
   `memcpy`・`memmove`・`memset`・`memset_explicit`、`snprintf`、`strcat`・`strchr`・`strcmp`・`strcpy`・`strlen`・`strncmp`・
   `strncpy`・`strnlen`・`strrchr`）。一方、kernelはlibcの公開ヘッダを多数読んでいる（`string.h` 166、`errno.h` 179、
   `sys/ioctl.h` 61、`sys/stat.h` 47、`locale.h` 178、`stdio.h` 4 など。i915のnative Vulkan実行器はVulkanのヘッダも読む）。
   根本対策の方針: kernel用のCランタイム（`kern_memcpy` 等）をkernel側に持つ。compilerが構造体のcopy等で暗黙に呼ぶ
   `memcpy`・`memset`・`memmove`・`memcmp` の4つは、kernel内でその名前の定義を残す。errno・ioctl・stat・time等の
   ABIの定数と構造体は `include/uapi/` から取る。整数型等はcompilerのfreestandingヘッダ（`stdint.h`・`stddef.h`・
   `stdbool.h`・`stdarg.h`・`limits.h`）かkernelの型に限る。Vulkanのヘッダの扱いはp033で決める。
7. **kcrtの方針（2026-09-23ユーザー指示）**:
   - kernel用Cランタイムのヘッダは `include/kern/kcrt.h`（kernel crt）とし、標準C APIの代替を `kern_*()` で作る
     （`kern_memcpy`、`kern_strlen`、`kern_snprintf` 等）。実装は `src/kern/kcrt.c` に置く。
   - compilerが暗黙に生成するcopy等（clangは構造体のcopy等を `__builtin_memcpy` として扱い、最終的に `memcpy` 等の
     呼出しを出す）は、libcを外したときのlinkエラーから見つけて、その名前の定義を `kcrt.c` に足す。
   - ~~copy・fill系はHAL側のCランタイムを呼ぶだけにする~~ → **方針変更（2026-09-23）**: kcrtはHALのCランタイム
     （`hal_memcpy` 等）を呼ばない。kcrtの関数はすべて `kcrt.c` に自前で実装する。HALのCランタイムはいつでも
     kernelから呼べるが、本来はkernelの初期化の段階で使うものであり、kernelの初期化が済んだらkcrtを使う。
     この使い分けは `kcrt.h` のコメントに書く（`hal.h` は変更しない）。`hal_memcpy` は今のところ使わない。
   - **heap allocator（調査結果）**: kernelには既にallocatorがある。`include/kern/kmem.h` の `kern_malloc`・`kern_calloc`・
     `kern_free`・`kern_memory_get_stats` で、`src/kern/entry.c` が実装している。ただし、その中の割当ての仕組み
     （`heap_allocator_*`）は `libc/heap.c` にあり、userlandのmallocと同じsourceをkernelへlinkしている。
     **決定（2026-09-23）**: `libc/heap.c` を複製して `src/kern/heap.c` を作り、以後はkernelとlibcで別々に保守する。
     関数名等はkernel用に整理してよい。`kern_malloc` 等のAPIは変えない。

## 互換ライブラリの決定（2026-09-23 ユーザー決定）

### 決まったこと

**D1: 本家と同じ版を名乗ってよい。** 互換品なので、`ZLIB_VERSION` や
`PNG_LIBPNG_VER_STRING` は本家の値（記録上は zlib 1.3.2、libpng 1.6.58。
`package-inventory.md`）を返す。libpng は `png_create_read_struct()` が
版の文字列を照合するので、これが合わないと呼べない。

**D2: 名前を分け、使い分ける。**

| | 誰が使うか | 置き場 |
| --- | --- | --- |
| **`libz-compat` / `libpng-compat`**（自前） | **base のプログラム**（zdesktop など） | `userland/base/libz-compat`、`userland/base/libpng-compat` → `/lib/libz-compat.so`、`/lib/libpng-compat.so` |
| **本家 zlib / libpng**（外部パッケージ） | **packages の外部プロジェクト**（curl・git・freetype・GTK・Qt・Chromium） | `userland/packages/libs/`（ws034-p021・p027）→ `/usr/lib` |

ws034-p021（zlib）・p027（libpng）は**そのまま残す**。置き換えない。
荒い実装で curl の gzip ファイル API や freetype の Adam7・16bit・palette を
まかなう必要が無くなる。

### 上の決定から決まること（root の判断、2026-09-23）

- **SONAME は `libz-compat.so`・`libpng-compat.so`。** base の他のライブラリ
  （`libc.so`・`libvulkan.so`・`libwayland-client.so`・`libtruetype.so`）に版付き SONAME は無く、
  それに揃える。版は API が名乗る文字列で表す（D1）。
- **ヘッダは `include/libc/compat/`**（2026-09-23 ユーザー決定、D4）。
  `include/libc/compat/zlib.h`・`include/libc/compat/png.h` が
  `/usr/include/compat/zlib.h`・`/usr/include/compat/png.h` に入り、
  base のプログラムは `#include <compat/zlib.h>` と書く。
  本家は package として `/usr/include/zlib.h`・`/usr/include/png.h` を入れるので、
  **同じ場所に置くと衝突する**。ファイル名は本家と同じままで、置き場だけを分ける。
- **関数名は本家と同じ**（`deflate`・`inflate`・`png_create_read_struct` …）。
  互換とはそういうことである。ただし下記の制限を伴う。

### 制限（この決定から生じるもの）

**1つのプロセスが `libz-compat` と本家 zlib の両方を link すると、同じ名前の関数が2つになる。**
base のプログラムが、本家 zlib に依存する package のライブラリを使うと起こりうる。
今そうなる組合せは無いが、GTK・Qt を base のプログラムから使い始めると起こる。
そのときは、base 側を本家へ寄せるか、compat 側の関数名に接頭辞を付けるかを決める。

### D3. libpng の API の範囲 — 決定（2026-09-23 ユーザー）

**simplified API**（`png_image_begin_read_from_file`・`png_image_finish_read`・
`png_image_write_to_file`）だけを作る。従来 API の全体は要らない。
GTK・Qt・freetype は本家を使い、compat を使うのは自分たちの base のプログラムだけで、
呼び方はこちらで決められるためである。

足りないものが出たときに従来 API を足す。そのときは
`png_create_read_struct()` の版照合（D1）を合わせる。

### D4. 公開ヘッダの置き場 — 決定（2026-09-23 ユーザー）

`include/libc/compat/` に置き、`/usr/include/compat/` へ入る（上記）。

### D5. libzdesktop の役目 — 決定（2026-09-23 ユーザー）

**libzdesktop は、Vulkan 以外の OS 依存を閉じ込めるラッパーである。**
「zdesktop と話すためのライブラリ」ではない。

理由は2つ。

1. **networkd の仕様は変わりうる。** zdesktop が networkd の protocol を直接呼んでいると、
   networkd を直すたびに zdesktop を直すことになる。間に1枚置けば、直す場所が1か所で済む。
2. **zdesktop を他の OS へ移すとき**、OS に依存する部分がこのライブラリに集まっていれば、
   そこだけを書き換えればよい。

したがって:

- **zdesktop は networkd・audiod などと直接話さない。** すべて libzdesktop を通す。
  WiFi の状態を networkd の購読（ws035-p018）から取るのも、このライブラリの中である。
- **Vulkan は例外。** 描画は標準の Vulkan/WSI をそのまま使い、ラッパーを挟まない。
- これは p013（タスクバーの WiFi）、p026（音量）、通知のすべてに効く。
  それらの Phase は libzdesktop に関数を足す形になる。

**この Phase（p042）は空の枠を作るだけ**で、どの機能を先に入れるかはその機能の Phase で決まる。

### D6. libz-compat・libpng-compat の優先順位 — 決定（2026-09-23 ユーザー）

**zdesktop が要るようになったときに入れる。** 先に作り置きしない。
どちらも zdesktop の依存であり、GTK・Qt・Chromium は本家を使う（D2）ので、
それらの前提ではない。

最初に要るのは、zdesktop が PNG の画像（アイコン等）を扱う段である。
その Phase（p013 のタスクバー、p025 のフレーム描画あたり）に着手するときに、
p041 → p040 の順で前に入れる。

## 現状

計画を作っただけで、Queue・実装・試験はまだ無い。GitHub Issueも未作成である。
**p040〜p042 の未決定は無くなった**（2026-09-23）。
p040・p041 は zdesktop が要るようになったときに入れる（D6）。
p042 は空の枠を作るだけなので、いつでも入れられる。

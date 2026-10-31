<!-- awesome-plan project=zedbsd record=ws291-p023 -->

# ws291-p023: 共有の image の画素の並びを client と compositor で決める（WSI の block-linear、まず設計）

Status: cleared（2026-10-31 21:04 Q1 の判定。共通の部分 I1・nvrtx の block-linear の置き方・NVC57E の block-linear の scanout（BL の kind の context DMA の直し effb3d149）・既定で有効（P2 16cd986f0）が main に。RTX で subs-1000 10.9 → 約 20 fps、画素の差 0、desktop の後退なし、ユーザーの目視 PASS（2 回目））
Parent: [WS291](../ws.md)
Queue: q1781（P3）

## 目的

2026-10-31 ユーザーから、RTX の subs-1000 は 0j の (b)（画面に出す image を GPU の得意な並びにする）で進める、という趣旨の指示を受けた。
あわせて、image の並びは普通は client が求める物なので、そこをどう扱うかを示す事を求められた。

p014 の結果: RTX は pitch-linear の色の target と block-linear の深度を組めないので、nvrtx は block-linear の影に描き、pass の終わりごとに
共有の image（WSI の image、pitch-linear）へ全画面を copy する。subs-1000 は 1 frame に submit が 1000 回なので copy も 1000 回（1 回約 29 µs）。

## Q1 の見立て（設計の出発点、設計で確かめる）

- `VkSwapchainCreateInfoKHR` に tiling は無く、swapchain の image の memory の並びは WSI と driver が決める。決める制約は読む相手
  （compositor の合成・display の scanout・他の GPU・CPU の読み）。Linux は compositor が surface ごとに読める並び（DRM format modifier）を
  知らせ（linux-dmabuf の feedback）、WSI が選ぶ。
- zedBSD の `kl_gpu_buffer_v1`（今は版 3）には並びの相談が無く、共有の image はいつも pitch-linear。
- 案: (1) 共有の resource が並びを持つ（読む側が正しく読む）、(2) compositor が surface ごとに読める並びを知らせる（`kl_gpu_buffer_v1` の版を上げる）、
  (3) WSI は読む相手が同じ device の時だけ block-linear を選び、他は pitch-linear、(4) 相手が変わったら知らせて swapchain の作り直し
  （`VK_SUBOPTIMAL_KHR`）。copy は submit ごとから、多くても present ごとに 1 回（合成なら 0）へ。
- HAL は変えない。他の GPU の driver（amdrdna・i915・powervr・venus など）が後で自分の並びを使える形にする。

## 範囲（この Queue）

1. 設計の文書 `plan/ws291/phase023/design.md`: 今の WSI・`kl_gpu_buffer_v1`・compositor・libkeiland-backend・kernel の GPU の resource の
   metadata と import・nvrtx の影の copy・scanout（display が block-linear を読めるか）・画面の撮影（keiland-shot）を読んだ事実、
   並びの表し方、protocol の版の変更、WSI の選び方と作り直し、fallback、他の GPU への広げ方、WS309 p015（game mode の V2）との関係、
   試験（subs-1000・big-1000・desktop・画素の比べ）、実装の Phase の分け方と見積もり。
2. design-reviewer の敵対的レビューと反映。
3. ユーザーへの問い（新しい protocol の版など製品の判断）を `plan/ws291/phase023/user-questions.md` に、AGENTS.md §3 の前置きの形で。
4. code は書かない。実機は使わない。

## q1781-i01（2026-10-31 P3、worktree p3-ws291wsi、基点 main d1014941e）

- 承認: Queue q1781-i01（設計）。同日 Q1 から、設計の後に同じ Queue で p023 の実装（共有の部分 I1 と nvrtx）を行う指示を受けた（ユーザーの決定: RTX の block-linear は必須）。
- 設計: [design.md](design.md)（第 2 版）、レビュー [design-review.md](design-review.md)、問いと答え [user-questions.md](user-questions.md)。
  第 1 版 2b92488b1、第 2 版 54c1b7ca6（Q1 が main に ffd702bdb で merge）。

### ユーザーの答え（2026-10-31、Q1 経由、要点）

- 問い 1（block-linear の scanout の画素の確かめ）: CRC は作らず、ユーザーが画面を目で見て確かめる。見るまでは block-linear の scanout は起動の parameter で既定 0
  （切る）にし、その間 RTX の game mode は 1 frame に 1 回の copy のまま。目視の手順は下（Q1 がユーザーの拾える試験の一覧に置く）。
- 問い 2（subs の run の固定費）: この実装の後に、run ごとの固定費（約 56 µs）を 1 つずつ測って削る Phase を続ける（design.md §11 の I7）。

### I1（共有の部分）

- commit 7b0a6644e: UAPI（並びの値・`layout_parameter`・`GPU_IMAGE_LAYOUTS` 46・`GPU_CAP_IMAGE_LAYOUTS`）、`gpu-share.h` の `drv_gpu_layout_ops`、`gpu.h` の pointer、
  `gpu.c` の core（登録・GET_INFO・ioctl 46 の入力と既定と答えの確かめ・非 LINEAR の export の範囲・別の device の非 LINEAR の import の EXDEV）、改名に伴う
  powervr・bcm2711・mali の試験、venus の初期化子、`plan/ws014/tests/gpu-uapi-layout.c` の確かめ、host 試験 `plan/ws291/tests/run-gpu-layouts-host.sh`。
- 確かめ: `sh plan/ws291/tests/run-gpu-layouts-host.sh` → UAPI ILP32・LP64 PASS、ordinary・ASan/UBSan とも `gpu-layouts-host: PASS`。`tests/mali-scanout-host-test.c` 19 PASS。
  `config/ci` の 19 config の `$(BUILD)/vmunix` が全部 warning 0・error 0。QEMU・実機は I1 では無し。

### ユーザーの目視の確かめ（block-linear の scanout、RTX 2070、10.0.10.3）

前提: main に p023 の nvrtx の部分（I5 の block-linear の scanout）と WS309 p015 の game mode（compositor が display の image を貸す形）が入った image。
これが無いと block-linear の画面は出ない（それまでは手順 4 の画面は今までと同じ pitch-linear の画面で、確かめにならない）。

1. 10.0.10.3 を zedBSD で起動する（Q1 の手順: Debian から `efibootmgr --bootnext` で zedBSD の項目）。
2. zedBSD に root で入り、`/boot/boot0/zedbsd.cfg` に 1 行 `nvrtx.disp.bl=1` を足して再起動する
   （`echo nvrtx.disp.bl=1 >> /boot/boot0/zedbsd.cfg && sync && reboot`）。
3. 起動の後、`dmesg | grep 'block-linear scanout'` で `nvrtx: display: block-linear scanout on` の行が出ている事を確かめる（I5 で足す行）。
4. desktop に login し、terminal から全画面の game mode で mview を動かす: `mview --content=game --copies=100 --spin=30`（約 30 秒）。
   その間、`dmesg | grep 'block-linear scanout'` に `first flip` の行が増える事（block-linear の image を実際に出した印）。
5. 画面を見る:
   - **正しい**: 100 体の model が、`nvrtx.disp.bl=0` の時（手順 6 の後に同じ command）と同じ形・色で、滑らかに回る。端まで欠けや筋が無い。
   - **誤り**: 画像が 8 行の高さの横の帯や 16 画素の幅の小さな四角に刻まれてずれて見える（block の並びを行の並びとして読んでいる）、色が入れ替わる、画面が黒い、
     ちらつく、一部だけが正しい。どれか 1 つでもあれば誤り。可能なら写真を撮って Q1 へ。
6. 確かめの後、`/boot/boot0/zedbsd.cfg` から `nvrtx.disp.bl=1` の行を消して再起動する（`sed -i '/^nvrtx.disp.bl=/d' /boot/boot0/zedbsd.cfg && sync && reboot`）。
   正しかった時は Q1 が既定を 1 にする判断をユーザーに聞く。

### nvrtx の実装（I3・I4・I5、commit 499f18262）と実機（2026-10-31 16:0x〜16:4x、10.0.10.3 RTX 2070）

- 実装: OPTIMAL・1 level・色・COLOR_ATTACHMENT を block-linear（影なし）、私用の record（`GPU_OP_RECORD_IMAGE_LAYOUT` 0x7a650001、`include/uapi/gpu-op.h`）の nvrtx の decoder、
  1 level の tiled の subresource layout、tiled の CPU の copy を 16 B ずつ、`drv_gpu_layout_ops`（NV block-linear parameter 0 と LINEAR）、export の記述の記録と再 export・bind の形の確かめ、
  SCANOUT の import と present の規則、`nvrtx.disp.bl=1`（既定 0、Turing だけ）と window の SET_STORAGE の block-linear、log の 2 行。host: nvrtx-vk 94/0、vk-d3 225/0、gpu-layouts-host PASS、
  amd64 系と rpi5・visionfive2 の vmunix warning 0。試験の script `plan/ws291/tests/rtx-p023.sh`（80b18f875）。
- 実機 A/B（`rtx-p013.sh` SHOT=1 ROUNDS=2 PROF=1、base = c8b142e18 の tree の RTX kernel cksum 948202092、new = 499f18262 の kernel 2917063051、userland は両腕とも
  この worktree の 13 file、証拠 `build/tmp/rtx-p023.NFTlYm/`（worktree の build、git の外））。fps（round 1・2）:

| | big-100 | big-1000 | subs-100 | subs-1000 | scene-1500 | scene-5000 |
| --- | --- | --- | --- | --- | --- | --- |
| base | 590.5・618.6 | 108.9・109.9 | 102.8・105.9 | 10.92・10.85 | 156.9・157.4 | 49.68・49.64 |
| new | 691.6・637.1 | 110.4・114.0 | **203.7・202.0** | **21.31・20.41** | 158.6・158.3 | 50.30・50.41 |

  - 影の数（PROF の perf の行、subs-100 の 5 s ごと）: base は executor の quad 32695・53119、new は 640・583（残りは present の copy など）。
  - 画素: one・each・subs の still は各腕の中で差 0、base の one と new の one も差 0（max 0）。見込み（約 16 fps）より上だった（copy の分に加え LOAD の pass の扱いも減った見込み、未分析）。
- desktop（`rtx-desk-main.sh` RUNS=1、new）: launch 64・type 61・output 66・drag 61・home 62・wiseview 65 fps（scroll・hover・keys は入力の間隔）。p014 の幅と同じ。
- scenes: base の腕だけ流れた（desktop・Notes・browser の keiland-shot と game mode の mview: `MVIEW DONE frames=597`、`KWL SCANOUT direct=1`）。new と new+`nvrtx.disp.bl=1` の腕は、
  Q1 のラップアップの指示（host の再起動）で new の kernel の deploy の後に止めた（未実施）。
- 機械の状態（16:4x）: 13 file と NVMe の kernel を戻して cksum 一致（vmunix 1463007375、expected の列と全一致）、cfg の `nvrtx.disp.bl=` の行 0、Debian（Linux 6.12.111+deb13-amd64、
  RTX 4 function vfio-pci）。lock /tmp/zedbsd-rdna.lock は返した。

### q1781-i03（2026-10-31 18:15〜18:30 P3、worktree p3-ws291wsi、main 76f123314 を 13417cc3f で取り込み）: block-linear の scanout の画面の崩れ

- 症状（ユーザーの目視、2026-10-31、`nvrtx.disp.bl=1`、写真 `build/review/ws291/p023-bl-visual-1.jpg`（main の checkout））: desktop は出るが、文字と icon が小さな四角に刻まれてずれて重なる。
- 原因: window は block-linear の surface を、nvrtx の唯一の VRAM の context DMA（先頭の語 0x45: VRAM・読み書き・大きな page・kind は pitch）を通して読んでいた。
  nouveau は memory の kind ごとに window の context DMA を作り、block-linear の framebuffer には kind の bit（bit 20）を立てた物を使う
  （Linux v6.12 `dispnv50/wndw.c` `nv50_wndw_ctxdma_new`、`nvkm/engine/dma/usergv100.c` `gv100_dmaobj_new`、handle は `dispnv50/handles.h` の
  `NV50_DISP_HANDLE_WNDW_CTX(kind)`、Turing の kind 0x06 は `wndwc57e.c` の `wndwc57e_modifiers`）。pitch の kind の object を通すと window は GOB の中を
  sector の並べ替え無しの 64 B × 8 行として読む。SET_STORAGE（一 GOB の block、BLOCKLINEAR）と pitch の語（行の bytes / 64）は nouveau の `wndwc37e_image_set`・`wndw.c` と同じで誤りでなかった。
- 確かめ（実機 10.0.10.3、QEMU は無し）:
  1. 崩れた kernel（2917063051、bl=1）で keiland-shot（compositor の chain の image を CPU が Turing の color 2D の GOB の並びで読み戻す）は崩れの無い desktop
     （`build/tmp/p023-i03/bad-kernel-bl1-shot.png`、worktree の build、git の外）。image の中身は正しく、display の読み方が違う。
     書き手の並びは Mesa 26.2.6 `src/nouveau/nil/tiling.rs`（`GOBType::choose`: Turing の色は `TuringColor2D`）と `copy.rs`（`CopyGOBTuring2D`）で、nvrtx の `execute_gob_offset` と同じ。
  2. その shot から「display がこう読んだら」の画を作って写真と比べた（scratchpad の script、記録は画だけ）: block の高さの食い違い（h=1・4）は window ごと横にずれ、
     Fermi・Tegra の sector の並びは細かい市松になり、どれも写真と違う。「GOB を sector の並べ替え無しの 8 行 × 64 B として読む」は写真とほぼ同じ（縞の logo の左の柱・
     濃い帯・右にずれた淡い四角・3 段に刻まれた Welcome の行）: `build/tmp/p023-i03/sim-pitchkind-linear-gob.png`。
- 直し（commit effb3d149、`src/drivers/gpu/nvrtx/display/nvrtx-disp.h`・`scanout.c`・`present.c`）: 最初の commit で、出す window の channel に
  block-linear の kind の context DMA（handle 0xfb000006、先頭の語 0x00100045、VRAM 全体）を足して bind し、`present_admit` は block-linear の surface をそれで読ませる。
  pitch の surface は今の object のまま。RAMHT の使用は 19 → 20（上限 24）。HAL・`nvrtx.disp.bl` の既定（0）は変えない。
- build: `make ZEDBSD_CONFIG=plan/ws291/tests/config.mk BUILD=build/p023/rtx-ctx build/p023/rtx-ctx/vmunix` warning 0（cksum 3845779468）。他の config の vmunix は未 build。
- 実機（直した kernel、P2 の 14 file、bl=1）: 起動して `block-linear scanout on`・`first flip 1920x1080`、display の失敗の行なし。game mode の mview
  （`measure.sh mview … --content=game --copies=100 --spin=10`、`VULKAN_ZEDBSD_WSI_LOG=1`）: 貸した chain が `tiling=0x10001`、`frames=599 fps=59.78`、`presents=594 copies=0`、
  `KWL SCANOUT direct=1`、lease は返った。keiland-shot は崩れの無い desktop（`build/tmp/p023-i03/fix-kernel-bl1-shot.png`）。
- **未実施**: display が読んだ画素の確かめ（CRC は作らない、ユーザーの決定。撮影は display の読みを見ない）。block-linear の kind の context DMA が Turing の color 2D の並びで読むことは
  nouveau と NVIDIA の driver の作り方（block-linear の scanout に block-linear の kind の object）からの推定で、画面はユーザーの目視の再確認が要る（下の手順）。
- 機械の戻し（18:27〜18:29）: 14 file を `.p7old` から戻し P2 の記した cksum と全一致、NVMe の vmunix 1463007375、cfg の `nvrtx.disp.bl=` の行 0、Debian（6.12.111+deb13-amd64、
  RTX の 4 function は vfio-pci）、lock を返した。`/boot/boot0/vmunix.old` は 2917063051（deploy が写した物）のまま。`.p7old` の 14 file は機械に残した。
- 再確認の目視（Q1 がユーザーの拾える試験の一覧に）: 上の「ユーザーの目視の確かめ」の手順で、kernel は effb3d149 を含む RTX の kernel（例 `build/p023/rtx-ctx/vmunix`）、
  userland は P2 の WS309 p015 の 14 file（compositor の chain も block-linear になるので mview 無しでも desktop で分かる）。正しければ文字・icon が bl=0 と同じに見える。

### 再開点（2026-10-31 P3）

- 済み: 設計（第 2 版）・レビュー・問いと答え、I1（7b0a6644e）、nvrtx の I3〜I5（499f18262）、compositor の非 LINEAR の取り込みの設計（design.md §9 の 8、80b18f875）、実機の A/B と desktop。
- 未実施: scenes の new と bl の腕（Notes・browser・glass の base との画素の比べ、`nvrtx.disp.bl=1` で起動して desktop と game mode が壊れない事）、I2 の probe（block-linear の画面を
  実際に出す経路は P2 の貸し出しと compositor の非 LINEAR の取り込みが main に入るまで無い）、ユーザーの目視（phase.md の手順、p015 の後）。
- 次の command（10.0.10.3 の割当の後、worktree の top で）: 13 file を build（`b=build/p023/rtx-new` に上の FILES の target、rtx-cursor-view.sh の build の形に libvulkan・mview・keiland-shot を足す）、
  `FILES='…' USB_MVIEW=$b/bin/mview RESTORE_CKSUM=1463007375 PARTS=scenes timeout 10800 flock -n /tmp/zedbsd-rdna.lock sh plan/ws291/tests/rtx-p023.sh OUT BASE_VMUNIX NEW_VMUNIX`
  （BASE は c8b142e18 か main の RTX kernel、NEW は main に入った 499f18262 の kernel。base の腕の shot は前回の OUT から使えない: scenes は base の腕から流し直す）。
- Phase の判定は Q1（subs-1000 は 10.9 → 20.4〜21.3 fps、影の quad は 1/50 以下、画素差 0、desktop 後退なし。scenes の残りと block-linear の scanout は未確かめ）。

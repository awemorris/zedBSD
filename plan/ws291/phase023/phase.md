<!-- awesome-plan project=zedbsd record=ws291-p023 -->

# ws291-p023: 共有の image の画素の並びを client と compositor で決める（WSI の block-linear、まず設計）

Status: in-progress（2026-10-31 新設。設計は済み、同日 Q1 の指示で実装に入った: I1 済み、nvrtx は進行中）
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

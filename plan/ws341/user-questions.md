<!-- awesome-plan project=zedbsd record=ws341-questions -->

# WS341 ユーザーへの問い（p001、2026-11-15 P8）

WS341 は「仮想 GPU の driver の枠組み」: program が `/dev/vgpu` で仮想の画面を作り、compositor がそこへ描いた frame を program が受け取って encode し、遠隔の display や RDP に送る。今は p001（調べと設計）が終わり、設計は [design.md](design.md)。下の答えで設計を確定し、実装の Phase（design.md §13）に進む。各問いに推しを付けた。

用語: **node** は `/dev/gpuN` の一つ一つ。**connector** は display の差し込み口（HDMI の口に当たる）。**head** は compositor が主の画面の他に出す display。**lease** は compositor がある display を使う間の占有。**hold** は program が frame を読む間、その image を compositor に返さない事。

## 問い 1: 仮想の画面を「新しい /dev/gpuN」として作るか、「1 つの仮想 display node の connector に挿す」か

2026-11-01 の決定は「/dev/vgpu の ioctl で /dev/gpuN を作る」形でした。調べると、今の compositor（libvulkan）は起動の時に `/dev` を 1 度だけ読んで display の node の一覧を固定し、後から出来た node を見ません。node が増えた事を知らせる仕組みも kernel にありません。また、program が閉じて node を消そうとしても、compositor が node を開いたままなので、compositor を再起動するまで node が残り、作る度に増えていきます。

- **A（推し）**: 起動時に仮想の display node を 1 つだけ登録し、connector を 4 つ持たせる。`/dev/vgpu` の create は空いた connector に仮想の monitor を「挿す」、program が閉じる（落ちる）と「抜く」。compositor から見ると monitor の抜き差しそのもので、WS353 で入った hotplug の道（kernel の fence → compositor の hotplug の thread → head）がそのまま使える。compositor の変更はほぼ要らない。代わりに、使わない時も connector 0 本挿しの /dev/gpuN が 1 つ居る（GPU の無い machine では作らない）。同時の仮想の画面は 4 つまで。
- **B**: 決定どおり create ごとに /dev/gpuN を作る。kernel に node の増減の知らせ、libvulkan の node の一覧の作り直し（増減に追従）、消した node が compositor の close まで残る事の受け入れが要る。実装の Phase が 2 つほど大きくなる。

推し: A。理由: 利用者に見える振る舞い（compositor に新しい画面が出て、program が終われば消える）は同じで、既存の monitor の抜き差しの道を使うので壊れにくい。

## 問い 2: 誰が `/dev/vgpu` で仮想の画面を作れるか

作れる人は、その画面に出る内容を全て受け取れます（画面の capture と同じ力）。

- **A（推し）**: GPU の node と同じ、root と video group（mode 0660）。login した利用者に自動では渡さない。RDP などの server は system の service として video group で動かす。
- B: root だけ（試験・service は root で動かす）。
- C: login した利用者（seat）にも渡す（利用者が自分で画面の共有の app を動かせる）。

推し: A。理由: 今の GPU の node の権限と揃い、service の形の RDP の server を想定できる。利用者が自分で共有する app（C）は、共有の確認の UI を作る時に改めて決める。

## 問い 3: program が compositor より遅い時

compositor は 60 Hz などで描き、program（encoder）が追い付かない事があります。

- **A（推し）**: 最新優先。program は受け取る時に最新の frame を受け取り、間の frame は飛ばす（数は program に伝える）。program が frame を読んでいる間だけ compositor の次の frame の受け付けを待たせるが、上限（1 refresh 周期、最大 50 ms）を過ぎたら待たせるのをやめ、program には「読みの途中で書き換わったかも」と伝える。compositor と desktop は program の遅れや停止で止まらない。
- B: 全 frame を必ず渡す（program が遅いと compositor も遅くなる。program が止まると desktop の表示も止まる）。

推し: A。理由: 遠隔の画面は最新を送るのが普通で、program の不具合で desktop が止まらない事を優先する（program が落ちても compositor は落ちない、という決定の延長）。

## 問い 4: 仮想の画面の使い方と既定

今の compositor は、主の画面の他の display を head として **mirror**（同じ desktop を写す）か **extended**（desktop を広げる）で出します。ただし extended の head には今は壁紙しか出ず、窓を移せません（WS113 の窓の移動の Phase が未）。

- 4a: 仮想の画面の既定の出し方。**推し: 今の displays.conf の規則どおり（無ければ extended）とし、RDP などで作業に使う時は mirror を選ぶ**。窓を仮想の画面へ移すのは WS113 の窓の移動の Phase で扱う（この WS には入れない）。
- 4b: driver を既定の kernel に入れるか。**推し: x86_64 は既定で入れる（QEMU の試験に要る）、arm64・riscv64 は実機で確かめるまで選べるだけ（既定は無し）**。driver が入っても GPU の無い machine では何も増えない。

## 問い 5: Vulkan Video の encode の確かめをどこで行うか

libvulkan にはまだ Vulkan Video の encode がありません（decode だけ）。encode は各 GPU の WS（WS324 RTX の NVENC、WS319 i915、WS322 VF2、WS325 RDNA）が作ります。

- **A（推し）**: この WS は「frame ごとに知らせを受け、Vulkan でその image を読める」口と、QEMU で frame を file に保存する道具（Y4M、software の H.264 の encoder（WS329）が出来ればその H.264）までで受け入れる。Vulkan Video の encode の確かめは、最初に encode が出来た GPU の WS の後に、この WS の最後の Phase（p010）として実機で行う。WS341 の completed はそれを待つ。
- B: A と同じだが、Vulkan Video の encode の確かめを encoder の WS の側に移し、WS341 は p009 までで completed にする。

推し: A。理由: 「Vulkan Video を使える事は要件」という決定があり、実際に encode が通るのを見てから完了とする方が確か。

## 問い 6: build の設定の変更の承認

driver を build に入れるには `mk/kernel.mk`・`mk/config.mk`・`mk/platform-x86_64/defaults.mk`・`config/drivers/` に数行が要ります。Makefile・mk/ は Claude が変えない規則なので、差分の案を [proposed/kernel-mk.diff](proposed/kernel-mk.diff) に置きました（`CONFIG_DRIVER_VGPU`、x86_64 だけ既定 y）。

推し: 承認（実装の Phase p004 の前に）。問い 4b の答えで既定の機種を合わせます。

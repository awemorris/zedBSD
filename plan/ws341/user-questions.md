<!-- awesome-plan project=zedbsd record=ws341-questions -->

# WS341 ユーザーへの問い（p001、2026-11-15 P8）

WS341 は「仮想 GPU の driver の枠組み」: program が `/dev/vgpu` で仮想の画面を作り、compositor がそこへ描いた frame を program が受け取って encode し、遠隔の display や RDP に送る。今は p001（調べと設計）が終わり、設計は [design.md](design.md)。下の答えで設計を確定し、実装の Phase（design.md §13）に進む。各問いに推しを付けた。

用語: **node** は `/dev/gpuN` の一つ一つ。**connector** は display の差し込み口（HDMI の口に当たる）。**head** は compositor が主の画面の他に出す display。**lease** は compositor がある display を使う間の占有。**hold** は program が frame を読む間、その image を compositor に返さない事。

## 問い 1: 仮想の画面を「新しい /dev/gpuN」として作るか、「1 つの仮想 display node の connector に挿す」か

2026-11-01 の決定は「/dev/vgpu の ioctl で /dev/gpuN を作る」形でした。調べると、今の compositor（libvulkan）は起動の時に `/dev` を 1 度だけ読んで display の node の一覧を固定し、後から出来た node を見ません。node が増えた事を知らせる仕組みも kernel にありません。また、program が閉じて node を消そうとしても、compositor が node を開いたままなので、compositor を再起動するまで node が残り、作る度に増えていきます。

- **A（推し）**: 起動時に仮想の display node を 1 つだけ登録し、connector を 4 つ持たせる。`/dev/vgpu` の create は空いた connector に仮想の monitor を「挿す」、program が閉じる（落ちる）と「抜く」。compositor から見ると monitor の抜き差しそのもので、WS353 で入った hotplug の道（kernel の fence → compositor の hotplug の thread → head）がそのまま使える。compositor の変更はほぼ要らない。代わりに、使わない時も connector 0 本挿しの /dev/gpuN が 1 つ居る（GPU の無い machine では作らない）。同時の仮想の画面は 4 つまで。
- **B**: 決定どおり create ごとに /dev/gpuN を作る。kernel に node の増減の知らせ、libvulkan の node の一覧の作り直し（増減に追従）、消した node が compositor の close まで残る事の受け入れが要る。実装の Phase が 2 つほど大きくなる。

推し: A。理由: 利用者に見える振る舞い（compositor に新しい画面が出て、program が終われば消える）は同じで、既存の monitor の抜き差しの道を使うので壊れにくい。

## 問い 2: 誰が `/dev/vgpu` で仮想の画面を作り、frame を読めるか

作れる人は、その画面に出る内容を全て受け取れます（画面の capture と同じ力）。また frame を読むには、program が GPU の render の node（`/dev/gpuN`）も開ける必要があります。今は graphical な login の間、sessiond が `/dev/gpu0`〜`/dev/gpu3` を login した利用者だけ（0600）に渡すので、その間は video group の service でも GPU を開けません。

- **A（推し）**: `/dev/vgpu` を GPU の node と同じ扱いにする。平時は root と video group（0660）、graphical な session の間は sessiond が他の GPU の node と一緒に login した利用者に渡す。画面を共有する program（RDP の server など）は session の中で利用者として動く。login の無い machine（headless の server）では root か video group の service として動く。
- B: root だけ（service は root で動かす。session の間も GPU は root なら開ける）。
- C: session の間も video group の service が使えるよう、sessiond が GPU の node の group の権限を残す（利用者の間の分離が弱まる）。

推し: A。理由: 今の GPU の node の権限の規則と揃い、画面の内容を受け取れるのは画面の持ち主（login した利用者）か管理者だけになる。

## 問い 3: program が compositor より遅い時

compositor は 60 Hz などで描き、program（encoder）が追い付かない事があります。

- **A（推し）**: 最新優先。program は受け取る時に最新の frame を受け取り、間の frame は飛ばす（数は program に伝える）。program が frame を読んでいる間だけ compositor の次の frame の受け付けを待たせるが、上限（8 ms）を過ぎたら待たせるのをやめ、program には「読みの途中で書き換わったかも」と伝える。compositor と desktop は program の遅れや停止で止まらない。
- B: 全 frame を必ず渡す（program が遅いと compositor も遅くなる。program が止まると desktop の表示も止まる）。

推し: A。理由: 遠隔の画面は最新を送るのが普通で、program の不具合で desktop が止まらない事を優先する（program が落ちても compositor は落ちない、という決定の延長）。

## 問い 4: 仮想の画面の使い方と既定

今の compositor は、主の画面の他の display を head として **mirror**（同じ desktop を写す）か **extended**（desktop を広げる）で出します。ただし extended の head には今は壁紙しか出ず、窓を移せません（WS113 の窓の移動の Phase が未）。

- 4a: 仮想の画面の既定の出し方。**推し: 今の displays.conf の規則どおり（無ければ extended）とし、RDP などで作業に使う時は mirror を選ぶ**。窓を仮想の画面へ移すのは WS113 の窓の移動の Phase で扱う（この WS には入れない）。
- 4b: driver を既定の kernel に入れるか。**推し: x86_64・arm64・riscv64 の既定に入れる**（変更ごとに 3 機種で build を確かめるため、QEMU の試験にも要る）。driver が入っても GPU の無い machine では何も増えず、program が画面を挿さない限り compositor に見える物も無い。

## 問い 5: Vulkan Video の encode の確かめをどこで行うか

libvulkan にはまだ Vulkan Video の encode がありません（decode だけ）。encode は各 GPU の WS（WS324 RTX の NVENC、WS319 i915、WS322 VF2、WS325 RDNA）が作ります。

- **A（推し）**: この WS は「frame ごとに知らせを受け、Vulkan でその image を読める」口と、QEMU で frame を file に保存する道具（Y4M、software の H.264 の encoder（WS329）が出来ればその H.264）までで受け入れる。Vulkan Video の encode の確かめは、最初に encode が出来た GPU の WS の後に、この WS の最後の Phase（p010）として実機で行う。WS341 の completed はそれを待つ。
- B: A と同じだが、Vulkan Video の encode の確かめを encoder の WS の側に移し、WS341 は p009 までで completed にする。

推し: A。理由: 「Vulkan Video を使える事は要件」という決定があり、実際に encode が通るのを見てから完了とする方が確か。

## 問い 6: build の設定の変更の承認

driver を build に入れるには `mk/kernel.mk`・`mk/config.mk`・`mk/platform-*/defaults.mk`・`config/drivers/` に数行が要ります。Makefile・mk/ は Claude が変えない規則なので、差分の案を [proposed/kernel-mk.diff](proposed/kernel-mk.diff) に置きました（`CONFIG_DRIVER_VGPU`、x86_64・arm64・riscv64 の既定 y）。試験の program と道具（`userland/tests/vgpu-loop`、試験用の compositor 役、`vgpu-capture`）の Makefile も build system の一部なので、それぞれの Phase（p005・p009）の前に既存の `userland/tests/*/Makefile` と同じ形の差分の案を出します。

推し: kernel-mk.diff を承認（実装の Phase p004a の前に）。問い 4b の答えで既定の機種を合わせます。userland の Makefile は出した時に改めて確かめてください。

## 問い 7: 物理の画面の無い machine で、仮想の画面を主の画面にしてよいか

compositor は主の画面（anchor、窓・bar・pointer が乗る）を 1 つ持ち、他は head です。今は主の画面が失われると「元の画面 → machine 自身の画面 → 他のどれか」へ移るので、仮想の画面が主になりえます。

- **A（推し）**: 物理の画面が 1 つでも繋がっている時は、仮想の画面を主にしない（最初の選択・失われた時の移動とも）。物理の画面が 1 つも無い時（headless の RDP の server）だけ、仮想の画面を主にしてよい。program が終わって主の画面が消えたら、compositor は画面の無い状態で次の画面を待つ（今の「画面が無い」時と同じ）。
- B: 仮想の画面は決して主にしない（headless の server では desktop が出ない。遠隔の画面を使うには物理の画面か、別の仕組みが要る）。

推し: A。理由: headless の server で遠隔の画面を使えるのは目的（遠隔の display・RDP）に合い、物理の画面のある machine では今の振る舞いを変えない。

## 問い 8: 「仮想である」印を compositor に名前で伝えてよいか

2026-11-01 の決定は「compositor からは普通の display に見え、仮想である flag は見える」でした。kernel には出力の flag `GPU_DISPLAY_REMOTE` を足しますが、compositor は規則で Vulkan だけを使い、表示の識別は名前の key（`zedbsd-port-v1:pci:...:edp:A` の形、WS113 の決定で私有の Vulkan 拡張は使わない）で行っています。

- **A（推し）**: 仮想の画面の名前の key を `zedbsd-port-v1:virtual:vgpu:<connector 番号>` とし、compositor は key の種類 `virtual` で見分ける（Settings に「仮想」と出す、主にしない、位置を connector ごとに覚える）。kernel の flag は UAPI を直に使う program（試験・道具）用。
- B: 私有の Vulkan 拡張で flag を compositor に渡す（WS113 の「私有の拡張は不採用」を変える事になる）。

推し: A。理由: 既存の表示の識別の決まりの中で決定 (3) の「仮想と分かる」を満たせ、新しい Vulkan の拡張が要らない。

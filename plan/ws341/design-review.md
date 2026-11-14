<!-- awesome-plan project=zedbsd record=ws341-design-review -->

# WS341 設計の敵対的レビュー（p001）

- 2026-11-15 design-reviewer（P8 が起動、q2112-i01）。対象は [design.md](design.md) 第 1 版（7e7a2be03c）、[proposed/vgpu.h](proposed/vgpu.h)、[proposed/kernel-mk.diff](proposed/kernel-mk.diff)、[user-questions.md](user-questions.md)。根拠は code を読んで確かめた物で、build・試験は流していない。
- P8 は重大 R1〜R4 の根拠（`wsi-swapchain.c` の lane、`wsi-swapchain.c` の linear の usage と `dedicated.c` の SAMPLED の確かめ、`devfs.c` の mode と group、`gpu.c` の `gpu_open` の group の規則）を自分で読んで確かめた。

## 指摘と反映

| # | 重さ | 指摘（要点） | 反映 |
| --- | --- | --- | --- |
| R1 | 重大 | compositor は anchor → head を同じ queue に present し、libvulkan の ordinary の lane の worker は 1 本、present の ioctl の間は process 全体の `display_mutex`。vgpu の head の待ちが anchor の present を遅らせ、物理の画面が frame を落とす。 | hold の上限を 8 ms 固定に縮め、p006 に「display の present を swapchain ごとの worker に、ioctl を `display_mutex` の外に」を入れて L にした。受け入れと試験（p009）に「遅い・止まった program の下で anchor の frame の間隔が変わらない」を足した（§3.3・§8・§12・§13・§14）。 |
| R2 | 重大 | head の swapchain の linear の image は SAMPLED を持たず、libvulkan の image の capability の import（dedicated の確かめ）が断る。program は読めない。p005 の自作の blob の試験では見つからない。 | p006 に「共有の route の linear の image に SAMPLED を足すか、dedicated の確かめを TRANSFER_SRC でも通す」を入れた。program は記述の usage で VkImage を作る事を docs に書く。p005 の compositor 役を libvulkan の本物の swapchain の道にした（§8・§12・§13）。 |
| R3 | 重大 | devfs は gpu<数字> 以外の名前を 0666・wheel で作る。`/dev/vgpu` は誰でも開ける。 | devfs の mode と group の選択に `vgpu` を足し、vgpu の open が `gpu_open` と同じ 3 つの規則を確かめる、と §4.1 に書いた（p004a）。 |
| R4 | 重大 | ログインの間 sessiond は `/dev/gpu0`〜`3` を利用者の 0600 に渡すので、video group の service は render node を開けず frame を読めない。問い 2 の推しが成り立たない。 | 問い 2 を書き直した。推しは「`/dev/vgpu` を GPU の node と同じ扱いにし、session の間は sessiond が利用者に渡す。共有の program は session の中で利用者として動く」。p007 を合わせた（§10・§13）。 |
| R5 | 中 | 登録の待ち方が未定。attaching を数えると GPU の無い machine で sessiond が 120 秒待つ。 | attaching を数えない。core に「render node の登録」の kernel の中の知らせを足し（p003）、vgpu はその driver の attach が終わる前に登録する。試験に GPU の無い image の起動の時間を足した（§3.1 の注・§9・§12）。 |
| R6 | 中 | headless の扱いが抜け。復旧の道で anchor が仮想の画面に移りうる。 | 問い 7 を足した。p008 の範囲を最初の選択・復旧・Settings の 3 か所と書いた（§8・§13）。 |
| R7 | 中 | revoke の後の release と acquire の順、revoke を行う文脈、参照を落とす文脈が未定。 | 「取り上げ済み・未応答」の状態を持ち、その release まで acquire は `EBUSY`。revoke は adoption_wait の thread、参照は眠れる thread の文脈で落とす（§4.3・§5.1・§11、vgpu.h の comment）。 |
| R8 | 中 | adoption_wait の `EBUSY`・`ETIMEDOUT` は libvulkan で NATIVE_WINDOW_IN_USE・DEVICE_LOST になる。 | present・adoption_wait の失敗は `ENXIO`・`ESTALE` だけ。上限では revoke して 0 を返す（§4.3）。 |
| R9 | 中 | 空きの connector の generation 0 は core が `EIO` にし、libvulkan の列挙全体が失われる。 | 全 connector の generation を起動時から 1 以上に（§4.3）。 |
| R10 | 中 | `gpu_close` は resource_destroy を lease の終わりより先に呼ぶ。vgpu の image の寿命が未定。 | image の object を refcount にし、記録と fd の発行がそれぞれ参照を持つ（§4.3 の「image の寿命」）。 |
| R11 | 中 | core の変更点の数え漏れ（import の入口、`gpu_scanout_import` の迂回、validate の 3 か所）。 | §9 を 7 項目に数え直した。 |
| R12 | 中 | venus の export は 4096 を超える image を断る。VGPU_SIZE_MAX 8192 と食い違う。 | v1 の上限を 4096 にした（§5.1、vgpu.h）。 |
| R13 | 中 | 試験の 2 thread は kill -9 を示せない。raw の ioctl の venus の blob は作れるか不明。anchor の間隔・GPU の無い起動・compositor の kill の試験が無い。 | 受け手と compositor 役を別の process に、compositor 役は `VK_KHR_display` の Vulkan の program に、足りない 3 つの試験を足した（§12）。 |
| R14 | 中 | userland の試験の Makefile、devfs、初期化の呼び口が承認・設計から漏れ。 | 問い 6 と kernel-mk.diff の注に userland の Makefile の承認を足し、devfs と初期化の呼び口を §4.1 に書いた。p005 の依存に承認を書いた。 |
| R15 | 中 | 決定 (3)「flag が compositor に見える」から外れているのに問いが無い。新しい flag の名前が VIRTUAL_CLOCK と紛らわしい。 | 問い 8 を足した。flag を `GPU_DISPLAY_REMOTE` に改め、VIRTUAL_CLOCK との違いを書いた（§0・§5.2・§8）。 |
| R16 | 軽 | VGPU_INFO の本文と struct の field が不一致。 | struct に大きさ・refresh の範囲と hold の上限を足した（ILP32・LP64 の layout を gcc -m32・-m64 で確かめた、全 struct 同じ大きさ）。 |
| R17 | 軽 | ioctl の group の列挙が誤り。 | 「他の header の group と重ならない」と改めた。 |
| R18 | 軽 | §14 の未確認 2 件は code で答えが出る。 | §14 を改めた（HOST_SCANOUT の host なら共有の route、cross-device の flag は DMABUF の型で付く）。 |
| R19 | 軽 | refresh_arm の汎用の timer は見当たらない。 | refresh_arm を持たず、core の refresh watcher に任せる（§4.3）。 |
| R20 | 軽 | `max_frame_bytes` = 幅×高さ×4 は stride の padding を考えない。 | 上限の値として返し、これで present を断らないと書いた。 |
| R21 | 軽 | vgpu の claim で text の console の keyboard が奪われる。 | 受け入れ（試験は SSH で操作するので実害なし）。docs の vgpu の節に書く（p002）。 |
| R22 | 軽 | image_id を再利用しない事、epoch に応じない program が memory を持ち続ける事を ABI に。 | vgpu.h の comment に書いた。docs にも書く（p002）。 |
| R23 | 軽 | generic.drivers の既定 n と x86_64 の y が食い違い。arm64・riscv64 が build の確かめから外れる。 | 3 機種の既定を y にした（kernel-mk.diff、問い 4b）。 |
| R24 | 軽 | design.md が未作成の design-review.md に link。 | この file を作った。 |
| R25 | 軽 | Phase の依存と大きさ（p007 は p002 に依らない、p005 は p006 に依る、p006 は L、p004 は分けるべき）。 | p004 を p004a・p004b に分け、依存と大きさを直した（§13）。 |

## 確かめて問題が無かった点（レビューの報告から）

- present の成功で libvulkan は前の front の hold を外す（§3.3 の前提は正しい）。
- display の node の一覧は instance ごとに固定、hotplug の登録は renderer に結んだ全ての display node に付く（§1・§3.1 の記述は正しい）。
- 接続していない output は libvulkan が列挙から外す。head の present の失敗で compositor は落ちず、head を閉じる。
- `GPU_DISPLAY_*` の 4096 は空きの値。

## 未実施

- レビューも P8 も、build・host の試験・QEMU は流していない（p001 は設計だけ）。
- amdrdna・nvrtx が direct chain を取るか、linear の format を持つかは未確認（p006）。

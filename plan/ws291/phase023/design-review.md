<!-- awesome-plan project=zedbsd record=ws291-p023-design-review -->

# ws291-p023 設計の敵対的レビュー（design-reviewer）と反映

- 対象: [design.md](design.md) の第 1 版（commit 2b92488b1、基点 main d1014941e）。レビュー: design-reviewer（2026-10-31、読むだけ、file・make・QEMU・実機は使っていない）。
- 判定: 条件付きで差し戻し。段 A（nvrtx だけ）の方向と事実の確かめはほぼ正確。段 B〜E は失敗の戻り・V1 の scanout の守り・p015 の export の経路・compositor の問い方・
  roundtrip の手順を直してから。R3 の probe を最初に。HAL の変更は無い。
- 反映: 第 2 版（このレビューの後のユーザーの決定と Q1 の interface の決めと合わせて書き直した）。第 2 版で、libvulkan・protocol・compositor・WSI は P2 の持ち分になったので、
  その部分の指摘は design.md §9 の「P2 側との約束」として渡す（Q1 経由）。

## 確かめられた事（レビューの要点）

- §1 の行番号はほぼ全部が基点の code と合う。段 A の block-linear の描画と読みは kind 0 の page で既に動いている（影と mip）。OPTIMAL の image に
  `vkGetImageSubresourceLayout` を呼ぶ所・OPTIMAL を map して LINEAR として読む所は、desktop の app・libvulkan・compositor の範囲には無い（tests/ の全部は未確認）。

## 指摘と反映

| # | 重さ | 指摘（要点） | 反映 |
| --- | --- | --- | --- |
| 1 | 高 | direct の image の作成・export の失敗は `swapchain_group_create` の中で起き、copy の経路に戻らない（wsi-swapchain.c:1139-1141・1518-1520）。`vulkan_memory_image_fd` は失敗を全部 OUT_OF_DEVICE_MEMORY にする（memory.c:643-644）。Wayland の拒否は後から EPROTO | §1.2 に事実を書き、§9 の 3 で P2 への約束（errno の写しと group の作り直し）にした |
| 2 | 高 | §6.3（usage に COLOR_ATTACHMENT が無いと拒む）と §9.1（どの usage でも direct）が食い違う。xserver の swapchain は TRANSFER_DST だけ（xserver/vulkan.c:564） | 並びを明示の値と私用の record で決める形にし、usage に依らなくした（§6.1）。record の無い時の既定だけが usage を見る |
| 3 | 高 | V1 の game mode で OPTIMAL の buffer は ENOTSUP で断られず、pitch 0 の誤った画面が黙って出る（share.c:155、present.c:838、gpu.c:4308-4310、scanout-zedbsd.c:271） | §7.5: nvrtx の `share_import` が SCANOUT を断る、present が stride 0 と記録との食い違いを断る。`scanout-zedbsd.c` の直しは P2 へ（§9 の 6） |
| 4 | 高〜中 | p015 は貸す image を `vkGetMemoryFdKHR`（allocation の capability、記述なし）で出す計画で、第 1 版 §14 の取り決めが使えない。「app は気づかない入れ替え」は memory の付け替えになり得るが nvrtx は 2 回目の bind を断る | §1.2 に事実、§9 の 4 と R4 で P2 の設計の判断として渡した |
| 5 | 中 | 段 A で、色の attachment を詰めた buffer に読み出す app（notes・libbrowser・libglesv2・WSI の CPU の経路）が、1 画素 4 B の BAR1 越しの copy になって遅くなる | §1.1 に事実、§6.2 で CPU の copy を 16 B の piece ごとにし、host の照合と desktop の確かめに notes・browser を足した |
| 6 | 中 | 段 A の受け入れに合成した desktop の画素の照合が無い | §10.3 に glass・blur・notes・browser の場面の keiland-shot の画素差 0 を足した |
| 7 | 中 | compositor の問い（`vkGetPhysicalDeviceImageFormatProperties2`）は nvrtx に処理が無く、OPAQUE_FD の意味とも混ざる | 第 2 版で compositor の問いは `GPU_IMAGE_LAYOUTS`（ioctl 46、libvulkan 経由、P2）に置き換わった。第 1 版の §8.2 は削った |
| 8 | 中 | 版 4 の event は今の 1 回の roundtrip に重ねられない（wsi-wayland.c:1200・1239） | §1.2 に事実、§9 の 5 で P2 への約束にした |
| 9 | 中 | deviceLUID の案の欠け（deviceNodeMask、Venus と Windows の host の LUID の上書き） | 案を取り下げた（§9 の 7）。device は `GPU_IMAGE_LAYOUTS` と fd で問う |
| 10 | 中 | 「width・height・format・usage で決まる」は他の GPU に広げる時に定義が足りない（UNORM と SRGB、create の flags） | 不透明な OPTIMAL をやめ、明示の値と `layout_parameter` で並びを名指す形にした（§4） |
| 11 | 中 | R3（kind 0 の block-linear を display が読むか）を一番前に。段 E の効果は 1 frame 約 29 µs と正直に | §8 の I2 を最初の段の一つにした（I1 と独立）。効果は §0.2 の 6。ただし game mode で影を出さない（subs が約 11 に戻らない）事が主の効果で、§0.2 の 5 に書いた |
| 12 | 低〜中 | bind の多重の守りは export する側の食い違いを見つけない | §7.4: export する側の最初の bind の並びを backing に記録し、export の記述と比べる |
| 13 | 低 | `gpu-buffer-zedbsd.c` は `uapi/gpu.h` を include できない（v1-check） | §1.4 に事実、§9 の 5 で P2 への約束 |
| 14 | 低 | 内部の矛盾（direct で待ちが増える R4、§8.2 の最後） | 第 1 版の R4 と §8.2 を削った |
| 15 | 低 | compositor の device が変わると OPTIMAL の buffer の取り込み直しが EPROTO | protocol は P2 の持ち分になったので、§9 で P2 の判断に委ねる（第 2 版の本文から該当の約束を外した。device の切り替えが実際にあるかは未確認のまま） |
| 16 | 低 | 事実と数の誤り（行の丸め、UAPI の値の数、LW の合計、nouveau の版） | §1.1 に 256 B と 64 B の丸めを書き、§6.3 の大きさを直した。UAPI の値は §4 に全部列挙。LW の合計は §11 で数え直した（19 LW）。nouveau は nvrtx の既存の出典と同じ v6.19 に揃えた |
| 17 | 低 | 段 D の fps の受けは揺れに埋もれる。QEMU は LINEAR の側しか試せない | §10.3 の主の受けを構造（PROF の影の quad 0）にした。WSI の選び方の host 試験は P2 の部分 |
| 18 | 低 | core は OPTIMAL の present の範囲を確かめられない（stride 0 で 0 B） | §5.2 で present の範囲は driver が記録の bytes で確かめる契約にし、§7.5 で nvrtx が stride 0 を断る |

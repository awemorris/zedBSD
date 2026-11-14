<!-- awesome-plan project=zedbsd record=ws341-p001 -->

# ws341-p001: 調べと設計

Status: in-progress（2026-11-15 P8、q2112-i01。設計と design-reviewer の反映まで済み。ユーザーの答え（user-questions.md の問い 1〜8）待ち）
Parent: [WS341](../ws.md)。

## 目的

ws.md の「p001 で決める事」に答える設計を作る: `/dev/vgpu` の ioctl、作った仮想の画面が既存の GPU の UAPI にどう見えるか、program が閉じた時の hot-plug の抜き、仮想の印、frame の通知と image への access、compositor から見た扱い（WS353 の hotplug の thread）、HAL の要否、UAPI の追加、試験の形、実装の Phase の表。実装はしない。

## 成果

- [design.md](../design.md)（第 2 版）、[design-review.md](../design-review.md)、[user-questions.md](../user-questions.md)、[proposed/vgpu.h](../proposed/vgpu.h)（UAPI の案）、[proposed/kernel-mk.diff](../proposed/kernel-mk.diff)（build の設定の差分の案、承認待ち）。

## 結果（2026-11-15 P8、基点 main 44bb7cdaac、branch `agent/p8-ws341`）

- 乖離の確かめ: design.md §1。ws.md の前提のうち、node の発見（libvulkan の一覧は instance ごとに固定）、image の共有の道（allocation の形でなく resource の形）、cross-device の scanout import が source の物理の page を要る事、WS318 の canceled と libvulkan に encode が無い事、sessiond の GPU node の渡しが 0〜3 だけな事を今の main に合わせた。
- 設計の要点: vgpu は物を持たない display controller。推しは起動時に 1 つの仮想 display node（connector 4 つ）を登録し、`/dev/vgpu` の create が connector に挿す形（問い 1）。image の capability を持つだけで page を読まず、program は frame ごとに fd を受け取り自分の render node で Vulkan で読む。枠組み（`include/drivers/gpu/gpu-scanout.h`）に page を要らない import の op を足す。HAL の API は変えない。UAPI は新しい `vgpu.h` と出力の flag `GPU_DISPLAY_REMOTE`。
- design-reviewer: 重大 4・中 11・軽 10。全てに反映か扱いを付けた（design-review.md）。重大の根拠は P8 が code で確かめた。
- 確かめた事: proposed/vgpu.h の構造の大きさが gcc の -m32 と -m64 で同じ（56・56・16・40・48・136・24、vgpu_frame の image は offset 72）。
- 未実施: build・QEMU（設計の Phase なので対象外）。amdrdna・nvrtx の共有の route は p006 で確かめる。

## Q1 への報告（範囲の外）

- docs の乖離: `docs/reference/gpu-display.md` に `GPU_DISPLAY_FENCE`・`GPU_DISPLAY_FENCE_CANCEL` の記述が無い。`docs/reference/gpu.md` の「Who may open」の段落が自己矛盾（group を認める、と、root と owner だけ、の両方）。
- 実装は sessiond（p007）・compositor（p008）・libvulkan（p006）に触れる。他の WS（WS353 の p016 など）と file が重なりうる。

## 受け入れ

- design.md・design-review.md・user-questions.md が揃い、ユーザーが問いに答え、設計を承認した時に cleared。

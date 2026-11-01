<!-- awesome-plan project=zedbsd record=ws341 -->

# WS341 仮想 GPU の driver の枠組み（scanout だけの仮想の display controller、user の program が作り、Vulkan Video で encode して USB・network の遠隔の display や RDP に送る）

- Master: [plan/master.md](../master.md)（2026-11-01 22:39 作成）。Primary Milestone: MG006。
- Status: planning（担当は未割当）
- 目標: kernel に仮想 GPU の driver の枠組みを足す。user mode の program の要求で `/dev/gpuN`（例 `/dev/gpu3`）のような追加の GPU を作れる。その GPU は scanout だけを持つ display controller 相当。program は Vulkan で scanout 用の image を作り、他の GPU がそこへ描く（zedBSD は copy を避けるので主に直の描画、copy は例外）。program は Vulkan Video でその image を H.264・H.265 に encode し、USB や network で送って、遠隔の display や RDP の session に使う。

## ユーザーの決定

- （2026-11-01）ユーザーから、上の目標の仮想 GPU の driver の枠組みを kernel に足す WS を立てる、という趣旨の指示を受けた。仮想の GPU は scanout だけの display controller 相当で、user mode の program が要求して作る。scanout の image は Vulkan で program が作り、他の GPU が描画（なるべく copy でなく）する。program は Vulkan Video で H.264・H.265 に encode し、USB・network で送って遠隔の display や RDP の session に使う。

## 関係する WS

- encode の hardware: [WS324](../ws324/ws.md)（RTX の NVENC、設計中）、[WS319](../ws319/ws.md)（i915 の VDEnc）、[WS322](../ws322/ws.md)（VF2 の Wave420L）、[WS325](../ws325/ws.md)（RDNA の VCN）。software の encoder: [WS329](../ws329/ws.md)（H.264）、[WS330](../ws330/ws.md)（H.265）。
- GPU の枠組みと compositor: `/dev/gpu` の UAPI（docs/reference/gpu.md）、Keiland の compositor（WS309 の thread の構成、game mode の VK_KHR_display）、[WS306](../ws306/ws.md)（GPU の driver の枠組みの UAPI）。
- 画像の共有: WS243 の NVDEC の export・import（GPU_CAP_ALLOCATION_SHARE）と同じ道で、他の GPU の描画の先にできるか。

- （2026-11-01 22:44）ユーザーから、次の趣旨の決定を受けた: (1) 仮想 GPU は /dev/vgpu の driver への ioctl で作り・消す。(2) 画面の大きさは /dev/gpuN を作る ioctl で指定する。(3) compositor からは普通の display に見え、仮想である flag は見える。(4) scanout の image は system の GPU の driver で作る（仮想 GPU は自分の memory を持たない）。(5) 遠隔の mouse・keyboard は含めない。(6) 転送の protocol はこの WS では扱わず、encode した結果を file に保存できればよい。
- （2026-11-01 22:44）frame の同期は、ユーザーの案（compositor が frame の描画の終わりを明示する）を Q1 が次の形に具体化して提案した（p001 の設計の出発点、決定は設計の受け入れで）: compositor の flip に描画の完了の fence を付けて渡すのを終わりの明示とする。encoder の program は /dev/vgpu の「次の frame を待つ」ioctl を専用の thread で待ち（fd の event の多重化は使わない）、encode し終えたら image を返す（返すまで compositor は再利用しない）。実の vblank の代わりに、作る時の refresh で仮想 GPU が vblank を出す。

## p001 で決める事（問いの候補）

- /dev/vgpu の ioctl の形（作る・消す・mode・次の frame を待つ・返す）、作った program の fd を閉じた時の扱い、権限（video group との関係）、HAL を変えずに済むか（変えるなら理由）。
- 仮想の flag の UAPI と compositor の扱い（普通の display として出すか、どこまで違えるか）、EDID 相当の情報、hot-plug の見せ方。
- system の GPU の driver で作る scanout の image を、仮想 GPU と encoder の Vulkan の device とでどう共有するか（同じ GPU・別の GPU、WS243 の export・import の道）。
- encode の結果を file に保存する形（H.264・H.265 の elementary stream か、mediafile の MP4 か）と、試験の道（QEMU・実機、encoder の有る GPU と software の encoder）。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws341-p001 | 調べと設計（上の問い、UAPI の案、compositor と encoder との結び、試験の道）、design-reviewer | planning | — |

## Resume point

- 2026-11-01 Q1: 作成。p001 の担当の割当を待つ。

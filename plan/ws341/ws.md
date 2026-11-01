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

## p001 で決める事（問いの候補）

- 仮想 GPU の作り方・消し方の口（UAPI、権限、video group との関係）と、HAL を変えずに済むか（変えるなら理由）。
- 仮想 display の mode（解像度・refresh・台数）を誰が決めるか、EDID 相当の情報、hot-plug の見せ方（compositor から見て普通の display か）。
- scanout の image の持ち主（program の Vulkan の device か仮想 GPU か）と、描く GPU との共有（同じ GPU・別の GPU、cross-device の import、copy が要る場合）。
- 1 frame ごとの同期（vblank の代わりの時計、flip の完了、program が encode を始める合図）。
- 入力（keyboard・mouse・touch）を遠隔から戻すか、この WS の範囲か。
- 転送の protocol（USB の何の class か、network は RDP・独自か）がこの WS の範囲か、別の WS か。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws341-p001 | 調べと設計（上の問い、UAPI の案、compositor と encoder との結び、試験の道）、design-reviewer | planning | — |

## Resume point

- 2026-11-01 Q1: 作成。p001 の担当の割当を待つ。

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
- （2026-11-01 22:50）frame の同期と program の扱い: ユーザーから次の趣旨の決定を受けた。仮想 GPU の user mode の program は Vulkan Video で encode するとは限らず、VNC のような algorithm を使う事もある。そのため決めるのは「frame のたびに通知を受け、その image に access できる」事だけ。その時に Vulkan Video を使える事は要件。Q1 の前の案（compositor の flip に描画の完了の fence を付けて終わりの明示とする、program は専用の thread で次の frame を待つ ioctl、使い終えたら image を返す、作る時の refresh で仮想の vblank）は p001 の設計の出発点として残すが、encode の手段（Vulkan Video・CPU の差分の符号化など）を前提にしない。
- （2026-11-01 22:50）/dev/vgpu の ioctl の形にはこだわらない（p001 で決める）。program が /dev/vgpu を閉じた時（終わった時・落ちた時を含む）は /dev/gpuN が正常に消え、compositor は落ちない。display の hot-plug（抜き）と同じ扱い。
- （2026-11-01 22:50）system の GPU で作った image を program の Vulkan の device と共有する件は、Vulkan Video で読むなら特に考えなくてよい（同じ GPU の device で開く前提）。

## p001 で決める事（問いの候補）

- /dev/vgpu の ioctl の形（作る・消す・mode・frame の通知の待ち・image への access・返す）、権限（video group との関係）、HAL を変えずに済むか（変えるなら理由）。
- program が閉じた・落ちた時に /dev/gpuN を消す流れと、compositor が hot-plug の抜きとして落ちずに扱う流れ（今の display の hot-plug の扱いとの差）。
- 仮想の flag の UAPI と compositor の扱い、EDID 相当の情報。
- frame の通知と image への access を、Vulkan Video でも CPU（VNC のような差分の符号化）でも使える形にする（CPU からの読みの道、変わった領域の情報を渡すか）。
- encode・符号化の結果を file に保存する試験の道（H.264・H.265 の elementary stream、encoder の有る GPU と software の encoder、QEMU・実機）。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws341-p001 | 調べと設計（上の問い、UAPI の案、compositor と encoder との結び、試験の道）、design-reviewer | planning | — |

## Resume point

- 2026-11-01 Q1: 作成。p001 の担当の割当を待つ。

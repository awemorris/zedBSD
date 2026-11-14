<!-- awesome-plan project=zedbsd record=ws339 -->

# WS339: x86 の PC の UEFI の起動の画面の解像度を monitor に合わせる

- Master: [plan/master.md](../master.md)（2026-11-01 作成）。Primary Milestone: MG004。
- Status: planning（2026-11-01 09:16 新設）
- 目標: amd64 の UEFI の loader（`bootloader/uefi/video.c`・`bootx64.c`）が、GPU の driver が動く前の画面（起動の splash と文字の console）の mode を、monitor の最適な解像度（EDID の preferred timing。`EFI_EDID_ACTIVE_PROTOCOL`、無ければ `EFI_EDID_DISCOVERED_PROTOCOL` で読める時）を第一に選ぶ。読めなければ今と同じ（logo の時 1920x1080 を好み、無ければ firmware のまま）。標準の起動の設定に `video=640x480` を置かない。
- 前提の直し（2026-11-15 P5）: 作成時の `platform/amd64/zedbsd.cfg`・`zedbsd-native-uefi.cfg` は古い path。今の標準の設定は `mk/platform-x86_64/bootcfg/zedbsd-uefi.cfg`・`zedbsd-bios.cfg` で、どちらにも既に `video=` は無い。残るのは参照の無い `zedbsd-loopback.cfg` だけ（扱いは p001 の案、mk の下なのでユーザーの判断）。
- 範囲の外: desktop の解像度（GPU の driver が決める）、amd64 以外の platform。
- 受け入れ: QEMU（OVMF）で、EDID の protocol が無い firmware で今と同じ動きになる事（boot-test.sh の login の確かめ）。EDID から大きさを選ぶ道は host 試験と 5330 で、EDID の最適な解像度で splash と console が出る事。この host の OVMF はどの display にも EDID の protocol を出さない（p001 で確かめた）ので、QEMU では EDID の道を確かめられない。
- Resume point: p001 は実装・host 試験・QEMU 済み（Q1 の cleared 待ち）。次は p002（5330 の受け入れ、実機の割当は Q1）。

## ユーザーの決定（要点）

- 2026-11-01 09:16 ユーザーから、起動の画面の解像度は monitor に合わせる（EDID を第一にし、`video=640x480` を外す）、という趣旨の答えを受けた（Q1 の推しの案）。

## Phase

| ID | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | loader の EDID による mode の選び方、標準の設定の video= の確かめ、host 試験と QEMU | in-progress（2026-11-15 P5 q2119、実装・試験済み、cleared は Q1） | — |
| p002 | 5330 の受け入れ（GOP が EDID の protocol を出すか、内蔵の panel の preferred の大きさで splash と console が出るか） | planned | p001 |

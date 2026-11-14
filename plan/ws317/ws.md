<!-- awesome-plan project=zedbsd record=ws317 -->

# WS317 hotplugd: hot-plug の device の通知・承認・登録と driver の開始と終了（volumed を置き換える）

- Master: [plan/master.md](../master.md)（2026-10-30 作成）。Primary Milestone: MG005。
- Status: planning（2026-10-30 Q1 が作成。今は取り組まない。2026-11-15 設計の変更が要る、と記録）
- 目標: PCIe の hot-plug（USB4・Thunderbolt の tunnel を含む）と USB の device とディスクを 1 つの daemon（hotplugd）で扱い、通知・承認・拒否・
  登録と削除・driver の開始と終了を行う。Keiland の compositor は hotplugd と話し、承認の選択を利用者に出す。

## ユーザーの決定（要点）

- 2026-10-30 ユーザーから、次の趣旨の指示を受けた（原文は載せない）:
  1. PCIe の device を自動で使い始めると host の memory に触れる懸念がある。volumed を置き換え、PCIe の hot-plug とディスクを含めて、通知・登録・承認・削除・driver の開始と終了を 1 つの daemon で行う。Keiland の compositor の volumed との通信をこれに替え、attach の通知で承認・拒否などの選択を出す。PCIe の登録は再起動で消え、再登録が要る。USB のディスクは起動の partition 以外は承認が要る。
  2. USB の keyboard・mouse は承認なしで自動で attach。USB のディスクは attach は自動、mount は承認制。「今後は自動」は無くてよい。再起動で自動の承認は消える。名前は hotplugd でよい。
  3. 方針を WS に記録し、今は取り組まない。
- 2026-11-15 ユーザーから、hotplug の扱いが compositor の側に入ったので、この WS は設計の変更が要る、記録だけしておく、という趣旨の指示を受けた。

## 決まった方針（Q1 のまとめ、2026-10-30 時点。上の決定が正）

- 名前: hotplugd（volumed を置き換える）。compositor の volumed との通信を hotplugd との通信に置き換える。
- 既定は拒否を kernel が守る: 承認の前は device を見つけるだけで driver を付けず、PCIe は bus master を切ったまま（hotplugd が落ちていても、起動の途中・ログインの前でも安全）。
  IOMMU（VT-d）で承認の後の DMA も driver の buffer に限る。
- 起動の時からある内蔵の device（hot-plug でない slot）は自動。承認を要するのは hot-plug の口の下（USB4・Thunderbolt の tunnel の root port、ACPI の外向きの口）の PCIe と、USB のディスク。
- USB の keyboard・mouse（HID）は承認なしで自動で attach。
- USB のディスク: attach（driver・block device）は自動、mount は承認制。起動した media（起動の partition、live の USB）は自動。
- 選択は 承認・拒否（「今後は自動」は無くてよい）。承認は再起動で消える（PCIe も USB も再登録が要る）。
- 承認できるのは画面の前でログインしている利用者。lock の間とログインの前は保留して解除の後に出す（Q1 の案、未確認）。
- USB4・Thunderbolt の security level（承認してから tunnel を張る）はこの承認に乗る（[WS316](../ws316/ws.md)）。

## Phase

| Phase | 目的 | 状態 | 依存 |
| --- | --- | --- | --- |
| ws317-p001 | 設計: kernel の既定の拒否（driver の attach の保留、bus master、IOMMU）、hotplugd の protocol、compositor の承認の UI、volumed からの移行 | planning | — |

## Resume point

- 2026-11-15 Q1: **設計の変更が要る**（ユーザーの指摘）。この WS を立てた後、compositor と seat の側に hotplug の扱いが入った: compositor の display の hotplug の thread（WS353 p004、kernel の hotplug の fence は p002・p003）、sessiond が入力の device を開き compositor に渡す形（WS353 p018・p020、試験 seat-hotplug・seat-owner）。上の「決まった方針」の「compositor の volumed との通信を hotplugd との通信に置き換える」と、承認の UI・device の開き方の分担は、着手の時にこの今の形に合わせて設計し直す。今は取り組まない（記録だけ）。
- 2026-10-30 Q1: 方針を記録。ユーザーの指示で今は取り組まない。

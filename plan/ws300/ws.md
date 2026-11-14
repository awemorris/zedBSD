<!-- awesome-plan project=zedbsd record=ws300 -->

# WS300 NVMe と VMD の低電力（S0i3 の時の D3 と、動作中の APST・ASPM）

- Master: [plan/master.md](../master.md)（2026-10-27 作成、q1615 P1 の計画）。Primary Milestone: MG003。Related: MG004、[WS052](../ws052/ws.md)（S0ix）、[WS298](../ws298/ws.md)（VMD）、[WS299](../ws299/ws.md)（NVMe の速さ）、[BUG-424](../bugs/BUG-424.md)（VMD の suspend・resume）。
- Status: incomplete（2026-11-15 P5 q2114-i01: 記録を main に移し design 第 3 版。p002 は BUG-424 で置き換わった。p003（HMB の停止・時間の log）と p005 の APST（UAPI 無し）を main の上に実装。p004 と UAPI はユーザーの判断待ち。5330 は BUG-428・429 の後。以前: 2026-10-29 P6 の branch で p001〜p005 を実装（merge されず）、2026-10-27 新設）
- 出典（要点）: 2026-10-27 ユーザーから、NVMe と VMD が S0i3 などの低電力に対応する WS を立て（まず計画、後で順に実装）、S0ix が動いた実績が無いのでそれにも取り組むよう指示を受けた（S0ix 自体は既存の WS052 で、Q1 の割り当て）。
- 全体の依存と着手の順: [plan/agents/P1/pm-plan.md](../agents/P1/pm-plan.md)。設計: [design.md](design.md)（第 3 版）、review: [design-review.md](design-review.md)（第 1 版に対する）。

- 2026-10-27 09:39 ユーザー（クリック）: 動作中の省電力（NVMe の APST、PCIe の ASPM L1.2・LTR）も入れる（docs/architecture/power-management.md の「runtime の省電力は対応外」を直す）。
- 2026-11-15（Q1 経由、q2114）: main の上で作り直して進める。新しい .c は mk の変更（ユーザーの承認）が要るので既存の file に収める形を先に選ぶ。Q300-2（状態を見る口の UAPI）・Q300-6（`_OSC` を見ずに ASPM を設定してよいか）はユーザーの判断待ちで、p004 と UAPI は手を付けない。

## 単一の到達目標

5330 で NVMe から起動した zedBSD が、(1) S0 idle（WS052）の時に VMD とその下の root port・NVMe を suspend して resume でき（BUG-424 で devices の往復までは 5330 で通った）、
S0i3 の妨げにならない状態（NVMe は D3、link は L1.2 か L2 相当）に置き、(2) 動作中の idle では NVMe が APST で自ら低い power state に移り、link が ASPM L1・L1 substates に入る。

## 今の前提（2026-11-15 P5、main 5202274642 で確かめた。詳細は [design.md](design.md) §1）

- VMD・PCI の共通: [BUG-424](../bugs/BUG-424.md)（debec3781b・295eea4623）で VMD の suspend・resume、driver の無い root port の config の保存・復元、upstream で数える suspend の順、
  restore の LnkCtl の ASPM を L1SS の後に、が main に入った。5330 の `sleepctl devices` の往復は成功（BUG-424 の b424d）。S0 idle に入って戻る事は未達（[BUG-428](../bugs/BUG-428.md) xHCI の port 3、[BUG-429](../bugs/BUG-429.md) TCO の watchdog）。
- NVMe: suspend は SHN → disable → D3hot、resume は全部の初期化と HMB の Memory Return つきの渡し直し（WS299 の HMB は main にある）。第 3 版で suspend の前の HMB の停止と APST（opt-in）を足した（p003・p005）。
- PCI の共通に ASPM・L1SS・LTR を設定する code は無い（save と restore だけ）。D3cold は ACPI の `_PS3` の側だけで、VMD の下の device に ACPI の node は無い。runtime PM の汎用の枠組みは無い。
- build: top の Makefile と `mk/`。新しい .c は mk の変更が要る（ユーザーの確認）。WS300 の計算は header の inline に置く。
- docs: `docs/architecture/power-management.md` は NVMe を「deepest power state」と書くが、実際は SHN＋D3hot。「idle の device の runtime の省電力」は対応外と書いてある。範囲を変えるなら文書も（ユーザーの review、p006 の後）。
- 2026-10-27 の調べ（`vmd_suspend` が常に EBUSY、HMB が無い、など）は古い。git の履歴にある。

## 範囲

- 含む: VMD の suspend・resume（root port の bus の番号・窓・bridge の control、VMCONFIG、MSI-X の付け替えの復元、子の順序）、NVMe の S0 idle の時の状態（D3hot か
  NVMe の最低の power state、`StorageD3Enable` に当たる platform の要求の扱い）、ASPM L1・L1.1/L1.2・LTR の設定（VMD の下の link、BIOS の値の扱い）、
  NVMe の APST（動作中）、NVMe の電源の状態を見る口（今の PS、APST の表、link の状態）。
- 含まない: S0ix の全体の流れと residency の読み（WS052）、NVMe の速さ（WS299）、他の PCIe device の ASPM の一般化（必要なら別の WS）、D3cold の電源を切る制御（5330 の VMD の下に ACPI の口が無い。p001 で要否を確かめ、要れば別に）。

## 制約

- 値と手順: NVMe 仕様（APST・Power State Descriptor）、PCIe 仕様（ASPM・L1 PM Substates・LTR）、Intel の VMD・ADL の資料、Microsoft の modern standby の storage の要件（公開文書）を優先。
  Linux（GPL）の vmd.c・nvme の値と手順は参照だけ、code は写さず block ごとに Value(s)/Procedure referenced。
- HAL の API を変えるなら差分ごとの事前承認。UAPI（状態を見る口を `/dev/system` か sysctl に足す）は別の明示の決定。
- 試験: host 試験（VMD の保存・復元の計画、APST の表の組み立て）、5330 の実機（`sleepctl devices` の往復、`sleepctl s0idle`、APST の遷移の数・時間、
  `/tmp/i915-hw.lock`、遠隔の電源は無いので止まったら Q1 → ユーザー）。QEMU の nvme で APST の Set Features の往復（QEMU が対応する範囲）。
- WS299 と `pci-nvme.c`・`pci-vmd.c` を共有する。WS299 の p003・p004（転送と queue・vector）の後に WS300 の p003 以降を当てる（順は pm-plan）。WS300 が変えた `pci-nvme.c` の関数は phase003・phase005 の「WS299 との重なり」に書く。

## 受け入れ（案）

1. 5330（NVMe から起動）で `sleepctl devices` の往復が 20 回続けて成功し、root の UFS の読み書きが続く（VMD・root port・NVMe の suspend・resume）。
2. WS052 の `sleepctl s0idle` が VMD で中止されない（S0ix の residency の受け入れ自体は WS052）。S0 idle の間の NVMe の状態（D3 か最低の PS）と link の状態を記録で示す。
3. 動作中に idle の NVMe が APST で非操作の power state に入る事を数で示し（遷移の数か滞在、または PMC の LTR・link の状態）、読み書きの速さが WS299 の値から落ちない（落ちるなら閾値の調整）。
4. 規約の全文、build warning 0、5330 の素の起動の回帰（`5330-run.sh` PASS）。

## Phase の表（2026-11-15 P5、q2114-i01。cleared は Q1）

| Phase | 内容 | 状態 | 依存 | LW |
| --- | --- | --- | --- | --- |
| [ws300-p001](phase001/phase.md) | 調査と設計（survey 2 つ、design 第 2 版と review、第 3 版で main に合わせた） | cleared（2026-11-15 Q1。設計の第 3 版で今の main に合わせた） | WS298（済） | 5 |
| [ws300-p002](phase002/phase.md) | VMD の suspend・resume、suspend の順 | canceled（2026-11-15 Q1。BUG-424 の debec3781b・295eea4623 で置き換わった。root port の D3hot と pci.vmd.d3=force の要否は p006 の 5330 の後） | p001 | — |
| [ws300-p003](phase003/phase.md) | S0 idle の NVMe: SHN＋D3hot のまま、shutdown の前に HMB を止める、suspend・resume の時間の log | cleared（2026-11-15 Q1、e952eadf2c を main に merge dbb0415a3f。x86_64・riscv64 の build warning 0、nvme-hmb-test PASS、QEMU の suspend の往復 2 回 PASS。5330 の BG5 での確かめは p006） | BUG-424、WS299 の HMB | 3 |
| [ws300-p004](phase004/phase.md) | ASPM L1・L1 substates・LTR（VMD の下の link、既定 off、`pci.vmd.aspm=on`） | planned（Q300-6 の判断待ち） | BUG-424、Q300-6 | 5 |
| [ws300-p005](phase005/phase.md) | APST（Set Features 0Ch、表、閾値、既定 off で `nvme.apst=on`）と状態を見る口（UAPI は Q300-2 の後） | cleared（2026-11-15 Q1、merge dbb0415a3f。APST（既定 off、nvme.apst=on）を header の inline と pci-nvme.c に、nvme-apst-test PASS（壊す 6 通りを捕捉）、build warning 0。状態を見る口（UAPI）は p005b に分けた） | p003 | 4 |
| ws300-p005b | NVMe の電力の状態を見る口（UAPI） | planned（Q300-2 の判断待ち、2026-11-15 Q1 が p005 から分けた） | p005、Q300-2 | 1 |
| ws300-p006 | 5330 の受け入れ（WS052 の s0idle と一緒に 1 本の script、APST の log、速さの回帰） | planned（BUG-428・429 の後、Q1 の割当） | p004、p005、WS052 p014 | 2 |
| ws300-p007 | 規約の全文の見直し | planned | p006 | 2 |

## 判断が要る点

- 範囲: 「動作中の APST・ASPM」を入れると docs/architecture/power-management.md の「runtime の省電力は対応外」を変える（ユーザーの review）。
- S0 idle の時の NVMe を SHN＋D3hot（今）のままにするか、Microsoft の要件に合わせた形にするか: 推し（Q300-1）は SHN＋D3hot＋HMB の停止（p003、design §4.2）。p006 の測りで residency が出なければ再検討。
- 状態を見る口（UAPI）の置き場所（Q300-2、ユーザーの判断待ち）: 推しは read-only の sysctl（design §8）。WS301 の決定と揃える。
- `_OSC` を評価せずに ASPM を OS が設定してよいか（Q300-6、ユーザーの判断待ち）。p004 はこの後。
- 他の推しで決めた事（Q300-3〜5）は design §13。

## Resume point（2026-11-15 P5、q2114-i01）

- 2026-11-15 Q1: p001・p003・p005（APST）cleared、p002 canceled、UAPI は p005b に分けた（Q300-2 待ち）。p004（ASPM）は Q300-6 待ち、p006 は 5330（BUG-428・429 の後）。system の shutdown（nvme_shutdown）も HMB を止めずに SHN する点は p006 の前に扱いを決める。

- worktree `p5-ws300`（branch `agent/p5-ws300`、基点 main 5202274642）。p003 の code 4966ba2a1a、p005 の APST 1f22d22af8・2fd8d9f591（`pci-nvme-protocol.h`・`pci-nvme.c`、host 試験 `tests/host/nvme-apst-test.c`）、QEMU の image の config `tests/config-amd64-sleep.mk`。
- 次: (1) Q1 の判断: p002 の canceled、p003・p005（APST）の cleared の可否。(2) ユーザーの判断 Q300-6 の後に p004 を main の上で作り直す（計算は header の inline、design §4.3）。Q300-2 の後に p005 の UAPI。
  (3) p006（5330: BUG-428・429 の後、WS052 p014 の script と一緒に、`nvme.apst=on` あり・なし、`nvme: suspended in N ms`、S0 idle の PC の滞在。root port の D3hot の要否もここで）。p006 の PASS の後に APST・ASPM の既定を on に。(4) p007。
- 第 2 版の branch `agent/p6-ws300` は merge しない（p002 は BUG-424、p003〜p005 は main の上で作り直した・作り直す）。消してよいかは Q1。

<!-- awesome-plan project=zedbsd record=ws300-design-review -->

# WS300 design.md 第 1 版の敵対的レビュー（design-reviewer、2026-10-29）

- 2026-11-15 P5（q2114-i01）の注: merge されなかった branch `agent/p6-ws300` から main に移した。指摘の対象は第 1 版の設計と branch の code（SHA は branch の物）。
  VMD の指摘（R3〜R6・R18・R20・R22）は BUG-424 の実装で扱いが変わった。今の扱いは [design.md](design.md) §4.1・§15・§16。

- 対象: [design.md](design.md) 第 1 版（worktree `p6-ws300`。レビューの開始時の HEAD ef1fda1d6）。入力: [ws.md](ws.md)・[survey-5330.md](survey-5330.md)・[survey-fable.md](survey-fable.md)、
  `src/drivers/pci/pci-power.c`・`pci-vmd.c`・`pci-vmd-plan.c`・`pci-nvme.c`・`pci.c`、`include/drivers/pci/pci.h`・`pci-vmd.h`・`pci-nvme-protocol.h`、`src/kern/sleep.c`。
- レビューの途中で実装の commit が入った（5f9f45362・d86fe0c5d・83b998e5e）。下では設計の文を対象にし、実装を確かめた所だけ「実装」と書く（実装の全体のレビューはしていない）。
- build・QEMU・web の取得はしていない。PCIe Base・ACPI の仕様の本文はこの場で読めていない。仕様の bit・規則は記憶で照合し、記憶に依る所は「推測」か「記憶」と書く。
- 重大度: 重大（受け入れ・起動・データを壊しうる、前提が誤り）、中（特定の条件で壊れる、直さないと p006 で詰まる）、軽（文の誤り・記録・小さな欠け）。

## 集計

重大 2、中 6、軽 15。

## 指摘

### R1（重大）§2・§4.3・§13 Q300-4: 「Debian でも ASPM・L1SS が切れているのは firmware が設定しないから」という前提が確かめられていない。OS が ASPM を触ってよいかの確認が無い

- 根拠: survey-5330 §2「VMD の下の link は firmware が設定しない（隠れた root port）ので、Debian でも ASPM・L1SS は切れたまま」。同じ survey で、NVMe の LTR の max は 3145728 ns（0x1003、Linux の VMD の quirk の値）、両端の L1SS の CTL1 の閾値は 616448 ns（Linux の計算の値と一致、design §2・§4.3.3）。
  つまり Linux 6.19 は VMD の quirk と L1SS の時間の計算を**実行した**のに、ASPM と L1SS の enable を**入れていない**。
- 起こる状況（推測、記憶に依る）: Linux の VMD の quirk は LTR を書いた後に全部の link state を許す呼び出しをする。それでも切れているなら、Linux が止められた理由がある見込み:
  (a) FADT の IAPC_BOOT_ARCH の bit 4「PCIe ASPM Controls」（立っていれば OSPM は ASPM を有効にしてはならない）、(b) `_OSC` で OS に PCIe の制御が渡されていない、
  (c) NVMe の DevCap の L1 の許容 latency（bits 11:9）が link の L1 の exit latency（NVMe 側 < 32 µs）より小さく、Linux が L1 を capable から外した（survey は DevCap の L1 の許容を読んでいない）、(d) 起動の parameter。
  zedBSD は `_OSC` を評価しない（`grep -rl _OSC src/` は `src/drivers/platform/rpi4/rpi4-audio.c` だけ）し、FADT の ASPM の bit も読まない。
  (a)(b) なら、design の p004 は platform が禁じた設定を既定で入れる。(c) なら p004 は 5330 で何も有効にしない（§4.3.3 の判定で外れる）ので、受け入れ 3 の前提が崩れる。
  また、隠れた root port の CLKREQ# の配線・設定を firmware が L1.2 向けにしていない場合、L1.2 の有効化は link の喪失（NVMe の I/O の timeout → recovery は CC の reset だけで link を戻さない → quarantine → root の disk の喪失）になりうる。5330 は遠隔の電源が無い。
- 直し方の案:
  1. p004 の実装の前に、Debian で読みだけを足す（Q1 の割当）: `dmesg | grep -i aspm`（「FADT indicates ASPM is unsupported」「OS doesn't have ASPM control」「can't override BIOS ASPM」の類の行の有無）、`lspci -vvv` の NVMe の DevCap の「L1 <x」、FADT の boot flags（`acpidump -n FACP` か zedBSD の ACPI の parse で IAPC_BOOT_ARCH）。結果を survey-5330 に足し、§14 に「Linux が ASPM を入れなかった理由」を未確認として置く。
  2. 設計に「FADT の PCIe ASPM Controls の bit が立っていれば ASPM・L1SS を一切変えない」を足す（driver の側で読める。HAL は不要）。
  3. `_OSC` を評価しない事は platform の契約の判断なので、Q300 の問いとして Q1（→ ユーザー）に上げる（推しで既定 on にしない）。
  4. (c) の可能性を §4.3.3 の「5330 の見込み」に書き、見込みが外れた時に p004 の受け入れをどうするかを決める。

### R2（重大）§10・§13 Q300-4・§4.3・§4.4: 5330 で一度も試していない起動の時の振る舞いを既定で有効にし、試験の前に main に入る

- 根拠: §10「p002〜p005 は『実装済み・試験未実施』で記録し」、§13 Q300-4「ASPM・APST を既定で有効」。このプロジェクトは担当の作業を原則すべて main に merge し、ユーザーは main を 5330 で使う（Q1 の運用）。
  §4.3.1 の ASPM・L1SS・LTR の設定は VMD の attach で**毎回の起動**に走り、APST は NVMe の probe で毎回走る（実装 83b998e5e の `nvme_apst_apply(controller, 0, 1)`、pci-nvme.c:6397）。
- 起こる状況: p006（5330 の初めての試験）より前に、ユーザーが main を 5330 で起動する。R1 の (a)〜(c) や BG5 の APST の不具合が当たれば、NVMe から起動できない・I/O が止まる。逃げ道の `pci.aspm=off`・`nvme.apst=off` は loader で手で打つ必要があり、何が原因かをユーザーが知る手段が無い。
- 直し方の案: p002〜p005 の新しい**動作中の**振る舞い（ASPM・L1SS・LTR の書き込み、APST）は既定 off にし、opt-in の語（例 `pci.vmd.aspm=on`・`nvme.apst=on`）で入れる。p006 で 5330 の PASS を確かめた後に既定を on に変える（変える Phase を §10 に書く）。
  S0 idle の時だけ走る物（VMD の suspend・port の D3hot・HMB の停止）は `sleepctl` を呼んだ時だけなので既定 on でもよいが、R4 の安全側の既定を入れる。あるいは p006 まで main に merge しないことを Q1 と決めて §10 に書く。

### R3（中）§4.1.4 2.2: 「secondary bus の番号が 0 なら reset」の判定は、plan が引退させた port で毎回誤る

- 根拠: `pci-vmd-plan.c:1381〜1396` の `retire_port` は、使わない port（`DRV_PCI_VMD_PORT_NO_BUS`・`SWITCH`・`OWN_BAR`・`NOT_D0`・`TOO_MANY_BARS`・`NO_WINDOW`）の bus の番号を 0 にする。
  §4.1.2 は「root bus の上の bridge で driver の無い物」を全部集めるので、引退した port も集まる。
- 起こる状況: 引退した port は resume のたびに「reset された」と log し、§4.1.4 2.4 の link の待ち（DLLLA の無い port は 1000 ms、ある port でも link が無ければ 1000 ms＋100 ms）を毎回払う。S0 idle の resume が port ごとに 1 秒以上遅れ、log の「reset の有無」（§9 の p006 の観察）が当てにならない。
- 直し方の案: 保存した header の dword 6（bus の番号）と比べ、保存した secondary が 0 でなく今が 0 の時だけ reset とする。待ちは下に function のある port だけにする。
  実装 d86fe0c5d の `vmd_port_resume` は既に `saved_secondary` と `has_function` でそうしている（`pci-vmd.c:1178`、HEAD 83b998e5e）ので、設計の文を実装に合わせて直す。

### R4（中）§4.1.3・§4.1.4・Q300-3: 隠れた root port と VMD の No_Soft_Reset を見ずに D3hot にしている。reset された時の復元は 64 byte＋PCIe の control では足りない

- 根拠: `pci-power.c` の `drv_pci_device_set_power_state` は PMCSR の No_Soft_Reset（bit 3、`PM_NO_SOFT_RESET` は定義だけ）を見ない。§4.1.4 は reset を見つけたら log を出して続ける。§14.1 で「D3hot で config が失われるか」は未確認。
- 起こる状況（推測）: No_Soft_Reset が 0 の port は D3hot → D0 で内部の reset を受ける。保存の範囲（header・MSI・DevCtl/LnkCtl/DevCtl2/LnkCtl2・LTR・L1SS・Slot/Root Control）の外にある、firmware が隠す前に設定した物
  （AER の mask・severity、ACS、DPC、PTM、Gen4 の equalization の preset、CLKREQ# の対応・clock gating などの vendor 固有の register）が戻らず、link が遅い速度で上がる、上がらない、誤りを報告する。
  VMD 自身についても同じで、VMD が reset されると VMCONFIG（0x44）の外の firmware の割当（root port を VMD の下に置く設定、VMLOCK 0x70 で lock された物。中身は公開の資料に無い、survey-fable §4）が戻らず、root port が見えなくなりうる。design は VMCONFIG の書き戻し（§4.1.4 1）しか持たない。
- 直し方の案:
  1. attach で port と VMD の PMCSR の No_Soft_Reset を読み、log に出す。Debian の `lspci -vvv` の「NoSoftRst+/-」を survey-5330 に足す（読みだけ）。
  2. No_Soft_Reset が 0 の port は既定で D0 に残す（今の `pci.vmd.port=d0` の振る舞いを既定にし、D3hot は `pci.vmd.port=d3` の opt-in）。VMD が 0 なら VMD の suspend で `drv_pci_device_keep_d0(VMD, true)` を呼んで D0 に残す（既存の口。HAL も core の変更も要らない）。
  3. それでも reset を見つけた時は、その resume を EIO にし、次の S0 idle から port を D0 に残す（その場の判断で状態を変える旗を host に持つ）。
  4. resume の初めに root port の Vendor ID を CFGBAR で読み、0xffff なら VMD の割当が失われたとして直ちに EIO（port ごとの 1 秒の待ちを払わない）。

### R5（中）§4.1.3: port を D3hot にする前に、その下の function の状態を確かめていない

- 根拠: §4.1.3 は「子は全部 suspend 済み」と書くが、core が suspend するのは driver の付いた function だけ（`pci-power.c` `collect_visitor`）。plan が引退させた port の下（R3）、driver の付かなかった function、driver が `drv_pci_device_keep_d0` を呼んだ function は D0 のまま。
- 起こる状況: 下に D0 の function がある downstream port を D3hot にする（PCIe の電源の規則では、下流の function を先に D3hot にしてから port を D3hot にする。記憶）。その function の DMA・割り込み・link の状態は未定義になる。将来 NVMe 以外の device（Wi-Fi など）が VMD の下に来る機械で起こる。
- 直し方の案: port を D3hot にするのは、secondary bus の全部の function の PMCSR が D3hot（3）を読むか、function が無い時だけにする。そうでない port は保存だけして D0 に残し、1 行の log を出す。plan の状態（USED か）を host に残し、USED でない port は保存も D3hot もしない。

### R6（中）§4.1.4: link と子の準備の確かめが「port の reset を見つけた時」だけで、子の CRS（準備中）も見ていない

- 根拠: §4.1.4 2.4。core は VMD の resume の直後に NVMe を D0 にし config を書き戻す（`pci-power.c` `resume_entry`）。
- 起こる状況（推測）: port が reset されなくても link が落ちている場合（VMD0 の `_PS3`/`_PS0` が触る `ISAT` の意味は不明、survey-5330 §4。platform が下流の電源を落とすなら endpoint は reset される）、
  core の NVMe の config の書き戻しが link の訓練中・endpoint の初期化中に当たる。PCIe の規則（記憶）では、Conventional Reset の後の endpoint は 1.0 s まで CRS を返してよい。port が reset された場合も、DLLLA の後 100 ms で endpoint が準備できている保証は無い。
- 直し方の案: 下に function のある port について、DLLLA を報告できるなら毎回 LnkSta の DLLLA を見て、落ちていれば待つ。link が上がった後（または reset の後）は、子の Vendor ID を 1 ms ごとに最大 1000 ms 読み、0xffff・0x0001（CRS の SV の値）でなくなってから戻る。
  VMD 自身が D0 に戻ったか（VMCONFIG が 0xffff でない）を最初に確かめ、戻っていなければ port の待ちをせず EIO（R4 の 4 と同じ）。

### R7（中）§4.3.4・§7: ASPM の設定の失敗の検出と後始末が実際の失敗の形に合っていない

- 根拠: VMD の下の config の読み書きは CFGBAR の MMIO（`pci-vmd.c` `vmd_config_read`・`write`）で、範囲の内なら error を返さない。device が応えない時の読みは 0xffffffff になる。§4.3.4 は「どこかの読み書きが失敗したら」その link の ASPM を切ったままにする。
- 起こる状況: 失敗の経路はほぼ通らず、0xffff の cap の値で計算が走る（LnkCap の全 bit・L1SS の cap が「全部ある」に見え、CMRT 255 µs・T_POWER_ON の scale 3（予約）などを書く）。
  また、手順 3 の途中（上流の L1SS の enable を入れた後、下流を入れる前）で失敗すると、ASPM は切れているが L1SS の enable が片側だけ立ち、D3hot（PCI-PM L1）の時に PCI-PM L1.2 の状態が両端で食い違う。
- 直し方の案: 計算の前に読んだ値を検証する（Vendor ID と cap の dword が 0xffffffff でない、T_POWER_ON の scale が 3 でない、CMRT・値の範囲）。失敗の後始末は「両端の ASPM を 0（下流 → 上流）、両端の L1SS の enable を 0（下流 → 上流）」まで戻す。host 試験に「cap が全 1」「途中の失敗」を足す（§9）。

### R8（中）§9: 試験の計画では、全部の機械に効く変更と VMD の suspend・resume の順序と後始末の誤りを見つけられない

- 根拠: §4.3.5 の restore の順の変更と §4.1.1 の持ち主の深さは全部の機械の S0 idle を通る。§9 は p002 の host 試験を「しない」とし、QEMU は「p006 で余れば」。
  既存の `plan/ws052/tests/p004-guest.sh roundtrip`（QEMU の nvme・xHCI の S0 idle の往復、dmesg の `nvme: suspended`・`nvme: resumed` を見る）は pci-power.c の restore と NVMe の suspend・resume を通す。
  QEMU の nvme は HMB を持たず APSTA も 0 の見込み（推測）なので、HMB の停止の「拒否」の道と APST を送らない道も通る。
- 起こる状況: p002〜p005 の誤り（restore の順、深さの計算、HMB の停止の位置、APST の送り）が 5330 の p006 まで見つからず、p006 で原因の切り分けが 4 つの Phase に広がる。VMD の port の後始末（§4.1.3 の途中の失敗で逆順に戻す）は 5330 でも故意には起こせない。
- 直し方の案:
  1. VMD の port の suspend・resume の判断と順（保存 → D3hot、失敗の逆順の戻し、reset の判定、待ちの要否）を、`pci-vmd-plan.c` と同じく config の口を受け取る hardware の無い関数に分け、config 空間の模型で host 試験する（R3〜R6 の判定もここで試せる）。
  2. 深さの計算（持ち主の表）は `device_depth` を純粋な関数（bus の親と持ち主を引く callback）にすれば host で試せる。
  3. p002〜p005 の実装のまとまりで、既存の QEMU の往復（p004-guest.sh roundtrip）を 1 回流す段を §9 と §10 に入れる（Q1 の「テストなしで実装」の指示との兼ね合いは Q1 が決める。少なくとも p006 の前提に入れる）。

### R9（軽）§4.3.5: 1 つの function ごとの restore で「両端の ASPM L1 が切れている間に L1SS を変える」が満たせる理由が書かれていない

- 根拠: §4.3.5 は自分の function の ASPM だけを切って L1SS を書く。対向の端の ASPM L1 は立ったままのことがある（VMD の resume で root port を戻す時、NVMe の LnkCtl はまだ ASPM L1 を持つ）。
- 起こる状況: 規則（記憶）は両端についての物。実際には ASPM L1 の入りは Upstream Port（endpoint）だけが始め、L1.2 は両端の enable が要るので、root port を先・endpoint を後に戻す今の順なら問題は起きない見込み（推測）。ただし設計にその理由が無く、他の機械（PCH の root port には driver が無く、endpoint だけが restore される）にも同じ理屈が要る。
- 直し方の案: §4.3.5 に「ASPM L1 の入りは Upstream Port が始める。root port（Downstream Port）を先に戻し endpoint を後に戻すので、endpoint の L1SS を書く間に link は ASPM L1 に入らない。driver の無い上流の port は触らない」を書き、出典の確かさ（記憶）を §12 に足す。実装 d86fe0c5d はこの順で書いている。

### R10（軽）§4.2・§4.4・§5: HMB の停止と APST の送りの置き場所の条件が設計に書かれていない

- 根拠: admin の command の完了は割り込みだけで待つ（`pci-nvme.c` `nvme_admin_execute_mode` の comment「Completion remains strictly interrupt-driven」、HEAD 83b998e5e の 4385 行）。`nvme_suspend_stop` は最初に割り込みを mask する。recovery の間は `stopping` が立ち、`recovery_command` 無しの admin は ENXIO（`nvme_admin_execute_mode` の最初の判定）。
- 起こる状況: 設計の「shutdown の前に」「recovery の後に」だけでは、mask の後に HMB を止める（毎回 timeout → S0 idle が毎回中止）、recovery の APST を `recovery_command` 0 で送る（毎回 ENXIO で log だけ）実装を許す。
- 直し方の案: §4.2 に「`nvme_suspend_stop` の最初、割り込みの mask の前」、§4.4 に「recovery は `recovery_command` 1、resume は `nvme_resume_steps` の中（suspend の失敗の戻し `nvme_resume_start` も同じ道を通る）」を書く。
  実装 83b998e5e はそうなっている（`pci-nvme.c:2450` の `nvme_hmb_disable` が mask の前、7180 の `nvme_apst_apply(controller, 1, 0)`、2562 が resume の中）。HMB の停止が timeout の時は `admin_fault` が立つが、戻しの `nvme_admin_queue_memory_reset` が消す（ef1fda1d6 の 6749 行、HEAD 83b998e5e の 6959 行）ので回復できることも §7 に書く。

### R11（軽）§4.4・§12: ITPT の式と、引いた出典・5330 の観測が食い違う

- 根拠: §4.4 は ITPT = max(100 ms, 50 ×（ENLAT＋EXLAT）)で PS3 → PS4 を 2395 ms とする。survey-5330 §1 の Debian の表は PS3 → PS4 を 2000 ms、parameter は `apst_primary_timeout_ms` 100・`apst_secondary_timeout_ms` 2000・`apst_primary_latency_tol_us` 15000（遷移の latency で 2 段に分ける形）。§12 はこの parameter を出典に挙げるが、式はそれを使っていない。下限 100 ms の出典も無い。
- 起こる状況: 動作の誤りではないが、出典の記録が実際の値と合わず、規約の全文の見直し・出典の監査で説明できない。5330 の BG5 で実績のある表（Debian の 100 ms・2000 ms）から外れる理由も無い。
- 直し方の案: どちらかに決めて書く。(a) 2 段の形（遷移の latency ≤ 15000 µs なら 100 ms、そうでなければ 2000 ms、許容 100000 µs）にして 5330 の Debian の表と一致させる。(b) 50 倍の形を保ち、§12 の出典を「Linux の初めの形の散文（survey-fable §1）」だけにし、下限 100 ms は設計の選択と書く。
  あわせて、目標の状態の idle の電力（IDLP）が今の状態より高くないことを確かめる条件を §4.4 に足すか、確かめない理由を書く（survey-fable §1 の「ITPS は idle の電力が高くない事」）。

### R12（軽）§4.2 の推しの理由と §13 Q300-3 の根拠が確かめられていない

- 根拠: §4.2 の理由 (1)「platform が D3 を求めている」は `VDSD == 1` の時だけで、§4.2 末尾と §14.3 は GNVS の `VDSD` を読まないと決めている。Q300-3「Windows・Linux の s2idle と同じ形」は、survey-fable §2 に「Intel の RST と VMD の S0ix の公開の記述は見つからない」とあり、Windows の側は根拠が無い。
- 起こる状況: Q300-1・Q300-3 の再検討（p006 の後）で、理由が事実か推測かが分からない。HMB の停止は DRAM の無い BG5 では S0 idle のたびに対応表の NAND への書き出しと suspend の時間を増やしうる（推測）が、その費用も書かれていない。
- 直し方の案: (1) を「`VDSD` が 1 なら」と条件つきで書くか、zedBSD の AML で `VDSD` を読む 1 行の log を p003 に足す（読むだけ。振る舞いは変えない）。Q300-3 の Windows の部分を「推測」にする。p006 の測りに「HMB の停止の時間（suspend の ms の内訳）」を足す。

### R13（軽）§10 と ws.md の食い違い（記録の整合）

- 根拠: ws.md の Phase の表は p003 の依存に「WS299 p003・p004」、制約に「WS299 と `pci-nvme.c`・`pci-vmd.c` を共有する」と書くが、design §10 の p003・p005 の依存には WS299 が無い。
  ws.md の「今の事実」は `nvme_suspend` を 2073〜2102、「HMB は無い」「protocol の header は 0x09/0x07 だけ」と書くが、今の code は 2265〜（HMB の Feature 0Dh も `pci-nvme-protocol.h:55` にある）で、design §1 の方が正しい。
- 起こる状況: Q1 の割当で WS299 p004・p005（queue・vector、`plan/ws299/ws.md:6` で p004 は判定待ち、p005 は案）と WS300 p003・p005 が同じ `pci-nvme.c` を同時に変え、merge が衝突する。
- 直し方の案: §10 の p003・p005 の依存に WS299 の該当の Phase を戻すか、外す理由（衝突の範囲が小さい等）を書く。ws.md の「今の事実」を design §1 に合わせて同じ commit で直す（phase → ws の写し）。

### R14（軽）§9 の「照合する試験・道具は無い」は誤り

- 根拠: `plan/ws052/tests/p004-guest.sh:94〜95` が dmesg の `nvme: suspended`・`nvme: resumed` を `grep -q` で照合する。`plan/ws052/phase009/phase.md:131` も期待の行に挙げる。
- 起こる状況: 今回は部分一致なので「in N ms」を後ろに足しても壊れない（実装 83b998e5e の形でも一致する）。ただし「無い」と記録すると、次に文字列を変える担当が照合を追従させない。
- 直し方の案: §9 の文を「p004-guest.sh と ws052 phase009 が部分一致で照合する。後ろに足すだけなら追従は不要」に直す。

### R15（軽）§12・§4.3.3: Linux の値の出典が「記憶」で、guardrail の書式（版・path・名前）を満たせない

- 根拠: §12「閾値 = CMRT + T_POWER_ON + 6 µs | Linux 6.x の ASPM の計算（GPL、値だけ、記憶）」、L1SS の CTL1・CTL2 の bit の位置も記憶。guardrail「Value referenced: <版> <path> (<表・define・関数の名前>)」を block ごとに。
- 起こる状況: code の comment に正確な版・path・関数名を書けず、後の出典の監査で通らない。記憶の値が版によって違えば（閾値の式は Linux の版で直されてきた。推測）出典と値が合わない。
- 直し方の案: 実装の時に、値の出典にする file を 1 つの版で読んで（ws.md の制約は Linux の値の参照を許す。code・comment・構造・名前は写さない）、版・path・関数名を §12 と comment に書く。bit の位置は FreeBSD の `pcireg.h`（BSD）で確かめられる分をそちらに寄せる（survey-fable §3）。
  なお、§4.3.2 の bit の位置（LnkCap 11:10・17:15・20、LnkCtl 1:0・6、LnkSta 13、DevCap 11:9、DevCap2 11、DevCtl2 10、L1SS の cap・CTL1・CTL2、LTR の 9:0・12:10、0x1003 = 3145728 ns）は、レビューの側の記憶と全部一致した（下の「問題の無かった点」）。

### R16（軽）§4.3.3・§14: NVMe の DevCap の L1 の許容 latency が未確認の一覧に無い

- 根拠: §4.3.3 の ASPM L1 の判定は endpoint の DevCap の L1 の許容（bits 11:9）を使うが、survey-5330 §2 の表に無い。link の L1 の exit は NVMe 側が < 32 µs（符号 5）。
- 起こる状況: NVMe が 32 µs 未満の許容（符号 0〜4）を報告すれば、5330 で ASPM L1 も ASPM L1.2 も入らず（PCI-PM L1.x だけ）、p004 の効果が動作中には出ない。R1 の (c) と同じ。
- 直し方の案: §14 に足し、R1 の Debian の読みで一緒に取る。

### R17（軽）§11・§4.1.3・§4.3.1: 新しい boot の語の文書と名前

- 根拠: 既存の `pci.vmd=off` などは `docs/reference/kernel-boot-parameters.md:458〜` にある。§11 の文書の計画は power-management.md だけで、`pci.vmd.port=d0`・`pci.aspm=off`・`nvme.apst=off` を載せない。`pci.aspm=off` は VMD の下の link だけに効くのに、名前は全部の PCIe を指すように読める。
- 直し方の案: §11 に kernel-boot-parameters.md への追記を足す（文書の変更なのでユーザーの review の対象かは Q1 が判断）。名前を `pci.vmd.aspm=off`（R2 を採るなら `pci.vmd.aspm=on`）にする。

### R18（軽）§4.1.3: 隠れた port が MSI-X を持つと save が毎回失敗する

- 根拠: `drv_pci_device_save_state` は MSI-X の capability があれば表を map して写す（`pci-power.c` の save、map の失敗は error で返す）。plan は port の BAR を 0 にする（`pci-vmd-plan.c:1101`）。VMD の `host_map_bar` は bus address 0 の BAR を EINVAL にする（`pci-vmd.c:739`、HEAD 83b998e5e）。
- 起こる状況（推測）: MSI-X の表を自分の BAR に持つ root port では、port の save が EINVAL → VMD の suspend が失敗 → S0 idle が毎回中止。Intel の CPU の root port は MSI-X を持たないことが多い（推測）ので 5330 では当たらない見込み。
- 直し方の案: attach で port の MSI-X の有無を log に出し、ある port は保存の対象から MSI-X を外す（core の save を使わず port 用に分けるか、port では map の失敗を無視する口を VMD の側で持つ）。少なくとも §14 に未確認として置く。

### R19（軽）§4.3.4 手順 2: LTR の max を書くのが LTR の有効化の後

- 根拠: 手順 2「DevCtl2 の LTR を上流、次に下流で有効に。endpoint の LTR の max が 0 なら書く」。
- 起こる状況: 有効化から max の書き込みまでの間、endpoint は max 0 に縛られた LTR を送る（害は小さい）。仕様は max を有効化の前に設定することを勧める（記憶、推測）。
- 直し方の案: 「endpoint の LTR の max を書く → 上流の LTR を有効 → 下流の LTR を有効」の順にする。

### R20（軽）§4.1.4 2.3: Slot Control の書き戻しは hot-plug の command になる

- 根拠: §4.1.4 は Slot Control を無条件に書き戻す。Slot Control への書き込みは、hot-plug の能力のある slot では Power Controller Control・indicator の command を出し、Command Completed を待たずに次を書くと未定義（記憶）。
- 起こる状況: 5330 の port に slot があり hot-plug の能力が有る場合、書き戻しが slot の電源の command として扱われる。値が同じなら害は無い見込み（推測）。
- 直し方の案: 今の値を読み、保存した値と違う時だけ書く。Power Controller Control（bit 10）は変えない。

### R21（軽）§6・§4.3.1: ASPM の設定は NVMe の driver が動いている時に NVMe の config を read-modify-write する

- 根拠: §4.3.1 は scan（`drv_pci_bus_scan_tree`、その中で NVMe の attach・probe が同期で走る）の後に設定する。`config_lock` は 1 回の読み書きだけを守る。
- 起こる状況: 今は NVMe の driver が触る register（Command、MSI-X の control）と ASPM の register（LnkCtl・DevCtl2・L1SS・LTR）が違うので衝突しない。将来 NVMe の側が DevCtl（Max Payload）などを触ると、両方の read-modify-write が混ざる。
- 直し方の案: §6 に「NVMe の driver は LnkCtl・DevCtl2・L1SS・LTR を触らない。触る時はこの設定と順を決める」と書く。あるいは plan の後・scan の前に、plan の config の口で link を設定する（driver の attach より前に link が決まる）。

### R22（軽）§4.1.2: 「driver の無い bridge」を attach の時に決めている

- 根拠: §4.1.2「attach の scan の後…driver の無い物を集める」。後で driver が付けば（reprobe、遅れて登録した driver）、core と VMD の両方が同じ port を save・D3hot にする。
- 直し方の案: suspend の時に `drv_pci_device_driver(port) == NULL` を確かめ直し、driver のある port は飛ばす。

### R23（軽）§10: p002 の 5330 での最初の試験は WS052 p014 になる

- 根拠: `plan/ws052/ws.md:69` の p014 の依存は WS300 p002、WS300 p006 の依存は WS052 p014。循環ではないが、p002 の実機の確かめは別の WS の Phase（WS052 p014）で初めて起こる。
- 直し方の案: §10 に「p002 の 5330 の往復は WS052 p014 の script で最初に流れる。そこで VMD の port の log（reset の有無、No_Soft_Reset、R4）を取る」と書き、WS052 p014 の担当に渡す観察の項目を決めておく。

## 問題の無かった点（確かめた事）

- HAL: 足す物は全部 driver の層（`pci-power.c` の持ち主の表、VMD の port の保存、NVMe の admin command）で、`include/hal/hal.h` の変更は要らない。§3.1 の説明は妥当。UAPI も足さない（§8）。
- 順序: 今の `device_depth`（`pci-power.c`）で NVMe 2・VMD 1 なので、今でも NVMe → VMD の順。同じ深さになった場合も、VMD の root bus は `root_buses` の頭に入る（`pci.c:336〜337`）ので VMD の下が先に集まり、安定な挿入の並べ替えで VMD より先に suspend される。持ち主の表（§4.1.1）はこの偶然を明示にする物で、正しい（実装 d86fe0c5d は登り数に上限も持つ）。
  resume の逆順で VMD → port → NVMe。VMD の suspend の途中の失敗は VMD が自分で戻し（core は driver の suspend の失敗では `resume_entry` を呼ばない）、core の save・D3hot・`_PS3` の失敗では `resume_entry` が VMD の resume を呼ぶので、VMD の resume は「VMD が D0 のまま」の場合も扱う必要がある（設計の手順で扱える）。
- 正常な往復では、port・VMD が D3hot の間に NVMe の config に触る経路は無い。例外は VMD の resume の失敗（port が D3hot のまま）で、NVMe の config・MMIO は UR で全 1 を読み、`nvme_controller_disable` が CC = 0xffffffff で直ちに EIO を返して quarantine になる（固まらない）。R6 の準備の確かめは別の話。
- VMD の BAR（header の dword 4〜9）と MSI-X の表は core の save・restore が戻し、MSI-X の表の書き戻しは Command の memory の有効化の後（restore の順）。子の MSI-X の表は NVMe の save で戻り、address は VMD の vector 1 を指したまま。bypass の bit は VMCONFIG の書き戻しで、NVMe の resume より前に戻る。
- ESTALE を返さない判断（§4.1.4）は正しい（`resume_entry` の ESTALE → `drv_pci_device_reprobe` → VMD の detach は EBUSY）。
- NVMe の recovery は CC の reset だけで FLR をしないので、ASPM・L1SS・LTR の設定は recovery を越えて残る。
- §4.3.2 の register の bit、§4.3.3 の判定（両端の大きい方の L1 の exit の符号 ≦ endpoint の許容、符号 7 は制限なし）、L1SS の enable を両端の cap の共通にする事、CMRT は上流だけ・T_POWER_ON と閾値は両端、§4.3.4 の切る（下流 → 上流）・入れる（上流 → 下流）の順、
  閾値の符号化（616 µs → scale 2・値 602 = 616448 ns）、0x1003 = 3145728 ns は、レビューの側の記憶と一致した（仕様の本文は未読）。実装 5f9f45362 の `include/drivers/pci/pci-link-plan.h` の定数も同じ値。
- NVMe の Identify（NPSS は byte 263 で 0 起点、APSTA は byte 265 bit 0、PSD は byte 2048 から 32 byte × 32、MP 15:0・MXPS bit 24・NOPS bit 25・ENLAT 63:32・EXLAT 95:64）、APST の entry（ITPS 7:3、ITPT 31:8 の ms）、ITPS を非操作の状態に限る事、未報告（0）の latency の状態を使わない事は正しい。
- HMB の停止の後の Memory Return つきの渡し直しは、明示の停止の後でも正しい使い方（NVMe の HMB の規則、記憶）。停止の timeout の後は `hmb_state` を UNKNOWN にして memory を保つ（実装 83b998e5e の `nvme_hmb_disable`）。

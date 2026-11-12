<!-- awesome-plan project=zedbsd record=ws333-p028 -->

# ws333-p028: 5330 を人手なしで戻せる起動（Intel TCO watchdog の driver と一回だけの起動）

Status: cleared（2026-11-13 Q1 判定: 5330 で watchdog=60 の zedBSD が 663 秒再起動せず、watchdog.stop=180 で止めると約 57 秒後に人手なしで Debian に戻った。host 試験・QEMU q35 PASS、kernel warning 0。ACPI timer は zedBSD でも止まっていて driver が動かす。p027 の C-state の比較は watchdog の状態をそろえて行う。driver は p024 の規約の見直しに含める）
Parent: [WS333](../ws.md)

## 目的

[p027](../phase027/phase.md) の危ない試験（`i915.start=manual`・S0 idle・新しい image）を、遠隔の電源の無い 5330 で人手なしに流せるようにする。2026-11-13 ユーザーが進め方を承認した。

- 一回だけの起動: 既にある。`~/boot-zedbsd.sh`（`plan/tools/hw5330/debian/boot-zedbsd.sh`）が UEFI の BootNext で zedBSD を一度だけ起動し、次の起動は Debian。
- 足りない物: zedBSD が止まった時に再起動させる hardware の watchdog。zedBSD には watchdog の driver が無い。

## 事実（2026-11-13 Q1 が 5330 の Debian で見た）

- `iTCO_wdt: Found a Intel PCH TCO device (Version=6, TCOBASE=0x0400)`、heartbeat 30 秒、`intel_pmc_bxt` を使う（NO_REBOOT の bit は PMC の側）。PCH は Alder Lake-P（SMBus 00:1f.4 8086:51a3、eSPI 00:1f.0 8086:5182）。

## 範囲

1. Debian で TCO の watchdog が本当に機械を再起動させるかを確かめる（`/dev/watchdog` を開いて叩くのを止める → 再起動して Debian に戻る）。戻らなければ止めて Q1 に返す。
2. zedBSD の Intel TCO watchdog の driver（v6 の PCH。TCOBASE と NO_REBOOT の見つけ方は Intel の PCH の datasheet と ACPI に基づき、本来どうあるべきかで設計する）。起動の option で有効にし（既定は off）、kernel の timer か thread で叩く。kernel が止まれば叩かれず再起動する。panic の時も叩かない。user の `/dev/watchdog` は範囲の外（要れば案）。
3. 確かめ: host 試験（register の列）、QEMU（q35 の ICH9 の TCO で動く範囲）、5330 で watchdog を有効にした zedBSD を一回だけの起動で起こし、わざと止めて Debian に戻る事。

## 受け入れ

- 5330 で、watchdog を有効にした zedBSD が止まると、人手なしに Debian に戻る。普通に動いている間は再起動しない（10 分以上）。build warning 0。

## 結果（2026-11-13 P3、q2028-i01）

### 1. Debian で TCO が再起動させるか（5330、00:42〜00:48）

- `/dev/watchdog` を開いて 1 回だけ書き、叩かずに 3 分待った: **再起動しなかった**。TCO_RLD は 0x32 のまま動かない。原因: ACPI の PM timer が止まっていた（PM_TMR 0x1808 が不変、PMC の `PWRMBASE+0x18FC`（ACPI_TMR_CTL）= 0x2 = ACPI_TIM_DIS）。Debian の acpi_pm は起動 0.59 秒の登録の時には数えていたので、firmware の後で Linux の側が止めている（起動直後に既に 0x2）。
- Debian で ACPI_TIM_DIS を 0 にした（`/dev/mem`、timer を動かすだけ）: TCO_RLD が 0.6 秒ごとに減り、0x32（50）を入れてから約 30 秒で再起動し、約 25 秒後に Debian に戻った（boot_id efb0adf6→e46d37e7）。→ **TCO は 5330 を再起動させる。hub の counter は ACPI timer の clock で数え、1 回目の run out の直後に reset する**（datasheet vol1 の「2 回目は固定の短い timer」と合う）。
- 記録の訂正: NO_REBOOT は intel_pmc_bxt（module の依存の名前だけ）ではなく **TCO1_CNT の bit 0（NR_MSUS）**。停止中 0x1801（HALT・TCO_LOCK・NR_MSUS）、iTCO が開くと 0x1000。strap は TCO2_STS bit 2 で 0。
- TCOBASE: SMBus 00:1f.4 の config 0x50 = 0x401（I/O 0x400）、0x54 = 0x101（TCO_BASE_EN・LOCK）。PMC 00:1f.2 は PCI に見えない（firmware が隠す）。DSDT は PMC の base を `\PWRM`（GNVS の field）で自分の method に使う。

### 2. 設計（出典は Intel 700 Series On-Package PCH Datasheet vol1 763122-001・vol2 764889-001（ADL-P と同じ 51A3 の系統）、ICH9 は QEMU v10.0.0 の model の値。Linux の source は読んでいない）

- 置き場 `src/drivers/platform/pcat/intel-tco.c`（amd64 の pcat の driver の並び、cpu-x86 と同じく `src/kern/platform/pcat.c` から weak で呼ぶ）。PCI driver「intel-tco」が SMBus 8086:51A3（hub）と LPC 8086:2918（ICH9）を bind する。bind するのは boot parameter `watchdog=<秒>`（4〜600、`off` か無しで何もしない）がある時だけ。arm は ACPI の attach の後（`drv_intel_tco_start()`）。
- boot parameter は `src/kern/boot.c` に key `watchdog`・`watchdog.stop` を足した（数字 1〜6 桁、watchdog= は off も）。
- **叩き方**: CPU ごとに bound の kernel thread（優先 SCHED_PRIOR_HIGH）が timeout/4 ごとに自分の stamp を今の generation にし、全 CPU の stamp が揃った時だけ generation を進めて TCO_RLD を書く。1 つの CPU が割り込みを止めて回る・scheduler が止まる・panic（その CPU が割り込みを止めて halt）のどれでも reload が止まり reset になる。panic の flag は要らない。
- **時間**: hub は count = ceil(秒/0.6)（1 回目で reset）、ICH9 は ceil(秒/1.2)（2 回目で reset）。
- **arm の順**: HALT → TCO_TMR → TIMEOUT・SECOND_TO_STS を消す → reset の許し（hub は NR_MSUS を 0 にして読み返す、strap なら拒む。ICH9 は RCBA+3410h の NO_REBOOT bit 5）→ hub は ACPI_TIM_DIS を 0 → reload → HALT を外す → 1.5 秒見て count が減る事を確かめる（減らなければ HALT と NO_REBOOT を戻して拒む）→ thread。NMI_NOW（TCO1_CNT bit 8）は書かない。前の reset が watchdog の物なら log に出す。
- **ACPI_TIM_DIS（Q1 の問い）**: arm と resume で 0 にし、戻さない。watchdog は arm から kernel の終わりまで要るので、止める時（戻す時）が無い。datasheet は「OS が耐えられる時だけ 1、deep idle の入りで 1・出で 0 の使い方もある」とする。**p027 への影響**: watchdog を有効にすると ACPI timer が動き続けるので、package C-state の A/B は watchdog の有無を揃えるか、watchdog 無しの起動と比べる。
- **PWRMBASE の見つけ方**: D31:F2 の BAR（datasheet の正規の場所）、function が隠されていれば firmware の ACPI の `\PWRM`（intel-gpio の `\SBRG` と同じ作法）。見つからなくても count が減れば arm する。
- **S0 idle**: PCI の suspend で HALT、resume で再 arm（NO_REBOOT・timer・reload）。眠っている間は reset しない。
- **わざと止める手段**: boot parameter `watchdog.stop=<秒>`（arm から N 秒後に全 CPU の thread が割り込みを止めて回る）。watchdog 自身が効く事を機械で示す診断の口で、test の build だけの物ではない。
- user の `/dev/watchdog` は範囲の外（作っていない）。案: zedBSD の user の処理が止まっても kernel が生きている場合（例えば i915 の停止で SSH だけ死ぬ）は戻らない。要るなら「user が N 秒ごとに叩く `/dev/watchdog`、閉じると kernel の thread に戻す」を別の Phase で。

### mk の差分（2026-11-13 ユーザー承認の mk の差分）

`mk/platform-x86_64/vmunix.mk` の KERN_SOURCES（`pcat-ide.c` の次）に `src/drivers/platform/pcat/intel-tco.c \` の 1 行。他の mk は変えていない。

### 3. 確かめ

- host 試験 `sh plan/ws333/tests/tco-host/run.sh build/tco-host`: **PASS**（ASan・UBSan。parameter の読み、count、off、hub の arm の列・10 分の reload・1 CPU の停止で 30 秒後に reset、strap と止まった clock の拒否、suspend/resume、ICH9 の RCBA と 2 回目の reset）。突然変異 8 つ（stamp が揃わなくても reload、count の確かめを常に成功、hub の NO_REBOOT を消さない、NMI_NOW を残す、timer を動かさない、ICH9 の NO_REBOOT を消さない、hub を ICH9 の count に、suspend の HALT を外す）を全部検出。
- kernel build: `sh plan/ws333/tests/hw5330/kbuild.sh build/p028/k "BUILD_JOBS=4"`（x86_64、CONFIG_KERN_THERMAL=y・CONFIG_DRIVER_X86_POWER_CONTROL=y）rc=0、**compiler の warning 0**（log の `warning` 1 件は sysroot の NoctLang の submake の「-j16 forced」。kbuild.sh は BUILD_JOBS を vmunix の段にだけ渡し sysroot の段に渡さないため。次からは `make sysroot BUILD_JOBS=4` を先に流す）。
- 書き方の直し（f29092d8ad: count の確かめの返りの順、log の if の前の comment）の後、vmunix だけ再 build（`make -j4 ... BUILD_JOBS=4 build/x86_64/vmunix`）rc=0、warning 0。host 試験 PASS。実機の結果は 4d0cd8f155 の kernel（sha256 dd4409877c485167）の物で、直しは動作を変えない。
- QEMU q35（ICH9、KVM）`bash plan/ws333/tests/tco-qemu/run.sh build/p028/qemu/hdd-image.img build/p028/qemu/run4`: **PASS**。image はこの tree の x86_64（THERMAL・X86_POWER_CONTROL、text login、小さな userland）、boot の FAT の zedbsd.cfg に mtools で parameter を足した写し。QMP と HMP で TCO（OVMF の ACPI base 0x600 + 60h = 0x660）と RCBA+3410h を 10 秒ごとに読む（console・serial の log は読まない）。
  - off（parameter 無し）: 100 秒 WATCHDOG 無し、count 0 のまま、NO_REBOOT は 1 のまま（9 回の probe）。
  - alive（watchdog=8）: 150 秒 WATCHDOG 無し、60 秒より後の 9 回の probe で count が 4〜5（1 run 7 count）を行き来し HALT 0・NO_REBOOT 0。
  - stop（watchdog=8 watchdog.stop=40）: QMP の接続から 49.6 秒で `WATCHDOG {"action": "reset"}`。
  - 途中の run1〜run3 の FAIL は試験の側の誤り（sfdisk の path、HMP の幅の書き方、off の判定を QEMU の初期値に合わせていなかった）で、直して run4。
- 5330 の実機 `sh plan/ws333/tests/hw5330/5330-p028.sh build/p028/k/vmunix p028c`（01:16〜01:38、/tmp/i915-hw.lock の下）: **PASS**。summary は build/hw5330/ws333/p028c/summary.txt（worktree）。
  - zedBSD の log（SSH の dmesg）: `watchdog: the ACPI timer was stopped; it runs again for the TCO counter` → `watchdog: armed, 60 s from the last reload to the reset (TCO at 0x400, count 100, 12 CPUs)`。**zedBSD でも arm の時に ACPI timer は止まっていた**（誰が止めたかは未確認。DSDT の PWMR の field は 0x18FC に触れない）。driver が動かして arm できた。
  - alive（watchdog=60）: 一回だけの起動で zedBSD、**663 秒**、30 秒ごとの SSH の uname が全部 zedBSD（reset 無し）。reboot で Debian へ。
  - stop（watchdog=60 watchdog.stop=180）: zedBSD が SSH に答えてから 178 秒で答えなくなり、**235 秒で人手なしに Debian が戻った**（止まってから約 57 秒、60 秒の設定と合う）。Debian の iTCO の bootstatus は 0（firmware が SECOND_TO_STS を消すらしい、記録だけ）。戻った Debian で TCO1_CNT 0x1801（firmware が再び HALT・NR_MSUS）。
  - 最後に p4 の vmunix と zedbsd.cfg を元に戻し sha256 を照合（vmunix 4c98d617279bf96b = WS333 の c6 kernel、zedbsd.cfg 1b33a395b0ff4bd7）。元の写しは Debian の ~/ws245/vmunix-p028-orig・zedbsd.cfg-p028-orig。
  - 途中の p028a（Debian の home を sudo の中で $HOME と書き /root になった、vmunix.old から元を保存し直した）・p028b（alive の判定に zedBSD に無い uptime を使った）は試験の側の誤りで、どちらも Debian に戻り p4 を戻した（p028b は照合済み）。
- QEMU と実機の証拠は上のとおり別。user の処理だけが止まる場合（kernel は生きている）は戻らない（範囲の外、上の案）。

### 残り・未実施

- cleared の判定は Q1。
- 規約の全文の見直し（WS333 の p024）にこの driver を足すか、Q1 が決める。
- p027 への申し送り: watchdog を有効にすると ACPI timer が動き続ける。package C-state の比べは watchdog の有無を揃える。
- 消してよい物（Q1 へ）: worktree の build/tco-host/mut（突然変異の写し）、build/p028/qemu/run1〜run4 の image の写し（各 2.4 GB、sparse）。5330 の ~/ws245/vmunix-p028-p028a・p028b・p028c（試験の kernel の写し）。

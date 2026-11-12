<!-- awesome-plan project=zedbsd record=ws333-p025 -->

# ws333-p025: p014 の食い違いの解析と直し（HWP の読み・E-core の domain・package の C-state）

Status: in-progress（2026-11-13 P1、q2023-i01: 5330 で 1 回流した。1〜3 は要求の上限を超えず、4 回の読みが変わり、E-core の highest_mhz は 3300。4 は E-core の module の 4 つが 1 つの clock と分かり `KERN_CPUPERF_SHARE_MODULE` にした（domain 10 → 4）。5 の全負荷 1.4 GHz は firmware の PL1（Debian も同じ）、package の C-state 0% は tick ではなく i915 の側が有力（未証明）。cleared は Q1）
Parent: [WS333](../ws.md)

## 目的

[p014](../phase014/phase.md)（5330 の CPU の受け入れ）の測定で出た食い違いを、code を読んで原因まで突き止め、直せる物は直す。考える Phase。実機の測定は原因と直しが揃った後に 1 回だけ流す。

## 解析する事（p014 の §3 と持ち越し）

1. **HWP の読みが古い**: `thermal cpu` を 5 秒ごとに 4 回読むと、最高の MHz が毎回同じ値。`delivered_mhz` は GET の時にその CPU で読む設計（design.md）。読みの道（ioctl → cpuperf → provider → MSR の APERF・MPERF か IA32_HWP_STATUS 等）を辿り、写しを返していないか、差分の基準（前回の値）を更新しているかを確かめる。
2. **E-core の MHz が要求の上限を超える**: powersave（E の max 12）で 3280 MHz、manual 0（E の max 8）で 2191 MHz。E-core の `highest_mhz` が 0。level と MHz の換算（CPUID 0x16・HWP_CAPABILITIES・bus の MHz・P と E の scale の違い、Intel SDM の hybrid の記述）を確かめる。
3. **manual 100 50 50 で spin が 1192 MHz**（要求 1-38 なら 3800 まで）。strategy の値から HWP_REQUEST の min・max・EPP・desired への写像と、package の HWP_REQUEST_PKG・HWP の enable の順を確かめる。1 と混ざっていないかを分ける。
4. **E-core が 4 つ組の domain に出ない**: `thermal cpu domains` で E-core が 1 CPU ずつ別の domain。topology の組み立て（CPUID 0x1A・0x1F の module の level、L2 の共有）を確かめる。
5. **package の C-state が 0%**（p012 から）と **全 CPU の負荷で P-core が 1.4 GHz に留まる**（p011 の PLATFORM_LIMIT 0x8）: kernel の側で妨げている物（MWAIT の hint、C-state の auto-demotion・undemotion、0xE2 の package の limit、tick・IPI の起こし）があるかを code と MSR の値で確かめる。kernel の外（ASPM・PSR・電力の上限の設定）なら、その根拠を記録して他の WS へ渡す材料にする。

## 進め方

- まず code と仕様（Intel SDM Vol. 3・4）だけで原因の仮説を立て、根拠（file:line、SDM の節）を書く。直せる物は直し、host の試験（plan/ws333/tests/ の cpu-x86-host、test-hwp 等）を足して確かめる。
- 実機が要る確かめは 1 本の script にまとめ、最後に Q1 の割当で 5330 で 1 回流す（p014 の script の直し: levels は strategy の後に読む、spin を CPU ごとに置く道具が無ければ全 CPU に 1 本ずつ等）。
- 解けない物は、試した事・残る仮説・要る情報を書いて Q1 に返す。

## 受け入れ

- 1〜5 の各々に、原因（根拠付き）か、kernel の外と判断した根拠が書かれている。
- 直した物は build warning 0、host の試験 PASS。5330 の測定で 3 の MHz が要求に従う（要求の上限を超えない、manual の上限の近くまで出る）事と、読みが 4 回で変わる事。

## 結果

2026-11-12〜13 P1（q2022-i01、branch agent/p1-ws333h、基点 main a43c56ce07）。code と Intel SDM rev. 093（Vol. 1・3・4、worktree の `build/specs/` に取り直し、SHA-256 は design.md §15.1 と一致）と p014 の log（`/home/awe/zedBSD-worktrees/p1-ws333g/build/hw5330/ws333/{run2,cpu3}/`、git の外）で解いた。file:行 は基点 a43c56ce07 の物。

### 1. HWP の読みが古い → 原因: 読み方（p014 の script と `/sbin/thermal` の一覧）

- `thermal cpu`（一覧）は `KERN_THERMAL_CPU_READ_SAMPLE` を付けずに GET_CPU を読んでいた（`userland/base/thermal/main.c:1352-1356`）。UAPI は「READ_SAMPLE が無ければ CPU を起こさず、delivered_mhz は最後の sample」（`include/uapi/thermal.h:425-427`）で、kernel は box の最後の sample を写すだけ（`src/kern/cpuperf.c:685`）。
- 最後の sample は strategy の apply の時に取られる（`cpuperf.c:2261-2264`）。その delivered_mhz は前の sample からの APERF/MPERF の比 × 最大の非 turbo の周波数（`cpu-x86-control.c:594`）で、APERF・MPERF は C0 の間だけ数える（SDM Vol. 3B §17.2「they measure performance only when the targeted processor is in the C0 state」、§22.7.2）。だから p014 の各行の 4 回の読みは同じ値で、中身は **1 つ前の strategy の期間の C0 の平均の MHz**。
- 確かめ（p014 の log で、各行の読みを 1 つ前の strategy の要求の上限と比べた。上限 = その domain の max × highest_mhz ÷ highest）: cpu3 は 4 行とも上限を超える CPU が 0、上限に最も近い CPU が上限の 0.99〜1.00（balanced の行 3264/3300、powersave の行 3280/3300、manual 100 の行 1192/1200、manual 0 の行 2191/2200）。run2 も 5 行とも上限を超える CPU が 0。p014 の「全 CPU の負荷で P-core 1652 MHz 以下」（cpu3 の loaded）も同じく前の期間（idle）の値。
- 直し（44d947fd00）: `thermal cpu` は書き込みで開けた時（root と desktop の group）に CPU ごとに sample（約 100 ms）してから出す。読みだけの fd は kernel が EPERM を返すので、その CPU から先は最後の sample を出す。`docs/architecture/power-management.md` に mhz の意味（C0 の平均・sample の間）と HWP の単位を書いた。UAPI（ioctl と struct）は変えていない。

### 2. E-core の MHz が要求の上限を超える・E-core の highest_mhz 0 → 原因: 1 の読み方と単位の見方、および測りの thread の移動（kernel の不具合）

- 上限を超えた数（powersave の 3280、manual 0 の 2191）は 1 のとおり前の strategy（balanced の E max 33、manual 100 の E max 22）の期間の値で、上限の中。
- 単位: HWP の level は「abstract unit-less」で周波数に結ばれない（SDM Vol. 3B §17.4）。5330 では測った最高の MHz ÷ highest が P 4343/56 ≈ 77.6 MHz、E 3300/33 = 100 MHz で、P と E の 1 unit が違う。p014 の「E の max 12 → 1200 MHz」は E では合うが、P の上限を units × 100 で読むと過大になる。script の判定は domain ごとに highest_mhz ÷ highest で換算する。
- E-core の highest_mhz 0（run2）: run2 の起動の log に `cpuperf: cpu 4 not measured (24)`（24 = EAGAIN）。測りの thread は `sched_set_cpu()` で CPU 4 に置くだけ（`cpuperf.c:3376`）で、`cpuperf_measure_main()` は自分が別の CPU で動いていると EAGAIN を返す（`cpuperf.c:3458`）。scheduler は idle の CPU が他の CPU の列で 1 tick 以上待った thread を取る（`src/kern/sched.c:2554` `steal_runnable()`、`:2510`、idle の loop の `:1603`）ので、置いただけの thread は移りうる（`sched.c:12` の「Threads are pinned to a CPU」の comment は steal と合わない。scheduler は WS333 の外なので変えていない）。cpu3 の起動では E も測れた（3300 MHz）ので毎回ではない。最初の strategy の前の highest_mhz 0 は設計どおり（測りは最初の strategy の後、design.md §4.5）。
- 直し（dc89af4412、`src/kern/cpuperf.c`）: 測りの thread は busy の時間の後にも CPU を確かめ（busy の途中で移ったら EAGAIN）、EAGAIN の時は新しい thread で 3 回まで測り直す（`CPUPERF_MEASURE_TRIES`）。host 試験に 3 場面（最初の 2 つの thread が別の CPU で始まる・busy の途中で移る・毎回移る（highest_mhz は 0 のまま、strategy は効く））。

### 3. manual 100 50 50 で 1192 MHz → 原因: 1 の読み方（写像は設計どおり）

- manual の写像（`cpuperf.c:2441-2447`）: max = min(efficient〜highest を speed で、sustained（guaranteed と efficient の大きい方）〜highest を noise で)。5330（P 1/11/21/56、E 1/8/12/33）で manual 100 50 50 は P 21+(56-21)×50/100 = 38、E 12+(33-12)×50/100 = 22、EPP 0。p014 の要求の写し（P 1-38・E 1-22・EPP 0）と一致し、HWP_REQUEST の encode（`cpu-x86-control.c:470-528`、window・package_control・valid は 0、HWP_CTL bit 0 を落として CPU の要求を有効に `:427-434`）にも食い違いは無い。
- 1192 は powersave の期間の E-core の上限 1200 に当たる（1 の確かめ）。manual 100 50 50 の上限は P 約 2950 MHz（38 × 77.6）、E 2200 MHz。
- 付記: p014 の直の設定 `cpu set all max=28` は「all」が千分率（`cpuperf.c:2567`）なので 28‰（P 2 units・E 1 unit）を求めていた。新しい script は `max=500`（範囲の中ほど）。

### 4. E-core が 4 つ組の domain に出ない → 原因: 設計どおり（共有は未確かめ）

- driver は全 CPU を `KERN_CPUPERF_SHARE_CORE` で出す（`src/drivers/platform/pcat/cpu-x86/cpu-x86.c:293`）。domain は同じ share と同じ core の番号で組む（`cpuperf.c` の `cpuperf_shares()`）ので E-core は 1 つずつ。WS301 design.md:143 は「4 つ組が clock を共有すると書く試験で確かめたら module を domain にする、それまで core ごと」と決めており、p014 の期待（4 つ組の domain）の方が設計と違った。L2 の module の番号は topology に出ている（`cpu-x86.c:243`）。
- 確かめは 5330 の script の「module」の段（同じ module の E-core の 1 つを highest、残りを efficient にして全 CPU を負荷、各 E-core を sample）。揃えば共有で、その時の直しは E-core の describe を `KERN_CPUPERF_SHARE_MODULE` にする 1 行（Q1 の判断の後）。HWP の要求は CPU ごとに書けるので、domain が core ごとのままでも制御は正しい（共有なら hardware が要求をまとめる）。

### 5. package の C-state 0% と全負荷の P-core 1.4 GHz → kernel の中の直す物は見つからない。外の候補と、決める数を取る script

- kernel の側で見た物: MWAIT の hint は CPUID 05H の sub-state の深い方（`src/hal/x86_64/bsp-pcat/idle-suspend.c:72`、C6、c10 の kernel は C10）、ECX bit 0（masked の割り込みで抜ける）。kernel は E2H（package の C-state の上限・auto-demotion）・電力の上限・clock modulation を書かない（driver に E2H の書きの呼びは無く、HAL の MSR の書きは EFER・STAR・PAT・MTRR・FS/GS だけ）。tick は全 CPU で 1 ms ごと（`idle-suspend.c:68`、tickless の idle は design.md §4.3.3 段 4、HAL の契約の外）。
- p014 の core の C6 の滞在は 87〜90%（Debian は C10 が約 99.9%）。それでも PC2 まで 0 なのは、core が同時に idle な時間（tick の位相は揃っている、p012）があっても package が入らない事を示し、uncore の側の妨げを示唆する。起動の log（p014 run2 の c6-boot）に `i915: display power: DC_STATE_EN=0x00000000 ... DMC program not loaded`、`well DC_off: 3 users, on` があり、display の DC state が使えない（i915 の WS の範囲）。E2H の package の auto-demotion（SDM Vol. 4 Table 2-35 bit 29・30）は 1 ms の tick で浅い状態に落としうる。どれが効くかは code だけでは決まらない（未解決）。
- 全負荷の P-core 1.4 GHz（p011 の数は READ_SAMPLE の新しい読みで、1 の読み方の誤りではない）: performance の P 1400・E 1200 MHz・package 25.2 W・PLATFORM_LIMIT（HWP_STATUS の excursion to minimum）、powersave の P 1600・E 1200・11.6 W。kernel は電力の上限を書かないので、制限の理由は MSR_CORE_PERF_LIMIT_REASONS（64FH、SDM Vol. 4 Table 2-39、ADL の組に入る）で読む。PL1（25 W 付近）・PROCHOT（Dell の EC）・その他を 64FH で分ける。
- 決める数（5330 の script）: Debian で同じ MSR（PC2〜PC10・E2H・64FH・PKG_POWER_LIMIT）を読み、idle と全負荷（Debian の既定と EPP performance）を取る。zedBSD は試験だけの patch（`plan/ws333/tests/hw5330/limit-diag.patch`、sample で 64FH と E2H を読み `thermal cpu 0 sample` の firmware_id・voltage_group に出す）の kernel で、3 つの strategy の全負荷と idle を取る。`NOI915=1` で i915 の無い kernel の idle（display を firmware のままに）も取れる（5330 に遠隔の電源が無く、この kernel は 5330 で起動した事が無いので既定は off）。

### build と試験（host）

- worktree で `make CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached`（rc 0）→ `make PLATFORM=x86_64 CONFIGMK=config/config.x86_64.mk CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y sysroot`（rc 0）。注: sysroot の中の NoctLang の build が `--parallel 16` で走った（`mk/build-jobs.mk:21` の BUILD_JOBS の既定。`BUILD_JOBS=4` を渡し忘れた）。
- kernel（`plan/ws333/tests/hw5330/kbuild.sh`、`build/tmp/p025k/`、git の外）: c6（tree のまま、sha256 716c57ba9d6c5091）・diag（limit-diag.patch、11a2cdac8aa13e90）・noi915（`CONFIG_DRIVER_PCI_I915=n` と patch、6e9266989f5e763e）、全部 rc 0・warning 0。直す前の tree の c6 は p014 と同じ sha256 a9236b5db6bf6e7a（再現する build）。`/sbin/thermal` の静的な build（`thermal-static.sh`、-Werror、`build/tmp/p025k/static/thermal`）は通る。package の Makefile での `thermal` の build は worktree に `config.mk` が無く未実施。
- host 試験: `cpu-x86-host` PASS、`cpuperf-host` PASS（8 場面、新しい 3 場面は直す前の cpuperf.c で FAIL）、`thermal-host` 29 場面 0 failed、`cpux86/run-unit.sh all` PASS、新しい `plan/ws333/tests/thermal-tool-host/run.sh` PASS（書き込みの fd では CPU ごとに sample、読みだけは最後の sample。直す前の main.c で 4 件 FAIL）。

### 5330 で流す準備ができた

- `sh plan/ws333/tests/hw5330/5330-p025.sh build/tmp/p025k p025`（worktree `/home/awe/zedBSD-worktrees/p1-ws333h` で、`/tmp/i915-hw.lock` の下。i915 の無い kernel も流すなら `NOI915=1`）。読むのは `build/hw5330/ws333/p025/summary.txt`。Debian の側は `sudo -n modprobe msr` と python3 で MSR を読み、EPP を一度 performance にして元に戻す。p4 は最後に tree の kernel（KDIR/c6）。
- 見方: 各 strategy と直の設定で `above ceiling: none`、manual 100 50 50 の nearest の ratio が 1 に近い、4 回の読みが違う、E-core の highest_mhz が 0 でない、module の段の MHz（揃う＝共有）、全負荷の 64FH の理由、Debian と zedBSD の PC2〜PC10。

### 未実施・残り（q2022-i01 の時点）

- 5330 の実機の 1 回（Q1 の割当待ち）。4（共有）と 5（理由）の結論はその数の後。
- 5 の直しは、理由が i915 の DC state なら i915 の WS、tick なら design.md §4.3.3 段 4（HAL の契約の外、ユーザーの判断）、PL1 や EC なら kernel の外の記録。
- scheduler に CPU に縛る口が無い事（`sched.c` の comment の「pinned」と steal の食い違い）は WS333 の外として Q1 に渡す。

## 5330 の 1 回（2026-11-13 00:04〜00:33 P1、q2023-i01、branch agent/p1-ws333i、基点 main 641f1ad69b）

### 準備

- worktree の `build/` に `make -j4 BUILD_JOBS=4 CONFIGMK=config/config.x86_64.mk llvm-toolchain-cached`（rc 0）→ `make -j4 BUILD_JOBS=4 PLATFORM=x86_64 CONFIGMK=config/config.x86_64.mk CONFIG_KERN_THERMAL=y CONFIG_DRIVER_X86_POWER_CONTROL=y sysroot`（rc 0、NoctLang は `--parallel 4`）。main の `build/` は使っていない。
- kernel（`kbuild.sh`、`build/tmp/p025k/`、git の外）: c6 716c57ba9d6c5091・diag 11a2cdac8aa13e90（前回と同じ sha256）、rc 0・warning 0。`thermal-static.sh` → `build/tmp/p025k/static/thermal`。
- 5330 は Debian（`uname`: Linux chaos 6.19.13+deb13-amd64）で始まり、各 run の後も Debian に戻った。`/tmp/i915-hw.lock` の下、`NOI915` は使っていない。

### 流した物（要約は `build/hw5330/ws333/<NAME>/summary.txt`、git の外）

| NAME | kernel（p4） | PARTS | 結果 |
| --- | --- | --- | --- |
| p025 | diag 11a2cdac8aa13e90 → c6 716c57ba9d6c5091 | 全部 | rc 0。module の段だけ script の不具合で無効（下） |
| p025b | 同上 | module idle | rc 0（script の直しの後） |
| p025-pc2 | Debian だけ（`5330-pc2-debian.sh`） | — | rc 0 |
| p025m | E-core を module の共有にした diag 2fd20d92aac74efb → c6 4c98d617279bf96b | strategies | rc 0 |
| p025c | c6 4c98d617279bf96b（起動の確かめだけ） | none | rc 0、p4 はこの kernel のまま |

- script の不具合（5ef2637190 で直した）: module の段の `while read … done < module-max.txt` の中の ssh が標準入力を読み、1 行目（domain 0）しか設定されなかった（`module-set-0.txt` だけ、`module-domains.txt` は E-core が全部 max 33）。ssh を全部 `-n` にし、`PARTS` で段を選べるようにし、idle の窓に全 CPU の core の C-state（2 回の累計の差）を足し、module の段は全 E-core を sample する（他の module が参照）。
- 足した道具（262a5d6377）: `5330-pc2-debian.sh` と `waker-debian.py`（Debian で全 CPU を共通の周期の倍数で起こし、zedBSD の 1 ms の tick を真似る）。

### (a) 1〜3: 要求の上限と読みの新しさ

- 一覧（`thermal cpu`、CPU ごとに 100 ms の sample）を、domain の max × highest_mhz ÷ highest の上限と比べた（5 % と 50 MHz の余裕）。p025・p025m の全部の strategy と直の設定の 4 回の読みで `above ceiling: none`。
  - performance（P 21-56・E 12-33、EPP 0）: 最高 4327〜4341 MHz（P、上限 4265〜4332）、3300（E）。balanced（1-56・1-33、128）: 3302〜4336。powersave（1-21・1-12、192）: P 1599〜1600 / 上限 1599、E 1200 / 1200。manual 100 50 50（P 1-38・E 1-22、0）: P 2899〜2900 / 2894（p025）、E 2200〜2202 / 2199（p025m）。manual 0 50 50（P 1-11・E 1-8、255）: 800 / 850 前後。直の `cpu set all max=500`（P 1-28・E 1-17）: P 2200 / 2132（比 1.03）、E 1700 / 1700。
  - p014 の「manual 100 で 1192 MHz」「E-core の上限超え」は再現しない。manual 100 50 50 の最も近い CPU の比は 1.00。
- 4 回の読みは変わる: CPU ごとの MHz が読みごとに違い（例 p025 balanced: 1 回目は CPU 8〜11 が 3300、2〜4 回目は CPU 4〜7 が 3300。spin の process が module の間を移る）、最高の値も performance で 3300・4327・3302・4341。
- E-core の highest_mhz: 4 回の起動とも 3299〜3300（P は 4265〜4340）。起動の log に `cpu … not measured` は無く、`cpu 4 (kind 2) runs at most about 3300 MHz`。

### (b) 4: E-core の module は 1 つの clock → `KERN_CPUPERF_SHARE_MODULE`

- p025b（全 CPU に負荷、module 3（CPU 8〜11）の CPU 10 だけ max 33、他の E-core は全部 max 8（efficient））: CPU 10 1642・CPU 11 1662・CPU 8 1687・CPU 9 1680 MHz、module 2（CPU 4〜7、全部 max 8）は 799〜800 MHz。max 8 の CPU 8・9・11 が自分の上限（約 800 MHz）を越えて CPU 10 と同じ clock で動いた → module の 4 つは 1 つの clock（hardware が要求をまとめる）。p025 の一覧でも、1 つの spin で同じ module の idle の 3 つが 3300 MHz を示す（C0 の短い間も module の clock）。
- 直し（262a5d6377）: `src/drivers/platform/pcat/cpu-x86/cpu-x86.c` で hybrid の efficient core の記録に `module_clock` を立て（`cpu-x86-private.h` に 1 field）、`cpu_x86_describe()` は その CPU を `KERN_CPUPERF_SHARE_MODULE` で出す。P-core と一種類の processor は `KERN_CPUPERF_SHARE_CORE` のまま。framework（`cpuperf.c`）は変えていない（module の共有は既にある: `cpuperf_shares()`、domain の intent は全 member に post）。
- host 試験: `cpu-x86-host` PASS（5330-topology で P は CORE・E は MODULE、新しい `one-kind`（hybrid でない HWP の processor は全部 CORE）83 check）。直す前の driver で 5330-topology が 8 件 FAIL を確かめた。`cpuperf-host` PASS（新しい `module-share` 34 check: domain 4 つ（0x3・0xc・0xf0・0xf00）、strategy が全 E-core に、module の直の設定がその 4 つだけに届く）。
- 5330（p025m）: 起動の log `12 of 12 CPUs answered, 4 domains`、`thermal cpu domains` は 0x3・0xc・0xf0・0xf00。5 つの strategy と直の設定で `above ceiling: none`。p025c で c6 4c98d617279bf96b が起動（4 domains）。
- 文書: `docs/architecture/power-management.md` は domain の組み方を書いていないので変えていない。WS301 design.md:143 の「確かめたら module を domain に」の条件を満たした。

### (c) 5: 全負荷の 1.4 GHz と package の C-state 0%

- 全負荷の 1.4 GHz の理由は firmware の電力の上限 PL1（kernel の外、直す物なし）:
  - zedBSD（diag、全 CPU に負荷）: performance P 1400〜1473・E 1199〜1200 MHz、package 12.59 W、64FH 0x14020400（今: PL1、記録: THERMAL・PL1・MAX_TURBO。SDM Vol. 4 Table 2-39）、HWP の platform-limit の印。balanced P 1413〜1428・E 1303〜1320、12.94 W、64FH 同じ。powersave（要求 P 1-21・E 1-12）P 1599〜1600・E 1199〜1200、13.53 W、64FH 0x14020000（今の理由なし＝要求で止まる）。
  - Debian（同じ日、`msr-debian.py load 5`）: 既定 P 1472〜1490・E 1390〜1391 MHz、12.82 W、64FH 0x1c020400（今 PL1）。EPP performance P 1504〜1506・E 1406 MHz、12.98 W、同じ。
  - MSR 610H は PL1 25 W・PL2 55 W（lock 0、powercap も 25 W）なのに 12.6〜13 W で PL1 が効く → 効いているのは firmware が置く MMIO の PL1（p002 の 13.0 W）。kernel は電力の上限を書かない。zedBSD の P・E が Debian より 100〜200 MHz 低いのは、同じ 13 W の中で zedBSD の package の他の電力（idle で約 2 W 多い、下）が大きいためと見られる（推定）。
- E2H（MSR_PKG_CST_CONFIG_CONTROL）は zedBSD・Debian とも 0x74008008（limit 8・lock 1・C1 の auto-demotion と undemotion・package の demotion と undemotion が 1、Table 2-35）。同じ値で lock されている → package の C-state の差の理由ではなく、kernel から変えられない。
- package の C-state 0% は tick ではない。i915 の側が有力（未証明）:
  - zedBSD idle（balanced、60 秒、2 回）: package 5.41・5.47 W、PC2・PC3・PC6・PC7 とも 0.0%。全 12 CPU の core C6 が 86.6〜91.1%（どの CPU も止まり続けてはいない）。
  - Debian idle: 3.25・3.34 W、PC2 98.8〜99.1%、PC3〜PC10 0%、cpu0 の core C7 99.6%。Debian は graphics の driver を入れていない（`lsmod` に i915・xe 無し、`/sys/class/drm` に device 無し。p012 の記録では iGPU は vfio-pci）。
  - Debian で全 CPU を 1 ms の共通の周期で起こす（`waker-debian.py`、zedBSD の tick の真似。Python なので 1 回の起きは tick より長い）: PC2 64.6%・cpu0 C7 83.6%・4.45 W。4 ms: PC2 90.5%・3.51 W。core の C-state の滞在は zedBSD（C6 約 88%）と同じ程度でも PC2 に入る → zedBSD の PC2 が完全に 0 なのは tick の起こしでは説明できない。
  - 残る差は zedBSD だけが GPU を動かしている事（i915 の GT・display）。起動の log では `rc6_enabled=1 rc6_ctl=0x40000`、`DC_STATE_EN=0x00000000`。GT が RC6 に入らない（forcewake の握りっ放し等）か display の engine の動きで package が PC0 に留まる、を候補とする。zedBSD の i915 は GT の RC6 の滞在（GEN6_GT_GFX_RC6）を読む口を持たず、ここでは確かめられない。
  - p014 の c10 の kernel も PC2 0%（p014 の表）なので MWAIT の hint の深さも理由ではない。
- kernel（WS333 の範囲）の中で直す物は見つからなかった。tick は Debian の真似で約 1.1 W（3.34 → 4.45 W）の重さがあり、tickless の idle（design.md §4.3.3 段 4）の要否の材料になる。

### p014 の判定の材料（design.md §8.4 の 1〜4）

| 項目 | 取れた物 | 残り |
| --- | --- | --- |
| 1 idle の電力 | p014: HLT 7.701 W > MWAIT c6 6.386 W（PASS の柱）、c10・periodic・Debian の数。今回: zedBSD 5.41〜5.47 W・Debian 3.25〜3.34 W、PC2 の差の切り分け（tick ではない） | package の C-state 0% の原因の確定（i915 の側の A/B）。Linux との差の目標（§11.2 の 9） |
| 2 i915 qs40 | 未（p014 も未） | WS291 p022 の qs40 × 600 の fps と GT の周波数 |
| 3 HWP の要求 | 全 strategy・直の設定で上限を超えない、manual 100 50 50 で上限の比 1.00、4 回の読みが変わる、E-core の 4 つ組の共有を確かめて domain にした | なし（PASS の柱「3 の MHz が要求に従う」は満たした） |
| 4 hwp=off・相対・S0 idle | p014 で `cpuperf.hwp=off` と相対の基準は取れた（p014 の記録） | S0 idle の回帰と S0 idle の後の要求 |

- 案: p014 は 3 と 1 の MWAIT < HLT で受け入れの数がそろった所まで cleared にし、残りを別の Phase にする。(i) i915 の WS（WS291 等）に「idle の GT の RC6 の滞在と forcewake の数を読む、5330 で `i915.start=manual`（同じ kernel で i915 を止めた形）と通常の A/B で package の PC2・電力を比べる」を渡す（5330 に遠隔の電源が無いので、起動しない時に人が電源を入れ直せる時間に）。(ii) WS333 に qs40 と S0 idle の 5330 の Phase を残す。(iii) tickless の idle（§4.3.3 段 4、HAL の契約の外）は (i) の後の数で要否を決める（p023 の条件）。

### 未実施・残り（q2023-i01）

- NOI915 の kernel と `i915.start=manual` の A/B（指示どおり使っていない。上の案 (i)）。
- qs40（受け入れ 2）と S0 idle（4 の残り）。
- p4 の kernel は 4c98d617279bf96b（この branch の tree の c6、module の共有を含む。p025c で起動を確かめた）。main の tree の c6 716c57ba9d6c5091 は Debian の `~/ws245/vmunix-p025-p025b`。
- 消してよい物（Q1 へ）: `plan/ws333/tests/hw5330/__pycache__/`（waker-debian.py の構文の確かめで py_compile が作った、git の外）、`build/tmp/oldsrc/`（直す前の driver の写し、試験の確かめ用）。

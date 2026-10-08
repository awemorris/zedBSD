<!-- awesome-plan project=zedbsd record=ws174-p003 -->
# ws174-p003: UEFI loader の key の検出と統合・docs・QEMU の試験

Parent: [WS174](../ws.md)
Status: cleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2）: T1-214 の再試行で全 cell PASS。1 回目の C3 の不検出は BUG-272（tracking））（旧: uncleared（2026-10-06 T1-213 FAIL。同日 P1 が直して T1 の再試験待ち。T1 の 5 cell が PASS するまで cleared にしない））
Disposition: normal
Queue: Q1 の dispatch（2026-10-05 夜、P1 へ p002・p003・p005）。承認: ユーザー「OKです。ブートローダの仕様変更を実装してください。BIOSは後日でよいです。」
依存: [ws174-p002](../phase002/phase.md)（`zbl_boot_override_apply()`）

## 範囲

設計 [design.md](../phase001/design.md)（第 3 版）§3・§4.1・§7.2〜7.4・§9.1 の K1・K2・§9.2・§10 の p003。Q1 の技術の決定で **S2 は外した**（標本は S0 と S1 の 2 点）。Q1 の割り当てで `boot-keys.c/.h` と K1・K2 はこの Phase。

- `bootloader/uefi/include/uefi.h`: `EFI_NOT_READY`、`EFI_INPUT_KEY`・`EFI_KEY_STATE`・`EFI_KEY_DATA`、修飾 key と toggle の bit、`EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL`、その GUID（UEFI 仕様の事実だけを自分の書き方で）。`EFI_SYSTEM_TABLE`・`EFI_BOOT_SERVICES` の slot は不変。
- `bootloader/uefi/boot-keys.h`・`boot-keys.c`（新）: `struct zbl_uefi_boot_keys`、`zbl_uefi_boot_keys_open()`（ConsoleInHandle → LocateProtocol、`SetState(VALID|EXPOSED)`、失敗は boot を止めない）、`zbl_uefi_boot_keys_sample()`（最大 32 回、累積）、`zbl_uefi_boot_keys_from_state()`（VALID の時だけ、左右の Ctrl → KMSG、左右の Shift → LOGIN）。file scope の static なし。
- `bootloader/uefi/bootx64.c`: `struct loader_context` に `keys`。S0 = `A64 UEFI ENTRY` の直後に `open` と `sample`。S1 = config の parse の直後に `sample` → `apply_boot_keys()`（key 無しなら呼ばない＝record は byte 単位で不変、-1 なら `fail_discovered("Override parameters")`）→ `zbl_logo_path()` → video の希望（if / else if / else: Ctrl 640x480、logo 1920x1080、他 0）→ `framebuffer_from_gop()` → `notice_boot_keys()`（`Boot: kernel messages (Ctrl)`・`Boot: console login (Shift)`・`A64 PARAMS OVERRIDE <record>`、SetMode の後）→ `show_logo()` → `quiet_boot`（書き換え後の record から）。`TEXT_MODE_WIDTH/HEIGHT`。
- `platform/amd64/vmunix.mk`: `$(BUILD)/uefi/boot-keys.o` の rule、`bootx64.o` の依存に `boot-keys.h`・`boot-override.h`、`BOOTX64.EFI` の link に `boot-keys.o`。
- docs: `docs/reference/kernel-boot-parameters.md`（§7c を新設、§7a・7b に 1 項ずつ、§9 の「行ごとに 1 token」「LoadOptions」「4 経路で同じ意味」に但し書き）、`bootloader/uefi/README.md`、`docs/howto/boot-and-storage.md`（Failure diagnosis）、`bootloader/README.md`（BIOS loader は key を読まない）。`plan/`・Bug への link は無い。
- 試験: `plan/ws174/tests/boot-keys-host-test.c`・`run-boot-keys-host-test.sh`（K1・K2）、`run-boot-keys-qemu.py`・`run-boot-keys-qemu.sh`（T1 用の 5 cell）。

## 検証（2026-10-06、P1 の worktree）

| 確認 | コマンド | 結果 |
| --- | --- | --- |
| host 試験 K1・K2 | `timeout 120 plan/ws174/tests/run-boot-keys-host-test.sh`（cc = gcc）と `CC=clang ... build/ws174-host-clang` | `30 checks, 0 failures` ×4（gcc・clang、通常・ASan/UBSan。mock は ms_abi の callback） |
| host 試験 O1〜O11（p002 の回帰） | `run-boot-override-host-test.sh`・`run-boot-override-kernel-host-test.sh` | 43/0 ×2、72/0 |
| UEFI loader | `make -j16 ZEDBSD_CONFIG=plan/ws174/tests/config-amd64-keys.mk BUILD=build/ws174-keys build/ws174-keys/uefi/BOOTX64.EFI` | rc 0、warning 0（`-Werror`）、未解決の symbol なし、`BOOTX64.EFI check: PASS` |
| amd64 BIOS loader（共有の parser・logo-path は不変） | 同じ BUILD で `build/ws174-keys/bootloader/BOOTZBSD.EXE` | rc 0、warning 0 |
| amd64 image | `flock /tmp/zedbsd-image-build.lock timeout 5400 plan/tools/guest/test-image.sh plan/ws174/tests/config-amd64-keys.mk build/ws174-keys` | rc 0、`check-amd64-native-image: ... OK`。log の warning は openssh・openssl の package と noct の既存の物で、bootloader・kernel の file には 0。cfg は `kernel=vmunix rootpart=… swap0=… logo=logo.ppm login=graphical kmsg=quiet`（cfg の生成は不変） |
| QEMU script の判定の部品 | `run-boot-keys-qemu.py` の `classify()` を合成の PPM（640x480 の text、1920x1080 の中央 80x30 の text、市松、黒）と実際の `boot-logo.ppm` を 1920x1080 に置いた画面で | text・text・logo・other・logo。ブロック文字（塊の明暗）は text に数えない |
| `git diff --check` | | 0 |

- **QEMU（T1）: 未実施**（Q1 経由で依頼する。下の「T1 への依頼」）。実装の担当は QEMU を起動していない。
- **実機: 未実施**（WS の受け入れ、ユーザーと Q1）。

## 確かめた事実と限界

- kernel の keyboard driver は LED を自分で設定しない: `src/drivers/platform/pcat/ps2-8042.c` は LED の command（0xED）を送らない。`src/drivers/usb/usb-hid.c` が出す output report は raw の interface（FIDO、hidraw）への要求だけ。従って `SetState(EXPOSED)` が消した NumLock 等の LED は kernel が戻さない（docs §7c に記載）。QEMU では観測できない。
- S2 は外した（Q1 の決定）。S1 の後（config の parse の後、kernel の読み込みの間）に打たれた key は使われない。
- 告知（`Boot: …`・`A64 PARAMS OVERRIDE`）は画面では best effort（GOP の直接の SetMode の後の ConOut、logo、kernel の console）。debug port には常に出る。判定には使わない。
- `kern.boot.kmsg` の sysctl は無い。Ctrl の判定は screendump の kernel の text と、SSH で読む `dmesg` の `boot: parameters:` の行（kernel の log buffer。console log ではない）で行う。
- `plan/ws013/tests/run-uefi-zedbsd-config-ovmf.sh` は `bootx64.c` を自分で link するが、既に `video.o`・`logo.o` を欠いた古い試験で、この Phase の前から link できない（触っていない）。

## T1 への依頼（Q1 経由）

- image: branch `agent/p1` の commit `b6af5b35`（source の最終。これより後の commit は plan の記録だけ）か、それを merge した main で `plan/tools/guest/test-image.sh plan/ws174/tests/config-amd64-keys.mk build/ws174-keys`（graphical boot、`kmsg=quiet`）。
- 実行: `timeout 1800 plan/ws174/tests/run-boot-keys-qemu.sh build/ws174-keys/hdd-image.img <OUTDIR>`。cell ごとに image の複写と OVMF の変数の複写から QEMU を起動し直す（同時に 1 つ、`-no-reboot`、NVMe の boot disk、usb-net port 2、usb-kbd port 3、QMP）。key は QMP `send-key`（0.1 s ごと、hold 50 ms、修飾 key も毎回押し直す）。
- 5 cell と判定（screendump の PNG と SSH だけ。console・serial の log は読まない）:

| cell | 操作 | (a) 画面 | (b) | (c) SSH |
| --- | --- | --- | --- | --- |
| C0 none | 無し | loader の後に logo | login prompt | `sysctl kern.boot.login` = `graphical`、dmesg `boot: parameters:` に `kmsg=quiet` と `logo=` |
| C1 Ctrl | `ctrl`+`spc` | loader の後に logo 無しで kernel の text | login prompt | `graphical`、`kmsg=console`、`logo=` 無し |
| C2 Shift | `shift`+`spc` | logo | login prompt | **`console`**、`kmsg=quiet`、`logo=` |
| C3 Ctrl+Shift | `ctrl`+`shift`+`spc` | logo 無しで kernel の text | login prompt | **`console`**、`kmsg=console`、`logo=` 無し |
| C4 late | loader が mode を変えて 5 s 後から `ctrl`+`spc`・`shift`+`spc` を 2 s | logo | login prompt | `graphical`、`kmsg=quiet`、`logo=`（loader の後の key は効かない） |

- 記録（判定ではない）: C1・C3 の loader の mode（640x480 が通ったか、`loader_modes`）、C1 の最初の frame に告知 `Boot: kernel messages (Ctrl)` が写るか（PNG を見る）。
- C1〜C3 の全部で key が検出されなければ `--no-usb-kbd --cells C1` で 1 回だけ再試行し（key は PS/2 へ）、分けて記録する。余裕があれば `--ctrl-alone`（R0: Space なしの `ctrl` だけ）を参考に流す（判定に入れない）。
- 結果: `<OUTDIR>/summary.json`、cell ごとの `result.json`・`frames/*.png`・`login.png`。PASS/FAIL と PNG を Q1 へ。C1〜C3 のどれかが FAIL なら p003 は uncleared。

## T1-213 の FAIL と直し（2026-10-06、P1 第 2 世代）

T1-213（main 357fed03、run 1・再試行・`--no-usb-kbd --cells C1`）は FAIL。証拠は T1 の worktree の `build/ws174-keys/t1-213{,-retry,-nousbkbd}/`。

### (a) 全 cell で `kern.boot.login` が空

- 原因（試験の側、source で確定）: zedBSD の `sysctl`（`userland/base/sysctl/main.c`）は `-n` を持たない。`sysctl -n kern.boot.login` は argc が 3 で usage を stderr に出して 2 で終わり、stdout は空。kernel の `kern.boot.login`（`src/kern/sysctl.c`）と `sessiond` の `sysctlbyname` は正しい（dmesg の `boot: parameters:` も期待どおりだった）。
- 直し: `run-boot-keys-qemu.py` は `sysctl kern.boot.login` を流して `kern.boot.login: VALUE` の行から値を取る。返り値・stdout・stderr を `result.json` の `sysctl_output` に残す。

### (b) key の検出が不安定

観測: 7 つの key の cell（C1・C2 ×2、C3 ×2、PS/2 の C1）で検出は C3 の再試行の 1 回だけ。その 1 回は Ctrl と Shift の両方の bit が立った（1 つの event を読んだ形）。key は電源投入から loader の後まで切れ目なく送られていた（C1 で 44 chord、loader の mode の変化は 2.6 s、送信はその 2 s 後まで）。

原因（source と UEFI の振る舞いからの推定。QEMU は起動していないので gdbstub での確認は無い）:

1. **loader は queue の event しか見ていなかった。** `zbl_uefi_boot_keys_sample()` は `ReadKeyStrokeEx` が `EFI_NOT_READY` を返すと何も見ずに抜けていた。UEFI の Ex の入力は queue が空の時も `EFI_NOT_READY` と共に**今押されている修飾 key の状態**を `KeyState` に入れて返す（UEFI 2.x の `ReadKeyStrokeEx` の `EFI_NOT_READY` の定義。EDK2 の ConSplitter・USB/PS/2 の keyboard driver もそう実装している、と記憶。**この OVMF での確認は未実施**）。loader はこれを捨てていた。
2. **loader の前の event は firmware が読んでしまう見込み。** EDK2 の BDS は boot option を起動する前に ConIn を読み捨てる（記憶、**未確認**）。送り続けた 44 chord がほぼ全部検出されなかったことと合う。残るのは StartImage から S1（config の parse の直後）までの数 ms〜数十 ms に firmware の queue に入った event だけで、それに当たるかは運（C3 の再試行で 1 回当たった）。
3. **試験の送り方も「押したまま」になっていなかった。** QMP `send-key` は chord ごとに修飾 key も離す（`hold-time` の後に全部の key を上げる）。人の手順（Ctrl を押したまま Space を叩く）と違い、標本の瞬間に Ctrl が上がっていることがある。

直し（待ち時間は足さない）:

- `bootloader/uefi/boot-keys.c`: `ReadKeyStrokeEx` が `EFI_NOT_READY` を返した時、その `KeyState` を `zbl_uefi_boot_keys_from_state()` に通して `held` に足す（`EFI_SHIFT_STATE_VALID` が無ければ 0）。毎回 `data` を 0 にしてから呼ぶので、状態を埋めない firmware では何も足さない。他の error（`EFI_DEVICE_ERROR` 等）の `data` は信じない。S0・S1 の 2 点で「その瞬間に押されている Ctrl/Shift」が読めるので、firmware が前の event を読み捨てても、押したままなら検出される。
- `plan/ws174/tests/run-boot-keys-qemu.py`: QMP `input-send-event` で、修飾 key を押したまま Space を 0.1 s ごとに叩く（Space は 50 ms で離す）。修飾 key は 0.1 s ごとに down を送り直す（押されている間は何も変えず、firmware や kernel の USB の reset で QEMU の usb-hid が状態を忘れた後に押し直す）。送信をやめる時に離す。C4 は chord ごとに前の chord の修飾 key を離す。R0（`--ctrl-alone`）は Ctrl だけを押したまま（Space なし）。
- docs（`docs/reference/kernel-boot-parameters.md` §7c、`bootloader/uefi/README.md`）: loader は queue と「その瞬間の修飾 key の状態」を読む、と書き直した。主の手順（押したまま Space を叩く）は不変。

待つ案（短い polling の窓、`WaitForEvent` と timer）は採らなかった: 原因 1 を直せば、押したままの key は待たずに S0・S1 で読める。再試験でも C1〜C3 が落ちる時に Q1 に諮る（下の「残る案」）。

### 検証（2026-10-06、P1 の worktree、QEMU なし）

| 確認 | コマンド | 結果 |
| --- | --- | --- |
| host 試験 K1〜K3 | `timeout 120 plan/ws174/tests/run-boot-keys-host-test.sh`、`CC=clang ... build/ws174-host-clang` | `41 checks, 0 failures` ×4（gcc・clang、通常・ASan/UBSan）。K3 を追加: 空の queue と押された Ctrl → KMSG、Ctrl+Shift → 両方、queue の Ctrl と押された Shift → 両方、離した後も保つ、VALID なし → 0、状態を埋めない firmware → 足さない、`EFI_DEVICE_ERROR` の状態は無視 |
| host 試験 O1〜O10（回帰） | `run-boot-override-host-test.sh` | 43/0 ×2 |
| UEFI loader | `make -j16 ZEDBSD_CONFIG=plan/ws174/tests/config-amd64-keys.mk BUILD=build/ws174-keys build/ws174-keys/uefi/BOOTX64.EFI` | rc 0、warning 0、`BOOTX64.EFI check: PASS` |
| amd64 image | `flock /tmp/zedbsd-image-build.lock timeout 5400 plan/tools/guest/test-image.sh plan/ws174/tests/config-amd64-keys.mk build/ws174-keys` | rc 0、log の warning 0、`check-amd64-native-image: ... OK` |
| 送り方の部品 | `Guest.send_chord()` を偽の QMP で（ctrl+spc → shift+spc → ctrl+shift+spc → ctrl） | 修飾 key の down＋spc の down、spc の up、次の chord に無い修飾 key の up、最後に全部の up の順 |
| `git diff --check` | | 0 |

- QEMU（T1）: 未実施（再依頼。下）。実機: 未実施。

### 残る案（再試験でも検出されない時に Q1 へ）

1. OVMF が `EFI_NOT_READY` で状態を返さない場合: gdbstub で `zbl_uefi_boot_keys_sample` に break を置き、`ReadKeyStrokeEx` の返り値と `KeyState` を見る（T1 か解析の担当）。
2. 待ちの案（ユーザーに見える変化。「1 秒止める」は No の決定）: S1 で最大 100〜200 ms、`ReadKeyStrokeEx` を数 ms ごとに読む polling（key が見つかれば直ちに抜ける）。key 無しの boot は毎回その分遅くなるので、P1 の推奨は採らないこと。

### T1 への再依頼（Q1 経由）

- image: branch `agent/p1` のこの commit（または merge 後の main）で `flock /tmp/zedbsd-image-build.lock plan/tools/guest/test-image.sh plan/ws174/tests/config-amd64-keys.mk BUILD`。
- 実行: `timeout 1800 plan/ws174/tests/run-boot-keys-qemu.sh BUILD/hdd-image.img OUTDIR --ctrl-alone`（R0 は参考、判定に入れない）。cell と判定は上の表のまま。ただし key は「修飾 key を押したまま Space を叩く」（`input-send-event`）、(c) は `sysctl kern.boot.login`（`-n` なし）の値。
- C1〜C3 の全部で検出されなければ `--no-usb-kbd --cells C1` を 1 回。
- 記録（判定ではない）: R0 の結果（Space なしの Ctrl の押しっぱなしが効くか）、C1・C3 の `loader_modes`、C1 の最初の frame の告知。


## 2026-10-06 Q1 の判定（T1-214）

run 1 で C3（Ctrl+Shift+Space）だけ不検出、再試行は全 cell PASS。C1・C2・R0 は 2 回とも検出。sysctl の試験の直しは有効。**間欠の不検出（1/12 の key の cell）が残るので p003 は uncleared のまま**。機能は実機の UAT（BUG-202 の診断）で使ってよい。再開の条件: 不検出の原因（firmware の ConIn の flush の時と S0・S1 の時の重なり、または QEMU の usb-kbd の reset）の gdbstub での確かめ、または実機で数回の起動の観察。手順は「押したまま Space を叩く」を続けること。

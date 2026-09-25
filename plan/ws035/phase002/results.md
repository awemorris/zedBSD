# ws035-p002 結果: `include/drivers/` を `src/drivers/` の階層に揃える

Queue q317 / 項目 q317-i01。実行 2026-09-23 04:50〜05:45（+09:00）、`/home/awe/zedBSD-rpi4`。
開始時 HEAD `e7a2fb87`（作業ツリーclean）。実行中に root が計画文書だけのcommit（`f4652a4f`、`954de426`）を
重ねたが、このPhaseの変更は含まれていない。**git commit はしていない。** `git mv` による rename は index に
staged のまま、他の編集は未stageである。

## 結果の要約

- `include/drivers/` のフラットなヘッダ38個を `git mv` で `src/drivers/` と同じ階層へ移した（直下のファイル0、
  空になった `graphics/`・`hid/` は削除）。中身の変更は登録関数の改名2行と、移したヘッダ同士の `#include` 7行だけ。
- i915・Venusの公開ヘッダは `include/drivers/pci/pci-i915.h`・`include/drivers/pci/pci-venus.h`（`gpu/i915.h` は作っていない）。
- `drv_i915_pci_driver_register` → `drv_pci_i915_driver_register`、`drv_venus_pci_driver_register` →
  `drv_pci_venus_driver_register`。定義・宣言・`src/kern/platform/pcat.c` の呼出しを直した（計6箇所）。
- HAL（`src/hal/`、`include/hal/`）の変更は **0行**。UAPI（`include/uapi/`）と `libc/` も変更なし。
- `src/drivers/gpu/i915-old/` は書き換えていない（旧 include のまま17行）。
- amd64・i915-amd64（と追加でVenus-amd64）の kernel、amd64・i915-amd64 の userland が warning 0 で通った。
- kernel include 監査は p001 基準と一致（PASS）。i915・GPU core の host 試験は PASS、一部は移動前から失敗（同一内容）。

## 移動（対応表）

| 旧 | 新 |
| --- | --- |
| `disklabel.h` | `disklabel/disklabel.h` |
| `dma.h` | `generic/dma.h` |
| `dp8390.h` | `ethernet/dp8390.h` |
| `gpu.h`、`gpu-display.h`、`gpu-fence.h`、`gpu-scanout.h`、`gpu-share.h` | `gpu/` 同名 |
| `i915.h` | `pci/pci-i915.h`（改名） |
| `venus.h` | `pci/pci-venus.h`（改名） |
| `graphics/pc98.h`、`hid/pc98-busmouse.h`、`pc98-lgy98.h` | `platform/pc98/graphics/pc98.h`、`platform/pc98/` 同名 |
| `graphics/pcat.h`、`hid/ps2-8042.h`、`pcat-ide.h` | `platform/pcat/graphics/pcat.h`、`platform/pcat/` 同名 |
| `hid/hid-report.h` | `usb/hid-report.h` |
| `pcat-ne2000.h` | `isa/pcat-ne2000.h` |
| `pci.h`、`pci-ehci.h`、`pci-uhci.h`、`pci-pcat.h`、`pci-nvme.h`、`pci-nvme-protocol.h`、`pci-xhci*.h`（4） | `pci/` 同名（xHCIはsourceの場所 `src/drivers/pci/` に合わせた） |
| `pci-intel-ax211.h` | `wifi/intel-ax211/pci-intel-ax211.h` |
| `usb.h`、`usb-*.h`（9） | `usb/` 同名 |

（すべて `include/drivers/` 配下。全38行は `plan/ws035/refactor-refs/moves.tsv` ではなく、移動前の
`plan/ws035/phase002/pre-refs/moves.tsv` と、生成し直した `plan/ws035/refactor-map.md` の「移動済み」表にある。）

include guard（例 `DRIVERS_I915_H`、`KERN_DRIVERS_PCI_H`）は「中身は変えない」に従い、そのままにした。

## 実行したコマンドと結果

### 1. 移動前の参照の再確認と、対応表の更新

- `plan/ws035/tests/refactor-refs.py` を更新した（下の「変更したファイル」）。i915・Venusの改名先を P002 表へ入れ、
  移動済み（旧パスが無く新パスがある）を判定して、旧パスを指す行を `stale.tsv` に出す機能を加えた。
- `timeout 300 python3 plan/ws035/tests/refactor-refs.py --out plan/ws035/phase002/pre-refs --map-md ""`（移動前）:
  p002 は編集が要る行 242（p001時点 231）。増えた11行は p001 後に追加された WS035 の道具自身の行だけで、
  source・build・他の試験の参照は p001 と同一（`refs.tsv` の非記録行を比較）。
- `pre-refs/refs.tsv` は、サイズを抑えるため plan記録の行を除いて保存した（適用には使わない行）。

### 2. 移動と置換

- `python3 plan/ws035/phase002/apply-moves.py --dry-run`、続けて本実行:
  `files edited: 128, lines edited: 213, moves: 38, renamed identifiers: 6`。
  - `refs.tsv` の各行について、`#include` は綴りを新しいパスへ、試験scriptのパス文字列は新パスへ置換。
  - 残した行: plan記録、`i915-old/`（17行）、`AGENTS.md:386`（q314の引き継ぎ記録）、WS035の道具自身のディレクトリ単位の記述。
- p001の目録が拾えなかった参照（`$repo/include/drivers/...`、`repo + "/include/drivers/..."` の形）を
  `refactor-refs.py` の接頭辞規則を広げて見つけ、手で直した（7行）:
  `plan/ws004/tests/qemu-usb-cdc-ecm.noct:237`、`plan/ws004/tests/run-usb-recovery-contract-test.sh:25`、
  `plan/ws006/tests/qemu-usb-hid-acceptance.sh:238`、`plan/ws006/tests/run-usb-hid-driver-test.sh:57`、
  `plan/ws018/tests/run-input-hid-host-test.sh:41`、`plan/ws018/tests/run-platform-layout-audit.sh:62,66`。
- `rmdir include/drivers/graphics include/drivers/hid`（空になったディレクトリ）。

### 3. 旧パスへの参照（受け入れ条件）

`timeout 900 python3 plan/ws035/tests/refactor-refs.py`（17秒、2回実行して出力が同一）:

- p002: 移動済み38、旧パスを指す行 525。うち plan記録・`i915-old/`・この道具自身・`AGENTS.md` を除く **1行**:
  `plan/ws018/tests/run-input-hid-host-test.sh:40` の `test -e "$repo/include/drivers/hid/ps2-mouse.h"`。
  これは p002 の移動対象ではなく、`b29b9c39` で既に削除されたファイルを指す、移動前から壊れた行である
  （同じscriptは移動前も失敗し、この行は移動と無関係）。ファイル単位の旧パス（38個）を指す行は **0**。
- `git grep` で旧パスの綴り（`drivers/pci.h` 等、接頭辞なし）も探し、plan記録と `i915-old/` 以外で0件。
- HAL配下で変わる行 0（`git diff -- src/hal include/hal` 0行）。

### 4. build（1回1800秒上限、`JOBS=32`）

`sh plan/ws035/tests/refactor-build.sh p002 <name> <config> <targets>`。LLVM はこのcheckoutの `build/llvm`、
sysroot は `build/ws035-p002/sysroot/amd64` に新しく作り、Noct host は p001 の私用toolchainの写しを使った。

| name | config | target | 結果 |
| --- | --- | --- | --- |
| sysroot-amd64 | `config/ci/config-amd64.mk` | `sysroot-amd64` | 成功。clang driver警告1件（`-no-pie` 未使用、p001と同じ既存の警告、kernel・userlandではない） |
| amd64 | `config/ci/config-amd64.mk` | `vmunix` | 成功、warning 0、`amd64 vmunix check: PASS`、2秒 |
| i915-amd64 | `plan/ws029/tests/config-i915-amd64.mk` | `vmunix` | 成功、warning 0、`vmunix check: PASS`、`drv_pci_i915_driver_register` がlinkされている |
| venus-amd64（追加） | `plan/ws014/tests/config-venus-amd64.mk` | `vmunix` | 成功、warning 0、`vmunix check: PASS`、`drv_pci_venus_driver_register` がlinkされている |
| amd64-user | `plan/ws035/tests/config-amd64-userland.mk` | rootfs-binの生成物174 | 成功、warning 0、174個すべて存在 |
| i915-amd64-user | `plan/ws029/tests/config-i915-amd64.mk` | rootfs-binの生成物53 | 成功、warning 0、53個すべて存在 |

最初の amd64 の実行は sysroot が無く止まったため、`build/ws035-p002/{sysroot,amd64}` を消して、sysroot を
先に別名でbuildしてからやり直した（上表はやり直しの結果）。

### 5. kernel include 監査

`kernel-include-audit.py` に `--out DIR` を加え、`plan/ws035/phase002/include-audit/` へ出力した。

- `amd64`: objects 215、failed 0、libcヘッダ kernel 32・HAL 7・kernel内libc 37。`--compare` 対 p001: **PASS**。
- `i915-amd64`: objects 277、failed 0、kernel 35・HAL 7・kernel内libc 37。`--compare`: **PASS**。
- 追加の確認: kernel が読む `include/drivers/` のヘッダ数は前後同じ（amd64 29、i915 24）で、フラットなパスは0。
  drivers以外の kernel 由来ヘッダの集合は前後で一致。

### 6. host 試験（ログは `plan/ws035/phase002/host-tests/`）

| 試験 | 結果 |
| --- | --- |
| `sh src/drivers/gpu/i915/tests/contracts/run.sh` | PASS（42 checks、通常＋ASan/UBSan） |
| `sh plan/ws031/tests/run-vk-host-tests.sh` | PASS（10 fixture、通常＋sanitizer、1分55秒） |
| `plan/ws014/tests/run-gpu-{fence-close,fence-payload,fence-reuse,fence,framework,job,placement,scanout,sharing,supervision,topology}-test.sh` | 11件すべて PASS |
| `plan/ws014/tests/run-pci-service-lifecycle-host.sh` | PASS |
| `plan/ws014/tests/run-gpu-build-selection-test.py`、`plan/ws029/tests/run-i915-build-selection-test.py` | PASS |
| `plan/ws031/tests/run-mview-host-test.sh` | PASS |
| `plan/ws031/tests/run-{capture,dp,lcd,lcd-modeset,native-decide,opregion}-host-test.sh` | 失敗。**移動前（HEADを `git archive` で展開した木）でも同じ失敗**: `drv_i915_perf_{now,add,report}` の未定義（試験のlink対象に `perf.c` が無い）。未定義の一覧は前後で一致 |
| `plan/ws029/tests/run-i915-host-tests.sh` | 失敗。移動前も同じ（`drv_i915_perf_now` 未定義）。error行は前後で一致 |

参考として、include を書き換えた他の plan 試験の runner 21件（`plan/ws004`・`ws006`・`ws018`・`ws019`・`ws025`・`ws002`）
を前後の木で実行した（`host-tests/related/`）。前後で終了状態・最初のerror・末尾2行が一致（パス名を正規化して比較）。
うち19件は移動前から失敗しており（`__UINT64_C_SUFFIX__`、`atomic_raw_load_acquire` の型衝突、`plan/ws025/temp` が無い等）、
移動で新たに「ヘッダが見つからない」errorは出ていない。移動前から失敗する試験では、書き換えた行まで到達しない
ものがあり、その行の正しさまでは実行で確かめていない。

### 7. `git diff --check`

PASS（新規ファイルも intent-to-add で含めて確認し、その後 index から戻した）。

## 変更したファイル

- 移動: `include/drivers/` の38ファイル（上表）。
- `#include` の置換: `src/drivers/` 71ファイル、`src/kern/main.c`、`src/kern/platform/{pcat,pc98,rpi4,sun4u,x68k}.c`、
  移したヘッダ5個（`gpu/gpu.h`、`gpu/gpu-share.h`、`pci/pci.h`、`pci/pci-xhci-control.h`、`usb/usb.h` の計7行）。
- 改名: `include/drivers/pci/pci-{i915,venus}.h`、`src/drivers/gpu/i915/i915.c`、`src/drivers/gpu/venus/venus.c`、`src/kern/platform/pcat.c`。
- plan 試験: `plan/{ws003,ws004,ws006,ws014,ws018,ws019,ws025,ws029,ws030}/tests/` の C・script（計52ファイル）。
- 道具:
  - `plan/ws035/tests/refactor-refs.py`: i915・Venusの改名先、移動済みの判定と `stale.tsv`（旧パスを指す行と、
    除外理由 `exempt`/`CHECK`）、`$repo/`・`"/include..."` 形の接頭辞、移動の終わったディレクトリ単位の名前を数えない、
    **自分の出力（`plan/ws035/refactor-refs/`、`refactor-map.md`）を走査しない**（下の残課題1）。
  - `plan/ws035/tests/kernel-include-audit.py`: `--out DIR`。
  - `plan/ws035/tests/refactor-build.sh`（新規）: refactor Phase 用の私用build（`build/ws035-<phase>/`）。
- 生成し直した記録: `plan/ws035/refactor-map.md` の生成節（手書きの本文は変えていない）、`plan/ws035/refactor-refs/` 一式
  （`stale.tsv` は新規）。

## 成果物

- `plan/ws035/phase002/apply-moves.py`（移動・置換・改名を行ったscript）
- `plan/ws035/phase002/pre-refs/`（移動前の目録。適用の入力）
- `plan/ws035/phase002/refactor-refs-run.txt`（移動後の目録の要約）
- `plan/ws035/phase002/build/*.log`、`*.targets`
- `plan/ws035/phase002/include-audit/{amd64,i915-amd64}.json`
- `plan/ws035/phase002/host-tests/`（`*.baseline-HEAD.log` は移動前の木での同じ試験）
- build出力 `build/ws035-p002/`（`.gitignore` 対象）

## 受け入れ条件の達成状況

| 条件 | 状況 |
| --- | --- |
| `include/drivers/` 直下にフラットなヘッダが無い（対応表どおり） | 達成。直下のファイル0、38個が対応表どおりの場所にある |
| 旧パスへの参照が、計画の記録と `i915-old/` を除いて0件 | ファイル単位の旧パス38個については達成（0件）。ただし `refactor-refs.py` の検査は1行を報告する: 移動前から存在しないファイル `include/drivers/hid/ps2-mouse.h` を指す試験の行（p002対象外、残課題2）。ほかに `AGENTS.md:386`（q314の記録）を記録として除外した |
| amd64・i915-amd64 の kernel と userland の build が通り、warning 0 | 達成（Venus-amd64 kernel も追加で確認） |
| `kernel-include-audit.py --compare` で libc ヘッダの読込みが増えていない | 達成（amd64・i915-amd64 とも PASS） |
| i915・GPU core の host 試験が PASS | contracts、vk host、GPU core 11件、PCI service、build selection 2件、mview は PASS。ws031 の表示系6件と ws029 の旧 host 試験は失敗するが、移動前から同じ失敗（`drv_i915_perf_*` 未定義）で、このPhaseの回帰ではない |
| `git diff --check` が PASS | 達成 |

## 未実施の確認

- amd64 以外の platform の build（範囲外、ユーザー決定で壊れてもよい）。pcat・pc98 等の `#include` も置換してあるが、compileは試していない。
- QEMU・実機での起動（範囲外）。
- rootfs の組み立て、disk-image（p001と同じ理由で rootfs-bin の生成物までに留めた）。
- 移動前から失敗する plan 試験では、書き換えた行の実行による確認はできていない。

## 残課題・人の判断が要る点

1. **目録の自己参照（p001の道具の不具合）**: p001 で `plan/ws035/refactor-refs/refs.tsv` が追跡されるようになったため、
   `refactor-refs.py` を繰り返すと前回の `refs.tsv` の行を参照として数え、実行ごとに `refs.tsv` が大きくなった
   （この作業中に一度 9.5 GB に達したので削除した。commit はしていない）。道具が自分の出力と `refactor-map.md` を
   走査しないよう直した。このため `refactor-map.md` の p003・p023・p004 の件数は p001 の表と数え方が変わっている
   （接頭辞規則を広げたことによる増加も含む。例: p023 の編集が要る行 137 → 281、p003 116 → 131、p004 37 → 49）。
   p003 以降は、この再生成後の数を使うこと。
2. `plan/ws018/tests/run-input-hid-host-test.sh:40`（存在しない `ps2-mouse.h`）と、同じscriptの `src/drivers/hid` の参照は
   移動前から壊れている。p002 では触っていない。直すか、試験を退役させるかは担当WSの判断。
3. `AGENTS.md:386` の `include/drivers/i915.h`・`drv_i915_pci_driver_register()` は q314 の記録として残した。
   現在の名前に書き換えるかは root の判断。
4. ws031 の表示系 host 試験6件と `plan/ws029/tests/run-i915-host-tests.sh` は、`drv_i915_perf_*` を含む `perf.c` を
   link 対象に入れていないため、移動前から失敗している（WS031 の範囲）。
5. include guard 名はパスと一致しない（`DRIVERS_I915_H` 等）。中身を変えない方針で残した。
6. `plan/ws035/kcrt-design.md` が index に `A` で入っている。並行する p033 のもので、このPhaseでは触っていない。

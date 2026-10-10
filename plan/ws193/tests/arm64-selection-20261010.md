# arm64選択の修正: 2026-10-10

Base/main: a5e1a182f、branch codex/fix-menuconfig-arm64、独立worktree menuconfig-arm64。
Scope: tools/menuconfig.py のselect_cpu_board()、3行。C変更無し。user config.mk/main/shared buildは変更無し。

## 原因と修正

MENU_CPUS のarm64行は `(arm64, arm64, rpi4, Raspberry Pi 4)`。最初はarchitecture、3番目がplatform。旧codeは最初の値をZEDBSD_PLATFORMへ入れるため、normalize_targetが未知platform arm64をamd64へ戻していた。実select_cpu_board関数とcontrolled chooser回答0→1→Backで、選択後platform amd64を再現。

current CPU lookup、CPU chooserのselected index、ZEDBSD_PLATFORMへの代入をすべて[2]へ統一した。tuple/configの契約は不変。

## 限定確認

- 実関数/controlled chooser: arm64選択後platform rpi4、CPU arm64/Board Raspberry Pi 4/target Raspberry Pi 4 Arm64、CPU menu再表示selected index1、cancelで保持。PASS。
- save/load: own build/menuconfig-arm64/selected-rpi4.mkへ保存、platform rpi4/architecture arm64/board rpi4/variant default、load後保持。PASS。
- `make --no-print-directory -s ZEDBSD_CONFIG=build/menuconfig-arm64/selected-rpi4.mk validate-image-config`: exit0。
- 逆方向にCPU x86_64を選択: platform amd64/variant native。PASS。
- Python3.13.5 py_compile、git diff --check: PASS。
- 実cursesの `TERM=xterm-256color make menuconfig ZEDBSD_CONFIG=build/menuconfig-arm64/live.mk` をPTYで起動。CPU / Board→CPU→arm64を選択し、ヘッダー/CPU/Boardの上記表示を出力で確認。再CPU menuではarm64がhighlight。qで戻り保存、make validate-image-config exit0。

新suite/全体menuconfig-host-test/回帰/image build/QEMUは実施せず、この選択処理に限定した確認で終了。RPi4実機/VC4/GPUは未試験。新policy/依存/toolchain変更無し。

## 引き継ぎ

修正はWIP commitへ保存しmain統合を確認する。過去のCI main/push承認はこのmenu taskには適用せず、pushはしない。WS193 p001/p002の旧clearedとWS全体の既存acceptanceを保持。shared Master/Queue/cache、GitHub計画公開はQ1。

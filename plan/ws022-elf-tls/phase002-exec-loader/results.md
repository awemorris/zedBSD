# WS022-p002 q128 implementation log

Status: completed
Date: 2026-09-09

- TLS prefix ABI v5、静的実行ファイルのtemplate/初回TCB配置、exec/spawn TP設定を実装。
- i386 per-CPU GDTにGS TLS descriptor追加。interrupt_frameはDS/ESのみを保存し、GSを変更しない。execはframeに架空のGS fieldを追加せず、commit後のhal_task_set_tlsでdescriptorとhidden cacheを更新する。
- 通常amd64 build PASS: `/tmp/zedbsd-q128-amd64-p002b.log`。
- 通常i386 PCAT build PASS: `/tmp/zedbsd-q128-pcat-p002b.log`。
- ABIヘッダ更新に対するdynamic object→sysroot依存漏れを検出し、amd64/pcat makefileを修正。最初のamd64 buildは古いTCBヘッダを参照してFAIL、修正後PASS。
- QEMU初回: fixture assemblyのHAL_ARCH define不足でELF生成失敗。前提manifestチェックを追加し、生成を修正。
- 2回目: corpusが/usr/shareへread-only/non-executableで格納されEACCES。guest準備でchmodするよう修正。これはENOEXEC検証合格ではない。
- 3回目 (`temp/q128-loader-c`): ATAルートのflush時status=C0/error5でmount失敗、ユーザーコード未到達。入力試験で通過済みのxHCI USB boot構成へ切替えてTLS検証を継続する。ATA失敗の原因は未確定。

## 完了証拠

- amd64 xHCI USB QEMU: `../temp/q128-loader-d/` PASS。実行可能modeへ設定した不正9ELFはENOEXECで旧imageへ復帰。fork/exec後の正常ELFはTLS初期値・zero fill・TP自己参照を確認してexit0。
- i386 PC/AT QEMU: `../temp/q128-loader-i386-d/` 同じ9例とGS TLSの正常exec PASS。Q35/xHCIはIRQ establishment EIOで起動不可のため、サポート済みpc/PIC/IDE構成を使用。初回fixtureはSSEを発行してSIGILL、通常i386のno-SSE/soft-floatフラグを合わせて修正した。
- p003でruntime/linkerを仕上げ、volatile経由の整列観測、zero-only等の拡張とpthread/fork/signal/dynamic受け入れを継続する。

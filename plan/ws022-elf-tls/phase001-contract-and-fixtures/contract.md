# WS022 x86 static TLS contract (q127)

Date: 2026-09-09
Status: frozen for p002/p003 implementation

## 現ソースと必要な変更

- `src/kern/elf.c` はPT_TLSを無視している。`platform/{amd64,pcat}/user.ld`にPT_TLS PHDRはない。
- amd64 HALはFS_BASEをtask switchで保存・復元する。execで0へ戻すので、exec commit後・IRQ再開前に新TPを設定する。
- i386 HALのtlsフィールドはsoftware値のみ。per-CPU GDTにuser TLS descriptorを追加し、GS selectorをtask初期frame/exec frameに設定する。switchでは移行先のbaseをCPU-local descriptorへ書き、SET_TLSの現thread変更時もhidden segment cacheを更新する。syscall/interrupt/signal returnでuser GSを失わないことを確認する。
- 現TCB先頭はDTVで、compilerのFS:0/GS:0で要求されるself pointerではない。TCB prefixを明示し、rtld ABI version 4→5として全in-tree consumerを同時に再構築する。旧バイナリ互換は維持しない。
- static libcは固定4KiBの空TCBだけを確保する。pthreadは既存thread_alloc/free/attach境界を使うため、その境界にTLS mappingの所有権を実装する。

## メモリ配置と上限

x86 Variant II。TP直前に実行ファイルTLS、TP先頭wordにTP自身を置く。
`A=max(p_align,1)`、`D=p_memsz + ((-p_vaddr-p_memsz) & (A-1))`。
TLS byte 0はTP-D。この式は選択LLVM `lld/ELF/InputSection.cpp:getTlsTpOffset()`のEM_386/EM_X86_64と一致する。
TPは少なくともpage境界へ整列するためA<=4096を満たす。TLS本体上限は1MiB、TCB予約tailは4096bytes。TCB実サイズが予約内に収まるcompile-time gateを置く。

共通UAPI prefixをTCB先頭へ置く。順序はself、mapping base、mapping size、immutable template address、template file size、TLS memory size、alignment、TPからのdistance。全フィールドはtarget pointer/size幅。カーネルはこのprefixだけを知り、libc固有TCBメンバを参照しない。rtldのDTV/pthread_private等はprefixの後ろに置く。

新VMでread-only初期化templateを確保し、file content lease内でp_fileszだけコピーする。各threadのRW mappingはzero initialized、TP-Dにtemplateのfile bytesだけをコピーする。親threadがTLSを書き換えていてもその内容は子へコピーしない。templateはVM所有でexec/process exitまで生存し、thread_freeでは解放しない。forkではprivate VM cloneとして継承し、現threadのTLS値を維持する。

## ELF検証

PT_TLSは一つまで。filesz<=memsz、file range、target user address range、vaddr+memsz overflow、power-of-two alignment（0/1は1）、A<=4096、memsz<=1MiB、offset/vaddr congruenceを検証する。TLS file imageが対応PT_LOADの同一file/vaddr範囲内にあることを確認する。空TLSは空のtemplateとして扱い、zero-fill-onlyも受け入れる。
不正入力はENOEXEC。割当失敗はENOMEM、I/O失敗は実errno。新VMの破棄で全候補mappingを回収し、旧VM/TP/credentialを失う前に失敗する。

## exec / spawn / thread lifetime

- Static executable (no PT_INTERP): loaderは新VM内にtemplate、初回TLS/TCBを準備しimage_infoへTPを返す。spawnは未公開threadへ設定、execはhal_task_exec_current成功直後・IRQ無効中に設定する。
- no-TLS executable: TP=0の既存起動を保持し、libcが必要時に空TCBを確保する。
- pthread_create: 現TCBのtemplate metadataをもとに独立mappingを作成し、既存thread_create syscallで開始前にTPを渡す。failed createはallocator ownerが一度だけfree。
- join/ detached reaperは既存終了確認後にthread_free。実行中の自分のTCBを解放しない。main-thread/process teardownはVMの所有権に従う。
- signal returnでTP変更を巻き戻したり別threadのTPをロードしない。i386 selectorとamd64 FS baseの切替テストを含める。

## dynamic runtime境界

現在rtldはDTVと__tls_get_addrによる動的TLSを持つ。既存dynamic executableの動作を維持し、kernelがdynamic executableへstatic TCBを先行設定してrtld thread_attachと衝突させない。PT_TLS自体の形式検証は共有するが、PT_INTERPありのtemplateとTCBはrtldが所有する。
起動DSOを含めたinitial-exec/local-exec配置とpost-dlopen static reservationは初回static実装に含めず、必要なら後続Phaseを作る。既存dynamic GD TLS回帰はp003必須gate。今回TCB prefix変更はrtld側のself初期化・size gateを含めて同時実施する。

## 実測fixtureと受け入れ

`tests/build-tls-fixtures.sh`はproject clangでobjectを作り、project ld.lldで明示static link。llvm-readelf/objdumpで2arch各1TLS PHDR、filesz4、memsz>filesz、align64、FS/GS local-execを確認した。
`temp/q127-fixtures-lld/`に正常2例と不正18例、manifest、disassemblyを保持する。
最初の`temp/q127-fixtures/`はdriverのhost ld既定選択を検出した履歴で、正式証拠から除外。

p002ではこのELFにzero-only/empty/nonzero first-byte offset/duplicate/truncated実ファイルを追加し、実loaderで検証。p003ではlibcリンクしたguest probeで初回値、アドレス整列、独立更新、repeated join、failed allocation、fork、exec rollback、signal/preemptionを確認する。fixture生成PASSはloader/runtime PASSを意味しない。

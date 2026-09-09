# x86 executable TLS

zedBSDの静的amd64/i386実行ファイルはELF `PT_TLS`を使う。
コンパイラのlocal-exec TLSは、amd64でFS、i386でGSを使うVariant II配置に従う。
TPの直前に各スレッド専用のTLSデータがあり、TP先頭wordはTP自身である。

`exec`は新しいアドレス空間内に初回TLSとTCBを用意してから切り替える。
不正なTLSヘッダは`ENOEXEC`、割当失敗は通常のメモリ不足として失敗し、
切替前のプロセスを保持する。no-TLSと空PT_TLSも引き続き実行できる。

`pthread_create`は実行ファイルの読み取り専用初期化テンプレートを複製する。
親が書き換えたTLS値は新しいスレッドへ引き継がず、`.tbss`はゼロで始まる。
`fork`は呼出し元の現在のTLS値を子プロセスへ引き継ぎ、その後の変更を分離する。
終了したスレッドのTLS mappingはjoinerまたはdetached reaperが回収する。

実装上限はTLS本体1MiB、整列4096bytes、TCB予約4096bytes。
loader/libc共通prefixは[zedbsd/tls.h](../../include/uapi/zedbsd/tls.h)、
libc/rtldの私有ABIはversion 5。ABI v4のバイナリ互換を提供せず、成果物を再構築する。

動的runtimeのGD TLSと`__tls_get_addr`/DTVは引き続きrtldが所有する。
起動DSOのinitial-exec/local-exec統合と、`dlopen`後のstatic TLS予約は対象外。
静的TLSの実装を、それらの対応完了と解釈しない。

設計と受け入れの正本は[WS022](../../plan/ws022-elf-tls/ws.md)を参照。

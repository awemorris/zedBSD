# HAL syscall / fault 入口の整理（議論中）

日付: 2026-09-10
状態: 議論用。ユーザーは3入口を提示し、vectorの必要性を質問した。
この資料は新規HAL宣言の承認や実装済みという意味ではない。
既存の承認済みspace/pmem整理とは別の変更として扱う。

## ユーザーが提示した構成

| 固定入口 | 役割 |
| --- | --- |
| kernel_syscall_handler | hal_syscall_set_handlerを廃止し、固定関数からsyscall処理 |
| kernel_user_fault_handler | ユーザー由来の例外保存フレームを伴う処理 |
| kernel_sys_fault_handler | カーネル由来の例外保存フレームを伴う処理 |

kernel_user_int_handlerは現在syscallの観測記録だけを行う。
記録機能の置き場所を整理して入口を廃止する案。
hal_set_trap_handlerと登録用配列は固定sys fault入口に置き換える案。
両fault入口はC ABIで呼ぶ。ユーザー由来でもコールバックはカーネル側
スタック上で実行する。保存フレーム実体やアセンブリのレイアウトを
共通カーネルに公開しない。

## 現ソースを確認した結果

- src/kern/user-probe.cはvector 14とx86 error_codeのbit 1/4でVMの
  read/write/execを決める。またvectorをSIGFPE/SIGTRAP/SIGILL等へ変換する。
- ARM64/m68k/SPARCのHALは共通処理向けにx86風のvectorやerror_codeを
  組み立てている。vectorだけを消すとアクセス種別やシグナル分類が失われる。
- thread->fault_vectorは今回のsrc/include内検索では記録側だけで、
  共通処理の読み取り側が見つからない。診断プローブのvectorとは分けて
  必要性を判断する。外部テストやABIの利用がないことまで断定しない。
- fault callbackの戻り値は処理済み/未処理を表し、syscall戻り値とは別契約。
- SPARCはuserとsysでスタック・保存レジスタウィンドウの経路が異なる。
  この差はHAL内に維持し、同じフレーム形状へ強制統一しない。
- ARM64 current-EL同期例外はCから戻っても現在は無限ループへ進む。
  sys faultが回復成功を返せる契約にするなら、復帰経路を合わせて直す必要がある。

## 提案：動作判断と診断の分離

共通カーネルの動作判断は正規化したtrap/access/detailで行う。
生vectorは診断の参考情報として取得できても、通常のVM解決やシグナル
選択で分岐しない。OTHERでも生vector依存の判断を再導入しない。

分類候補はPAGE_FAULT、PROTECTION、ILLEGAL_INSN、ARITHMETIC、BREAKPOINT、
ALIGNMENT、MACHINE_CHECK、OTHER。アクセスはREAD/WRITE/EXECと、非該当のNONE。
ゼロ除算とオーバーフロー等の区別にはdetailが必要。PCと対象アドレスも保持する。
これは候補であり、定数名・値・引数型・並びはまだ確定していない。

他OSのINT syscallを将来受ける場合も、HALで入口を判別してABI識別へ
正規化できる。共通カーネルへINT番号そのものを渡すことは必須ではない。
今回の整理に互換ABI実装を追加する提案ではない。

## 実装前に確定する点

1. fault引数の正規化情報とdetailの最小分類。異なる原因を誤って
   MACHINE_CHECKへまとめず、現在のシグナル意味を失わないこと。
2. 生vector/raw errorを診断引数として保持するか、HAL側の診断へ閉じるか。
   診断取得APIを追加する場合も、先に具体的な宣言を提示する。
3. user/sys各入口のIRQ状態、回復成功時の再実行/復帰、失敗時の停止責任。
   userだけがシグナル配送とuser-returnフレーム管理へ入る。
   sysからuserの割り込み許可・スケジューラ処理を無条件に呼ばない。
4. syscallプローブの保存先。syscall実行入口に生vectorを持ち込むためだけに
   公開引数を増やさない。

## 合意後のphaseに含める検証

全HAL呼び出し元、保存フレーム公開/撤去、IRQ入口/出口を更新する。
user page faultの解決・不正アクセスのsignal、sys未処理例外の診断、
syscall restart/sigreturn、OTHER分類をfocused fixtureと適切なQEMUで確認する。
旧登録API/関数ポインタ/疑似x86 vector依存の残存を確認する。
この設計が確定するまでは実装queueに含めない。

## Subsequent execution instruction

User paused I/O performance for expert review and instructed HAL modifications.
This advances the fixed-entry design into ws025-p038; q295 implements syscall
registration removal first. Fault stack/return uncertainties still require
consultation where unresolved. Original discussion text above is historical.

# 移管済み: PowerPC移植

2026-09-12ユーザー指示で新規WS027へ移管。現行計画は [WS027](../ws027/ws.md)。WS003は再利用しない。

<details>
<summary>移管前の計画</summary>

# PowerBook G4 / PPC Open Firmware 起動計画

Date: 2026-09-12
Parent: ws003
Focused Goal: fg009
Primary Milestone: MG003
Related Milestone: MG008
Status: planned (first milestone); later phases planning

PowerBook G4 A1010 / 867MHzを移植先とし、まずQEMU mac99上で、Open Firmware → APM+FATの独自ローダ → zedboot.cfg → 同じFATのvmunix → PPCカーネル初期化を成立させる。後続でamd64上のUSB OHCI、PPCユーザーABI、USB root、rootfs.img/data.imgのループバック利用へ進む。今回は計画のみ。

## 合意した起動構成

- 対象実機はPowerBook G4 A1010 / 867MHz。初期検証は `qemu-system-ppc` / `mac99` / 32bit big-endian G4、単一CPU。QEMUは実機の完全なモデルではない。
- 初期媒体はIDE接続のAPMディスク＋FATパーティション。FATには独自ローダ、`zedboot.cfg`、`vmunix`を置く。初期FATはFAT16を設計案とし、APMのpartition type、容量、パスをp033で検証・固定する。
- ファームウェアのELFロードを必須にしない。独自ローダの配布形式はXCOFFを第一候補とし、作成手段・ロード配置・OF入口ABIをp033で検証する。単なるELFの拡張子変更ではない。ローダ内部でPPC ELF32 big-endianのvmunixを検証・配置する。
- ローダのディスクアクセスはOF Client Interfaceのopen/seek/read/closeを使う。IDE/OHCIのハードウェアドライバをローダに持たせない。OFが独自ローダを最初に読めることはp033の独立した成立条件。
- ファイル名は今回の指示どおり `/zedboot.cfg`。現行UEFIの名前は `/zedbsd.cfg` なので、その改名は本計画に含めない。構文・意味は現行UEFIに揃える。`kernel=vmunix` は選択した同じFATからの相対パス。設定欠落・不正を別ディスクや固定カーネルへのfallbackで隠さない。
- 制限付きparserの重複kernel、パス境界、最大長とboot0生成を引き継ぐ。UEFI固有型やELF64/ExitBootServices処理はPPCへ持ち込まず、再利用単位を実装時に確認する。
- boot disk/partition/FAT UUID、設定由来のパラメータ、OFデバイスツリー、利用可能・予約メモリ、コンソール情報をhandoffへ渡す。OFのメモリを破壊せず読み込みを完了し、最後のOF I/Oとカーネルのデバイス所有権取得を分離する。

初期の最小設定（parserの例。root mountの成功を要求しない）:

```ini
kernel=vmunix
```

## 到達点と範囲

最初の受け入れはp033→p034→p035。QEMUで実際にIDEディスクからファームウェアが独自ローダを読み、設定の指すvmunixがkernel_entryまで進み、メモリ初期化・例外・タイマーの動作が観測できること。`-kernel` による直接ロードだけでは合格にしない。rootやinit未成立はこの段階の失敗条件ではなく、到達した境界をログに明記する。

後続はUSB 1.1のUSB OHCIをamd64で実装・検証し、PPCでまずIDEロード＋USB root、その後USBロード＋USB rootへ進む。USB OHCIとIEEE1394 OHCIは別仕様。FireWire、IBM POWER/pseries、実機のGPU加速・電源管理・SMPは初期範囲外。

最終構成案は同じFATに `rootfs.img` と `data.img` を置き、既存boot0とoverlay-root/overlay-dataのパス解決・loopデバイス・overlay mountを利用する。正確な設定行・image内部形式・読み書き方針はp038で現行実装と照合して確定する。新しい `root=` 等の独自構文は作らない。将来の実機受け入れは別途具体化し、QEMU成功を実機成功へ読み替えない。

## 設計境界・検証

HALは最新のinclude/hal/hal.hを実装し、通常の表示・入力はkern_text/ドライバ側、HALはearly outputとCPU機構を担う。ドライバはkern_*のIRQ・DMA・I/O操作を使う。PPCのbig-endianとUSBのlittle-endian表現、DMAアドレス上限・キャッシュ同期・バリアをamd64の成功だけで済ませない。

共通Guardrail、C規約全文、assembly ABI/配置規約に従う。実装開始は別の有限Queueで行い、現行HAL契約を変更する必要が判明した場合は変更範囲を具体化する。aggregate make check、既存ディスクの書換え、commit/push、他WSの再開を本計画から実行しない。

実装時の証拠はsource/config/firmware/image hash、正確なQEMU argv、ELF/XCOFFセグメント・予約領域検査、起動ログ、対象別build結果（適用時 make -j16）、未実施項目。媒体試験は生成した使い捨てimageで行う。現在はコード・image・build・runtime検証を実施していない。

## Phase一覧

| Phase | 内容 | 状態 | 依存 |
| --- | --- | --- | --- |
| p033 | OF起動契約・APM/FAT imageとXCOFFローダ入口 | planned | なし |
| p034 | zedboot.cfg・FAT読み取り・PPC ELF handoff | planned | p033 |
| p035 | PPC HAL・mac99基板対応とカーネル初期起動 | planned | p034 |
| p036 | amd64でUSB OHCI・USBストレージを検証 | planning | なし |
| p037 | PPCユーザーABI・libcとinit到達 | planning | p035 |
| p038 | PPC USB boot・rootfs.img/data.img統合 | planning | p035, p036, p037 |
| p039 | PPC/OHCI変更の最終規約・統合確認 | planning | p038 |

p033→p034→p035が初期経路。p036はPPC作業と独立。p035→p037、p035+p036+p037→p038→p039。

## 根拠

ユーザーの本タスクでのAPM+FAT、独自ローダ、zedboot.cfg、同一FATのvmunix、後続のrootfs.img/data.imgという指示。先行のHFS+直接ELFロード案は採用しない。既存UEFI仕様: bootloader/uefi/README.md、zedbsd-config.c/.h。USB HCD: include/drivers/usb.h。

## GitHub

- [ws003-p033](https://github.com/awemorris/zedBSD/issues/367)
- [ws003-p034](https://github.com/awemorris/zedBSD/issues/368)
- [ws003-p035](https://github.com/awemorris/zedBSD/issues/369)
- [ws003-p036](https://github.com/awemorris/zedBSD/issues/370)
- [ws003-p037](https://github.com/awemorris/zedBSD/issues/371)
- [ws003-p038](https://github.com/awemorris/zedBSD/issues/372)
- [ws003-p039](https://github.com/awemorris/zedBSD/issues/373)

</details>

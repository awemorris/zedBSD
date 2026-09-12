<!-- awesome-plan project=zedbsd record=ws003-p036 -->

<!-- awesome-plan-current:start -->

Status: planning
Phase disposition: canceled
Lifecycle: closed
Current Focus: No

2026-09-12、[ws027-p004](https://github.com/awemorris/zedBSD/issues/378)（[ws027](https://github.com/awemorris/zedBSD/issues/374)）へ計画を移管。旧Phaseは取消・終了し再利用しない。実行・完了を意味しない。

<!-- awesome-plan-current:end -->

<details>
<summary>終了前の計画・試行履歴（現在の再実行指示ではない）</summary>

# ws003-p036: amd64でUSB OHCI・USBストレージを検証

## 作業

既存drv_usb_hcd_opsへUSB OHCIを追加し、制御/bulk/必要なinterrupt転送、ED/TD/HCCA、完了・timeout・解除時DMA停止を実装する。QEMU pci-ohciとusb-storageを用い、USB 1.1 full-speedの既存BOT層に接続する。PPCを想定したendian変換を明示する。

## 受け入れ

amd64 QEMUでOHCI経由の列挙・ディスク読み書き・mountを確認し、USB上のrootからinit/シェルへ進む。OHCI以外のコントローラ経由で成功していないことを確認する。既存USBホストの適切な回帰を記録。

## 依存

他Phaseの実行成果に依存しない。

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

</details>

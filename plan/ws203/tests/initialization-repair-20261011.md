# RPi4 GENET / VL805 initialization repair / 2026-10-11

Baseline: main6b722f47e、独立worktree codex/rpi4-sshd（先行のnetwork起動read-only記録08e919a08を保持）。User写真のGENET PHY1 id600d84a2、initialization failed21・MMIO release failed3、別写真/xHCIのattach failed at controller start3。zedBSD errno21=ENOTSUP、3=EINVAL。

## Findings and changes

* GENET PHY IDは対応範囲内、PHY初期化後に恒常的にENOTSUPを返すarm64 hal_irq_set_modeへ進む。GICv2 SPIはlevel/edgeを設定可能なので、masked lineのICFGR trigger bitを設定/readbackし、high polarityを受け付ける。低極性は未対応、無効IDはinvalid、有効化済みlineはbusy。SGI/PPIは現在modeと一致する場合だけ成功。既存HAL APIの実装補完でありinclude/hal/hal.hの変更はない。
* USB: VL805 scratchpad pointer arrayは8*count bytes（通常page未満）。kern_pmem_alloc_limitedは要求sizeを記録するが、uncached HALはpage単位のsizeを要求するためEINVAL。drv_dma_alloc_coherentは非coherent deviceに対してbacking lengthをoverflow確認後page単位へroundし、その同じsizeでmap/unmap/free/accountingする。buffer.size/max_segment_sizeのpayload契約は従前のまま。coherent deviceの要求sizeは変えない。
* 従来DMA host fixtureはkern_pmemのrun.sizeまでroundしていたため実機の失敗を隠した。sizeを本番と同じに保持するよう訂正、freeのpage数はallocatorと同じceil計算。旧本番DMAで失敗、新実装で成功することを確認。
* MMIO releaseはSYS+direct addressをuser-only hal_space_unmapへ渡して必ずEINVALになっていた。ARM64の2MiB共有direct device blockはboot code/複数driverも使う永久mappingなので、releaseはdirect rangeを検証して成功し、他callerのmappingを剥がさない。別uncached windowは拒否する。handle以外の新allocationはない。
* GENET attachとxHCI controller startに段階別失敗ログを追加。xHCIの変更範囲ではfallible callと判定を分け、reset/start/cleanupの順序は維持。

Ethernet失敗はDHCPより前のNIC登録。以前の「linkはできているよう、net lan enableが呼ばれていない」という推測は写真で更新した。共通net.conf/rc.conf/service定義とnetworkdのautomatic UP/DHCP経路は[先行確認](network-startup-20261011.md)のとおり、設定変更は不要。

## Verification

Current config SHA256: 56c11753594045c9edce375c3eefa3dd0c3f9bfb50de3e5ea81867d380760c06（main configを複写）。共有LLVMはread-only symlink、toolchain/source/他worktree成果物を変更しない。

Commands in owned worktree:

```sh
make -j16 build/arm64/vmunix
make -f plan/ws048/tests/host-test.mk OUT=build/ws048-dma-repair build/ws048-dma-repair/dma-host-test build/ws048-dma-repair/dma-uncached-host-test
build/ws048-dma-repair/dma-host-test
build/ws048-dma-repair/dma-uncached-host-test
make -f plan/ws203/tests/host-test.mk OUT=build/ws203-repair-host build/ws203-repair-host/genet-host-test
build/ws203-repair-host/genet-host-test /home/awe/zedBSD-claude1/vendor/raspberrypi-firmware/boot/bcm2711-rpi-4-b.dtb
git diff --check
sha256sum -c plan/ws203/tests/initialization-repair-20261011.sha256
```

* Final kernel build exit0、warnings0/errors0、arm64 vmunix checks PASS（entry0xffff000000080000）。build/rpi4-initialization-final-build.log。zedBSD Clang23.1.0 d7f1bbaca898fb5f4cc373b082e915ec1a07310f、armv8-a/mno-outline-atomics/fullLTO/Wall/Wextra/Werror。
* DMA existing host checks: coherent+weak missing mapping38、uncached provided71 PASS。GCC14.2.0、ASan/UBSan/LeakSanitizer。sandboxのptrace制約でLeakSanitizerが初回失敗、承認済みescalated実行でclean PASS。これはsource試験失敗と区別する。
* Before-control: corrected fixture + main6b722f47e src/drivers/generic/dma.c、WS048_UNCACHED、GCC14.2.0/Wall/Wextra/Werror。run.size%PAGE assertion（line264）でexit1。この同じfixtureと新DMAはPASS。本番kern_pmem契約を再現している。
* GENET既存host model PASS（FDT/attach failures/PHY/TX wrap/RX budget/IRQ/close-reopen）、ASan/UBSan/LeakSanitizer clean。実GIC/GPU/PHY電気特性の再現ではない。
* One-off owned build fixtures（今後保守するtestとして登録しない）: production gic.cをincludeしたregister modelでmasked IRQ189のlevel/edge、隣接bit保持、有効line拒否、invalidID/trigger、固定PPIチェックPASS。実IRQ wrapperはsourceで確認。production hal_space_unmap_deviceの関数を抽出した短いfixtureでGENET direct alias release、NULL/empty/uncached window/overflow拒否PASS。他のmappingは変更しないことをsourceで確認。
* Full coding-style14項目を最終変更source/既存test変更に手動適用。public/private API、declaration/forward、評価順、DMA所有/解放、最終payload/size/overflow、IRQ masking/readback、MMIO共有寿命を確認。clang-format19.1.7は全変更Cのpreviewをowned buildへ生成し該当範囲確認、広範囲のlegacy reformatは行わない。style-check.pyの差分行の指摘0、GIC/helper IRQ/MMIO変更関数の独立抜粋も指摘0。既存未変更legacy箇所の指摘はこの差分の適合とは区別。git diff --check clean。

Hardware facts reference: [Arm GICv2 IHI0048B Table4-18](https://documentation-service.arm.com/static/5f8ff21df86e16515cdbfafe)、register encodingのみ参照、code/commentの複製なし。

## Outcome / handoff

ws203-p004、ws048-p010の限定source/build criteriaはcleared。RPi4のDHCP/SSH、USB keyboard/mouse/LANと実機再起動は未実施、WS203/WS048はincompleteのまま。新kernelを含むimageを再buildしてuser確認へ。症状が続く場合はgenet: initialization failed at ... / xhci: controller start failed at ...の行で再調査。QEMU/full image/pushは未実施。共有Master/Queue/history/FutureWorkとGitHub publicationはQ1 pending。main統合は具体的commitの承認待ち。

## Main integration / 2026-10-11

Current user「main仁藤剛してください。」を直前の7b16e364c統合承認への回答（mainに統合してください）として受領。clean main6b722f47eから修正7b16e364c53761f96a982797f88793f7f520dbb6へfast-forward統合、競合なし。先行read-only調査記録08e919a08も含む。main上で全8 source/test SHA256一致、source diffなし、現在config.mkが検証済みworktreeとbyte-identicalであることをread-back確認。前turnのwarning0 kernel buildと限定host checksが統合sourceに適用されるため追加のbuild/試験は行わない。sourceのmain統合は完了、先行の承認待ち表記は当時の履歴。新imageでの実機DHCP/SSH・USB入力受入は未達のまま、WS203/WS048はincomplete。pushなし。共有Master/Queue/history/FutureWork/GitHubはQ1投影保留。

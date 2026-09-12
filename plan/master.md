<!-- awesome-plan-current:start -->

## 現在状態 — 2026-09-12

Active Queue: none
fg006: completed

2026-09-12、ユーザーがWS003 p022・p023・p024の完了を報告し、MarkdownとGitHubの更新を指示した。3 Phaseをcleared（disposition: normal）として受け入れ、fg006を完了とする。WS003はインストーラ等の残件があるためincompleteを維持する。新しいQueueは作成せず、保留中の実行は再開しない。

対象: [p022](https://github.com/awemorris/zedBSD/issues/88)、[p023](https://github.com/awemorris/zedBSD/issues/89)、[p024](https://github.com/awemorris/zedBSD/issues/90)。

根拠は今回のユーザー完了報告。既存Markdownの自動検証・旧試行の結果は履歴として保持する。今回エージェントがbuild・QEMU・実機検証を再実施したものではなく、新しいartifact hashやroot/init/login到達点は報告されていないため追加しない。旧試行のunclearedや未実施項目を過去に遡ってPASSへ変更しない。

<!-- awesome-plan-current:end -->

# zedBSD master plan

[GitHub Project — zedBSD / Awesome Plan](https://github.com/users/awemorris/projects/2)

<!-- traceability:start -->

## Objectives

- **O1**：寛容なライセンスで企業が自由に使いやすいUNIX互換システムを作り、GPLのLinuxカーネルに依存せず、Linux/Androidを置き換え可能な水準の完成度で提供する。
- **O2**：デスクトップ、ラップトップ、SBC、タブレット、モバイルなど様々なスケールで動作するカーネルとユーザランドを提供し、開発者が独自ディストリビューションを自由にカスタマイズ・リブランディング・配布できるようにする。
- **O3**：UNIX/BSD/Linuxの歴史的遺産から現代のシステムに必要なエッセンスを抽出し、networkd、netconf、serviceなどをシンプルで一貫した仕組みとして再実装する。
- **O4**：ページベースMMUを備える32bit/64bitコンピュータへUNIX互換OSを確実に移植できる、明確で最小限のHALを定義し、人類の共有知とする。レトロコンピュータへの移植でも実証する。
- **O5**：AI時代のOSSのあり方を、大規模なAI活用開発を通じて探索・思索し、成果・失敗・人間の判断を再利用可能な知見として共有する。

## Milestone Goals

Objectives → Milestone Goals → WS → Phase → Queue試行/結果を対応付ける。番号は着手順ではない。Milestoneの達成は所属WSの完了数ではなく、到達点の証拠で判定する。

| Milestone | 対応Objective | 到達点・受け入れの核 | Primary WS |
| --- | --- | --- | --- |
| **MG001：継続開発できる基盤が揃う** | O4, O5 | 文書化した環境でビルドでき、設計境界・規約・試験・制限を追跡できる。 | [ws009](https://github.com/awemorris/zedBSD/issues/10), [ws010](https://github.com/awemorris/zedBSD/issues/11), [ws021](https://github.com/awemorris/zedBSD/issues/22), [ws023](https://github.com/awemorris/zedBSD/issues/24), [ws026](https://github.com/awemorris/zedBSD/issues/27) |
| **MG002：UNIXアプリケーションの実行基盤が成立する** | O1 | プロセス・メモリ・libc・ローダ/TLSの対応範囲を、互換性台帳と代表アプリの結果で確認できる。 | [ws001](https://github.com/awemorris/zedBSD/issues/2), [ws022](https://github.com/awemorris/zedBSD/issues/23) |
| **MG003：対象機へ導入して単独起動できる** | O2, O4 | 合意した機種・媒体でインストール後の単独起動・ログインを確認できる。実機とQEMUの証拠を区別する。 | [ws003](https://github.com/awemorris/zedBSD/issues/4), [ws004](https://github.com/awemorris/zedBSD/issues/5), [ws019](https://github.com/awemorris/zedBSD/issues/20), [ws020](https://github.com/awemorris/zedBSD/issues/21) |
| **MG004：データを保持しメモリ/ストレージを実用的に使える** | O1, O2 | 永続化、低メモリ時の進行、媒体世代、既定構成の性能を合意した用途で確認できる。 | [ws016](https://github.com/awemorris/zedBSD/issues/17), [ws024](https://github.com/awemorris/zedBSD/issues/25), [ws025](https://github.com/awemorris/zedBSD/issues/26) |
| **MG005：シンプルで一貫したネットワーク/サービス管理を利用できる** | O1, O2, O3 | networkd/netconf/serviceの責務・設定・操作が一貫し、永続化と失敗後の復旧を確認できる。 | [ws002](https://github.com/awemorris/zedBSD/issues/3), [ws005](https://github.com/awemorris/zedBSD/issues/6), [ws011](https://github.com/awemorris/zedBSD/issues/12), [ws012](https://github.com/awemorris/zedBSD/issues/13) |
| **MG006：グラフィカルな操作環境を利用できる** | O2 | 入力・描画・ウィンドウ・端末・GUIツールの一連の操作を合意した環境で確認できる。 | [ws006](https://github.com/awemorris/zedBSD/issues/7), [ws007](https://github.com/awemorris/zedBSD/issues/8), [ws008](https://github.com/awemorris/zedBSD/issues/9), [ws014](https://github.com/awemorris/zedBSD/issues/15), [ws017](https://github.com/awemorris/zedBSD/issues/18), [ws005](ws005/ws.md) |
| **MG007：用途別の独自ディストリビューションを構成・配布できる** | O1, O2 | 第三者が用途別に構成し独自ブランドでビルド・配布できる。Linux/Android代替の対象用途・機能/品質基準を具体化し実証する。 | [ws013](https://github.com/awemorris/zedBSD/issues/14), [ws015](https://github.com/awemorris/zedBSD/issues/16) |
| **MG008：最小HALの移植契約を公開し異なる機種で実証できる** | O4 | 32bit/64bitのHAL契約・移植手順と異種/レトロ機での実証を公開し、移植者が必要な実装を判断できる。 | [ws018](https://github.com/awemorris/zedBSD/issues/19) |
| **MG009：AI活用OSS開発の経験を検証可能な知見として公開できる** | O5 | 設計権限・レビュー・変更追跡・失敗からの回復について事例と根拠を公開し、知見と未解決の問いを整理する。 | 未割当：成果を担う作業の具体化が必要 |

### 未充足・保留の扱い

- MG007：既存WS013/WS015は一部の用途別機能だけを担う。リブランディング/配布とLinux/Android代替の実証全体は未充足。Future扱いは解除しない。
- MG008：WS018の旧完了は移植契約全体の完成を証明しない。現行HAL契約の公開・異種機での実証範囲を確認する必要がある。
- MG009：WS009/WS026は関連する基盤・資料を提供するが、事例研究と知見公開の担当作業は未定義。既存WSへ無断で追加しない。
- MG002の互換範囲、MG003の機種/媒体、MG004の用途別性能などの詳細基準は既存証拠から確定する。正式なUNIX認証やAndroidアプリ互換方式はこの階層だけでは決定しない。
- 全Milestoneは構造設定段階であり、今回completedとは判定しない。既存WSの完了・停止・取消し・保留を維持する。

## Current Focused Goals

| Goal | 当面の成果 | Milestone | 担当 |
| --- | --- | --- | --- |
| fg004 | PC98、Dell Latitude 5320、Let's Note SV7、Let's Note LX6でインストールが行える | MG003 | [ws003](ws003/ws.md) |
| fg005 | 有線LAN常駐管理、起動時の接続待機、DEへのネットワーク状態通知 | MG005 / MG006 | [ws005](ws005/ws.md) |
| fg007 | HAL契約の可読性改善：コンソールAPIの集約、アロケータの kernel_alloc/kernel_free 化、kernel_entry() 前関数の prekern 命名 | MG008 / MG001 | 未定（WS018は完了。再開か新WSかの判断が必要） |

2026-09-11ユーザー指定。4機種は順位ではない。旧fg001〜fg003は履歴のまま。旧Priorityリストを再作成せず、順序付けは未指定として保持する。

#### fg007：HALリファクタリングの確定事項

2026-09-11ユーザー指示。正本は `include/hal/hal.h` のXXX注釈。以下は本文で確定した決定であり、
実行許可ではない。Queue投入前に下の未決事項を確定する。

- コンソール出力を `hal_cons_write()` に集約し、row・column・attrib を引数へ追加する。
  `hal_cons_write_at()`、`hal_cons_write_at_attr()`、`hal_cons_write_n()`、`hal_cons_clear_row()`、
  `hal_cons_clear_to_eol()`、`hal_cons_clear_to_eol_at()`、`hal_cons_save_state()` を削除する。
  `hal_cons_write_n_at()`、`hal_cons_restore_terminal()`、`struct hal_cons_state` は削除済み。
- `hal_cons_get_size(unsigned *cols, unsigned *rows)` を追加し、`HAL_CONS_COLUMNS` と
  `HAL_CONS_ROWS` を廃止する。`HAL_CONS_NORMAL_ATTRIBUTE` を `HAL_CONS_ATTRIB_NORMAL` へ改名する。
- `hal_cons_set_event_mode(int enable)` を追加する。`enum hal_cons_mode` と `hal_cons_set_mode()`
  は削除済みで、イベントモードの選択はこの関数が担う。
- `struct hal_key_event` を廃止し、`hal_cons_read_event()` と `hal_cons_poll_event()` の引数を
  `keysym` と `flags` へ展開する。入力能力の申告は廃止し、スタブでも常時サポートとする。
- `hal_set_allocator()`、`hal_malloc()`、`hal_free()` を廃止し、HALがカーネルの `kernel_alloc()` と
  `kernel_free()` を直接呼ぶ。
- `kernel_entry()` より前に呼ばれる関数は `prekern_` を接頭辞とする。それ以外は `kernel_entry()`
  以降から呼ばれるものとして扱う。例：`bsp_boot_init` → `prekern_bsp_boot_init`、
  `pcat_cons_init` → `prekern_pcat_cons_init`。これによりアロケータ使用可否が名前で判別できる。

未決の判断：

- `amd64_cpu_init()` と `amd64_descriptor_init()` は `amd64_cmain()` と `amd64_ap_entry()` の
  両方から呼ばれる。AP側は `kernel_entry()` 以降のため `prekern_` を付けられない。
  分割・別名・規約の例外のいずれを取るか未定。
- `prekern_` を推移的に適用するか、cmain から直接呼ばれる非static関数に限るか未定。
- `hal_*` と `kernel_*` の公開APIは対象外とする想定。`hal_puts()` は前後どちらからも呼ばれる。
- 担当WS。WS018（MG008）は完了のため、再開するか新WSを立てるかはユーザー判断。

#### fg007：hal.h 再編への追従（2026-09-11、amd64、未コミット）

ユーザーによる hal.h 再編に amd64 を合わせた。pmem 側は完了し、残りはコンソールのみ。

完了した内容：

- pmem を `hal_pmem_alloc(size, align, *paddr)` と `hal_pmem_free(*paddr, size)` へ移行した。
  VRAM と MMIO の確保機能は pmem から外し、`hal_space_map_device()` で写す設計にした。
- hal.h へ追加した入口は3つ。`hal_pmem_to_kernel()` は RAM の直接マップ変換、
  `hal_pmem_alloc_limited()` はデバイス到達上限と境界制約付きの確保、
  `hal_space_map_device()` と `hal_space_unmap_device()` はデバイス範囲の写像。
- `struct kern_pmem`（paddr と size）を `include/kern/pmem.h` に新設した。
  `hal_pmem_free()` がサイズを要求するため、run を保持するカーネル側はこれを使う。
- amd64 の固定デバイス窓の表を pmem 割り当て器から `space.c` へ移した。
  Local APIC、I/O APIC、PCI BAR はすべて `hal_space_map_device()` 経由になった。
- `src/hal/pmem-constraints.c` を削除した。制約付き確保は `hal_pmem_alloc_limited()` に一本化。
- `enum hal_trap_cause` と `hal_trap_mode` への改名に追従した。hal.h 内に旧名の書き残しが
  1085行と1102行の2箇所あった。
- `hal_irq_get_affinity()` を `struct hal_cpu_mask` 2本の新署名へ合わせた。
- `hal_irq_set_handler()` を `hal_irq_register()` と `hal_irq_unregister()` へ分割した。
- `hal_pmem_stats` は `struct hal_memstat` と `hal_get_memstat()` へ改名済みだったので追従した。
- `HAL_KEY_*` をカーネル側 `KERN_KEY_*` と HAL 側 `src/hal/cons-keys.h` に分離した。

残る32件はすべてコンソール関連である。hal.h のコンソール宣言が `hal_cons_putc()` だけに
なったため、`/dev/console` が12件、`tty.c` が9件、pcat graphics が11件ビルドできない。
これは early console 化そのものなので、下の計画に従って別途進める。

#### fg008：ドライバとHALの分離（2026-09-12、着手済み、未コミット）

方針は、ドライバがHALを直接触らないこと。HAL API相当の操作はカーネル側に用意し、
単なるラッパーであってもカーネルに置く。カーネルがコア機能として持つAPIは `kern_*()`
で命名する。インライン化は要求しない。LTOは将来の検討とし、今は前提にしない。

置換済み（468箇所、27ファイル、ビルドと起動を確認）：

- `hal_printf()` → `kern_logf()`（ドライバ282、カーネル40）。
- `kernel_alloc()`/`kernel_free()` → `kern_malloc()`/`kern_free()`（186）。
  `kernel_alloc`/`kernel_free` はHALが呼ぶ入口として残る。

この置換で診断の経路が一本化された。`kern_log_write()` は従来リングバッファと
プラットフォームのデバッグポートにしか出しておらず、amd64 では画面に出なかったため、
公開済みのコンソールへもミラーするようにした。あわせて `hal_printf()` と `kern_logf()`
を並べて呼ぶ二重出力（`VFS_LOG` マクロと main.c の4箇所）を1本に畳んだ。

## 足場の実装と置換（2026-09-12、amd64、未コミット）

足場を用意し、ドライバの置換を行った。ビルドは PASS、ログインとシェル実行を確認済み。

用意した足場：

- `include/kern/device-io.h` と `src/kern/device-io.c`。ポートI/O 6本、MMIO 8本、
  順序バリア4本。`kern_io_in8()`、`kern_mmio_read32()`、`kern_io_barrier()` など。
- `include/kern/irq.h` と `src/kern/irq.c`。登録・解除、MSI、マスク、EOI、
  割り込み禁止と復帰。`kern_irq_ack_t` を不透明な型として定義し、HAL の
  `hal_irq_ack_t` がドライバに出ないようにした。HAL の状態値は errno へ写す。
- `src/kern/pmem.c`。`kern_pmem_alloc()`、`kern_pmem_alloc_limited()`、
  `kern_pmem_free()` は `struct kern_pmem` を受け取るので、呼び出し側が
  物理アドレスとサイズを別々に持ち回らなくてよい。`kern_device_map()` と
  `kern_device_unmap()`、`kern_page_size()`、`kern_memstat()`、
  `kern_boot_handoff()`、`kern_rtc_read_counter()` も置いた。
- `include/kern/thread.h` に `kern_thread_block()` と
  `kern_thread_wakeup(struct thread *)`。ドライバは既に `struct thread *` を
  保持していたので、構造体の変更は不要だった。
- `include/kern/atomic.h` に不足していた `atomic_raw_store_relaxed()`、
  `atomic_spin_hint()`、`atomic_acquire_fence()` を足した。

置換した件数（src/drivers のみ）：

- 一括置換 264件、22ファイル。I/O、MMIO、バリア、割り込み、アトミック、スレッド待機。
- 個別書き換え 30件。デバイス写像、割り込み登録、コンソールの停止と再開。
- 状態定数と残りの呼び出し 48件。`HAL_OK` → 0、`HAL_ERR_*` → errno、
  ブート handoff、RTC、MSI、統計。
- 最終 12件。物理メモリの確保と解放、acquire フェンス、VGA アパーチャの属性。

合計 354件。前段の `hal_printf`/`kernel_alloc` 置換 468件と合わせて 822件。

amd64 ビルド対象のドライバに残る HAL 参照：

| ファイル | 残り |
| --- | --- |
| `generic/system-device.c` | `struct hal_memstat` の各フィールド、`HAL_SPACE_EXEC` |
| `pci/pci-pcat.c` | `HAL_SPACE_*` 属性 4種 |
| `pci/pci.c` | `hal_atomic_uint_try_acquire`、`enum hal_error` |
| `fs/ufs.c`、`generic/dma.c`、`pci/pci-xhci.c` | `HAL_FATAL` |

いずれも受け皿の型か定数をカーネル側に定義する必要がある。統計構造体は内容が
プラットフォーム固有なので、カーネル側の型に写す設計が要る。`HAL_FATAL` は
カーネルの致命エラー表明へ、`HAL_SPACE_*` はページ保護属性としてカーネルに定義する。

pc98 配下のドライバは未対応。この port はコンソール移行も済んでいない。

`hal/hal.h` を直接 include するドライバはまだ38ファイルある。上の残りを片付けた
時点で include を外し、以後は外したままビルドが通ることを完了条件にする。

## カーネル側に用意すべき足場

ドライバに残るHAL直接呼び出しは295箇所。5群に分かれる。

1. アトミックとバリア（約110）
   `hal_atomic_load_acquire` 45、`hal_atomic_store_release` 17、
   `hal_atomic_compare_exchange_acq_rel` 8、`hal_atomic_relax` 4、その他。
   `hal_io_mb` 33、`hal_io_wmb` 31、`hal_io_rmb` 19、`hal_compiler_barrier` 6。
   受け皿は既存の `include/kern/atomic.h`。`kern_atomic_*` と `kern_barrier_*` を足す。

2. デバイスI/O（約50）
   `hal_io_outp8` 17、`hal_io_inp8` 8、`hal_io_inp16` 2、`hal_io_outp16` 1、
   `hal_mmio_read*`/`hal_mmio_write*` 各3。
   新規に `include/kern/device-io.h` を置き、`kern_io_in8()`、`kern_io_out8()`、
   `kern_mmio_read32()` などを定義する。

3. 割り込み（約50）
   `hal_irq_mask` 9、`hal_irq_send_eoi` 7、`hal_irq_unregister` 6、`hal_irq_unmask` 6、
   `hal_irq_enable` 6、`hal_irq_disable` 5、`hal_irq_unregister_msi` 5、
   `hal_irq_register` 4、`hal_irq_register_msi` 2、`hal_irq_set_handler` 2、
   `hal_irq_service_wait` 1。
   新規に `include/kern/irq.h`。`hal_irq_ack_t` がドライバに15箇所露出しているので、
   カーネル側の不透明な型に置き換える。`hal_irq_set_handler` の2箇所は
   `hal_irq_register` への移行漏れ。

4. 物理メモリとアドレス空間（約25）
   `hal_pmem_free` 9、`hal_pmem_to_kernel` 4、`hal_pmem_alloc` 3、
   `hal_pmem_alloc_limited` 1、`hal_space_map_device` 3、`hal_space_unmap_device` 2、
   `hal_space_get_page_size` 3、`hal_get_memstat` 1、`hal_get_arch_handoff` 2。
   `include/kern/pmem.h` が既に `struct kern_pmem` を持つので、そこへ
   `kern_pmem_alloc()`、`kern_pmem_free()`、`kern_pmem_to_kernel()`、
   `kern_device_map()` を足す。`hal_physaddr_t` の露出3箇所も置き換える。

5. スレッドの待機と起床（29）
   `kernel_notify_task` 24、`kernel_wait_task` 5。
   これはタスクではなくスレッドに対する操作なので、カーネルAPIとして
   `kern_thread_block()` と `kern_thread_wakeup(struct thread *)` を用意する。
   ドライバが `hal_task_t` を持ち回らなくて済むよう、ワーカー構造体が
   `struct thread *` を保持する形に変える。USB と PCI のワーカーが対象。

6. その他
   `hal_cons_resume`/`hal_cons_suspend` 6箇所は `pcat-graphics.c` に残る古い呼び出しで、
   `drv_pcat_text_resume()`/`_suspend()` への置換漏れ。`hal_rtc_read_counter` 2箇所は
   時刻APIをカーネル側に用意する。`HAL_OK` 31、`HAL_FATAL` 9、`HAL_ERR_*` 6 の
   定数露出も、カーネル側の返り値規約へ寄せる。

完了の判定は、`src/drivers` から `hal/hal.h` の include を外してもビルドが通ること。
現在38ファイルが直接 include している。

## zedbsd 接頭辞の段階的廃止

`ZEDBSD_` はヘッダガード564件、それ以外1483件。`zedbsd_` は69件。
上位は `ZEDBSD_PATH_MAX` 93、`ZEDBSD_PAGE_SIZE` 86、`ZEDBSD_FAT32` 32、
`zedbsd_peercred` 26、`ZEDBSD_TEST_CHECKPOINTS` 24。

カーネルのコア機能に属するものから `kern_*`/`KERN_*` へ移す。
ヘッダガードは機械的に置換できるので最後でよい。UAPI に露出しているもの
（`ZEDBSD_TTY_IOC_GROUP` など）はユーザーランドとの互換に影響するため、
移行時期を別に決める。

## 接頭辞移行と NoctLang の追従（2026-09-12、amd64、未コミット）

C シンボルの接頭辞を 3252 件置換した。`ZEDBSD_` → `KERN_`、`zedbsd_` → `kern_`。
UAPI も含む。ビルドと起動、ログイン、シェル実行を確認済み。

残すのはビルド専用の makefile 変数（`ZEDBSD_CONFIG`、`ZEDBSD_PLATFORM` など）、
`include/uapi/zedbsd/` のディレクトリ名、noct 統合のファイル名
（`.zedbsd-source-manifest`、`zedbsd.cmake`）である。いずれもパスとディレクトリ構造に
波及するので、別の移行として扱う。

### NoctLang 本家への還元と main 追従

ローカルパッチのうち、ターミナルの部分入力と BeUI の 2 件を本家へ送った。
クロスビルドの配線は本家に入れるべきではないのでパッチのまま残した。
本家の zedBSD ターゲットは、zedBSD 上でセルフコンパイルするときのターゲットとする。

取得先を main の先頭 `fcf5759e` に移した。パッチレベルは `zedbsd7`。
パッチは 0001（ターゲットアダプタの接続）と 0002（プロジェクトの LLVM と sysroot）の
2 件だけになり、旧 0003・0004・0005 は削除した。

ビルド toolchain の noct とユーザランドの noct の両方を入れ替えた。
toolchain smoke は PASS、world ビルドはエラーなし、QEMU でログイン後に
ユーザランドの noct が Noct 2.0.1 と応答することを確認した。

注意点として、パッチの削除行は上流の本文と一致させる必要があるため、
接頭辞移行の対象外である。追加行だけが新しい名前を使う。

## PC-98 実機で swap0 prepare が失敗する（2026-09-12、未コミット、実機未検証）

GDC と PBR の修正後、実機は画面が正常に読めるところまで進み、
`vfs: swap0 prepare failed (error 21)` で VFS 初期化が止まる。直前に
`ide: sda flush LBA=2048 count=0 ... stage=request status=50 error=00` が
出ている。status 50 はドライブが正常に待機している状態で、ドライブ側の
エラーではない。

### 原因

21 は EOPNOTSUPP。pc98-ide の flush は、IDENTIFY のワード 83 で FLUSH CACHE
の対応を申告しないドライブに対して、ワード 87 のキャッシュ報告が有効かつ
無効化されている場合だけ 0 を返し、それ以外は EOPNOTSUPP を返していた。
ATA-4 より前のドライブはワード 83 も 87 も有効でないので、実機の
ドライブは必ず EOPNOTSUPP になる。

スワップファイルの準備は、エクステントを確定するために起動 FAT を同期
する。その同期の末尾の bio_flush がこの EOPNOTSUPP を返し、swap の準備、
ひいては VFS 初期化全体が失敗する。

QEMU の IDE モデルは FLUSH CACHE 対応を申告するので再現しない。

### 修正

FLUSH CACHE を持たないドライブには、ホストがコミットを頼む手段がない。
書き込みはドライブが完了を報告した時点で終わっている。bio_flush は
先行する書き込みの完了をすでに待っているので、そこで flush は完了
したものとして 0 を返す。PC/AT の IDE ドライバと SD ドライバも同じ
扱いである。使わなくなったワード 85/87 の読み取りは削除した。

実機のログで説明がつくよう、プローブ時に
`ide: sda has no FLUSH CACHE; flushes complete with the writes` を 1 回出す。

### 状態

QEMU の内蔵 ROM で起動から init まで回帰なし（QEMU のドライブは
FLUSH CACHE 対応なので、この経路自体は通らない）。実機での確認は未実施。

## PC-98 実機の画面の乱れ（2026-09-12、未コミット、実機未検証）

実機で VFS のメッセージは読めるが画面が乱れる、という報告。同じ 2 行が画面の
複数箇所に少しずつずれて繰り返され、字が二重にじむ。

### 原因

書き直した PC-98 コンソールが uPD7220 のカーソル形状コマンドに誤った値を
送っていた。第 1 パラメータの下位 5 ビットは 1 文字行あたりの走査線数から
1 を引いた数で、これは**カーソルだけでなく表示そのもの**が使う。16 ライン
フォントなので 15 でなければならないところ、1 を送っていた。コントローラは
2 走査線ごとに表示アドレスを進めるので、同じ内容が画面を何度も繰り返し、
そのたびに開始位置がずれる。写真の症状と一致する。

カーソルアドレスコマンドのパラメータも 2 バイトのところを 3 バイト送って
おり、余った 1 バイトがコマンド FIFO に残って後続のコマンドをずらす。

旧コンソールは表示時 0x8f、非表示時 0x0f、続けて 0x20 と 0x7b を送り、
アドレスは 2 バイトだった。その値に戻した。

QEMU の GDC モデルはこの欄を表示アドレスの計算に使っていないため、
エミュレータでは再現しない。

### 訂正：実機 ROM は読まれていなかった

`-L ~/roms-real` あるいは小文字リンクを張った `/tmp/roms98` を渡しても、
QEMU は分割形式のファイル名（`pc98bios.bin`、`pc98itf.bin`、`pc98ide.bin`）を
先に探して内蔵 `pc-bios` から読む。用意したのはバンク形式
（`pc98bank0..7.bin`）だけだったので、実機 ROM は一度も使われていない。

ゲストの 0xE0000 を吸い出して確認した。内蔵 `pc98bios.bin` の先頭 32K と
md5 が一致し、実機 `pc98bank5.bin` とは一致しない。

したがって「実機 ROM で再現した」「実機 ROM で直った」という以前の記録は
すべて内蔵 ROM での結果であり、誤りである。PBR の 64 KiB 境界の修正も、
実機 ROM 下では検証できていない。修正の根拠は ROM の逆アセンブルから
得た転送先検査そのものなので内容は変わらないが、検証状況の記述を訂正する。

なお実機 ROM を読ませると QEMU が POST から進まないという報告があり、
そちらは QEMU 側の問題として切り分けが必要。

### 状態

QEMU の内蔵 ROM では回帰なし。実機での確認は未実施。

## ls の欠落と /usr/bin の ENOSPC（2026-09-12、未コミット）

`ls` が実在するファイルより少ない件数しか出さない、`ls /usr/bin` が
NOSPACE になる、という報告を amd64 で再現して直した。

### 原因

overlay は各パスに合成 inode 番号を割り当てるため `identities` 表を持つ。
`st_ino` を安定させるための表である。この枠が返却されるのはファイルが削除
された場合だけで、単に inode がキャッシュから追い出されたときは保持された
ままだった。つまりこの表は、システムが一度でも触ったパスの恒久的な台帳で、
上限は 128 だった。`/bin` だけで 140 件ある。

溢れたあとの挙動が二通りに分かれる。

`overlay_dir_upper_has()` は検索の失敗を「上位層がこの名前を持つ」と解釈して
エントリを隠す。そのため一覧から実在するファイルが黙って消える。報告にあった
「zedinst は起動できるのに表示されない」はこれである。

表に出てくる方は `stat`、`lstat`、`getdents` が ENOSPC を返す形で、シェルは
それを「コマンドが無い」と報告する。`ls /usr/bin` の NOSPACE も同じ。

### 切り分け

syscall のディスパッチで `-ENOSPC` を一時的にログすると、失敗しているのが
`stat`(54)、`lstat`(55)、`getdents`(8) だと分かり、パス解決の層に絞れた。
最初は inode プールの枯渇を疑って `inode_alloc` にログを入れたが一度も
失敗しておらず、その仮説は捨てた。

### 修正

identity は、カーネルがその inode を覚えている間だけ固定されていればよい。
inode が回収されたあとは誰も番号を観測できないので、そこで枠を返す。
これで表の占有は生きている overlay inode の数で頭打ちになる。あわせて
`OVERLAY_IDENTITY_MAX` を `OVERLAY_INODE_MAX` と同じにしたので、ドライバが
確保できる数を超えて要求されることがなくなり、溢れ自体が起こらない。

枠を返す前に、他の生きている inode が同じ番号を使っていないことを確認する。
キャッシュはパスごとに inode を 1 つしか持たないので通常は 1 対 1 だが、
確認は安く、こうすれば回収が他の inode の番号を奪うことがない。

### 確認

| 対象 | 修正前 | 修正後 |
| --- | --- | --- |
| `ls /bin` の件数 | 途中で欠落 | 140、ホストと一致 |
| `ls /sbin` の件数 | 途中で欠落 | 27、ホストと一致 |
| `ls /usr/bin` | No space left on device | holoris.nct |
| `ls /usr/share` | No space left on device | 4 件 |
| `/bin` 列挙を 3 回繰り返す | 以後コマンドが見つからない | 影響なし |

PC/AT と PC-98 もビルドして起動を確認した。

## PC-98 実機で起動しない問題（2026-09-12、未コミット）

実機でローダのあとビープが鳴って止まる、という報告を再現し、原因を特定して
直した。私のリファクタとは無関係の、以前からある不具合である。

### 再現方法

`~/roms-real` の実機 ROM を QEMU の `-L` に渡すと再現する。ただし QEMU が
探すのは `pc98bank0.bin` 以下の小文字名で、置かれているのは大文字名なので、
そのまま `-L ~/roms-real` としても読まれず再現しない。小文字のシンボリック
リンクを張った `/tmp/roms98` を使う。最初これに気付かず、再現しないと
誤って報告した。

### 症状の正体

ビープは BIOS のものではなく、[partition-pbr.S](bootloader/pc98/partition-pbr.S)
の `boot_failure` が鳴らしている。デバッグポートに `'P'` を出し、ポート 0x37 に
0x06 を書いてビープを開始し、無限に hlt する。

停止位置は CS=0x1fc0 の hlt で、実行トレースを見ると直前に IDE BIOS へ入って
戻り、`jc boot_failure` で落ちていた。つまり INT 1Bh の読み取りがキャリーを
返している。

### 原因

実機 ROM の IDE BIOS を逆アセンブルすると、読み取りの入口に転送先の検査が
ある。転送先リニアアドレスの下位 16 ビットに転送長を足し、桁上がりが出たら
`AH=0x20` で拒否する。64 KiB ページを跨ぐ転送を許さないという規則である。

ローダ本体は MZ ヘッダが物理 0x10000 でちょうど終わるように
`LOAD_SEG 0x0ffc` へ置かれる。したがって最初の 1 セクタだけが
0x0FFC0..0x101BF となり、0x10000 を跨いで必ず拒否される。2 セクタ目以降は
0x101C0 から始まり、この記録が受け付ける上限 127 セクタまで、どれもページ内に
収まる。FAT とルートディレクトリの読み取りは `TABLE_SEG 0x0a00` 宛てなので
跨がず、失敗までに BIOS 呼び出しは 1401 回成功していた。

QEMU 内蔵の IDE BIOS はこの検査をしないので、実機 ROM を読ませない限り
表面化しない。

### 修正

最初の 1 セクタだけ `TABLE_SEG` に読んでから `LOAD_SEG:0` へ 256 ワード
転写し、2 セクタ目以降は従来どおり直接読む。跨ぐ読み取りが 1 回しかない
ことは上のとおり上限まで成立するので、これで足りる。

### 確認

| ROM | 結果 |
| --- | --- |
| 実機 ROM | ログイン、シェル、uname が `pc98 i386` |
| QEMU 内蔵 ROM | 同上、回帰なし |

## arm64 の移植と AArch64 ツールチェーン（2026-09-12、未コミット）

arm64/Raspberry Pi 4 を現行の HAL 契約に合わせ、カーネルがビルドできる状態に
戻した。この port はしばらくビルドされておらず、makefile が削除済みのソースを
参照したままだった。

### ツールチェーン

プロジェクトの LLVM は `LLVM_TARGETS_TO_BUILD=X86` で構成されていたため、
clang は AArch64 を出力できなかった。一方 lld は全バックエンドを持つので
リンクだけはできていた。構成を `AArch64;X86` に変え、再構成の判定に使う
identity 文字列も合わせた。これで arm64 もシステムのクロス GCC に依存せず、
プロジェクトのツールチェーンでビルドできる。

`platform/arm64/vmunix.mk` のコンパイラ指定を `aarch64-linux-gnu-gcc` から
プロジェクトの clang へ変えた。

### GCC 前提だった箇所

ツールチェーンを変えたことで、GCC でしか通らなかった箇所が 4 つ表面化した。

- `libc/include/stdint.h` が `__UINT32_C()` 系の関数形マクロを使っていた。
  これは GCC 拡張で、clang は x86 では提供するが AArch64 では提供しない。
  どの対象も定義する `__UINT32_C_SUFFIX__` から組み立てる形に直した。
  狭い型では接尾辞が空なので、貼り付けは 2 段の展開を経る。
- `src/hal/arm64/dispatch.S` の FP 退避が `-mgeneral-regs-only` に阻まれた。
  GCC のアセンブラはこのフラグを無視するが、clang の内蔵アセンブラは従う。
  `.arch armv8-a+fp+simd` を宣言した。C 側は従来どおり FP を出さない。
- `__builtin_setjmp` と `__builtin_longjmp` が AArch64 の clang に無い。
  `libc/setjmp-aarch64.S` を新設し、x19-x28、フレームポインタ、リンク
  レジスタ、スタックポインタ、v8-v15 の下半分を保存する実装を書いた。
  `jmp_buf` の語数は対象ごとに決まる。他アーキは従来の builtin のまま。
- リンク後の未定義シンボル検査が weak 未定義で落ちた。GNU nm は `-u` から
  weak を除くが llvm-nm は含める。強い未定義だけを選ぶようにした。

### ビルドされていなかったために残っていたバグ

`src/drivers/platform/rpi4/rpi4-sdhci.c` の for 文の途中に閉じ括弧が紛れ込み、
else ブロックがループの前で閉じていた。コンパイルされていなかったので
誰も気付いていない。括弧を戻し、`%llu` に渡す引数を明示的に広げた。

### HAL の移植

x86 と同じ内容である。

- `page.c`：物理アドレスとサイズの契約へ。`hal_pmem_alloc_limited()` と
  `hal_pmem_to_kernel()` を追加し、`hal_pmem_get_stats()` を
  `hal_get_memstat()` に改名した。
- `space.c`：ページテーブルのページを物理アドレスで持つ。
  `hal_space_map_device()` と `hal_space_unmap_device()` を追加。
  arm64 は物理空間全体をカーネル半分に直接写像するので別名の取得で足りる。
- `task.c`：`hal_task_create_for_init_context()` へ改名し、確保失敗を
  NULL で返す。アクセサ 2 本を新しい契約に合わせた。
- `irq.c`：`hal_irq_register()` と `hal_irq_unregister()` を追加し、
  アフィニティ取得を 2 ポインタの契約へ。
- `bsp-rpi4/cons.c`：112 行の全機能コンソールから、UART とフレームバッファ
  の両方へ書く出力専用の early console へ。入力は持たない。
  `prekern_bsp_cons_init()` と `prekern_bsp_cons_irq_init()` に改名。

`config/ci/config-rpi4.mk` を追加した。これまで arm64 には CI config が
無かった。

### 状態

| プラットフォーム | ビルド | 起動 |
| --- | --- | --- |
| amd64 | PASS | ログイン、シェル |
| PC/AT i386 | PASS | ログイン、シェル |
| PC-98 | PASS | ログイン、シェル |
| arm64 rpi4 | vmunix PASS | 未確認 |

arm64 は `/dev/graphics` の表示層と入力ドライバが未実装なので、
`kern_text_register()` を呼ぶ主体がまだ無い。UART はバイト列なので、
キーボードに相当する evdev ドライバをそこに被せる形になる。

## PC-98 の起動（2026-09-12、未コミット）

PC-98 がログインシェルまで到達した。`uname -a` が `pc98 i386` を返す。
amd64 と PC/AT に回帰はない。

### 出発点

カーネルはもともと `boot: starting init /sbin/init` まで到達していた。
デバッグポート 0xe9 の出力でそれが確認できた。画面に何も出ないのは
`kern_text_register()` を呼ぶ主体が無かったからで、ハングではなかった。

### 表示層

`drivers/platform/pc98/graphics/text.c` を新設した。PC/AT と違い、文字は
`0xa0000` の 16bit 語、属性は `0xa2000` のバイトという 2 面構成で、
JIS X 0208 のグリフは 2 セルを占める。右半分は同じコードの bit15 を立てた
もので、ハードウェアはそれで右半分と判断する。カーソルは uPD7220 の
CSRFORM と CSRW で動かす。UTF-8 から JIS への変換は旧 HAL コンソールから
引き継いだ。

セル配列が唯一の真実なので、グラフィックモードが画面を取って返しても
内容が失われない。

### 属性バイトの形式が違った

最初は画面が真っ黒になった。カーネルが渡す `KERN_TEXT_ATTRIB_NORMAL` は
0x07 で、これは VGA の「下位ニブルが前景色」という形式である。PC-98 は
上位 3bit が緑・赤・青のガンで、bit0 を立てないとセルが表示されない。
0x07 をそのまま書くと色が黒、つまり黒地に黒になる。

変換を入れた。前景の色ビットを 3 つのガンへ写し、黒前景は読めるように
白へ倒し、非黒の背景は反転ビットにし、最後に表示ビットを立てる。

### キーボード

`drivers/platform/pc98/pc98-keyboard.c` を新設した。8251 の USART が
ポート 0x41 と 0x43 にあり、IRQ1 を使う。スキャンコードは 1 バイトで、
bit7 が離鍵を表すため、割り込みをまたいで持ち越すプレフィックス状態が無い。
PC/AT の 8042 と違ってマウスとは別の IC なので、このファイルは evdev
デバイスを 1 つだけ登録する。

プラットフォームの入力初期化では、キーボードとマウスを独立に起動し、
片方の失敗がもう片方を巻き込まないようにした。マウスの無い機械でも
キーボードは要る。

### 削除

`src/hal/i386/bsp-pc98/keyboard-map.h` を削除した。HAL がキーボードを
持たなくなったので参照が無くなった。同名のファイルが m68k にもあるが、
そちらは別物で残る。

### 状態

| プラットフォーム | ビルド | 起動 |
| --- | --- | --- |
| amd64 | PASS | ログイン、シェル、uname |
| PC/AT i386 | PASS | ログイン、シェル、uname |
| PC-98 | PASS | ログイン、シェル、uname |

## /dev/console の機種非依存化と cons-wait.h の削除（2026-09-12、未コミット）

`cons-wait.h` を削除した。ビルド対象での最後の利用者は PC-98 の HAL コンソール
だったので、その縮退と同時に外した。arm64 と m68k は今も include しているが、
どちらも CI config が無くビルド対象ではない。

### カーネル側にテキスト表示の受け口を置いた

`/dev/console` は `drivers/platform/pcat/graphics/text.h` を直接 include して
いた。機種非依存であるべきドライバが 1 つの基板に依存していたことになる。
`src/kern/tty.c` も同じ依存を持っていて、pcat 以外ではリンクできなかった。

`include/kern/text-display.h` に `struct kern_text_ops` を定義し、
各基板の表示ドライバが自分の表を publish する形にした。`/dev/console` と
`tty.c` は `kern_text_*()` だけを呼ぶ。表が未登録の間は全て no-op で、
表示を持たない基板ではそれが正しい振る舞いになる。

| 綴り | 役割 |
| --- | --- |
| `kern_text_register()` | 表示ドライバが起動時に 1 度 publish |
| `kern_text_ready()` | 表が登録済みかを報告 |
| `kern_text_putc()` ほか | `/dev/console` と `tty.c` が呼ぶ入口 |

表は 1 文字ごとに読まれるので、ロックではなく atomic な store と load で扱う。

### PC-98

HAL コンソールを 2090 行から 330 行余りの出力専用へ縮退した。テキスト VRAM は
文字が `0xa0000` の 16bit 語、属性が `0xa2000` のバイトという 2 面構成で、
カーソルは uPD7220 の CSRW コマンドで動かす。早期コンソールは ASCII のみで、
日本語の経路は表示ドライバ側の仕事とした。

`prekern_bsp_cons_init()` と `prekern_bsp_cons_irq_init()` に改名し、後者は
IRQ1 をマスクするだけにした。キーボードは 8251、マウスはバスマウスで
別 IC・別割り込みなので、ドライバも別になる。

表示ドライバの GDC 4 面と Cirrus アパーチャは、物理アロケータの固定クレームを
やめて `kern_device_map()` に移した。これらは RAM ではなくデバイスメモリである。

### バスマウスをハンドラ登録型にした

`hal_irq_service_wait()` の最後の利用者だった。カーネルスレッドが HAL の中で
線が鳴るのを待つ方式で、EOI から次の wait までにエッジトリガの割り込みを
取りこぼし、コンテキストスイッチをまたいで割り込みコントローラの in-service
状態を保持してしまう。登録型ハンドラにはどちらの問題も無い。

ハンドラはライフサイクルの mutex を取れないので、サンプルを読んで EOI を
出してから publish する。start と stop は mutex の下で線をマスクしてから
登録を変える。

### 並行編集で入っていたバグ

`src/hal/x86/rtc.c` に私が触っていない整形が入っており、`& CMOS_UIP != 0` と
括弧が落ちて条件が反転していた。HEAD 版は `(x & CMOS_UIP) == 0` で正しい。
括弧を戻した。src/hal/i386 配下にも同様の整形が多数入っている。

### 状態

amd64、PC/AT、PC-98 の 3 つとも world がビルドできる。amd64 と PC/AT は
QEMU でログインとシェル実行を確認した。PC-98 はまだ起動確認をしていない。
表示ドライバ側のテキスト層と 8251 キーボードドライバが未実装で、
`kern_text_register()` を呼ぶ主体がまだ無い。

## i386 PC/AT の移植と表示面の二本立て（2026-09-12、未コミット）

PC/AT の i386 port を現行の HAL 契約に合わせ、ログインシェルまで到達した。
amd64 に回帰はない。

### ブートローダが画面をどう決めているか

追いかけた結果、ユーザの想定と実際が一点だけ違っていた。

| 経路 | フレームバッファ |
| --- | --- |
| amd64 UEFI | GOP を前提 |
| amd64 BIOS | `bootloader/pcat/vbe.inc` が VBE で LFB を設定 |
| i386 PC/AT | 取得しない。常にテキストモード |

amd64 の BIOS ローダが LFB を諦める条件は 4 つある。VBE 2.0 未満、
コントローラ情報が `VESA` でない、1024x768・800x600・640x480 のいずれでも
32bpp で SUPPORTED と GRAPHICS と LINEAR が揃うモードが無い、モード設定が
失敗する。諦めた場合は handoff のフラグ `0x0008` が立たず、
framebuffer の base/size/width/height/stride/format は書かれないままになる。

i386 PC/AT は Multiboot で起動し、ヘッダのフラグは `0x00010003` である。
ビット2のビデオモード要求が立っていないので、ローダはモードを設定せず
`multiboot_info` のフレームバッファ欄も埋まらない。VBE を試すコードも無い。
つまりこの port には最初から LFB が来ない。

### 表示面を二つ持たせた

`/dev/graphics` のテキスト層に面の概念を入れた。セル配列が唯一の真実で、
面はその publish 先である。

| 面 | 使う条件 | 文字の出し方 | カーソル |
| --- | --- | --- | --- |
| フレームバッファ | LFB がある | グリフをピクセルに描く | セル反転 |
| VGA テキストメモリ | LFB が無い | セル語をそのまま書く | CRTC |

セル配列は元から `(属性 << 8) | 文字` という VGA のセル語そのものだったので、
テキストメモリ面は 1 セル 1 ストアで済む。グラフィックモードへ切り替えるときは
既存の suspend/resume がそのまま効き、セル配列が残るので戻ると画面が復元される。
VGA のグラフィックモードは backend.c に元からある 640x480x4 planar を使う。

`drv_pcat_text_init()` は LFB 経路の中だけで呼ばれていたので、両経路の後で
呼ぶよう `text_console_start()` に切り出した。これが無いと i386 では
`kernel_putc` が公開されず、起動は進むのに画面には何も出ない。実際その状態を
ハングと読み違えた。

### HAL 側の移植

- `page.c`：`struct hal_pmem` と request/stats 構造体を廃し、物理アドレスと
  サイズの契約にした。`hal_pmem_alloc_limited()` と `hal_pmem_to_kernel()` を
  追加し、`hal_pmem_get_stats()` を `hal_get_memstat()` に改名した。
  MMIO と VRAM の固定クレーム機構はこのファイルから出した。
- `space.c`：ページテーブルのページを物理アドレスで持つようにした。
  `hal_space_map_device()` と `hal_space_unmap_device()` を追加。i386 では
  32bit 空間全体がシステム半分に別名で見えるので、窓の検査だけで足りる。
- `irq.c`：`hal_irq_register()` と `hal_irq_unregister()` を追加。
  アフィニティ取得を 2 ポインタの契約に合わせた。
- `task.c`：`hal_task_init()` を `hal_task_create_for_init_context()` にし、
  確保失敗を HAL_FATAL ではなく NULL で返すようにした。カーネルが判断する。
  アクセサ 2 本を `get_` 付きの名前と struct 無しの契約に合わせた。
- `bsp-pcat/cons.c`：1190 行から 300 行余りへ。出力専用の early console で、
  `kernel_putc` が公開されたらそちらへ委譲する。キーボードは持たない。
  初期化は `prekern_bsp_cons_init()` と `prekern_bsp_cons_irq_init()` に改名。
  後者は IRQ1 をマスクするだけで、8042 ドライバが登録するまで黙らせる。
- `src/hal/x86/io.c` を新設。ポート I/O の HAL 実装が x86 に無く、
  `kern_io_out8()` が amd64 では未使用のため gc-sections で消えていた。
  命令は両アーキで同一なので共有する。

ドライバ側では `src/drivers/pci/` の `paddr_t` 6 箇所を `uint64_t` にした。
i386 では 32bit なので、カーネル API へ渡す型が合わなかった。

### 残り

PC-98 は未着手。HAL の同じ移植に加えて `/dev/graphics` の再構築が要る。
キーボードとマウスは別 IC で割り込みも別なので、ドライバも 2 本になる。

## ヘッダ名前空間から zedbsd を外す（2026-09-12、amd64、未コミット）

ユーザ API は `include/uapi/` にあり、`/usr/include/uapi/` へ入る。36 本。
綴りはどこでも `<uapi/X.h>` である。カーネルのビルドでも、ユーザランドの
ビルドでも、インストール済みヘッダに対してコンパイルする第三者から見ても
同じファイルを指す。探索順に依存する解決がない。

内部ヘッダは `include/kern/` のままで、参照は `<kern/X.h>` である。
インストールしない。`src/kern/` にも `-Isrc` 経由で引かれるヘッダが 20 本あり、
これも `<kern/X.h>` で引く。

libc 自身の契約 5 本は `libc/include/` 直下へ移した。各ファイルの XXX コメントが
そう書いていたとおりである。

| 綴り | 指すもの |
| --- | --- |
| `<kern/X.h>` | カーネル内部。`include/kern/` か `src/kern/`。インストールしない |
| `<uapi/X.h>` | ユーザ API。全ビルドとインストール後で同一 |

### インストール先に uapi を残す理由

`/usr/include/kern/` に入れると内部ヘッダと 14 個の名前が衝突する。
`atomic.h`、`poll.h`、`process.h`、`quota.h`、`resource.h`、`signal.h`、
`syscall.h`、`sysctl.h`、`thread.h`、`usync.h`、`writeback.h`、`io-stats.h`、
`cache-memory.h`、`readahead.h` である。

自分のソースだけなら綴り分けで済む。済まないのは libc のヘッダである。
`libc/include/sys/resource.h` はインストール後の利用者のために `<kern/resource.h>`
と書くしかないが、カーネルもユーザランドも `-Iinclude` を sysroot より先に置くので、
この参照が内部ヘッダに解決されてしまう。実際 `CTL_MAXNAME` と `RLIMIT_*` が
これで消えた。`uapi` を接頭辞に残すと、この綴りが一意になり衝突自体が起きない。

sysroot を作る規則は `include/` だけを剥がす。移行前は `include/uapi/` を
剥がしていたので、名前空間がインストール先に出てこなかった。

### noct 側の追従

上流 NoctLang が zedBSD のパスを 3 箇所参照していた。CMake ツールチェーンの
必須ファイル一覧に `include/uapi/zedbsd/system.h`、BeUI バックエンドに
`<zedbsd/graphics.h>` と `<zedbsd/input.h>` である。パッチ 0001 で当ててビルドを
通したが、これは本来上流で直すべき 3 行である。パッチレベルは `zedbsd11`。

### 付随して直したもの

sysroot のヘッダ一覧が生の `find` だったため、エディタのバックアップが
`/usr/include/` にコピーされていた。`! -name '*~'` を足した。リポジトリには
まだ 164 個残っている。

### 残る zedbsd

`/usr/lib/zedbsd/<arch>/` にリンカスクリプトを置いている。これは OS ベンダの
ライブラリディレクトリなので、ヘッダとは別の判断になる。ビルド専用の makefile
変数と noct 統合のファイル名も未着手のままである。

ビルドは PASS。QEMU でログイン、シェル、ユーザランド noct の応答を確認した。

#### fg007：段階5 early console への縮退（2026-09-11、amd64、未コミット）

amd64 の HAL コンソールを出力専用の早期コンソールへ縮めた。early console 化は完了。

- `src/hal/amd64/bsp-pcat/cons.c` は 2407行から 1054行になった。公開していた
  `hal_cons_*` 19関数、キーイベントのリング、待ち行列、キーボード所有権のテスト
  フィクスチャ6個、8042 のポート定数を削除した。残るのは `hal_putc()`、その描画経路、
  出力ロック、`prekern_pcat_cons_init()`、`prekern_pcat_cons_irq_init()` である。
- `src/hal/cons-keys.h` を削除した。キーイベントの符号化はカーネル側の
  `KERN_KEY_*`（include/kern/input-device.h）だけになった。
- `src/hal/cons-wait.h` は i386・arm64・m68k がまだ使うため残す。amd64 からの参照は消えた。
- イベントモードの分岐を削除した。早期コンソールは常にターミナル表示である。

検証（QEMU、QMP の send-key）：

- `login:` まで到達し、`root` の打鍵がエコーされる。
- 空パスワードでログインし、`root@zedbsd:/root$` のシェルで `ls` が実行できる。
- 8042 → evdev → `/dev/console` の keymap 変換 → `/dev/graphics` のテキスト層、という
  入力と出力の往復が通っている。

fg007 の到達点：

- hal.h の Console 節は存在しない。HAL の表示関連は `hal_putc()` 1本。
- 表示は `/dev/graphics`（`graphics/text.c`）、入力は `ps2-8042.c` の evdev、
  端末制御は `/dev/console` というプラットフォーム独立のマルチプレクサ、という構成になった。

残り：

- HAL 側の `kernel_wait_task()`/`kernel_notify_task()` は `src/hal/amd64/irq.c` の
  `hal_irq_service_wait()` にのみ残る。これはサービス方式IRQ廃止の別作業に属する。
- hal.h に説明要求の XXX が13件残る。
- i386・arm64・sparcv9・m68k は未対応。
- 上位モデルによる監査。

#### fg007：init 停止の解決と段階4（2026-09-11、amd64、未コミット）

`boot: starting init /sbin/init` で止まる症状を解決した。QEMU で `login:` まで到達し、
QMP の send-key で `root` を打鍵すると `login: root` とエコーされて `Password:` が出る。
8042 ドライバ → evdev → `/dev/console` → テキスト層の往復が動作している。

原因は2つあった。

1. `drv_pcat_text_init()` の挿入先の誤り。`backend.c` のグラフィックモード進入関数に入って
   いて、起動時に走る `pcat_graphics_prepare_hardware()` には無かった。`text_ready` が0のまま
   となり、`/dev/console` 経由の書き込みは `putc_locked()` の先頭で黙って捨てられていた。
   カーネル自身のログは HAL 早期コンソールが描いていたため、描画が動いているように見えた。
2. グリフ取得の戻り値の読み違い。`drv_pcat_font_get_glyph()` は成功時に1を返すが、
   テキスト層は0を成功と扱っていたため、全文字が空白で描かれ、カーソルの反転セルだけが
   見える状態になっていた。

段階4を同時に実施した。

- `hal_cons_putc()` を `hal_putc()` に改名し、hal.h の HAL C runtime 節へ移した。
  hal.h の Console 節は消えた。
- カーネルが関数ポインタ `kernel_putc`（初期値 NULL、`src/kern/entry.c`）を提供し、
  `hal_putc()` は ACQUIRE で読んで非 NULL なら委譲する。`backend.c` はテキスト層の初期化
  直後に RELEASE ストアで `drv_pcat_text_putc` を公開する。以降 `hal_printf()` と
  `/dev/console` は同じカーソルを共有する。
- テキスト層は制御文字でカーソルが移動する前に反転セルを消すようにした。

結果として、1024x768 のフレームバッファ全面が 128x48 のテキストグリッドになった。
HAL 早期コンソールの 80x30 中央寄せは、引き渡しまでの表示にだけ使われる。

残り：

- 段階5。amd64 `cons.c` を早期コンソールの最小構成へ縮める。`src/hal/cons-keys.h` と
  `src/hal/cons-wait.h` の削除、HAL 側の `kernel_wait_task()`/`kernel_notify_task()` 利用の撤去。
- 他アーキは未対応。
- 上位モデルによる監査。

#### fg007：段階3 ps2-8042.c の実装（2026-09-11、amd64、未コミット）

ビルド PASS。evdev デバイスが2つとも1つのドライバから登録され、VFS も完了する。
`boot: starting init /sbin/init` の先へ進まない症状は段階1-2から継続。

実装：

- `src/drivers/platform/pcat/ps2-8042.c`（926行）。既存 `ps2-mouse.c` を吸収し、
  キーボード側を追加した。`controller_lock` 1本で IRQ1 と IRQ12 の両方を持つ。
  ヘッダは `include/drivers/hid/ps2-8042.h`、入口は `drv_pcat_ps2_8042_init()`。
- スキャンコード表 `scan_symbols[128]` と `scan_symbol()` を HAL の cons.c から移した。
  キーコードへの変換は既存の `drv_input_key_from_symbol()` を使う。
  capability は表から機械的に構築するので二重管理にならない。
- キーボード割り込みは、バイトを読んで EOI してから publish する。8042 は出力バッファを
  読むと IRQ1 を下げるので、次のバイトが即座に新しいエッジを作れる。ps2-mouse と同じ方針。
- `/dev/console` の自前キーボードデバイス登録と capability 構築を削除した。
- HAL の `prekern_pcat_cons_irq_init()` は IRQ1 を登録しなくなった。マスクするだけである。
  これに伴い cons.c から `keyboard_interrupt()`、`keyboard_controller_init()`、
  設定バイト操作、`pump_keyboard_locked()`、`hal_cons_modifiers()`、修飾キー状態、
  スキャンコード表が不要になり削除した。

起動ログの確認：

    input: /dev/input/event0: PC/AT PS/2 mouse
    input: /dev/input/event1: PC/AT PS/2 keyboard

途中で見つけた不具合：

- IRQ1 の二重所有。HAL の cons.c が先に `hal_irq_register()` していたため、
  ps2-8042.c の登録が EBUSY（zedBSD では17）になっていた。HAL 側を外して解決。
- `/dev/console` のキーボードデバイスが残っていて event 番号が衝突していた。削除して解決。

残る症状：

- init 起動後に出力が止まる。段階1-2から続く未解決点で、キーボード実装とは独立している。
  上位モデルによる監査の対象とする。

#### fg007：early console 段階1-2 の実装（2026-09-11、amd64、未コミット）

ビルドは PASS。QEMU で `boot: starting init /sbin/init` まで到達するが、ログインまで進まない。

実装した内容：

- `src/drivers/platform/pcat/graphics/text.c`（481行）と `text.h` を新設した。セル配列、
  カーソル、スクロール、グリフ描画、VGA 16色パレットを持つ。フレームバッファは
  `drv_pcat_graphics_backend_get_framebuffer()` から取る。グリッドは
  幅/8 × 高さ/16 で算出し、端数は中央寄せする。
- `backend.c` に `drv_pcat_graphics_backend_get_framebuffer()` を追加し、
  `drv_pcat_text_init()` をフレームバッファ確定時に呼ぶようにした。
- `/dev/console` と `tty.c` の描画を `drv_pcat_text_*` へ向けた。
- `hal_cons_suspend()`/`hal_cons_resume()` は `drv_pcat_text_suspend()`/`_resume()` になり、
  同一ドライバ内の状態管理になった。hal.h から外せる状態。
- `/dev/console` の `console_input_worker()` スレッドを削除した。キーボード入力は
  段階3の `ps2-8042.c` が evdev へ発行するまで無い。
- `amd64_device_map()` と `hal_space_map_device()` は、固定デバイス窓が早期ページングで
  既に張られているため、重複マップをエラーにしない実装にした。
- `hal_pmem_alloc()`/`hal_pmem_free()` のページ丸めを割り当て器内部に閉じた。
  呼び出し側が要求サイズをそのまま渡せば解放できる。

未解決：

- `boot: starting init /sbin/init` の後が出ない。userland からの `/dev/console` 書き込みが
  進んでいない。カーネル自身の起動ログは新しいテキスト層で正しく描けているので、
  描画そのものは動作している。
  ロックランクは確認済みで原因ではない。`LOCK_RANK_CONSOLE_TEXT = 137` を新設し、
  `tty.c` の `LOCK_RANK_TTY = 135` より上の葉ロックにしたが症状は変わらなかった。
  次に見るべきは `console_write()` から `tty_render()` への経路と、
  `console_input_worker()` 削除に伴う `/dev/console` の読み側の待ち合わせ。
  init が `/dev/console` を開いて読もうとし、入力源が無いまま待っている可能性がある。
- `/dev/console` が自前登録している `keyboard_input` デバイスはまだ残っている。
  段階3で `ps2-8042.c` に置き換える。

#### fg007：early console 化の計画（2026-09-11、amd64先行）

`hal_cons_*` を早期コンソールへ縮退させ、起動後のコンソールはカーネル側が持つ。
HAL に残すのは `hal_cons_putc()` のみで、用途は HAL 内と、カーネル初期化段階の
`hal_printf()` による診断出力に限る。

構成（2026-09-11ユーザー整理。旧案の「`/dev/console` は `/dev/graphics` に依存しない」は
これに置き換える）：

- `/dev/console` はプラットフォーム独立。`/dev/graphics` と evdev を利用する
  マルチプレクサであり、ファイル操作、tty ディシプリン、ANSI/CSI 解釈、input 購読、
  `drv_input_keymap_translate()` による文字変換を持つ。アーキ別実装は作らない。
- `/dev/graphics` が表示を所有する。`pcat-graphics.c` に文字出力とカーソル機能を統合し、
  セル配列・スクロール・属性・グリフ描画を持つ。グラフィックと文字は同時に使える。
  プラットフォームによってはグラフィックモードが無くてもよい。
- キーボードは `drivers/platform/pcat/ps2-keyboard.c` に分離し、evdev ドライバとする。
  `ps2-mouse.c` と同型で、`hal_irq_register()` と `drv_input_device_emit()` を使う。

この構成ではフレームバッファの所有者が `/dev/graphics` 一つになるため、
`hal_cons_suspend()`/`hal_cons_resume()` は不要になり hal.h から外せる。
`pcat-graphics.c` の当該6箇所は同一ドライバ内の状態管理になる。

確認済みの前提：

- `hal_cons_getc()` は HAL のコンソール実装以外から呼ばれていない。早期コンソールに入力は不要。
- `pcat.framebuffer` と `pcat.boot-font` の handoff は既に `graphics/backend.c` と
  `graphics/font.c` が使っている。`drv_pcat_graphics_backend_*` に fill/blit/glyph/flush、
  `drv_pcat_font_*` にフォント操作がある。
- `/dev/console` は既に input レポートを購読し keymap で文字へ変換する経路を持つ。
- `src/hal/amd64/bsp-pcat/cons.c` は2407行で、表示系と 8042 キーボード系が同居している。
- 現在のビルド残りは27件。`console.c` 12、`tty.c` 9、`pcat-graphics.c` 6。

段階：

1. `pcat-graphics.c` に文字出力とカーソルの内部インタフェースを足す。
2. `/dev/console` と `tty.c` の描画をそこへ向ける。
3. `ps2-keyboard.c` を新設し evdev へ発行する。`/dev/console` の `console_input_worker()` と
   自前登録の `keyboard_input` デバイスを廃止し、購読経路へ一本化する。
4. hal.h のコンソール宣言を `hal_cons_putc()` まで縮退し、amd64 cons.c を早期コンソールに縮小。
   `src/hal/cons-keys.h` と `src/hal/cons-wait.h` も不要になる。
5. HAL から `kernel_wait_task()`/`kernel_notify_task()` の利用が消える。

決定済み（2026-09-11ユーザー指示）：

- `/dev/graphics` は「表示デバイス」として全プラットフォームに置く。グラフィックモードのほうが
  任意であり、テキストしか持たない機種でも `/dev/graphics` が存在する。
- `/dev/console` から `/dev/graphics` へはカーネル内の関数呼び出しで密結合してよい。
  ioctl は経由しない。
- 責務の境目は、`/dev/graphics` がセル配列・カーソル位置・スクロール・属性・グリフ描画を持ち、
  `/dev/console` が ANSI/CSI を解釈して位置指定の書き込み・カーソル移動・範囲消去へ翻訳する。
  現在 `tty.c` にある CSI 処理はコンソール側に残る。

出力の一本化：

- `hal_cons_putc()` を `hal_putc()` へ改名し、hal.h の Console 節ではなく冒頭の
  HAL C runtime 節へ移す。これにより hal.h から Console 節そのものが消える。
- カーネルは関数ポインタ `kernel_putc` を提供する。初期値は NULL。
- `hal_putc()` は `kernel_putc` を見て、非 NULL ならそちらへ委譲し、NULL なら自分の
  早期コンソールへ書く。これにより `hal_printf()` の出力は引き渡し後に自動的に
  本来のコンソールへ流れ、呼び出し側の変更が要らない。
- 差し替えは `/dev/graphics` と `/dev/console` が完全に使える状態になってから、
  RELEASE 順序の単一ストアで行う。
- `kernel_putc` は割り込み文脈と panic からも呼ばれるため、眠らないこと、
  IRQ 保存のロックで保護することを契約とする。

panic の扱い（2026-09-11修正）：

- panic で HAL コンソールへ退避する設計は採らない。カーネルは表示デバイスを変更しうるため
  （GOP から GPU など）、引き渡し後の HAL 側の出力先は既に無効になっている可能性がある。
  よって panic も通常経路、すなわち `hal_putc()` から `kernel_putc` を使う。
- 引き渡し後、HAL は自分の早期コンソール状態に触れない。

8042 の扱い（2026-09-11決定）：

8042 はコントローラICが1個で、キーボードとマウスはその2ポートである。データポート 0x60、
コマンド/ステータスポート 0x64、設定バイト、出力バッファのいずれも共有され、コマンド列も
状態を持つ（マウス宛は 0x64 へ 0xD4 を書いてから 0x60 へ書く）。別々のロックで触ると競合する。

よって、ICが1個であることに合わせ、1つのドライバソース
`drivers/platform/pcat/ps2-8042.c` がコントローラを所有し、evdev デバイスを2つ登録する。

- IRQ1（キーボード）と IRQ12（マウス）の両方をこのドライバが `hal_irq_register()` で登録する。
- `controller_lock` は1本。設定バイトの read-modify-write、コマンド送出、
  入力バッファ待ち、データ読み出しと AUX 判定をこのロックの下で行う。
- 既存の `ps2-mouse.c`（627行、IRQ12）はこのファイルへ吸収する。
- キーボードのスキャンコード表と解釈は `src/hal/amd64/bsp-pcat/cons.c` から移す。
- 出力は `drv_input_device_emit()` のみ。HAL のキーイベント API は使わない。

これで fg007 の設計判断はすべて解決済みとなる。

影響範囲の実測（plan/ と build/ の複製を除く）：コンソール定数198箇所、write/clear系75箇所、
`hal_key_event` 関連59箇所、アロケータ278箇所。`prekern_` 対象は5アーキの起動前経路で約50関数。
HAL内のアロケータ使用26箇所のうち起動経路は6箇所のみで、残る20箇所は実行時のため対象外。


### トレーサビリティの読み方

各WSのPrimaryは一つ。Relatedは横断的な貢献先であり親ではない。各Phaseの親WSは既存IDから追跡し、達成を裏付けるのはそのPhaseの現行結果・成果物である。取消しPhaseと置換済み実装は現在の達成証拠に数えない。Phaseの実行/結果は既存Queueと記録へ辿る。

<!-- traceability:end -->


Last updated: 2026-09-11
Status: active

このファイルは現在の優先順位・WS状態・未解決事項の索引です。
詳細設計と受け入れは各WS/Phase、実行履歴はQueueと結果資料に置きます。
新しい経緯を本文へ追記し続けず、状態と参照先を更新します。

## 現在地

2026-09-11のユーザー指示により現在のPriorityリストを削除。WS025のp029/p030/p032/p038をcleared、WS025をcompletedとし、既存completedのWS019/WS006/WS022/WS002とともに閉鎖する。未実施の検証をPASSへ変更せず、今回の計画上の受け入れとして記録する。q303はfinished/stoppedのまま。active QueueとPriorityリストはない。後続のユーザー指示でfg004（インストーラ）、fg005（ネットワーク）、fg006（V13起動改善）、fg007（HALリファクタリング）を追加した。

旧Priority順はWS025 → WS006 → WS022 → WS019 → WS002 → WS009。2026-09-11に削除済みであり、実行順・承認として再利用しない。WS009/DOC-54のWS014手動保留は維持する。

## Workstream registry

Future Listへ移したWS013・WS015は次節で管理する。完了WSの詳細証拠は各WSを参照。

| WS | Primary Milestone | 内容 | 状態 | 残作業・参照点 |
| --- | --- | --- | --- | --- |
| [WS001](ws001/ws.md) | MG002 | POSIX準拠 | 継続 | 準拠性台帳・コード規約の残件。 |
| [WS002](ws002/ws.md) | MG005 | システムサービス | completed | p021をユーザー判断でcleared。p023/p024完了、POSIXの引継ぎと既知バグの再発条件は保持。 2026-09-11ユーザー指示で閉鎖。 |
| [WS003](ws003/ws.md) | MG003 | x86・PC-98実機対応 | incomplete | fg006のp022/p023/p024は2026-09-12ユーザー報告でcleared。fg004の4機種インストーラほか残件を保持。 |
| [WS004](ws004/ws.md) | MG003 | ハードウェア拡張 | 継続 | 主要USB/WLAN経路完了。NVMe実機・転送・ドライバ共通化等の後続項目を保持。 |
| [WS005](ws005/ws.md) | MG005 | ネットワーク・WLAN | incomplete | fg005: net lan、network-enable（どちらかIP・既定30秒・timeoutでも起動継続）、DE状態通知をp013〜p017で計画。既存p001〜p012の完了は維持。 |
| [WS006](ws006/ws.md) | MG006 | 入力・evdev | completed | q147。両USB構成の通常ビルドで実Xzed/PTYとUSB-root/HID受け入れ。 2026-09-11ユーザー指示で閉鎖。 |
| [WS007](ws007/ws.md) | MG006 | グラフィックス・デスクトップ | 一部未クリア | p004の正確なGUI再現条件、amd64残件、統合試験。 |
| [WS008](ws008/ws.md) | MG006 | Noct・BeUI | 完了 | q063。 |
| [WS009](ws009/ws.md) | MG001 | ドキュメント | 手動保留項目待ち | p001〜p008完了。残るDOC-54はWS014 GPUの保留解除後にPhase化。 |
| [WS010](ws010/ws.md) | MG001 | スクリプト・イメージツール | 完了 | q063。 |
| [WS011](ws011/ws.md) | MG005 | ネットワーク設定コンソール | 完了（ユーザー確認） | commit confirmed完了。VLANキャンセル、bridgeはF-001へ移管。 |
| [WS012](ws012/ws.md) | MG005 | サービス管理コンソール | 完了 | q018。 |
| [WS014](ws014/ws.md) | MG006 | ネイティブGPU | 明示保留 | GPU設計の保留解除時に再開。 |
| [WS016](ws016/ws.md) | MG004 | 実行時swap制御 | 完了 | q021。 |
| [WS017](ws017/ws.md) | MG006 | LFB描画高速化 | 依存待ち | WS022後にmmap・Xzed高速描画・受け入れ。 |
| [WS018](ws018/ws.md) | MG008 | カーネル所有権・構成統一 | 完了 | p001〜p020。I/O後続はWS025。 |
| [WS019](ws019/ws.md) | MG003 | インストール・ディスク管理 | completed | 完了（q186）。amd64共存・専用・グラフィカル版とPC98 FATインストールを受け入れ済み。 2026-09-11ユーザー指示で閉鎖。 |
| [WS020](ws020/ws.md) | MG003 | Intel Mac UEFI・Variant | 完了 | 2026-09-05ユーザー実機確認。 |
| [WS021](ws021/ws.md) | MG001 | LLVM・sysroot | 完了 | q064。 |
| [WS022](ws022/ws.md) | MG002 | ELF TLS | completed | q128：p001〜p003、両x86 QEMU・dynamic回帰、PC98確認。 2026-09-11ユーザー指示で閉鎖。 |
| [WS023](ws023/ws.md) | MG001 | x86 HALコーディング規約 | 完了 | q067。 |
| [WS024](ws024/ws.md) | MG004 | 単一64-bit UFS | 完了 | q102。 |
| [WS025](ws025/ws.md) | MG004 | I/O・キャッシュ・物理メモリ | completed | 2026-09-11ユーザー判断でp029/p030/p032/p038をcleared、WSを閉鎖。p028 canceledを維持。 |
| [WS026](ws026/ws.md) | MG001 | テスト資産の整理 | 計画のみ | 不要・重複テストの整理と現行ソースへの追随。Phase未詳細化・Queue未投入。 |

## Future List(やりたいことリスト)

[Future Work](future-work.md)にF-001〜F-003の由来・再開点を保持。現在の実行対象ではない。

## Known bugs

正本は [既知バグ台帳](known-bugs.md)。再現済み・ユーザー報告・解決済みを区別して記録する。

- **BUG-010：実機USBブート後にhaltするとUSBエラーが出る**。ユーザー報告、未修正。
  [WS002-p023](ws002/phase023/results.md)でq135にQEMU再現・修正済み。実機とEHCI/UHCIの追加確認は残る。
- AX211のfirmware内部assertは残存する。無限診断・復帰不能の修正と実機復帰試験は
  [WS025-p035結果](ws025/phase035/results.md)を参照。

## 主要な依存関係

- WS025の追加残件は2026-09-11ユーザー判断でcleared。既存のI/O契約と旧証拠を保持し、未検証の実装・実機結果を他WSの前提充足に流用しない。
- WS022 → WS017。WS006のevdev移行はデスクトップ各consumerへ反映する。
- WS019のインストーラ受け入れ → WS003の実機NVMeインストール・起動。
  WS013の完了済みブート設定基盤を使い、将来のRuntime CPARを前提にしない。
- WS001の準拠性とWS009の文書は各実装WSから更新する。

## 方針・履歴への索引

- [設計方針・決定の参照資料](master-design-policy.md)：独立実装・ライセンス境界、モジュール設計、ツールチェーン、個別設計判断。
- [整理前master全文](old/master-history-2026-09-09.md)：旧優先順位、milestone、依存図、完了経緯、保留の履歴。現在の実行指示には使わない。
- [Awesome Plan設定](config.md)・[Guardrail](guardrail.md)・[コーディング規約](coding-style.md)。旧MWP-Q/governanceはold/の履歴。
- [q124結果](history/queue-q124.md)、[q123：p031未クリア](history/queue-q123.md)、[q122：WS025統合受け入れ](history/queue-q122.md)。
- [FS関連フォローアップ](old/fs-report-followups.md)。

長期目標は独立実装した共通OS基盤をデスクトップ・モバイル・組込み・サーバーへ提供すること。
設計詳細や過去の結果をこの索引へ重複掲載せず、所有する資料へリンクする。

### Priority execution clarification (2026-09-09)

- 実装可能な作業を先行し、WS006 EHCI/UHCIのQEMU解析は後段へ回すが、現在のゴール内で修正を目指す。
- WS025 p027–p030は実機不在を実装の停止条件にしない。QEMU機能検証と実機性能の未測定を分離する。
- ユーザーはUAS実機を所有していない。ローカルQEMUには`usb-uas`があるため検証経路を確認する。QEMU検証が不可能と確認された場合にUASをFuture Listへ移す。
- WS019 p008の旧形式証跡に代えてp014で現行UFS formatterを再検証・必要改修し、installerの前提とする。

<!-- awesome-plan-operations:start -->

## Awesome Plan運用

[Queue](https://github.com/awemorris/zedBSD/issues/362) · [Guardrail](https://github.com/awemorris/zedBSD/issues/363) · [Future Work](https://github.com/awemorris/zedBSD/issues/364) · [Bug Board](https://github.com/awemorris/zedBSD/issues/365) · [Past Log](https://github.com/awemorris/zedBSD/issues/366)

GitHub modeで運用。q303はfinished/stopped、active Queueはありません。仕様固定版は314a669f57265da3084ffac810871b0e660c9526。次回はリポジトリAGENTS.mdとplan/config.mdから開始します。

<!-- awesome-plan-operations:end -->

GitHub Milestones: [MG001](https://github.com/awemorris/zedBSD/milestone/1) · [MG002](https://github.com/awemorris/zedBSD/milestone/2) · [MG003](https://github.com/awemorris/zedBSD/milestone/3) · [MG004](https://github.com/awemorris/zedBSD/milestone/4) · [MG005](https://github.com/awemorris/zedBSD/milestone/5) · [MG006](https://github.com/awemorris/zedBSD/milestone/6) · [MG007](https://github.com/awemorris/zedBSD/milestone/7) · [MG008](https://github.com/awemorris/zedBSD/milestone/8) · [MG009](https://github.com/awemorris/zedBSD/milestone/9)

## 2026-09-11 実行保留

ユーザーがp022のキュー実行を保留。fg004/fg005/fg006とp022→p023→p024の計画は保持し、active Queueは作成せず、build・実機操作・実装は開始していない。MasterのGitHub同期は今回明示された範囲で行う。

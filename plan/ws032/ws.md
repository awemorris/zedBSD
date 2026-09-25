<!-- awesome-plan project=zedbsd record=ws032 -->

# WS032: 外部 package のクロスビルド導入（clang・OpenSSL・OpenSSH）

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-23（q315）
Primary Milestone: MG002
Related Milestones: なし
Objectives: O1
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

`userland/packages/` に clang・OpenSSL・OpenSSH を、release の tarball の取得・検証・patch・クロスビルドで追加し、menuconfig で選んだものを image に載せて実機で動かす。

## 結果

共通の取得機構（`userland/packages/external.mk`）、クロスビルドの契約、libc の不足の補完、C++ runtime、OpenSSL・OpenSSH・clang と lldb を入れた。実機で `clang hello.c` から実行、公開鍵の ssh、openssl、lldb の breakpoint と watchpoint を確認した。設計は [external-design.md](external-design.md)、版と入手元とライセンスは [provenance.md](provenance.md)、upstream への patch は [patches-for-upstream](patches-for-upstream)。

## 制限・移管

BUG-026・BUG-027・BUG-028 を残す。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws032-p001 | 設計固め: 外部設計確定、3パッケージの版・入手元の確定、ライセンス監査方針 | cleared（q315） |
| ws032-p002 | 共通取得機構: `userland/packages/external.mk`（取得・検証・展開・パッチ）と `download`/`patch` 接続、host試験 | cleared（q315） |
| ws032-p003 | クロスビルド契約: wrapper・autoconf cross cache・CMake toolchain file、動的リンクの成立を最小C/C++プログラムで確認 | cleared（q315） |
| ws032-p004 | libc・ヘッダの不足補完（`netinet/tcp.h`、`sys/param.h`、`chroot`、`dl_iterate_phdr`、`posix_memalign` ほか、実ビルドで確定した分） | cleared（q315） |
| ws032-p005 | C++ランタイム: libunwind → libc++abi → libc++ をクロスビルドし、例外・RTTI・static initializer・`thread_local` を実機で確認 | cleared（q315） |
| ws032-p006 | OpenSSL: 版pin・Configureターゲット・クロスビルド・`libcrypto.so`/`libssl.so`/`openssl`、実機確認 | cleared（q315） |
| ws032-p007 | OpenSSH: cross configure・privsep・`ssh`/`sshd`/`ssh-keygen`/`scp`/`sftp`、実機で公開鍵ログイン | cleared（q315） |
| ws032-p008 | clang: LLVM クロスビルド（host tablegen 再利用）、ツール一式を `/usr/bin` へ、実機で `clang hello.c -o hello` → 実行 | cleared（q315） |
| ws032-p009 | イメージ統合: menuconfig 登録・rootfs/disk-image 投入・ライセンス通知・provenance 記録・既定構成への非影響確認 | cleared（q315） |
| ws032-p010 | レビュー: 規約全文確認、静的確認、回帰、制限整理 | cleared（q315） |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws032/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。

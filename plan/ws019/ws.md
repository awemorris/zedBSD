<!-- awesome-plan project=zedbsd record=ws019 -->

# WS019: インストールとディスク管理

<!-- awesome-plan-current:start -->
Status: completed
Completed: 2026-09-11（q186、ユーザー指示で閉鎖）
Primary Milestone: MG003
Related Milestones: MG006, MG004
Objectives: O1, O2, O4
Parent: [Master](../master.md)
Queue: なし
Resume point: なし（新しい要求は新しい WS として立てる。この WS は再開しない）
<!-- awesome-plan-current:end -->

## 目標

既存 OS と共存するインストール、ディスク全体を使う専用インストール、PC-98 の FAT インストールを成立させ、インストール先から単独で起動できるようにする。

## 結果

block 情報と mount の列挙、partition の検査と編集、`mkfs`・`mkswap`・`diskpart`、FAT32 と native UFS の初期化、原子的な公開、image の検証と copy、swap file、テキストと BeUI のグラフィカルなインストーラ（`/bin/zedinst`）を実装した。amd64 の共存・専用、グラフィカル版、PC-98 の FAT インストールを受け入れた。

## 制限・移管

4 機種の実機での受け入れは WS028 が担う。

## Phase 一覧

| Phase | 内容 | Status |
| --- | --- | --- |
| ws019-p001 | overlay installer-v1 contract | cleared |
| ws019-p002 | basic block information and mount enumeration | cleared（q076） |
| ws019-p003 | userspace partition inspection | cleared（q076） |
| ws019-p004 | existing-FAT overlay `/bin/zedinst` | cleared（q159） |
| ws019-p005 | QEMU NVMe overlay-install acceptance | cleared（q160） |
| ws019-p006 | mode selection and whole-disk provisioning | cleared（q182） |
| ws019-p007 | dedicated native UFS root installation | cleared（q183） |
| ws019-p008 | target `/sbin/mkfs` | cleared（q079） |
| ws019-p009 | target `/sbin/mkswap` | cleared（q079） |
| ws019-p010 | conservative whole-disk partition reload | cleared（q077） |
| ws019-p011 | userspace existing-table editing and notification | cleared（q077） |
| ws019-p012 | explicit auxiliary filesystem mounts | cleared（q077） |
| ws019-p013 | installer prerequisite contracts | cleared（q129） |
| ws019-p014 | current UFS formatter integration | cleared（q129） |
| ws019-p015 | atomic publication and command durability | cleared（q130） |
| ws019-p016 | retained boot-source identities | cleared（q131） |
| ws019-p017 | standard-command installer staging | cleared（q133） |
| ws019-p018 | FAT growth capacity admission | cleared（q133） |
| ws019-p019 | read-only pristine image verification | cleared（q148） |
| ws019-p020 | reliable df capacity observations | cleared（q151） |
| ws019-p021 | process-path mount and unmount | cleared（q154） |
| ws019-p022 | populated tmpfs teardown | cleared（q155） |
| ws019-p023 | update Noct for large File.seek | cleared（q155） |
| ws019-p024 | Noct terminal input progress | cleared（q158） |
| ws019-p025 | Public installer runtime contracts | cleared（q159） |
| ws019-p026 | Noct-managed image copy and progress | cleared（q159） |
| ws019-p027 | installation source selection and mounted-image admission | cleared（q162） |
| ws019-p028 | complete tree enumeration and attribute-preserving copy | cleared（q161） |
| ws019-p029 | BeUI graphic installer frontend | cleared（q184） |
| ws019-p030 | whole-disk GPT initialization codec | cleared（q163） |
| ws019-p031 | exclusive whole-disk administration | cleared（q164） |
| ws019-p032 | public diskpart GPT initialization | cleared（q165） |
| ws019-p033 | partition administration reservations | cleared（q166） |
| ws019-p034 | portable FAT32 initializer | cleared（q167） |
| ws019-p035 | reserved FAT32 mkfs command | cleared（q168） |
| ws019-p036 | native UFS initialization and wide geometry | cleared（q169） |
| ws019-p037 | shared reserved native UFS command | cleared（q170） |
| ws019-p038 | immutable root-image source view | cleared（q171） |
| ws019-p039 | owned Noct root-image source view | cleared（q172） |
| ws019-p040 | filesystem file-extent capability | cleared（q173） |
| ws019-p041 | UFS file extent provider | cleared（q174） |
| ws019-p042 | reject self-overlapping backing ranges | cleared（q175） |
| ws019-p043 | file-owned indirect metadata extents | cleared（q176） |
| ws019-p044 | complete file claim finalization | cleared（q177） |
| ws019-p045 | UFS backing admission and native swap | cleared（q178） |
| ws019-p046 | shared native installation layout | cleared（q179） |
| ws019-p047 | native root file swap startup | cleared（q180） |
| ws019-p048 | non-mutating formatter admission for native installation | cleared（q181） |
| ws019-p049 | complete native installer transaction and text flow | cleared（q182） |
| ws019-p050 | PC98 graphical FAT installation | cleared（q186） |

## 記録の所在

各 Phase の計画・結果・試験は、2026-09-24 の plan 整理で削除した。git の commit `04bc9eab` 以前の `plan/ws019/` にある。Queue ごとの履歴は [plan/history](../history/index.md) に残る。

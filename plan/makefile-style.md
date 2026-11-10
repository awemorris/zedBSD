Makefile Style
==============

zedBSD の build system（top の `Makefile`、`mk/`、`mk/platform-<機種>/`、userland の package の
Makefile と `userland/base/package.mk`）を書く時の規則。

2026-11-10、ユーザーから、Makefile を一緒に作るための規則を形作り、使いながら磨いていく、という
趣旨の指示を受けて作った。完成した規則ではない。新しく分かった事は、ユーザーと相談して足す。

## A. 構造

1. top の `Makefile` は、目的（target）の並びと include の順だけを書く。中身は `mk/` に置く。
2. 共通の物は `mk/<主題>.mk`、機種の物は `mk/platform-<機種>/<段>.mk`
   （defaults・libc・sysroot・vmunix・firmware・bootloader・programs・rootfs・image）に分ける。
3. 選べる項目は `mk/config.mk` に、`# [名前]` と `#  - Description:` の注釈を付けて 1 つずつ並べる。
   機種の既定は `mk/platform-<機種>/defaults.mk` に書く。
4. kernel の項目は `mk/kernel.mk` に、`# [名前]` の節ごとに `ifneq ($(filter y,$(CONFIG_X)),)` で囲み、
   `KERN_CPPFLAGS += -DCONFIG_X=1` と `KERN_SOURCES += …` を並べる。
5. CPU の architecture ごとにまとめる整理は、ボードが増えてからする。具体的な例から抽象化する
   （ユーザーの決定）。

## B. 規則

1. **読む順**: ある変数（`VMUNIX_OBJS`・`KERN_CFLAGS` など）を使う規則や `-include` は、その変数を
   定義する mk より後に読む。前に置く時は、展開が遅れる書き方（recipe の中か `=`）だけにする。
   機種の mk の後に読む物は `mk/tracking.mk` のように分けて、top の `Makefile` で機種の mk の後に include する。
2. **`:=` と `+=`**: 機種の mk は flag を `:=` で定義し直すことがあるので、共通の側から足す時は、
   その後に `+=` する。どちらが後に読まれるかを、変更のたびに確かめる。
3. **作り直しの追跡**: object を作る規則には、flag の stamp と header の依存（`-MMD -MP`）の両方を付ける。
   新しい object の群を足したら、`mk/tracking.mk` にも足す。
4. **config から作る file**: config の値で中身が変わる file（`cmdline.txt`・`config.txt` など）は、毎回作って、
   中身が変わった時だけ置き換える（`cmp -s` で比べてから `mv`）。
5. **名前の照合**: `CONFIG_*` の名前は、`mk/config.mk`・`defaults.mk`・`mk/kernel.mk`・source の `#if` の
   4 か所で揃える。名前を変える時は 4 か所を同じ commit で直す。
6. **選ぶ名前の意味**: `CONFIG_USERLAND_PROGRAMS` に書く名前が何を入れるかを変えない。package を
   分ける時は、`mk/userland.mk` の `pkg_aliases` で、元の名前が同じ物を選ぶようにする。
7. **子の make**: 子の make は別の directory で走るので、path は repository の root から書く
   （`$(REPO_ROOT)/…`）。自分の directory は、他の mk を include する前に変数に覚えておく。
8. **確かめ方**: 変えたら、次の 4 つを見る。
   - `make -n`（dry run）で、走る command が意図どおりか。
   - `make -p` で、変数が意図どおりに展開されているか。
   - 何も変えずに 2 回目を流して、作り直しが 0 か。
   - header（か flag）を 1 つ変えて、それに依る object だけが作り直されるか。

## C. 進め方

1. Makefile を変える時は、概要と差分を示し、ユーザーの OK の後に当てて commit する。
2. 規則が増えたら、この文書に足す。

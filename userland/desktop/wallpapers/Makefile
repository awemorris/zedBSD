# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib

# The desktop's wallpapers, in every image with the desktop (wayland among ZEDBSD_USER_PROGRAMS; ws099-p019,
# 2026-10-05 user: "壁紙が1枚しか入っていない。前に生成した抽象的な壁紙も、とりあえず収録しましょう。"):
#   /usr/share/keiland/wallpaper.png       the default, the generated Dawn (Settings' first tile; 2026-10-08 user:
#                                          "デフォルトのデスクトップ背景を Dawn にしてください")
#   /usr/share/keiland/wallpapers/*.png    the others Settings > Wallpaper offers: the tree's Birch-Lake.png (the
#                                          default before 2026-10-08), the tree's Lakeside.png (the
#                                          compositor's own landscape) and the abstract gradients generate.py draws
#                                          (ws089-p009, PNG since ws138-p002; they are not kept in git, the build
#                                          makes them in $(BUILD)/wallpapers)
# A test's extra file of the same destination wins (the destination would otherwise be named twice).
# ZEDBSD_KEILAND_WALLPAPERS := n leaves them all out; := y puts them in an image without the desktop too.
# The Linux and FreeBSD builds install the same pictures (keiland-linux.mk, keiland-freebsd.mk).
ZEDBSD_KEILAND_WALLPAPERS ?= $(if $(filter wayland,$(ZEDBSD_USER_PROGRAMS)),y,n)
ZEDBSD_KEILAND_WALLPAPER_DEFAULT := Dawn
ZEDBSD_KEILAND_WALLPAPER_GENERATED := Aurora Dawn Lagoon Meadow Twilight
ZEDBSD_KEILAND_WALLPAPER_NAMES := $(filter-out $(ZEDBSD_KEILAND_WALLPAPER_DEFAULT),$(ZEDBSD_KEILAND_WALLPAPER_GENERATED))
ZEDBSD_KEILAND_WALLPAPER_FILES := $(addprefix $(BUILD)/wallpapers/,$(addsuffix .png,$(ZEDBSD_KEILAND_WALLPAPER_GENERATED)))
$(ZEDBSD_KEILAND_WALLPAPER_FILES) &: userland/desktop/wallpapers/generate.py
	$(PYTHON) userland/desktop/wallpapers/generate.py $(BUILD)/wallpapers >/dev/null

# The pictures as DESTINATION=SOURCE, and those the image takes: all of them, less any a test's extra files name.
ZEDBSD_KEILAND_WALLPAPER_DATA := \
	/usr/share/keiland/wallpaper.png=$(BUILD)/wallpapers/$(ZEDBSD_KEILAND_WALLPAPER_DEFAULT).png \
	/usr/share/keiland/wallpapers/Birch-Lake.png=userland/desktop/wallpapers/Birch-Lake.png \
	/usr/share/keiland/wallpapers/Lakeside.png=userland/desktop/wallpapers/Lakeside.png \
	$(foreach name,$(ZEDBSD_KEILAND_WALLPAPER_NAMES),/usr/share/keiland/wallpapers/$(name).png=$(BUILD)/wallpapers/$(name).png)
ZEDBSD_KEILAND_WALLPAPER_TAKEN := $(if $(filter y,$(ZEDBSD_KEILAND_WALLPAPERS)),$(foreach item,$(ZEDBSD_KEILAND_WALLPAPER_DATA),\
	$(if $(findstring $(firstword $(subst =, ,$(item)))=,$(ZEDBSD_TEST_EXTRA_FILES)),,$(item))))
ZEDBSD_PACKAGE_INPUTS += $(foreach item,$(ZEDBSD_KEILAND_WALLPAPER_TAKEN),$(lastword $(subst =, ,$(item))))
ZEDBSD_PACKAGE_FILES += $(foreach item,$(ZEDBSD_KEILAND_WALLPAPER_TAKEN),\
	--file $(item) --mode $(firstword $(subst =, ,$(item)))=0644)

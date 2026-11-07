# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

include $(dir $(lastword $(MAKEFILE_LIST)))../package.mk

KEILAND_WALLPAPERS          ?= $(if $(filter wayland,$(ZEDBSD_USER_PROGRAMS)),y,n)
KEILAND_WALLPAPER_DEFAULT   := Dawn
KEILAND_WALLPAPER_GENERATED := Aurora Dawn Lagoon Meadow Twilight
KEILAND_WALLPAPER_NAMES     := $(filter-out $(ZEDBSD_KEILAND_WALLPAPER_DEFAULT),$(ZEDBSD_KEILAND_WALLPAPER_GENERATED))
KEILAND_WALLPAPER_FILES     := $(addprefix $(BUILD)/wallpapers/,$(addsuffix .png,$(ZEDBSD_KEILAND_WALLPAPER_GENERATED)))

# The pictures as DESTINATION=SOURCE, and those the image takes: all
# of them, less any a test's extra files name.
KEILAND_WALLPAPER_DATA := \
	/usr/share/keiland/wallpaper.png=$(BUILD)/wallpapers/$(KEILAND_WALLPAPER_DEFAULT).png \
	/usr/share/keiland/wallpapers/Birch-Lake.png=userland/desktop/wallpapers/Birch-Lake.png \
	/usr/share/keiland/wallpapers/Lakeside.png=userland/desktop/wallpapers/Lakeside.png \
	$(foreach name,$(KEILAND_WALLPAPER_NAMES),/usr/share/keiland/wallpapers/$(name).png=$(BUILD)/wallpapers/$(name).png)

KEILAND_WALLPAPER_TAKEN := \
	$(if $(filter y,$(KEILAND_WALLPAPERS)), \
		$(foreach item,$(KEILAND_WALLPAPER_DATA),\
			$(if $(findstring $(firstword $(subst =, ,$(item)))=,$(TEST_EXTRA_FILES)), \
				,$(item) \
			) \
		) \
	)

PACKAGE_INPUTS += \
	$(foreach item,$(KEILAND_WALLPAPER_TAKEN), \
		$(lastword $(subst =, ,$(item))))

PACKAGE_FILES += \
	$(foreach item,$(KEILAND_WALLPAPER_TAKEN), \
		--file $(item) --mode $(firstword $(subst =, ,$(item)))=0644)

$(KEILAND_WALLPAPER_FILES) &: userland/desktop/wallpapers/generate.py
	$(PYTHON) userland/desktop/wallpapers/generate.py $(BUILD)/wallpapers >/dev/null

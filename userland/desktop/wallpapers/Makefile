# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

WALLPAPERS_DIR := $(dir $(lastword $(MAKEFILE_LIST)))
REPO_ROOT      ?= $(abspath $(WALLPAPERS_DIR)/../../..)
BUILD          ?= $(REPO_ROOT)/build/$(if $(PLATFORM),$(PLATFORM),x86_64)
PYTHON         ?= python3

KEILAND_WALLPAPERS          ?= $(if $(filter wayland,$(CONFIG_USERLAND_PROGRAMS) $(USERLAND_PROGRAMS) $(ZEDBSD_USER_PROGRAMS)),y,n)
KEILAND_WALLPAPER_DEFAULT   := Dawn
KEILAND_WALLPAPER_GENERATED := Aurora Dawn Lagoon Meadow Twilight
KEILAND_WALLPAPER_NAMES     := $(filter-out $(KEILAND_WALLPAPER_DEFAULT),$(KEILAND_WALLPAPER_GENERATED))
KEILAND_WALLPAPER_FILES     := $(addprefix $(BUILD)/wallpapers/,$(addsuffix .png,$(KEILAND_WALLPAPER_GENERATED)))

# The pictures as DESTINATION=SOURCE, and those the image takes: all
# of them, less any the root's extra files (ROOTFS_EXTRA_FILES) name.
KEILAND_WALLPAPER_DATA := \
	/usr/share/keiland/wallpaper.png=$(BUILD)/wallpapers/$(KEILAND_WALLPAPER_DEFAULT).png \
	/usr/share/keiland/wallpapers/Birch-Lake.png=$(WALLPAPERS_DIR)Birch-Lake.png \
	/usr/share/keiland/wallpapers/Lakeside.png=$(WALLPAPERS_DIR)Lakeside.png \
	$(foreach name,$(KEILAND_WALLPAPER_NAMES),/usr/share/keiland/wallpapers/$(name).png=$(BUILD)/wallpapers/$(name).png)

KEILAND_WALLPAPER_TAKEN := \
	$(if $(filter y,$(KEILAND_WALLPAPERS)), \
		$(foreach item,$(KEILAND_WALLPAPER_DATA),\
			$(if $(findstring $(firstword $(subst =, ,$(item)))=,$(ROOTFS_EXTRA_FILES)), \
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

ifndef TOPLEVEL_BUILD
.PHONY: all build
all: build
build: $(KEILAND_WALLPAPER_FILES)
endif

$(KEILAND_WALLPAPER_FILES) &: $(WALLPAPERS_DIR)generate.py
	@mkdir -p $(BUILD)/wallpapers
	$(PYTHON) $(WALLPAPERS_DIR)generate.py $(BUILD)/wallpapers >/dev/null

# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

override ARCHITECTURE             := i386
override BOARD                    := pc98

CONFIG_KERN_SMALL_MEMORY          := y

CONFIG_ROOTFS_DEVELOPMENT         := y

CONFIG_DRIVER_FS_UFS              := y
CONFIG_KERN_NET                   := y
CONFIG_KERN_NET_ETHERNET          := y
CONFIG_KERN_NET_IPV4              := y
CONFIG_DRIVER_HID                 := y
CONFIG_DRIVER_GRAPHICS            := y
CONFIG_DRIVER_GRAPHICS_PC98       := y

CONFIG_USERLAND_STATIC            := y
CONFIG_USERLAND_PROGRAMS          += \
	ca-certificates
CONFIG_USERLAND_SINGLEBINARY      += \
	awk \
	basename \
	batch \
	bc \
	cat \
	chmod \
	chown \
	cmp \
	compress \
	cp \
	curses \
	cut \
	date \
	dd \
	df \
	dhcpc \
	diff \
	dirname \
	dmesg \
	du \
	echo \
	env \
	etc \
	expr \
	false \
	fetch \
	find \
	grep \
	halt \
	head \
	host \
	hostname \
	ifconfig \
	install \
	join \
	kill \
	less \
	ln \
	login \
	ls \
	mkdir \
	mkfifo \
	mkfs \
	mkswap \
	mktemp \
	more \
	mv \
	nice \
	nohup \
	nslookup \
	ntpdate \
	od \
	passwd \
	ping \
	poweroff \
	printf \
	ps \
	pwd \
	readlink \
	realpath \
	reboot \
	renice \
	rm \
	rmdel \
	rmdir \
	route \
	sed \
	seq \
	shutdown \
	sleep \
	sort \
	split \
	ssh \
	stty \
	su \
	swapoff \
	swapon \
	sync \
	tail \
	tee \
	terminfo \
	test \
	time \
	top \
	touch \
	tr \
	true \
	tty \
	uname \
	uncompress \
	uniq \
	useradd \
	userdel \
	usermod \
	vi \
	wc \
	which \
	who \
	whoami \
	xargs \
	zcat

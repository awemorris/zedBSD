override ARCHITECTURE             := i386
override BOARD                    := pc98

CONFIG_ROOTFS_DEVELOPMENT         := y

CONFIG_DRIVER_LOOP                := y
CONFIG_DRIVER_FS_UFS              := y
CONFIG_DRIVER_FS_FAT              := y
CONFIG_DRIVER_FS_OVERLAYFS        := y
CONFIG_KERN_NET                   := y
CONFIG_KERN_NET_IPV4              := y
CONFIG_KERN_NET_IPV6              := y
CONFIG_KERN_NET_ETHERNET          := y
CONFIG_KERN_NET_WIFI              := y
CONFIG_DRIVER_HID                 := y
CONFIG_DRIVER_CPU_PERF            := y
CONFIG_DRIVER_GRAPHICS            := y
CONFIG_DRIVER_GRAPHICS_PC98       := y
CONFIG_DRIVER_LGY98               := y

CONFIG_USERLAND_PROGRAMS          += \
	admin \
	ar \
	at \
	awk \
	base64 \
	basename \
	batch \
	bc \
	blkid \
	bt \
	cal \
	cat \
	cflow \
	chgrp \
	chmod \
	chown \
	cksum \
	cmp \
	comm \
	compress \
	cp \
	crontab \
	csplit \
	curses \
	cut \
	cxref \
	date \
	dd \
	delta \
	df \
	dhcpc \
	diff \
	dirname \
	dmesg \
	du \
	echo \
	ed \
	egrep \
	env \
	etc \
	expand \
	expr \
	false \
	fetch \
	fgrep \
	file \
	find \
	fmt \
	fold \
	fuser \
	gencat \
	get \
	getconf \
	gettext \
	grep \
	halt \
	head \
	host \
	hostname \
	iconv \
	id \
	ifconfig \
	infocmp \
	install \
	ipcrm \
	ipcs \
	join \
	kill \
	less \
	libz-compat \
	link \
	ln \
	locale \
	localedef \
	logger \
	login \
	logname \
	lp \
	lpr \
	ls \
	m4 \
	mailx \
	make \
	mesg \
	mkdir \
	mkfifo \
	mkfs \
	mkswap \
	mktemp \
	more \
	msgfmt \
	mv \
	netclock \
	newgrp \
	ngettext \
	nice \
	nl \
	nm \
	nohup \
	nslookup \
	ntpdate \
	od \
	passwd \
	paste \
	patch \
	pathchk \
	pax \
	ping \
	poweroff \
	pr \
	printf \
	prs \
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
	sact \
	sccs \
	sed \
	seq \
	service \
	shutdown \
	sleep \
	sort \
	split \
	ssh \
	stat \
	strings \
	stty \
	su \
	sudo \
	swapoff \
	swapon \
	sync \
	tabs \
	tac \
	tail \
	talk \
	tee \
	terminfo-extra \
	terminfo \
	test \
	tic \
	time \
	timeout \
	top \
	touch \
	tput \
	tr \
	true \
	truncate \
	tsort \
	tty \
	uname \
	uncompress \
	unexpand \
	unget \
	uniq \
	unlink \
	useradd \
	userdel \
	usermod \
	uudecode \
	uuencode \
	val \
	vi \
	volumed \
	wc \
	what \
	which \
	who \
	whoami \
	write \
	xargs \
	zcat \
	ca-certificates

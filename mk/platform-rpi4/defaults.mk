override ARCHITECTURE             := arm64
override BOARD                    := rpi4

CONFIG_ROOTFS_DEVELOPMENT         := y
CONFIG_BOOT_SPLASH                := n
CONFIG_QUIET_BOOT                 := n
CONFIG_GRAPHICAL_LOGIN            := y

CONFIG_DRIVER_PCI                 := y
CONFIG_DRIVER_USB                 := y
CONFIG_DRIVER_LOOP                := y
CONFIG_DRIVER_FS_UFS              := y
CONFIG_DRIVER_FS_FAT              := y
CONFIG_DRIVER_FS_OVERLAYFS        := y
CONFIG_KERN_NET                   := y
CONFIG_KERN_NET_IPV4              := y
CONFIG_KERN_NET_IPV6              := y
CONFIG_KERN_NET_ETHERNET          := y
CONFIG_KERN_NET_WIFI              := y
CONFIG_DRIVER_USB_CDC_NCM         := y
CONFIG_DRIVER_USB_CDC_ECM         := y
CONFIG_DRIVER_USB_RTL815X         := y
CONFIG_DRIVER_RPI4_ETHERNET       := y
CONFIG_DRIVER_RPI4_WIFI           := y
CONFIG_DRIVER_USB_RTL8822BU       := y
CONFIG_DRIVER_GRAPHICS            := y
CONFIG_DRIVER_GRAPHICS_BOOTLFB    := y
CONFIG_DRIVER_GPU                 := y
CONFIG_DRIVER_RPI4_GPU            := y
CONFIG_GPU_JOB_RESERVATION_MS     := 10000
CONFIG_GPU_JOB_EXECUTION_MS       := 60000
CONFIG_GPU_JOB_STOP_MS            := 10000
CONFIG_GPU_CONTROL_MS             := 10000
CONFIG_DRIVER_AUDIO               := y
CONFIG_DRIVER_RPI4_AUDIO          := y
CONFIG_DRIVER_HID                 := y
CONFIG_DRIVER_I2C_HID             := y
CONFIG_DRIVER_USB                 := y
CONFIG_DRIVER_PCI_EHCI            := y
CONFIG_DRIVER_PCI_XHCI            := y
CONFIG_DRIVER_USB_HUB             := y
CONFIG_DRIVER_USB_HID             := y
CONFIG_DRIVER_USB_STORAGE         := y
CONFIG_DRIVER_USB_UASP            := y
CONFIG_DRIVER_USB_CCID            := y
CONFIG_DRIVER_USB_BT              := y
CONFIG_DRIVER_USB_UVC             := y
CONFIG_DRIVER_BT                  := y
CONFIG_DRIVER_INPUT_BRIDGE        := y
CONFIG_DRIVER_RPI4_BT             := y
CONFIG_DRIVER_CPU_PERF            := y
CONFIG_KERN_CPU_PERF              := y
CONFIG_DRIVER_RPI4_BT             := y
CONFIG_DRIVER_CAMERA              := y
CONFIG_DRIVER_I2C                 := y
CONFIG_DRIVER_GPIO                := y

CONFIG_USERLAND_PROGRAMS          += \
	account-admin \
	admin \
	ar \
	at \
	audiod \
	awk \
	base64 \
	basename \
	batch \
	bc \
	blkid \
	bluetoothd \
	bootlog \
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
	diskpart \
	dmesg \
	du \
	echo \
	ed \
	efiboot \
	egrep \
	emacs \
	env \
	etc \
	expand \
	expr \
	false \
	fetch \
	fgrep \
	fidoctl \
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
	lspci \
	lsusb \
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
	noct \
	nohup \
	nslookup \
	ntpdate \
	od \
	passkey-fido2 \
	passkey \
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
	wifi \
	write \
	xargs \
	zcat \
	browser \
	calendar \
	files \
	imageview \
	keiland-ime \
	ime-dict-ja \
	ime-dict-skk \
	libgl \
	libbrowser \
	libegl \
	libgif-compat \
	libglesv2 \
	libjpeg-compat \
	libkeiland \
	libmedia \
	libpdf \
	libpng-compat \
	libtruetype \
	libvulkan \
	libwayland-egl \
	libwayland-client \
	mailer \
	mediastorage \
	monitor \
	music \
	notes \
	pdfviewer \
	phone \
	photos \
	keiland-preview \
	keiland-printd \
	sessiond \
	settings \
	terminal \
	textedit \
	videoplayer \
	wayland \
	wallpapers \
	xserver \
	i915-firmware \
	intelax211-firmware \
	intelbt-firmware \
	nvidia-gsp-firmware \
	amd-navi-firmware \
	rtl8822b-firmware \
	bcm43455-firmware \
	brcmbt-firmware \
	hand-hershey \
	noto-color-emoji \
	ca-certificates \
	mview \
	zgears

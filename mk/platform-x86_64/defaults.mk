override CONFIG_ROOTFS_DEVELOPMENT         := y
override CONFIG_BOOT_SPLASH                := y
override CONFIG_GRAPHICAL_LOGIN            := y

override CONFIG_DRIVER_PCI                 := y
override CONFIG_DRIVER_USB                 := y
override CONFIG_DRIVER_PCI_NVME            := y
override CONFIG_DRIVER_PCI_AHCI            := y
override CONFIG_DRIVER_PCI_VMD             := y
override CONFIG_DRIVER_LOOP                := y
override CONFIG_DRIVER_FS_UFS              := y
override CONFIG_DRIVER_FS_FAT              := y
override CONFIG_DRIVER_FS_OVERLAYFS        := y
override CONFIG_KERN_NET                   := y
override CONFIG_KERN_NET_IPV4              := y
override CONFIG_KERN_NET_IPV6              := y
override CONFIG_KERN_NET_ETHERNET          := y
override CONFIG_KERN_NET_WIFI              := y
override CONFIG_DRIVER_USB_CDC_NCM         := y
override CONFIG_DRIVER_USB_CDC_ECM         := y
override CONFIG_DRIVER_PCI_RTL8125         := y
override CONFIG_DRIVER_USB_RTL815X         := y
override CONFIG_DRIVER_PCI_INTEL_AX211     := y
override CONFIG_DRIVER_USB_RTL8822BU       := y
override CONFIG_DRIVER_GRAPHICS            := y
override CONFIG_DRIVER_GRAPHICS_BOOTLFB    := y
override CONFIG_DRIVER_GRAPHICS_PCAT       := y
override CONFIG_DRIVER_GPU                 := y
override CONFIG_DRIVER_PCI_I915            := y
override CONFIG_DRIVER_PCI_NVRTX           := y
override CONFIG_DRIVER_PCI_AMDRDNA	   := y
override CONFIG_DRIVER_PCI_VENUS	   := y
override CONFIG_DRIVER_AUDIO               := y
override CONFIG_DRIVER_PCI_HDA             := y
override CONFIG_DRIVER_HID                 := y
override CONFIG_DRIVER_I2C_HID             := y
override CONFIG_DRIVER_USB                 := y
override CONFIG_DRIVER_PCI_OHCI            := y
override CONFIG_DRIVER_PCI_UHCI            := y
override CONFIG_DRIVER_PCI_EHCI            := y
override CONFIG_DRIVER_PCI_XHCI            := y
override CONFIG_DRIVER_TYPEC               := y
override CONFIG_DRIVER_USB_HUB             := y
override CONFIG_DRIVER_USB_HID             := y
override CONFIG_DRIVER_USB_STORAGE         := y
override CONFIG_DRIVER_USB_UASP            := y
override CONFIG_DRIVER_USB_CCID            := y
override CONFIG_DRIVER_USB_BT              := y
override CONFIG_DRIVER_USB_UVC             := y
override CONFIG_DRIVER_BT                  := y
override CONFIG_DRIVER_INPUT_BRIDGE        := y
override CONFIG_DRIVER_CPU_PERF            := y
override CONFIG_DRIVER_DELL_SMM_BOOST      := y
override CONFIG_DRIVER_CAMERA              := y
override CONFIG_DRIVER_ACPI                := y
override CONFIG_DRIVER_I2C                 := y
override CONFIG_DRIVER_GPIO                := y
override CONFIG_DRIVER_PCI_LPSS_I2C        := y
override CONFIG_DRIVER_PCI_LPSS_GPIO       := y
override CONFIG_DRIVER_PCI_AMD_GPIO        := y

override CONFIG_USERLAND_PROGRAMS := \
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
	wallpapers\
	xserver \
	i915-firmware \
	intelax211-firmware \
	intelbt-firmware \
	nvidia-gsp-firmware \
	amd-navi-firmware \
	rtl8822b-firmware \
	hand-hershey \
	noto-color-emoji \
	ca-certificates \
	mview \
	zgears

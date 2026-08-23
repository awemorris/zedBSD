/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */
#ifndef ZEDBSD_UAPI_TERMIOS_H
#define ZEDBSD_UAPI_TERMIOS_H

#include <stdint.h>
#include <sys/ioctl.h>

typedef uint32_t tcflag_t;
typedef uint8_t cc_t;
typedef uint32_t speed_t;

#define NCCS	20
struct termios {
	tcflag_t c_iflag;
	tcflag_t c_oflag;
	tcflag_t c_cflag;
	tcflag_t c_lflag;
	cc_t c_cc[NCCS];
	speed_t c_ispeed;
	speed_t c_ospeed;
};

struct winsize {
	uint16_t ws_row;
	uint16_t ws_col;
	uint16_t ws_xpixel;
	uint16_t ws_ypixel;
};

#define VINTR	0
#define VQUIT	1
#define VERASE	2
#define VKILL	3
#define VEOF	4
#define VTIME	5
#define VMIN	6
#define VSTART	7
#define VSTOP	8
#define VSUSP	9
#define VEOL	10
#define VWERASE	11
#define VLNEXT	12
#define VREPRINT	13

#define IGNBRK	0x00000001U
#define BRKINT	0x00000002U
#define ICRNL	0x00000004U
#define INLCR	0x00000008U
#define IGNCR	0x00000010U
#define IXON	0x00000020U
#define IXOFF	0x00000040U
#define ISTRIP	0x00000080U

#define OPOST	0x00000001U
#define ONLCR	0x00000002U
#define OCRNL	0x00000004U
#define ONOCR	0x00000008U
#define ONLRET	0x00000010U

#define CREAD	0x00000001U
#define CS8	0x00000002U
#define CLOCAL	0x00000004U
#define HUPCL	0x00000008U

#define ECHO	0x00000001U
#define ECHOE	0x00000002U
#define ECHOK	0x00000004U
#define ECHONL	0x00000008U
#define ICANON	0x00000010U
#define IEXTEN	0x00000020U
#define ISIG	0x00000040U
#define NOFLSH	0x00000080U
#define TOSTOP	0x00000100U
#define ECHOCTL	0x00000200U

#define B0	0U
#define B9600	9600U
#define B19200	19200U
#define B38400	38400U

#define TCSANOW	0
#define TCSADRAIN	1
#define TCSAFLUSH	2
#define TCIFLUSH	0
#define TCOFLUSH	1
#define TCIOFLUSH	2
#define TCOOFF	0
#define TCOON	1
#define TCIOFF	2
#define TCION	3

#define ZEDBSD_TTY_IOC_GROUP	't'
#define TCGETS	_IOR(ZEDBSD_TTY_IOC_GROUP, 1, struct termios)
#define TCSETS	_IOW(ZEDBSD_TTY_IOC_GROUP, 2, struct termios)
#define TCSETSW	_IOW(ZEDBSD_TTY_IOC_GROUP, 3, struct termios)
#define TCSETSF	_IOW(ZEDBSD_TTY_IOC_GROUP, 4, struct termios)
#define TIOCGWINSZ	_IOR(ZEDBSD_TTY_IOC_GROUP, 5, struct winsize)
#define TIOCSWINSZ	_IOW(ZEDBSD_TTY_IOC_GROUP, 6, struct winsize)
#define TIOCGPGRP	_IOR(ZEDBSD_TTY_IOC_GROUP, 7, int32_t)
#define TIOCSPGRP	_IOW(ZEDBSD_TTY_IOC_GROUP, 8, int32_t)
#define TIOCSCTTY	_IO(ZEDBSD_TTY_IOC_GROUP, 9)
#define TIOCNOTTY	_IO(ZEDBSD_TTY_IOC_GROUP, 10)
#define TIOCFLUSH	_IOW(ZEDBSD_TTY_IOC_GROUP, 11, int32_t)
#define TIOCGPTN	_IOR(ZEDBSD_TTY_IOC_GROUP, 12, uint32_t)
#define TIOCSPTLCK	_IOW(ZEDBSD_TTY_IOC_GROUP, 13, int32_t)
#define TIOCGSID	_IOR(ZEDBSD_TTY_IOC_GROUP, 14, int32_t)
#define TCSBRK	_IOW(ZEDBSD_TTY_IOC_GROUP, 15, int32_t)
#define TCXONC	_IOW(ZEDBSD_TTY_IOC_GROUP, 16, int32_t)
#define TIOCDRAIN	_IO(ZEDBSD_TTY_IOC_GROUP, 17)

#endif

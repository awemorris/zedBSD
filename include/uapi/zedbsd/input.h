/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_UAPI_INPUT_H
#define ZEDBSD_UAPI_INPUT_H

#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#define ZEDBSD_EV_VERSION 0x010001
#define EV_VERSION ZEDBSD_EV_VERSION
#define ZEDBSD_EVDEV_IOC_GROUP 'E'

struct input_event {
	struct timeval time;
	uint16_t type;
	uint16_t code;
	int32_t value;
};

#define input_event_sec time.tv_sec
#define input_event_usec time.tv_usec

struct input_id {
	uint16_t bustype;
	uint16_t vendor;
	uint16_t product;
	uint16_t version;
};

struct input_absinfo {
	int32_t value;
	int32_t minimum;
	int32_t maximum;
	int32_t fuzz;
	int32_t flat;
	int32_t resolution;
};

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_ABS 0x03
#define EV_MSC 0x04
#define EV_SW 0x05
#define EV_LED 0x11
#define EV_SND 0x12
#define EV_REP 0x14
#define EV_MAX 0x1f

#define SYN_REPORT 0
#define SYN_CONFIG 1
#define SYN_MT_REPORT 2
#define SYN_DROPPED 3

#define KEY_RESERVED 0
#define KEY_ESC 1
#define KEY_1 2
#define KEY_2 3
#define KEY_3 4
#define KEY_4 5
#define KEY_5 6
#define KEY_6 7
#define KEY_7 8
#define KEY_8 9
#define KEY_9 10
#define KEY_0 11
#define KEY_MINUS 12
#define KEY_EQUAL 13
#define KEY_BACKSPACE 14
#define KEY_TAB 15
#define KEY_Q 16
#define KEY_W 17
#define KEY_E 18
#define KEY_R 19
#define KEY_T 20
#define KEY_Y 21
#define KEY_U 22
#define KEY_I 23
#define KEY_O 24
#define KEY_P 25
#define KEY_LEFTBRACE 26
#define KEY_RIGHTBRACE 27
#define KEY_ENTER 28
#define KEY_LEFTCTRL 29
#define KEY_A 30
#define KEY_S 31
#define KEY_D 32
#define KEY_F 33
#define KEY_G 34
#define KEY_H 35
#define KEY_J 36
#define KEY_K 37
#define KEY_L 38
#define KEY_SEMICOLON 39
#define KEY_APOSTROPHE 40
#define KEY_GRAVE 41
#define KEY_LEFTSHIFT 42
#define KEY_BACKSLASH 43
#define KEY_Z 44
#define KEY_X 45
#define KEY_C 46
#define KEY_V 47
#define KEY_B 48
#define KEY_N 49
#define KEY_M 50
#define KEY_COMMA 51
#define KEY_DOT 52
#define KEY_SLASH 53
#define KEY_RIGHTSHIFT 54
#define KEY_LEFTALT 56
#define KEY_SPACE 57
#define KEY_CAPSLOCK 58
#define KEY_F1 59
#define KEY_F2 60
#define KEY_F3 61
#define KEY_F4 62
#define KEY_F5 63
#define KEY_F6 64
#define KEY_F7 65
#define KEY_F8 66
#define KEY_F9 67
#define KEY_F10 68
#define KEY_F11 87
#define KEY_F12 88
#define KEY_RIGHTCTRL 97
#define KEY_RIGHTALT 100
#define KEY_HOME 102
#define KEY_UP 103
#define KEY_PAGEUP 104
#define KEY_LEFT 105
#define KEY_RIGHT 106
#define KEY_END 107
#define KEY_DOWN 108
#define KEY_PAGEDOWN 109
#define KEY_INSERT 110
#define KEY_DELETE 111
#define KEY_MAX 0x2ff

#define BTN_MOUSE 0x110
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
#define BTN_SIDE 0x113
#define BTN_EXTRA 0x114

#define REL_X 0x00
#define REL_Y 0x01
#define REL_HWHEEL 0x06
#define REL_WHEEL 0x08
#define REL_MAX 0x0f

#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_MT_SLOT 0x2f
#define ABS_MT_POSITION_X 0x35
#define ABS_MT_POSITION_Y 0x36
#define ABS_MT_TRACKING_ID 0x39
#define ABS_MAX 0x3f

#define REP_DELAY 0x00
#define REP_PERIOD 0x01

#define INPUT_PROP_POINTER 0x00
#define INPUT_PROP_DIRECT 0x01
#define INPUT_PROP_MAX 0x1f

#define BUS_PCI 0x01
#define BUS_USB 0x03
#define BUS_BLUETOOTH 0x05
#define BUS_HOST 0x19

#define EVIOCGVERSION _IOR(ZEDBSD_EVDEV_IOC_GROUP, 0x01, int)
#define EVIOCGID _IOR(ZEDBSD_EVDEV_IOC_GROUP, 0x02, struct input_id)
#define EVIOCGREP _IOR(ZEDBSD_EVDEV_IOC_GROUP, 0x03, unsigned int[2])
#define EVIOCSREP _IOW(ZEDBSD_EVDEV_IOC_GROUP, 0x03, unsigned int[2])
#define EVIOCGNAME(length)                                                     \
	ZEDBSD_IOC(ZEDBSD_IOC_OUT, ZEDBSD_EVDEV_IOC_GROUP, 0x06, (length))
#define EVIOCGPHYS(length)                                                     \
	ZEDBSD_IOC(ZEDBSD_IOC_OUT, ZEDBSD_EVDEV_IOC_GROUP, 0x07, (length))
#define EVIOCGUNIQ(length)                                                     \
	ZEDBSD_IOC(ZEDBSD_IOC_OUT, ZEDBSD_EVDEV_IOC_GROUP, 0x08, (length))
#define EVIOCGPROP(length)                                                     \
	ZEDBSD_IOC(ZEDBSD_IOC_OUT, ZEDBSD_EVDEV_IOC_GROUP, 0x09, (length))
#define EVIOCGKEY(length)                                                      \
	ZEDBSD_IOC(ZEDBSD_IOC_OUT, ZEDBSD_EVDEV_IOC_GROUP, 0x18, (length))
#define EVIOCGLED(length)                                                      \
	ZEDBSD_IOC(ZEDBSD_IOC_OUT, ZEDBSD_EVDEV_IOC_GROUP, 0x19, (length))
#define EVIOCGBIT(event_type, length)                                          \
	ZEDBSD_IOC(ZEDBSD_IOC_OUT, ZEDBSD_EVDEV_IOC_GROUP,                     \
		   0x20 + (event_type), (length))
#define EVIOCGABS(axis)                                                        \
	_IOR(ZEDBSD_EVDEV_IOC_GROUP, 0x40 + (axis), struct input_absinfo)
#define EVIOCGRAB _IOW(ZEDBSD_EVDEV_IOC_GROUP, 0x90, int)

#endif

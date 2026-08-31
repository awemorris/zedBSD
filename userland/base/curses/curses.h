/* -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*- */

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Declares the zedBSD userland curses interface.
 */

#ifndef ZEDBSD_CURSES_H
#define ZEDBSD_CURSES_H

typedef unsigned long chtype;
typedef struct _zed_window WINDOW;

struct _zed_window {
	int cursor_y;
	int cursor_x;
	int max_y;
	int max_x;
};

#define OK 0
#define ERR (-1)
#define TRUE 1
#define FALSE 0

#define A_NORMAL 0UL
#define A_BOLD (1UL << 16)
#define A_REVERSE (1UL << 17)
#define A_CHARTEXT 0xffUL

extern WINDOW *stdscr;
extern int LINES;
extern int COLS;
extern int COLORS;
extern int COLOR_PAIRS;

WINDOW *initscr(void);
int endwin(void);
int refresh(void);
int wrefresh(WINDOW *);
int clear(void);
int erase(void);
int move(int, int);
int wmove(WINDOW *, int, int);
int addch(const chtype);
int waddch(WINDOW *, const chtype);
int addstr(const char *);
int waddstr(WINDOW *, const char *);
int getch(void);
int wgetch(WINDOW *);
int cbreak(void);
int nocbreak(void);
int echo(void);
int noecho(void);
int keypad(WINDOW *, int);
int curs_set(int);
int attron(int);
int attroff(int);
int attrset(int);
int start_color(void);
int has_colors(void);
int init_pair(short, short, short);
int setupterm(const char *, int, int *);
int tigetflag(const char *);
int tigetnum(const char *);
char *tigetstr(const char *);
int putp(const char *);

#endif

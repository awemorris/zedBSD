/*
 * WS307 p004: a small GTK 4 application built on an installed zedBSD with
 * its own clang and pkg-config (plan/ws307/tests/p004-guest-run.sh):
 *
 *     cc -o p004-hello p004-hello.c $(pkg-config --cflags --libs gtk4)
 *
 * It shows a window with a label, says "P004 MAPPED" when the window is on
 * the screen, and ends by itself after HELLO_SECONDS with "P004 QUIT".
 *
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */

#include <gtk/gtk.h>
#include <stdio.h>

/* How long the window stays before the application ends (seconds). */
#define HELLO_SECONDS	20U

static void hello_activate(GtkApplication *application, gpointer data);
static void hello_mapped(GtkWidget *widget, gpointer data);
static gboolean hello_quit(gpointer data);

int
main(
	int argc,
	char *argv[])
{
	GtkApplication *application;
	int status;

	/* One instance of its own: no other process is asked to show it. */
	application = gtk_application_new("org.zedbsd.P004Hello", G_APPLICATION_NON_UNIQUE);
	g_signal_connect(application, "activate", G_CALLBACK(hello_activate), NULL);

	/* Runs until the timer ends it. */
	status = g_application_run(G_APPLICATION(application), argc, argv);
	g_object_unref(application);
	printf("P004 QUIT status=%d\n", status);
	fflush(stdout);
	return status;
}

/* Makes the window with its label, and the timer that ends the application. */
static void
hello_activate(
	GtkApplication *application,
	gpointer data)
{
	GtkWidget *window;
	GtkWidget *label;

	(void)data;

	/* The window and its label. */
	window = gtk_application_window_new(application);
	gtk_window_set_title(GTK_WINDOW(window), "p004 hello");
	gtk_window_set_default_size(GTK_WINDOW(window), 360, 160);
	label = gtk_label_new("Built on zedBSD with clang and pkg-config");
	gtk_window_set_child(GTK_WINDOW(window), label);

	/* Shows it, and says so once it is on the screen. */
	g_signal_connect(window, "map", G_CALLBACK(hello_mapped), NULL);
	gtk_window_present(GTK_WINDOW(window));

	/* Ends the application after a while. */
	g_timeout_add_seconds(HELLO_SECONDS, hello_quit, application);
}

/* Says that the window is on the screen. */
static void
hello_mapped(
	GtkWidget *widget,
	gpointer data)
{
	(void)widget;
	(void)data;

	/* The line the test waits for, with the GTK the program runs on. */
	printf("P004 MAPPED gtk=%u.%u.%u\n", gtk_get_major_version(), gtk_get_minor_version(),
	    gtk_get_micro_version());
	fflush(stdout);
}

/* Ends the application once. */
static gboolean
hello_quit(
	gpointer data)
{
	/* Ends the main loop, and with it the application. */
	g_application_quit(G_APPLICATION(data));

	/* The timer runs once. */
	return G_SOURCE_REMOVE;
}

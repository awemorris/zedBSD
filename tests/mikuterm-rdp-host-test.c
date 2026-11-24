/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Host unit tests for mikuterm's RDP configuration, terminal_view_ops tab,
 * and librdp + h264sw decoder integration.
 */

#include "apps/mikuterm-common/mikuterm.h"
#include "apps/mikuterm-common/rdp-decoder.h"
#include "apps/mikuterm-common/rdp-hosts.h"
#include "apps/mikuterm-common/rdp-tab.h"
#include "userland/desktop/librdp/rdp.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_rdp_hosts_and_conf(void);
static void test_rdp_tab_view(void);
static void test_rdp_h264sw_decoder(void);

/* Stub for kl_app_post in host unit test without full display loop. */
int
kl_app_post(
	struct kl_app *app,
	uint32_t code,
	void *data)
{
	(void)app;
	(void)code;
	(void)data;
	return 0;
}

/*
 * Runs all mikuterm RDP host tests.
 */
int
main(
	void)
{
	test_rdp_hosts_and_conf();
	test_rdp_tab_view();
	test_rdp_h264sw_decoder();

	printf("mikuterm-rdp-host-test: all tests passed\n");
	return 0;
}

/* Tests rdp/hosts parsing/formatting and mikuterm.conf RdpPort/RdpResize. */
static void
test_rdp_hosts_and_conf(
	void)
{
	static const char hosts_txt[] =
		"# Sample rdp/hosts\n"
		"Host workstation\n"
		"    HostName 10.0.2.2\n"
		"    Port 13389\n"
		"    User awe\n"
		"    Domain ZED\n"
		"    Width 1920\n"
		"    Height 1080\n";
	static const char conf_txt[] =
		"RdpPort 3390\n"
		"RdpResize no\n";
	struct rdp_host_list list;
	struct rdp_host_entry target;
	struct mikuterm_conf conf;
	char *formatted;
	size_t formatted_sz;

	assert(rdp_hosts_parse(hosts_txt, sizeof(hosts_txt) - 1U, 3389U, &list) == 0);
	assert(list.count == 1U);
	assert(strcmp(list.entries[0].name, "workstation") == 0);
	assert(strcmp(list.entries[0].host_name, "10.0.2.2") == 0);
	assert(list.entries[0].port == 13389U);
	assert(strcmp(list.entries[0].user, "awe") == 0);
	assert(strcmp(list.entries[0].domain, "ZED") == 0);
	assert(list.entries[0].width == 1920U);
	assert(list.entries[0].height == 1080U);

	formatted = NULL;
	formatted_sz = 0U;
	assert(rdp_hosts_format(&list, &formatted, &formatted_sz) == 0);
	assert(formatted != NULL && formatted_sz > 0U);
	assert(rdp_hosts_parse(formatted, formatted_sz, 3389U, &list) == 0);
	assert(list.count == 1U);
	assert(list.entries[0].port == 13389U);
	free(formatted);

	assert(rdp_hosts_parse_target("rdp://ZED\\alice@192.168.1.10:3391",
				      3389U, &target) == 0);
	assert(strcmp(target.domain, "ZED") == 0);
	assert(strcmp(target.user, "alice") == 0);
	assert(strcmp(target.host_name, "192.168.1.10") == 0);
	assert(target.port == 3391U);

	mikuterm_conf_defaults(&conf);
	assert(conf.rdp_port == 3389U);
	assert(conf.rdp_resize == 1);
	assert(mikuterm_conf_read(&conf, conf_txt, sizeof(conf_txt) - 1U, 0) == 0);
	assert(conf.rdp_port == 3390U);
	assert(conf.rdp_resize == 0);
}

/* Tests terminal_view_ops draw, title, input, resize, and close on rdp_tab. */
static void
test_rdp_tab_view(
	void)
{
	struct rdp_tab *tab;
	uint32_t frame[4 * 4];
	uint32_t canvas[8 * 8];
	char title[64];
	struct terminal_view_state vst;
	struct terminal_picture_event ev;
	struct rdp_tab_input in;
	uint32_t rw;
	uint32_t rh;
	size_t i;

	assert(rdp_tab_create("10.0.2.2", 4U, 4U, 1, NULL, 1U, &tab) == 0);
	assert(tab->connection.ops->view != NULL);

	tab->connection.ops->view->title(&tab->connection, title, sizeof(title));
	assert(strcmp(title, "RDP: 10.0.2.2") == 0);

	for (i = 0U; i < 16U; i++)
		frame[i] = 0xFF112233U;

	assert(rdp_tab_put_frame(tab, frame, 4U, 4U) == 0);
	memset(&vst, 0, sizeof(vst));
	assert(tab->connection.ops->view->draw(&tab->connection, &vst, canvas,
					       8U, 8U) == 0);
	assert(canvas[0] == 0xFF112233U);
	assert(canvas[7 * 8 + 7] == 0xFF181C20U);

	/* Tests keyboard, text, and mouse input translation. */
	memset(&ev, 0, sizeof(ev));
	ev.kind = TERMINAL_PICTURE_KEY;
	ev.code = 30U;
	ev.pressed = 1;
	tab->connection.ops->view->input(&tab->connection, &ev);
	assert(rdp_tab_take_input(tab, &in) == 1);
	assert(in.kind == RDP_TAB_IN_KEY && in.code == 30U && in.flags == 0U);

	memset(&ev, 0, sizeof(ev));
	ev.kind = TERMINAL_PICTURE_BUTTON;
	ev.code = 0x110U;
	ev.pressed = 1;
	ev.x = 120;
	ev.y = 240;
	tab->connection.ops->view->input(&tab->connection, &ev);
	assert(rdp_tab_take_input(tab, &in) == 1);
	assert(in.kind == RDP_TAB_IN_MOUSE && in.x == 120U && in.y == 240U);

	/* Tests resize queueing. */
	assert(tab->connection.ops->resize(&tab->connection, 80U, 24U, 800U,
					   600U) == 0);
	assert(rdp_tab_take_resize(tab, &rw, &rh) == 1);
	assert(rw == 800U && rh == 600U);

	tab->connection.ops->close(&tab->connection);
}

/* Tests h264sw decoder ops creation, invalid input handling, and destruction. */
static void
test_rdp_h264sw_decoder(
	void)
{
	static const uint8_t sps_pps[] = {
		0x00U, 0x00U, 0x00U, 0x01U, 0x67U, 0x42U, 0xC0U, 0x1EU,
		0xD9U, 0x00U, 0xA0U, 0x47U, 0xFEU, 0x88U,
		0x00U, 0x00U, 0x00U, 0x01U, 0x68U, 0xCEU, 0x38U, 0x80U
	};
	void *ctx;
	uint32_t pixels[64 * 64];
	uint32_t dw;
	uint32_t dh;
	int rc;

	ctx = NULL;
	assert(rdp_h264sw_decoder_ops.create(&ctx) == 0);
	assert(ctx != NULL);

	dw = 0U;
	dh = 0U;
	rc = rdp_h264sw_decoder_ops.decode(ctx, sps_pps, sizeof(sps_pps),
					   pixels, 64U, 64U, &dw, &dh);
	assert(rc == 0 || rc == 11);

	rdp_h264sw_decoder_ops.destroy(ctx);
}

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

#include "userland/desktop/phone/phone.h"
#include "userland/desktop/libkeiland/ui/chooser.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Checks path-based selection and per-contact drafts without connecting or sending messages.
 */
int
main(
	int argc,
	char **argv)
{
	struct ph_view view;
	struct keiui_chooser chooser;
	struct kl_file_chooser_options options;
	FILE *snapshot;
	FILE *image;
	char photo[4096];
	char uri[8192];
	char bad[3] = {'a', 0, 'b'};
	int error;
	int same;
	int present;
	unsigned index;
	char contact[32];

	/* A private fixture supplies original files and a versioned metadata response. */
	assert(argc == 2);
	snprintf(photo, sizeof(photo), "%s/photo space.png", argv[1]);
	image = fopen(photo, "wb");
	assert(image != NULL);
	fwrite("\x89PNG\r\n\x1a\n", 1U, 8U, image);
	fclose(image);
	memset(&view, 0, sizeof(view));
	memset(&options, 0, sizeof(options));
	options.mode = KL_FILE_CHOOSER_MEDIA;
	error = keiui_chooser_init(&chooser, &options);
	assert(error == 0 && chooser.count == 0U && chooser.place_count == 1U);
	error = keiui_chooser_can_go_up(&chooser);
	assert(error == 0);
	keiui_chooser_open_path(&chooser);
	assert(!chooser.typing_path);
	error = keiui_chooser_go(&chooser, "/");
	assert(error == ENOTSUP);

	/* The chooser lists metadata and returns the original path without reading image bytes. */
	snapshot = tmpfile();
	assert(snapshot != NULL);
	fprintf(snapshot, "# keiland-media 1\t%s\nP\t%s\t%s\t%s\t8\t10\t20\t0\t0\t0\t0\tphoto space.png\n", argv[1], "0123456789abcdef0123456789abcdef", photo, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
	fflush(snapshot);
	fseek(snapshot, 0L, SEEK_SET);
	error = keiui_chooser_media(&chooser, fileno(snapshot));
	assert(error == 0 && chooser.count == 1U);
	keiui_chooser_select(&chooser, 0L);
	keiui_chooser_accept(&chooser);
	assert(chooser.answered && chooser.result == KL_FILE_CHOOSER_CHOSEN);
	same = strcmp(chooser.answer, photo);
	assert(same == 0);

	/* Selection is copied into a draft that stays with its original contact. */
	error = ph_draft_add(&view, chooser.answer, "contact-a", 0);
	assert(error == 0);
	keiui_chooser_fini(&chooser);
	fclose(snapshot);
	error = ph_draft_add(&view, photo, "contact-a", 0);
	assert(error == 0 && view.attachment_count == 1U);
	error = ph_draft_add(&view, photo, "contact-b", 0);
	assert(error == 0 && view.attachment_count == 2U);
	assert(ph_draft_count(&view, "contact-a") == 1U);
	assert(ph_draft_count(&view, "contact-b") == 1U);

	/* Encoded URI paths and local-host names are accepted; foreign hosts are rejected. */
	snprintf(uri, sizeof(uri), "# comment\r\nfile://%s/photo%%20space.png\r\n", argv[1]);
	error = ph_draft_uris(&view, uri, strlen(uri), "contact-a");
	assert(error == 0 && view.attachment_count == 2U);
	error = ph_draft_uris(&view, "file://remote/path.png\n", 23U, "contact-a");
	assert(error != 0);

	/* Dropped text appends and remains editable; binary and excessive text cannot truncate a draft. */
	kl_field_set(&view.message, "before ");
	error = ph_draft_text(&view, "日本語", strlen("日本語"));
	assert(error == 0);
	same = strcmp(view.message.text, "before 日本語");
	assert(same == 0);
	error = ph_draft_text(&view, bad, sizeof(bad));
	assert(error == EINVAL);
	kl_field_set_limit(&view.message, view.message.length);
	error = ph_draft_text(&view, "x", 1U);
	assert(error == E2BIG);

	/* A full draft still accepts reselecting an existing attachment without duplicating it. */
	for (index = 2U; index < PH_DRAFT_MAX; index++) {
		snprintf(contact, sizeof(contact), "contact-%u", index);
		error = ph_draft_add(&view, photo, contact, 0);
		assert(error == 0);
	}

	/* Capacity applies only to genuinely new draft entries. */
	error = ph_draft_add(&view, photo, "contact-a", 0);
	assert(error == 0 && view.attachment_count == PH_DRAFT_MAX);
	error = ph_draft_add(&view, photo, "another-contact", 0);
	assert(error == E2BIG);

	/* Removing a normal path never removes the original; an owned temporary drop is cleaned. */
	ph_draft_remove(&view, 0U);
	present = access(photo, F_OK);
	assert(present == 0);
	ph_draft_release(&view);
	error = ph_draft_add(&view, photo, "contact-a", 1);
	assert(error == 0);
	ph_draft_release(&view);
	present = access(photo, F_OK);
	assert(present != 0);
	printf("PASS media selection: compositor metadata model, chosen path, contact-bound drafts, dedup, URI/text drops, owned cleanup\n");

	/* Succeeded: no transport or message-store operation was called. */
	/* Succeeded: this fixture operation completed. */
	return 0;
}

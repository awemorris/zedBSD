/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The release the helper installs from: where its description is asked
 * for, and the one asset of it that holds the system.
 *
 * The description is the answer of GitHub's REST API for releases
 * (https://docs.github.com/en/rest/releases/releases): a list of them
 * ("list releases", the newest first), or one.  The releases offered are
 * the published ones: not a draft, not a pre-release (the project's
 * nightly builds are pre-releases), with a tag the helper can pass as a
 * word, and with an asset of the expected name that carries the SHA-256
 * GitHub computed when it was uploaded ("digest": "sha256:HEX") and an
 * https address to download it from (browser_download_url).  A release
 * whose asset has no digest is not offered: the system is never installed
 * unchecked.
 *
 * The place of the description and an extra trusted CA come from a file
 * of root's (RELEASE_CONFIG), a line a setting, "KEY VALUE":
 *
 *   api URL         the API's address (https)
 *   asset NAME      the asset's name
 *   ca-file PATH    a CA file trusted besides the system's roots
 *
 * Without the file the defaults hold: the project's own releases.
 */

#ifndef USERLAND_DESKTOP_INSTALLER_ADMIN_RELEASE_H
#define USERLAND_DESKTOP_INSTALLER_ADMIN_RELEASE_H

#include <stddef.h>
#include <stdint.h>

/* The file of the settings, and its largest size. */
#ifndef RELEASE_CONFIG
#define RELEASE_CONFIG		"/etc/installer-helper.conf"
#endif
#define RELEASE_CONFIG_MAX	4096U

/* The project's releases, newest first, as the API lists them (pre-releases among them). */
#define RELEASE_DEFAULT_API	"https://api.github.com/repos/awemorris/zedBSD/releases?per_page=100"

/* The most releases offered. */
#define RELEASE_LIST_MAX	32U

/* The longest address (fetch's own limit, with its NUL), path, tag and name. */
#define RELEASE_URL_MAX		1536U
#define RELEASE_PATH_MAX	256U
#define RELEASE_TAG_MAX		128U
#define RELEASE_NAME_MAX	128U

/* The largest description the helper reads, and the largest asset it takes. */
#define RELEASE_TEXT_MAX	(4U * 1024U * 1024U)
#define RELEASE_ASSET_MAX	(64ULL * 1024ULL * 1024ULL * 1024ULL)

/* Where to ask, which asset, and what else to trust. */
struct release_config {
	char api[RELEASE_URL_MAX];
	char asset[RELEASE_NAME_MAX];
	char ca_file[RELEASE_PATH_MAX];
};

/* A release's asset: the release's tag and publication date, the asset's name, where to get it, its size and its SHA-256. */
struct release_asset {
	char tag[RELEASE_TAG_MAX];
	char published[32];
	char name[RELEASE_NAME_MAX];
	char url[RELEASE_URL_MAX];
	uint64_t size;
	uint8_t sha256[32];
};

void release_config_default(struct release_config *config);
int release_config_parse(const char *text, size_t length, struct release_config *config);
int release_url_valid(const char *url);
int release_tag_valid(const char *tag);
int release_list(const char *text, size_t length, const struct release_config *config, struct release_asset *list, unsigned capacity,
		 unsigned *count);
int release_choose(const char *text, size_t length, const struct release_config *config, const char *tag, struct release_asset *asset);

#endif

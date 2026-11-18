/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The places of the sidebar and the names of places.
 *
 * Favorites are Today (the dashboard), the home folder and the user's
 * favorite folders (by default the usual ones under the home folder);
 * Devices are the removable devices the desktop tells (ws132-p005), mounted
 * or not; Locations are the recent files, the trash and the computer's
 * root.  A favorite whose folder does not exist is kept (pale), so the
 * sidebar keeps its shape on a new account.
 */

#include "files.h"
#include "mounts.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*
 * One of the usual folders under the home folder.
 */
struct places_folder {
	const char *label;
	const char *name;
	unsigned icon;
};

/* The usual folders, in the sidebar's order. */
static const struct places_folder places_folders[] = {
    {"Desktop", "Desktop", KL_ICON_DESKTOP},
    {"Documents", "Documents", KL_ICON_DOCUMENTS},
    {"Downloads", "Downloads", KL_ICON_DOWNLOADS},
    {"Pictures", "Pictures", KL_ICON_PICTURES},
    {"Music", "Music", KL_ICON_MUSIC},
    {"Movies", "Movies", KL_ICON_MOVIES}};

/*
 * The file systems whose mounts are not shown as places: the desktop tells
 * every file system with files (ws188-p004), and memory, overlays and
 * images are no volume of the user's to show (a tmpfs still keeps its
 * Trash, trash.c).
 */
static const char *const places_hidden_types[] = {
    "tmpfs", "devfs", "proc", "procfs", "sysfs", "devpts", "kernfs", "fdesc", "swap", "bind",
    "cgroup", "cgroup2", "efivarfs", "securityfs", "pstore", "bpf", "tracefs", "debugfs", "mqueue",
    "hugetlbfs", "fusectl", "configfs", "autofs", "binfmt_misc", "nsfs", "rpc_pipefs", "overlay", "squashfs"};

/* The folders whose mounts belong to the system rather than to the user. */
static const char *const places_system_folders[] = {
    "/sys", "/proc", "/dev", "/run", "/boot", "/snap", "/var/lib"};

static struct fm_place *places_add(struct fm_places *places, unsigned section, unsigned icon, const char *label, unsigned kind, const char *path);
static void places_favorites(struct fm_places *places, const char *home);
static void places_add_folder(struct fm_places *places, const char *path, const char *home);
static void places_devices(struct fm_places *places);
static void places_mounts(struct fm_places *places);
static int places_device_path(const struct fm_places *places, const char *path);
static int places_file(char *path, size_t size);
static int places_write(struct fm_places *places, int moved, int to, int removed, const char *added);

/*
 * Fills the sidebar: Favorites (Today, the home folder, and the user's
 * folders or the usual ones) and Locations (recent files, the trash, the
 * computer and mounted volumes).
 */
void
fm_places_init(
	struct fm_places *places,
	const char *home)
{
	struct fm_device devices[FM_DEVICES_MAX];
	struct fm_place *place;
	int device_count;

	/* The sidebar starts empty, but for the removable devices, which only the desktop's news changes. */
	device_count = places->device_count;
	if (device_count < 0 || device_count > FM_DEVICES_MAX)
		device_count = 0;
	memcpy(devices, places->devices, (size_t)device_count * sizeof(devices[0]));
	memset(places, 0, sizeof(*places));
	memcpy(places->devices, devices, (size_t)device_count * sizeof(devices[0]));
	places->device_count = device_count;

	/*
	 * Favorites: Today (the dashboard) at the top and the home folder under
	 * it (ws127-p011, the user's decision of 2026-10-05), fixed, then the
	 * user's folders.
	 */
	place = places_add(places, FM_SECTION_FAVORITES, KL_ICON_TODAY, "Today", FM_LOCATION_TODAY, home);
	if (place != NULL)
		place->fixed = 1;
	place = places_add(places, FM_SECTION_FAVORITES, KL_ICON_HOME, "Home", FM_LOCATION_FOLDER, home);
	if (place != NULL)
		place->fixed = 1;
	places_favorites(places, home);

	/* Devices: the removable devices (ws132-p005). */
	places_devices(places);

	/* Locations: the recent files, the trash, the computer's root and the volumes. */
	(void)places_add(places, FM_SECTION_LOCATIONS, KL_ICON_RECENTS, "Recents", FM_LOCATION_RECENTS, "");
	(void)places_add(places, FM_SECTION_LOCATIONS, KL_ICON_TRASH, "Trash", FM_LOCATION_TRASH, "");
	(void)places_add(places, FM_SECTION_LOCATIONS, KL_ICON_COMPUTER, "Computer", FM_LOCATION_FOLDER, "/");
	places_mounts(places);
}

/*
 * Adds a folder to the Favorites (after the others) and keeps the list;
 * a folder already there is not added again.  Returns 0, EEXIST or an
 * errno value.
 */
int
fm_places_add_favorite(
	struct fm_places *places,
	const char *path)
{
	int index;
	int match;
	int error;

	/* Refuses a folder that already belongs to the existing Favorites list. */
	for (index = 0; index < places->count; index++) {
		/* Only persisted favorite folders participate in duplicate detection. */
		if (places->items[index].section != FM_SECTION_FAVORITES || places->items[index].location.kind != FM_LOCATION_FOLDER)
			continue;

		/* The same path cannot occupy two persisted sidebar positions. */
		match = strcmp(places->items[index].location.path, path);
		if (match == 0)
			return EEXIST;
	}

	/* Writes the existing folders followed by the newly requested favorite. */
	error = places_write(places, -1, -1, -1, path);
	if (error != 0)
		return error;

	/* Succeeded: the caller may refill the sidebar from its persisted list. */
	return 0;
}

/*
 * Removes one persisted favorite folder while retaining all other sidebar positions.
 */
int
fm_places_remove_favorite(
	struct fm_places *places,
	int removed)
{
	int error;
	int favorite;

	/* A missing sidebar index cannot identify a persisted favorite. */
	if (removed < 0 || removed >= places->count)
		return EINVAL;

	/* Today, Home and the locations do not belong to the persisted favorite-folder list. */
	favorite = fm_place_is_favorite_folder(&places->items[removed]);
	if (favorite == 0)
		return EINVAL;

	/* Writes the list while omitting only the selected favorite-folder position. */
	error = places_write(places, -1, -1, removed, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the caller may refill the sidebar without the removed favorite. */
	return 0;
}

/*
 * Persists the same before/after ordering used when a favorite is dropped on another favorite.
 */
int
fm_places_move_favorite(
	struct fm_places *places,
	int moved,
	int to)
{
	int ends[2];
	int end;
	int error;
	int favorite;

	/* Both dragged endpoints must identify persisted favorite folders rather than Home or locations. */
	ends[0] = moved;
	ends[1] = to;
	for (end = 0; end < 2; end++) {
		/* Rejects an endpoint outside the current sidebar generation. */
		if (ends[end] < 0 || ends[end] >= places->count)
			return EINVAL;

		/* Only favorite folders participate in persisted ordering. */
		favorite = fm_place_is_favorite_folder(&places->items[ends[end]]);
		if (favorite == 0)
			return EINVAL;
	}

	/* Writes the moved folder before an earlier target or after a later target, as before. */
	error = places_write(places, moved, to, -1, NULL);
	if (error != 0)
		return error;

	/* Succeeded: the caller may refill the sidebar in its persisted order. */
	return 0;
}

/*
 * Reports the name a place is shown by: the dashboard is Today, the home
 * folder is Home, the root is Computer, and a folder is its last part.
 */
const char *
fm_location_name(
	const struct fm_location *location,
	const char *home)
{
	const char *slash;
	int match;

	/* Places that are not folders have names of their own. */
	switch (location->kind) {
	case FM_LOCATION_TODAY:
		/* The dashboard has its own stable sidebar name (ws127-p011). */
		return "Today";
	case FM_LOCATION_RECENTS:
		/* The recent-file query is a named virtual location. */
		return "Recents";
	case FM_LOCATION_TRASH:
		/* The trash query is a named virtual location. */
		return "Trash";
	case FM_LOCATION_SEARCH:
		/* The search query has a stable virtual-location name. */
		return "Search";
	default:
		break;
	}

	/* The home folder and the root. */
	match = strcmp(location->path, home);
	if (match == 0)
		return "Home";

	/* The root represents the computer rather than its empty path component. */
	match = strcmp(location->path, "/");
	if (match == 0)
		return "Computer";

	/* A folder is named by the last part of its path. */
	slash = strrchr(location->path, '/');
	if (slash == NULL || slash[1] == '\0')
		return location->path;

	/* Succeeded: the displayed folder name is its final path component. */
	return slash + 1;
}

/*
 * Tells whether a place of the sidebar is one of the user's favorite
 * folders, which the list of favorites holds (Today and Home are fixed).
 */
int
fm_place_is_favorite_folder(
	const struct fm_place *place)
{
	/* Another section, a place that is no folder, or a fixed one. */
	if (place->section != FM_SECTION_FAVORITES)
		return 0;
	if (place->location.kind != FM_LOCATION_FOLDER)
		return 0;
	if (place->fixed != 0)
		return 0;

	/* Succeeded: a favorite folder of the user's list. */
	return 1;
}

/* Adds a place to a section of the sidebar; NULL when the sidebar is full. */
static struct fm_place *
places_add(
	struct fm_places *places,
	unsigned section,
	unsigned icon,
	const char *label,
	unsigned kind,
	const char *path)
{
	struct fm_place *place;

	/* The sidebar holds a fixed number of places. */
	if (places->count == FM_PLACES)
		return NULL;

	/* The place, after the others. */
	place = &places->items[places->count];
	memset(place, 0, sizeof(*place));
	place->section = section;
	place->icon = icon;

	/* Fixed label capacity deliberately preserves the existing bounded sidebar truncation. */
	(void)snprintf(place->label, sizeof(place->label), "%s", label);
	place->location.kind = kind;

	/* Fixed path capacity retains the existing sidebar-owned bounded location copy. */
	(void)snprintf(place->location.path, sizeof(place->location.path), "%s", path);

	/* The published count now includes one fully initialized sidebar entry. */
	places->count++;

	/* Succeeded: the fixed sidebar owns the newly populated place. */
	return place;
}

/* Adds the favorite folders: the user's list, or the usual folders under the home folder. */
static void
places_favorites(
	struct fm_places *places,
	const char *home)
{
	char file[FM_PATH_MAX];
	char line[FM_PATH_MAX];
	char path[FM_PATH_MAX];
	char *newline;
	char *read;
	FILE *in;
	size_t index;
	int error;

	/* The user's list, a path a line. */
	error = places_file(file, sizeof(file));
	in = NULL;

	/* An unavailable config path leaves the ordinary default favorites usable. */
	if (error == 0)
		in = fopen(file, "r");

	/* Reads every saved favorite only when its actual list stream was acquired. */
	if (in != NULL) {
		/* Consumes saved paths in their persisted sidebar order. */
		for (;;) {
			read = fgets(line, sizeof(line), in);
			if (read == NULL)
				break;

			/* A saved line contributes its path without the record terminator. */
			newline = strchr(line, '\n');
			if (newline != NULL)
				*newline = '\0';

			/* Relative saved paths never become sidebar favorites. */
			if (line[0] == '/')
				places_add_folder(places, line, home);
		}

		/* The list is read. */
		error = fclose(in);
		if (error != 0)
			return;

		/* Succeeded: the saved favorites supplied this sidebar generation. */
		return;
	}

	/* Without one, the usual folders. */
	for (index = 0; index < sizeof(places_folders) / sizeof(places_folders[0]); index++) {
		/* Retains the existing bounded path policy for each default favorite folder. */
		(void)snprintf(path, sizeof(path), "%s/%s", home, places_folders[index].name);
		places_add_folder(places, path, home);
	}

	/* Succeeded: default folders supply the sidebar when no saved list is available. */
	return;
}

/* Adds a favorite folder: the usual ones keep their icons, others get a folder's; a missing one is pale. */
static void
places_add_folder(
	struct fm_places *places,
	const char *path,
	const char *home)
{
	struct fm_location location;
	struct fm_place *place;
	struct stat status;
	const char *label;
	char usual[FM_PATH_MAX];
	unsigned icon;
	size_t index;
	int error;
	int match;

	/* The icon of a usual folder, else a folder's. */
	icon = KL_ICON_FOLDER_LINE;
	for (index = 0; index < sizeof(places_folders) / sizeof(places_folders[0]); index++) {
		/* Default-folder identity uses the existing bounded home-relative path spelling. */
		(void)snprintf(usual, sizeof(usual), "%s/%s", home, places_folders[index].name);
		match = strcmp(usual, path);
		if (match == 0)
			icon = places_folders[index].icon;
	}

	/* The place, named as the folder is. */
	location.kind = FM_LOCATION_FOLDER;
	(void)snprintf(location.path, sizeof(location.path), "%s", path);
	label = fm_location_name(&location, home);
	place = places_add(places, FM_SECTION_FAVORITES, icon, label, FM_LOCATION_FOLDER, path);
	if (place == NULL)
		return;

	/* A missing folder is shown pale. */
	error = stat(path, &status);
	if (error != 0)
		place->missing = 1;

	/* Succeeded: this favorite retains its name and availability for sidebar painting. */
	return;
}

/* Adds the removable devices: a mounted one leads to its folder, one not mounted is mounted by a double click. */
static void
places_devices(
	struct fm_places *places)
{
	struct fm_place *place;
	int index;

	/* Each device, as the desktop told it. */
	for (index = 0; index < places->device_count; index++) {
		place = places_add(places, FM_SECTION_DEVICES, KL_ICON_VOLUME, places->devices[index].name, FM_LOCATION_FOLDER, places->devices[index].path);
		if (place != NULL)
			place->device = index + 1;
	}
}

/* Tells whether a path is where a removable device is mounted (it is shown under Devices). */
static int
places_device_path(
	const struct fm_places *places,
	const char *path)
{
	int index;
	int match;

	/* Each mounted device. */
	for (index = 0; index < places->device_count; index++) {
		if (!places->devices[index].mounted)
			continue;
		match = strcmp(places->devices[index].path, path);
		if (match == 0)
			return 1;
	}

	/* No device's. */
	return 0;
}

/* Adds the mounted volumes other than the root and the virtual file systems. */
static void
places_mounts(
	struct fm_places *places)
{
	struct fm_mount mount;
	struct fm_location location;
	struct fm_mounts *table;
	const char *label;
	size_t index;
	size_t prefix;
	int hidden;
	int match;
	int error;
	int available;

	/* The mounts as the desktop last told them (mounts.c, ws188-p004); none before its first answer. */
	table = NULL;
	error = fm_mounts_open(&table);
	if (error != 0)
		return;

	/* Each real mount retains the existing root, virtual and system-folder filtering. */
	for (;;) {
		available = fm_mounts_next(table, &mount);
		if (available <= 0)
			break;

		/* The computer's root already has its own common Places entry. */
		match = strcmp(mount.path, "/");
		if (match == 0)
			continue;

		/* Virtual filesystems remain absent from the user's mounted-volume sidebar. */
		hidden = 0;
		for (index = 0; index < sizeof(places_hidden_types) / sizeof(places_hidden_types[0]); index++) {
			match = strcmp(mount.type, places_hidden_types[index]);
			if (match == 0)
				hidden = 1;
		}

		/* Preserves the existing system-directory prefix policy for mount locations. */
		for (index = 0; index < sizeof(places_system_folders) / sizeof(places_system_folders[0]); index++) {
			prefix = strlen(places_system_folders[index]);
			match = strncmp(mount.path, places_system_folders[index], prefix);
			if (match == 0)
				hidden = 1;
		}

		/* A removable device's mount is shown under Devices (ws132-p005). */
		match = places_device_path(places, mount.path);
		if (match != 0)
			hidden = 1;

		/* Neither a virtual filesystem nor a system folder becomes a user volume. */
		if (hidden != 0)
			continue;

		/* The volume's label retains the mount point's existing common location-name policy. */
		location.kind = FM_LOCATION_FOLDER;
		snprintf(location.path, sizeof(location.path), "%s", mount.path);
		label = fm_location_name(&location, "");
		(void)places_add(places, FM_SECTION_LOCATIONS, KL_ICON_VOLUME, label, FM_LOCATION_FOLDER, mount.path);
	}

	/* Releases only this enumeration's stream or native snapshot. */
	fm_mounts_close(table);

	/* Succeeded: common Places policy consumed the selected OS's actual mount records. */
	return;
}

/* Prepares the actual Favorites directory and its bounded persistent-list pathname. */
static int
places_file(
	char *path,
	size_t size)
{
	char folder[FM_PATH_MAX - 16];
	const char *config;
	const char *home;
	int error;
	int length;

	/* Ordinary config selection uses an absolute XDG path or the user's home folder. */
	config = getenv("XDG_CONFIG_HOME");
	home = getenv("HOME");
	if (home == NULL)
		home = "";

	/* An absolute XDG path owns the application's configuration directory. */
	if (config != NULL && config[0] == '/') {
		length = snprintf(folder, sizeof(folder), "%s/files", config);
		if (length < 0)
			return EIO;
	} else {
		/* The home-relative config parent must fit before any directory is created. */
		length = snprintf(folder, sizeof(folder), "%s/.config", home);
		if (length < 0)
			return EIO;

		/* A truncated parent cannot identify the requested user configuration. */
		if ((size_t)length >= sizeof(folder))
			return ENAMETOOLONG;

		/* Creates only the existing home-relative config parent when needed. */
		error = mkdir(folder, 0700);
		if (error != 0 && errno != EEXIST)
			return errno;

		/* Derives the application's subdirectory without truncating its identity. */
		length = snprintf(folder, sizeof(folder), "%s/.config/files", home);
		if (length < 0)
			return EIO;
	}

	/* Neither config selection may silently publish a truncated directory. */
	if ((size_t)length >= sizeof(folder))
		return ENAMETOOLONG;

	/* A caller-owned or existing application directory contains the sidebar list. */
	error = mkdir(folder, 0700);
	if (error != 0 && errno != EEXIST)
		return errno;

	/* Reports a list pathname only when its complete config identity fits the caller buffer. */
	length = snprintf(path, size, "%s/sidebar", folder);
	if (length < 0)
		return EIO;

	/* Truncation cannot cause persistence to overwrite an unrelated pathname. */
	if ((size_t)length >= size)
		return ENAMETOOLONG;

	/* Succeeded: the caller can open the exact persistent Favorites list. */
	return 0;
}

/* Writes the existing favorite order and checks every write and the final stream close. */
static int
places_write(
	struct fm_places *places,
	int moved,
	int to,
	int removed,
	const char *added)
{
	char file[FM_PATH_MAX];
	FILE *out;
	int index;
	int written;
	int error;
	int favorite;
	int closed;

	/* Prepares only the configured user's real sidebar-list location. */
	error = places_file(file, sizeof(file));
	if (error != 0)
		return error;

	/* Opens the existing list with its established replacement behavior. */
	out = fopen(file, "w");
	if (out == NULL)
		return errno;

	/* Keeps every surviving favorite in order, inserting the moved folder beside its target. */
	error = 0;
	for (index = 0; index < places->count; index++) {
		/* The removed or relocated entry must not retain its old position. */
		if (index == removed || index == moved)
			continue;

		/* Only ordinary favorite folders belong to this persistence file. */
		favorite = fm_place_is_favorite_folder(&places->items[index]);
		if (favorite == 0)
			continue;

		/* A move toward an earlier index inserts the folder before its target. */
		if (index == to && to < moved) {
			written = fprintf(out, "%s\n", places->items[moved].location.path);
			if (written < 0) {
				error = EIO;
				goto cleanup;
			}
		}

		/* Persists the current surviving favorite at its ordinary position. */
		written = fprintf(out, "%s\n", places->items[index].location.path);
		if (written < 0) {
			error = EIO;
			goto cleanup;
		}

		/* A move toward a later index inserts the folder after its target. */
		if (index == to && to > moved) {
			written = fprintf(out, "%s\n", places->items[moved].location.path);
			if (written < 0) {
				error = EIO;
				goto cleanup;
			}
		}
	}

	/* A newly added favorite follows all surviving folders in the persisted list. */
	if (added != NULL) {
		written = fprintf(out, "%s\n", added);
		if (written < 0) {
			error = EIO;
			goto cleanup;
		}
	}

cleanup:
	/* Buffered write failure must be observed before persistence is reported as successful. */
	closed = fclose(out);
	if (closed != 0 && error == 0)
		error = errno;

	/* A failed write or close cannot authorize the caller to report a saved Favorites list. */
	if (error != 0)
		return error;

	/* Succeeded: the real sidebar stream accepted and closed the requested favorite order. */
	return 0;
}

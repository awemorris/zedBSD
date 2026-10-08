/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The pages of Settings (plan/ws089/design.md section 2): the list's
 * groups, each page's name, summary and picture, the word that names it
 * on the command line, the words a search finds it by, and how it is
 * drawn.  A page that is not ready shows its frame and says it comes in a
 * later version of Kei.
 */

#include "settings.h"

#include <string.h>

/*
 * The table of pages, indexed by their IDs (enum se_page_id), for the
 * whole run.
 */
const struct se_page se_pages[SE_PAGES] = {
	{ SE_PAGE_HOME, SE_GROUP_NONE, SE_GLYPH_GRID, "Settings", "Change how Kei looks and works.", "home", "home overview all", 1, se_home_draw, NULL, NULL, NULL },
	{ SE_PAGE_WIFI, SE_GROUP_CONNECTIVITY, SE_GLYPH_WIFI, "Wi-Fi", "Join wireless networks and turn the radio on or off.", "wifi", "wifi wireless wlan network ssid join key password", 1, se_wifi_page_draw, se_network_press, se_network_key, NULL },
	{ SE_PAGE_ETHERNET, SE_GROUP_CONNECTIVITY, SE_GLYPH_ETHERNET, "Ethernet", "Wired connections and their addresses.", "ethernet", "ethernet wired lan cable address mac dhcp static router dns", 1, se_ethernet_page_draw, se_wired_press, se_wired_key, NULL },
	{ SE_PAGE_BLUETOOTH, SE_GROUP_CONNECTIVITY, SE_GLYPH_BLUETOOTH, "Bluetooth", "Pair and connect wireless devices.", "bluetooth", "bluetooth pair devices headphones keyboard mouse", 1, se_bluetooth_draw, se_bluetooth_press, NULL, NULL },
	{ SE_PAGE_VPN, SE_GROUP_CONNECTIVITY, SE_GLYPH_SHIELD, "VPN", "Connect securely to other networks.", "vpn", "vpn tunnel private network", 0, se_soon_draw, NULL, NULL, NULL },
	{ SE_PAGE_NETWORK, SE_GROUP_CONNECTIVITY, SE_GLYPH_GLOBE, "Network", "Manage connections and internet settings.", "network", "network internet connection address dns ip usage", 1, se_network_page_draw, se_network_press, se_network_key, NULL },
	{ SE_PAGE_APPEARANCE, SE_GROUP_PERSONALIZATION, SE_GLYPH_PALETTE, "Appearance", "The look of windows and the desktop.", "appearance", "appearance look theme dark light mode transparency opacity glass", 1, se_appearance_draw, se_look_press, se_look_key, se_look_drag },
	{ SE_PAGE_WALLPAPER, SE_GROUP_PERSONALIZATION, SE_GLYPH_PICTURE, "Wallpaper", "The picture behind your windows.", "wallpaper", "wallpaper background picture desktop", 1, se_wallpaper_draw, se_look_press, NULL, NULL },
	{ SE_PAGE_NOTIFICATIONS, SE_GROUP_PERSONALIZATION, SE_GLYPH_BELL, "Notifications", "Choose which applications may notify you.", "notifications", "notifications alerts banners", 0, se_soon_draw, NULL, NULL, NULL },
	{ SE_PAGE_SOUND, SE_GROUP_PERSONALIZATION, SE_GLYPH_SPEAKER, "Sound", "Volume and the sound output.", "sound", "sound volume audio speaker mute", 1, se_sound_draw, se_sound_press, NULL, se_sound_drag },
	{ SE_PAGE_DISPLAY, SE_GROUP_PERSONALIZATION, SE_GLYPH_MONITOR, "Display", "Arrange the displays and set the brightness.", "display", "display screen resolution monitor scale extend mirror arrange brightness", 1, se_display_draw, se_display_press, NULL, se_display_drag },
	{ SE_PAGE_LANGUAGES, SE_GROUP_PERSONALIZATION, SE_GLYPH_GLOBE, "Languages", "The input method and the display language.", "languages", "languages language input method ime japanese skk english keyboard", 1, se_languages_draw, se_languages_press, se_languages_key, NULL },
	{ SE_PAGE_STORAGE, SE_GROUP_DEVICES, SE_GLYPH_DISK, "Storage", "How the disks and folders are used, and the Trash.", "storage", "storage disk space usage free folder analyze trash empty", 1, se_storage_draw, se_storage_press, NULL, NULL },
	{ SE_PAGE_POWER, SE_GROUP_DEVICES, SE_GLYPH_BATTERY, "Power", "Sleep and the screen.", "power", "power battery sleep screen energy suspend idle", 1, se_power_draw, NULL, NULL, se_power_drag },
	{ SE_PAGE_KEYBOARD, SE_GROUP_DEVICES, SE_GLYPH_KEYBOARD, "Keyboard", "Key repeat and the layout.", "keyboard", "keyboard keys repeat layout", 1, se_keyboard_draw, NULL, NULL, se_input_drag },
	{ SE_PAGE_MOUSE, SE_GROUP_DEVICES, SE_GLYPH_MOUSE, "Mouse", "Pointer speed, acceleration and scrolling.", "mouse", "mouse pointer speed acceleration scroll wheel natural", 1, se_mouse_draw, se_input_press, NULL, se_input_drag },
	{ SE_PAGE_TOUCHPAD, SE_GROUP_DEVICES, SE_GLYPH_TOUCHPAD, "Touchpad", "Pointer speed, acceleration and scrolling.", "touchpad", "touchpad trackpad pointer speed acceleration scroll natural", 1, se_touchpad_draw, se_input_press, NULL, se_input_drag },
	{ SE_PAGE_PRINTERS, SE_GROUP_DEVICES, SE_GLYPH_PRINTER, "Printers", "Add and manage printers.", "printers", "printers print printer ipp lpd jobs", 1, se_printers_draw, se_printers_press, se_printers_key, NULL },
	{ SE_PAGE_SHARING, SE_GROUP_DEVICES, SE_GLYPH_SHARE, "Sharing", "Remote Login and, later, cloud storage.", "sharing", "sharing share files remote login ssh sshd cloud", 1, se_sharing_draw, se_sharing_press, NULL, NULL },
	{ SE_PAGE_USERS, SE_GROUP_SYSTEM, SE_GLYPH_PEOPLE, "Users", "Accounts on this computer.", "users", "users accounts people login password", 1, se_users_draw, se_users_press, se_users_key, NULL },
	{ SE_PAGE_UPDATES, SE_GROUP_SYSTEM, SE_GLYPH_REFRESH, "Updates", "Keep Kei up to date.", "updates", "updates upgrade version software", 0, se_soon_draw, NULL, NULL, NULL },
	{ SE_PAGE_ABOUT, SE_GROUP_SYSTEM, SE_GLYPH_INFO, "About", "This computer and the version of Kei.", "about", "about version system computer kei hardware welcome", 1, se_about_draw, se_about_press, NULL, NULL }
};

/*
 * Finds the page a word names (the command line's), or NULL when no page
 * has that word.
 */
const struct se_page *
se_page_find(
	const char *word)
{
	unsigned index;
	int differs;

	/* Each page's word. */
	for (index = 0; index < SE_PAGES; index++) {
		differs = strcmp(se_pages[index].word, word);
		if (differs == 0)
			return &se_pages[index];
	}

	/* No page has that word. */
	return NULL;
}

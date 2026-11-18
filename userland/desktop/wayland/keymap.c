/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The keyboard's XKB keymap (ws035-p078): a US layout that toolkits
 * compile with libxkbcommon to turn the evdev key codes the compositor sends into
 * characters.
 *
 * The keymap is complete in itself (no include statements), as the
 * wl_keyboard.keymap format xkb_v1 asks.  Its key codes are the evdev codes
 * plus 8; its real modifiers are in the standard order, so the masks
 * the compositor sends in wl_keyboard.modifiers (Shift 0x1, Lock 0x2, Control
 * 0x4, Mod1 = Alt 0x8, Mod2 = NumLock 0x10, Mod4 = Super 0x40) mean the same
 * to the client.  It is written once to a file at start-up, and each
 * keyboard gets a read-only descriptor of it.
 */

#include "keymap.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * The keymap's text.  It lives for the whole run and is copied once into
 * the keymap file (kwl_keymap_open).
 */
static const char keymap_text[] =
	"xkb_keymap {\n"
	"xkb_keycodes \"evdev\" {\n"
	"\tminimum = 8;\n"
	"\tmaximum = 255;\n"
	"\t<ESC> = 9; <AE01> = 10; <AE02> = 11; <AE03> = 12; <AE04> = 13; <AE05> = 14;\n"
	"\t<AE06> = 15; <AE07> = 16; <AE08> = 17; <AE09> = 18; <AE10> = 19; <AE11> = 20;\n"
	"\t<AE12> = 21; <BKSP> = 22; <TAB> = 23;\n"
	"\t<AD01> = 24; <AD02> = 25; <AD03> = 26; <AD04> = 27; <AD05> = 28; <AD06> = 29;\n"
	"\t<AD07> = 30; <AD08> = 31; <AD09> = 32; <AD10> = 33; <AD11> = 34; <AD12> = 35;\n"
	"\t<RTRN> = 36; <LCTL> = 37;\n"
	"\t<AC01> = 38; <AC02> = 39; <AC03> = 40; <AC04> = 41; <AC05> = 42; <AC06> = 43;\n"
	"\t<AC07> = 44; <AC08> = 45; <AC09> = 46; <AC10> = 47; <AC11> = 48; <TLDE> = 49;\n"
	"\t<LFSH> = 50; <BKSL> = 51;\n"
	"\t<AB01> = 52; <AB02> = 53; <AB03> = 54; <AB04> = 55; <AB05> = 56; <AB06> = 57;\n"
	"\t<AB07> = 58; <AB08> = 59; <AB09> = 60; <AB10> = 61; <RTSH> = 62;\n"
	"\t<KPMU> = 63; <LALT> = 64; <SPCE> = 65; <CAPS> = 66;\n"
	"\t<FK01> = 67; <FK02> = 68; <FK03> = 69; <FK04> = 70; <FK05> = 71; <FK06> = 72;\n"
	"\t<FK07> = 73; <FK08> = 74; <FK09> = 75; <FK10> = 76; <NMLK> = 77; <SCLK> = 78;\n"
	"\t<KP7> = 79; <KP8> = 80; <KP9> = 81; <KPSU> = 82; <KP4> = 83; <KP5> = 84;\n"
	"\t<KP6> = 85; <KPAD> = 86; <KP1> = 87; <KP2> = 88; <KP3> = 89; <KP0> = 90;\n"
	"\t<KPDL> = 91; <LSGT> = 94; <FK11> = 95; <FK12> = 96; <KPEN> = 104;\n"
	"\t<RCTL> = 105; <KPDV> = 106; <PRSC> = 107; <RALT> = 108; <HOME> = 110;\n"
	"\t<UP> = 111; <PGUP> = 112; <LEFT> = 113; <RGHT> = 114; <END> = 115;\n"
	"\t<DOWN> = 116; <PGDN> = 117; <INS> = 118; <DELE> = 119; <PAUS> = 127;\n"
	"\t<LWIN> = 133; <RWIN> = 134; <COMP> = 135;\n"
	"\tindicator 1 = \"Caps Lock\";\n"
	"\tindicator 2 = \"Num Lock\";\n"
	"};\n"
	"xkb_types \"basic\" {\n"
	"\tvirtual_modifiers NumLock,Alt,Super;\n"
	"\ttype \"ONE_LEVEL\" {\n"
	"\t\tmodifiers = none;\n"
	"\t\tlevel_name[Level1] = \"Any\";\n"
	"\t};\n"
	"\ttype \"TWO_LEVEL\" {\n"
	"\t\tmodifiers = Shift;\n"
	"\t\tmap[Shift] = Level2;\n"
	"\t\tlevel_name[Level1] = \"Base\";\n"
	"\t\tlevel_name[Level2] = \"Shift\";\n"
	"\t};\n"
	"\ttype \"ALPHABETIC\" {\n"
	"\t\tmodifiers = Shift+Lock;\n"
	"\t\tmap[Shift] = Level2;\n"
	"\t\tmap[Lock] = Level2;\n"
	"\t\tlevel_name[Level1] = \"Base\";\n"
	"\t\tlevel_name[Level2] = \"Caps\";\n"
	"\t};\n"
	"\ttype \"KEYPAD\" {\n"
	"\t\tmodifiers = Shift+NumLock;\n"
	"\t\tmap[None] = Level1;\n"
	"\t\tmap[Shift] = Level2;\n"
	"\t\tmap[NumLock] = Level2;\n"
	"\t\tmap[Shift+NumLock] = Level1;\n"
	"\t\tlevel_name[Level1] = \"Base\";\n"
	"\t\tlevel_name[Level2] = \"Number\";\n"
	"\t};\n"
	"};\n"
	"xkb_compatibility \"basic\" {\n"
	"\tvirtual_modifiers NumLock,Alt,Super;\n"
	"\tinterpret.useModMapMods = AnyLevel;\n"
	"\tinterpret.repeat = False;\n"
	"\tinterpret Shift_L+AnyOfOrNone(all) { action = SetMods(modifiers=Shift,clearLocks); };\n"
	"\tinterpret Shift_R+AnyOfOrNone(all) { action = SetMods(modifiers=Shift,clearLocks); };\n"
	"\tinterpret Control_L+AnyOfOrNone(all) { action = SetMods(modifiers=Control,clearLocks); };\n"
	"\tinterpret Control_R+AnyOfOrNone(all) { action = SetMods(modifiers=Control,clearLocks); };\n"
	"\tinterpret Alt_L+AnyOfOrNone(all) { virtualModifier = Alt; action = SetMods(modifiers=modMapMods,clearLocks); };\n"
	"\tinterpret Alt_R+AnyOfOrNone(all) { virtualModifier = Alt; action = SetMods(modifiers=modMapMods,clearLocks); };\n"
	"\tinterpret Super_L+AnyOfOrNone(all) { virtualModifier = Super; action = SetMods(modifiers=modMapMods,clearLocks); };\n"
	"\tinterpret Super_R+AnyOfOrNone(all) { virtualModifier = Super; action = SetMods(modifiers=modMapMods,clearLocks); };\n"
	"\tinterpret Caps_Lock+AnyOfOrNone(all) { action = LockMods(modifiers=Lock); };\n"
	"\tinterpret Num_Lock+AnyOf(all) { virtualModifier = NumLock; action = LockMods(modifiers=NumLock); };\n"
	"\tinterpret Any+Exactly(Lock) { action = LockMods(modifiers=Lock); };\n"
	"\tindicator \"Caps Lock\" { whichModState = locked; modifiers = Lock; };\n"
	"\tindicator \"Num Lock\" { whichModState = locked; modifiers = NumLock; };\n"
	"};\n"
	"xkb_symbols \"us\" {\n"
	"\tname[group1] = \"English (US)\";\n"
	"\tkey <ESC> { [ Escape ] };\n"
	"\tkey <AE01> { [ 1, exclam ] };\n"
	"\tkey <AE02> { [ 2, at ] };\n"
	"\tkey <AE03> { [ 3, numbersign ] };\n"
	"\tkey <AE04> { [ 4, dollar ] };\n"
	"\tkey <AE05> { [ 5, percent ] };\n"
	"\tkey <AE06> { [ 6, asciicircum ] };\n"
	"\tkey <AE07> { [ 7, ampersand ] };\n"
	"\tkey <AE08> { [ 8, asterisk ] };\n"
	"\tkey <AE09> { [ 9, parenleft ] };\n"
	"\tkey <AE10> { [ 0, parenright ] };\n"
	"\tkey <AE11> { [ minus, underscore ] };\n"
	"\tkey <AE12> { [ equal, plus ] };\n"
	"\tkey <BKSP> { [ BackSpace ] };\n"
	"\tkey <TAB> { [ Tab, ISO_Left_Tab ] };\n"
	"\tkey <AD01> { [ q, Q ] };\n"
	"\tkey <AD02> { [ w, W ] };\n"
	"\tkey <AD03> { [ e, E ] };\n"
	"\tkey <AD04> { [ r, R ] };\n"
	"\tkey <AD05> { [ t, T ] };\n"
	"\tkey <AD06> { [ y, Y ] };\n"
	"\tkey <AD07> { [ u, U ] };\n"
	"\tkey <AD08> { [ i, I ] };\n"
	"\tkey <AD09> { [ o, O ] };\n"
	"\tkey <AD10> { [ p, P ] };\n"
	"\tkey <AD11> { [ bracketleft, braceleft ] };\n"
	"\tkey <AD12> { [ bracketright, braceright ] };\n"
	"\tkey <RTRN> { [ Return ] };\n"
	"\tkey <LCTL> { [ Control_L ] };\n"
	"\tkey <AC01> { [ a, A ] };\n"
	"\tkey <AC02> { [ s, S ] };\n"
	"\tkey <AC03> { [ d, D ] };\n"
	"\tkey <AC04> { [ f, F ] };\n"
	"\tkey <AC05> { [ g, G ] };\n"
	"\tkey <AC06> { [ h, H ] };\n"
	"\tkey <AC07> { [ j, J ] };\n"
	"\tkey <AC08> { [ k, K ] };\n"
	"\tkey <AC09> { [ l, L ] };\n"
	"\tkey <AC10> { [ semicolon, colon ] };\n"
	"\tkey <AC11> { [ apostrophe, quotedbl ] };\n"
	"\tkey <TLDE> { [ grave, asciitilde ] };\n"
	"\tkey <LFSH> { [ Shift_L ] };\n"
	"\tkey <BKSL> { [ backslash, bar ] };\n"
	"\tkey <AB01> { [ z, Z ] };\n"
	"\tkey <AB02> { [ x, X ] };\n"
	"\tkey <AB03> { [ c, C ] };\n"
	"\tkey <AB04> { [ v, V ] };\n"
	"\tkey <AB05> { [ b, B ] };\n"
	"\tkey <AB06> { [ n, N ] };\n"
	"\tkey <AB07> { [ m, M ] };\n"
	"\tkey <AB08> { [ comma, less ] };\n"
	"\tkey <AB09> { [ period, greater ] };\n"
	"\tkey <AB10> { [ slash, question ] };\n"
	"\tkey <RTSH> { [ Shift_R ] };\n"
	"\tkey <KPMU> { [ KP_Multiply ] };\n"
	"\tkey <LALT> { [ Alt_L, Meta_L ] };\n"
	"\tkey <SPCE> { [ space ] };\n"
	"\tkey <CAPS> { [ Caps_Lock ] };\n"
	"\tkey <FK01> { [ F1 ] };\n"
	"\tkey <FK02> { [ F2 ] };\n"
	"\tkey <FK03> { [ F3 ] };\n"
	"\tkey <FK04> { [ F4 ] };\n"
	"\tkey <FK05> { [ F5 ] };\n"
	"\tkey <FK06> { [ F6 ] };\n"
	"\tkey <FK07> { [ F7 ] };\n"
	"\tkey <FK08> { [ F8 ] };\n"
	"\tkey <FK09> { [ F9 ] };\n"
	"\tkey <FK10> { [ F10 ] };\n"
	"\tkey <FK11> { [ F11 ] };\n"
	"\tkey <FK12> { [ F12 ] };\n"
	"\tkey <NMLK> { [ Num_Lock ] };\n"
	"\tkey <SCLK> { [ Scroll_Lock ] };\n"
	"\tkey <KP7> { [ KP_Home, KP_7 ] };\n"
	"\tkey <KP8> { [ KP_Up, KP_8 ] };\n"
	"\tkey <KP9> { [ KP_Prior, KP_9 ] };\n"
	"\tkey <KPSU> { [ KP_Subtract ] };\n"
	"\tkey <KP4> { [ KP_Left, KP_4 ] };\n"
	"\tkey <KP5> { [ KP_Begin, KP_5 ] };\n"
	"\tkey <KP6> { [ KP_Right, KP_6 ] };\n"
	"\tkey <KPAD> { [ KP_Add ] };\n"
	"\tkey <KP1> { [ KP_End, KP_1 ] };\n"
	"\tkey <KP2> { [ KP_Down, KP_2 ] };\n"
	"\tkey <KP3> { [ KP_Next, KP_3 ] };\n"
	"\tkey <KP0> { [ KP_Insert, KP_0 ] };\n"
	"\tkey <KPDL> { [ KP_Delete, KP_Decimal ] };\n"
	"\tkey <LSGT> { [ less, greater ] };\n"
	"\tkey <KPEN> { [ KP_Enter ] };\n"
	"\tkey <RCTL> { [ Control_R ] };\n"
	"\tkey <KPDV> { [ KP_Divide ] };\n"
	"\tkey <PRSC> { [ Print ] };\n"
	"\tkey <RALT> { [ Alt_R, Meta_R ] };\n"
	"\tkey <HOME> { [ Home ] };\n"
	"\tkey <UP> { [ Up ] };\n"
	"\tkey <PGUP> { [ Prior ] };\n"
	"\tkey <LEFT> { [ Left ] };\n"
	"\tkey <RGHT> { [ Right ] };\n"
	"\tkey <END> { [ End ] };\n"
	"\tkey <DOWN> { [ Down ] };\n"
	"\tkey <PGDN> { [ Next ] };\n"
	"\tkey <INS> { [ Insert ] };\n"
	"\tkey <DELE> { [ Delete ] };\n"
	"\tkey <PAUS> { [ Pause ] };\n"
	"\tkey <LWIN> { [ Super_L ] };\n"
	"\tkey <RWIN> { [ Super_R ] };\n"
	"\tkey <COMP> { [ Menu ] };\n"
	"\tmodifier_map Shift { <LFSH>, <RTSH> };\n"
	"\tmodifier_map Lock { <CAPS> };\n"
	"\tmodifier_map Control { <LCTL>, <RCTL> };\n"
	"\tmodifier_map Mod1 { <LALT>, <RALT> };\n"
	"\tmodifier_map Mod2 { <NMLK> };\n"
	"\tmodifier_map Mod4 { <LWIN>, <RWIN> };\n"
	"};\n"
	"};\n";

/*
 * The read-only descriptor of the keymap file, which each keyboard gets a
 * copy of; -1 until kwl_keymap_open made it (or when it could not).
 */
static int keymap_fd = -1;

/*
 * Makes the keymap file: the text, with its terminating NUL, in a file
 * nobody else can name, kept open read-only.  Returns 0, or an errno (the
 * keyboards then say they have no keymap).
 */
int
kwl_keymap_open(void)
{
	char path[64];
	ssize_t written;
	int writer;
	int reader;

	/* A file of this process's own. */
	(void)snprintf(path, sizeof(path), "/tmp/keiland-keymap-%ld", (long)getpid());
	writer = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (writer < 0)
		return errno;

	/* The text and its NUL. */
	written = write(writer, keymap_text, sizeof(keymap_text));
	if (written != (ssize_t)sizeof(keymap_text)) {
		close(writer);
		(void)unlink(path);
		return EIO;
	}

	/* A read-only descriptor of it, and the name gone. */
	reader = open(path, O_RDONLY | O_CLOEXEC);
	close(writer);
	(void)unlink(path);
	if (reader < 0)
		return errno;

	/* Succeeded: the keyboards can be given the keymap. */
	keymap_fd = reader;
	return 0;
}

/*
 * Gives a new read-only descriptor of the keymap file and its size for one
 * keyboard; -1 when there is no keymap.
 */
int
kwl_keymap_descriptor(
	uint32_t *size)
{
	int copy;

	/* No keymap file. */
	*size = 0;
	if (keymap_fd < 0)
		return -1;

	/* A copy the keymap event carries away. */
	copy = fcntl(keymap_fd, F_DUPFD_CLOEXEC, 0);
	if (copy < 0)
		return -1;

	/* Succeeded: the text's size, with its NUL. */
	*size = (uint32_t)sizeof(keymap_text);
	return copy;
}

/*
 * Gives the keymap's text (for the host test).
 */
const char *
kwl_keymap_text(void)
{
	/* Succeeded: the text. */
	return keymap_text;
}

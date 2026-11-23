/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

package io.noctvm.keiland;

import android.app.Activity;
import android.view.Gravity;
import android.view.Menu;
import android.view.MenuItem;
import android.view.SubMenu;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.PopupMenu;

/**
 * Shows a menu the native code described as a table of items, as the system's popup menu, and
 * reports the item chosen.
 *
 * The table is the native menu's (label, parent's position, flags), in order.  An item that is the
 * parent of another opens a submenu (the platform closes the menu it was chosen in, which is not a
 * dismissal).  Whatever ends the menu, one answer is given: the position of the item chosen, or -1
 * when the menu went away without a choice.
 */
final class Menus {
	/** The flags of an item. */
	private static final int FLAG_ENABLED = 1;
	private static final int FLAG_CHECKABLE = 2;
	private static final int FLAG_CHECKED = 4;
	private static final int FLAG_SEPARATOR = 8;

	/** The menu open now, if any; a new one replaces it. */
	private static PopupMenu current;

	/** The 1 by 1 view the menu is anchored to, placed at the point asked for. */
	private static View anchor;

	private Menus() {
	}

	/**
	 * Shows a menu at a point of the Activity's window, in pixels, from the UI thread.  The answer
	 * of the request is given once, to the native code.
	 */
	static void show(Activity activity, final int request, int x, int y, String[] labels, int[] parents, int[] flags) {
		final boolean[] answered = new boolean[1];
		final boolean[] opening = new boolean[1];
		PopupMenu menu;
		Menu root;
		Menu[] menus;
		boolean[] parentOf;
		int[] groups;
		int count;
		int index;
		int parent;
		int group;
		SubMenu sub;
		MenuItem item;

		/* A menu that was open goes away without an answer (its request is no longer asked). */
		if (current != null) {
			current.setOnDismissListener(null);
			current.dismiss();
			current = null;
		}

		/* The point is a view the menu hangs from. */
		placeAnchor(activity, x, y);
		menu = new PopupMenu(activity, anchor, Gravity.START);
		root = menu.getMenu();
		root.setGroupDividerEnabled(true);

		/* Which items are parents. */
		count = labels.length;
		parentOf = new boolean[count];
		for (index = 0; index < count; index++) {
			if (parents[index] >= 0 && parents[index] < count) {
				parentOf[parents[index]] = true;
			}
		}

		/* Each item in its menu: a separator starts a new group, which the menu shows as a line. */
		menus = new Menu[count];
		groups = new int[count + 1];
		for (index = 0; index < count; index++) {
			parent = parents[index];
			Menu target = root;
			if (parent >= 0 && parent < index && menus[parent] != null) {
				target = menus[parent];
			}
			group = groups[parent + 1];
			if ((flags[index] & FLAG_SEPARATOR) != 0) {
				groups[parent + 1]++;
				continue;
			}
			if (parentOf[index]) {
				sub = target.addSubMenu(group, index, Menu.NONE, labels[index]);
				menus[index] = sub;
				sub.getItem().setEnabled((flags[index] & FLAG_ENABLED) != 0);
				((Menu) sub).setGroupDividerEnabled(true);
			} else {
				item = target.add(group, index, Menu.NONE, labels[index]);
				item.setEnabled((flags[index] & FLAG_ENABLED) != 0);
				if ((flags[index] & FLAG_CHECKABLE) != 0) {
					item.setCheckable(true);
					item.setChecked((flags[index] & FLAG_CHECKED) != 0);
				}
			}
		}

		/* The choice, or the dismissal, is answered once. */
		menu.setOnMenuItemClickListener(new PopupMenu.OnMenuItemClickListener() {
			@Override
			public boolean onMenuItemClick(MenuItem chosen) {
				/* An item that opens a submenu is not a choice; the menu it closes is not a dismissal. */
				if (chosen.hasSubMenu()) {
					opening[0] = true;
					return false;
				}
				if (!answered[0]) {
					answered[0] = true;
					KeilandBridge.nativeMenu(request, chosen.getItemId());
				}
				return true;
			}
		});
		menu.setOnDismissListener(new PopupMenu.OnDismissListener() {
			@Override
			public void onDismiss(PopupMenu closed) {
				if (opening[0]) {
					opening[0] = false;
					return;
				}
				if (current == closed) {
					current = null;
				}
				if (!answered[0]) {
					answered[0] = true;
					KeilandBridge.nativeMenu(request, -1);
				}
			}
		});
		current = menu;
		menu.show();
	}

	/** Puts the 1 by 1 anchor view in the Activity's content at a point (made again for another Activity). */
	private static void placeAnchor(Activity activity, int x, int y) {
		ViewGroup content;

		content = activity.findViewById(android.R.id.content);
		if (anchor == null || anchor.getContext() != activity || anchor.getParent() != content) {
			anchor = new View(activity);
			content.addView(anchor, new FrameLayout.LayoutParams(1, 1));
		}
		anchor.setX(x);
		anchor.setY(y);
	}
}

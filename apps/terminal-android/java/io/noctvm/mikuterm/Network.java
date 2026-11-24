/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

package io.noctvm.mikuterm;

import android.content.Context;
import android.net.ConnectivityManager;

/**
 * Watches the system's default network and tells the native thread when it comes up or goes away.
 */
final class Network {
	private static ConnectivityManager.NetworkCallback callback;

	private Network() {
	}

	/** Registers the default network callback once for the application. */
	static synchronized void register(Context context) {
		ConnectivityManager manager;
		ConnectivityManager.NetworkCallback created;

		if (callback != null || context == null) {
			return;
		}
		manager = (ConnectivityManager) context.getApplicationContext().getSystemService(Context.CONNECTIVITY_SERVICE);
		if (manager == null) {
			return;
		}
		created = new ConnectivityManager.NetworkCallback() {
			@Override
			public void onAvailable(android.net.Network network) {
				MikutermBridge.nativeNetwork(true, network.getNetworkHandle());
			}

			@Override
			public void onLost(android.net.Network network) {
				MikutermBridge.nativeNetwork(false, network.getNetworkHandle());
			}
		};
		try {
			manager.registerDefaultNetworkCallback(created);
			callback = created;
		} catch (RuntimeException e) {
			/* Unavailable on this system; the native side simply receives no network updates. */
		}
	}
}

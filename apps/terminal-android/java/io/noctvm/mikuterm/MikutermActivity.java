/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

package io.noctvm.mikuterm;

import android.Manifest;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import io.noctvm.keiland.KeilandActivity;

/**
 * mikuterm's Activity: the one the launcher starts.
 *
 * The native library is loaded here, by this application's class loader, so that its JNI_OnLoad
 * finds mikuterm's Java classes (a native thread's own search cannot).
 */
public class MikutermActivity extends KeilandActivity {
	private static boolean askedNotificationPermission;

	static {
		System.loadLibrary("mikuterm");
	}

	@Override
	protected void onKeilandCreated() {
		MikutermBridge.attach(this);
		Network.register(this);
		handleIntent(getIntent());
	}

	@Override
	protected void onNewIntent(Intent intent) {
		super.onNewIntent(intent);
		setIntent(intent);
		handleIntent(intent);
	}

	@Override
	protected void onActivityResult(int requestCode, int resultCode, Intent data) {
		if (SftpStorage.onActivityResult(this, requestCode, resultCode, data)) {
			return;
		}
		super.onActivityResult(requestCode, resultCode, data);
	}

	@Override
	protected void onDestroy() {
		MikutermBridge.detach(this);
		super.onDestroy();
	}

	/** Asks for the notification permission once when the first session opens on Android 13 or later. */
	void requestNotificationPermissionOnce() {
		if (askedNotificationPermission || Build.VERSION.SDK_INT < 33) {
			return;
		}
		askedNotificationPermission = true;
		if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
			requestPermissions(new String[] { Manifest.permission.POST_NOTIFICATIONS }, 1);
		}
	}

	private void handleIntent(Intent intent) {
		if (intent != null && SessionService.ACTION_STOP_ALL.equals(intent.getAction())) {
			MikutermBridge.nativeStopAll();
		}
	}
}

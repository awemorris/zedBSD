/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

package io.noctvm.mikuterm;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

/**
 * Foreground service that keeps mikuterm's process alive while at least one SSH session is open,
 * showing a low-priority notification with the session count and an action to disconnect all.
 */
public final class SessionService extends Service {
	/** Intent action sent to MikutermActivity when the user taps "Disconnect all" in the notification. */
	static final String ACTION_STOP_ALL = "io.noctvm.mikuterm.ACTION_STOP_ALL";

	private static final String ACTION_UPDATE = "io.noctvm.mikuterm.ACTION_UPDATE";
	private static final String EXTRA_SESSIONS = "sessions";
	private static final String CHANNEL_ID = "sessions";
	private static final int NOTIFICATION_ID = 1;

	/** Starts, updates or stops the foreground service for the given number of active sessions. */
	static void update(Context context, int sessions) {
		Intent intent;

		if (context == null) {
			return;
		}
		if (sessions <= 0) {
			context.stopService(new Intent(context, SessionService.class));
			return;
		}
		intent = new Intent(context, SessionService.class);
		intent.setAction(ACTION_UPDATE);
		intent.putExtra(EXTRA_SESSIONS, sessions);
		context.startForegroundService(intent);
	}

	@Override
	public int onStartCommand(Intent intent, int flags, int startId) {
		int sessions;
		Notification notification;

		sessions = 1;
		if (intent != null) {
			sessions = intent.getIntExtra(EXTRA_SESSIONS, 1);
		}
		if (sessions <= 0) {
			stopSelf();
			return START_NOT_STICKY;
		}

		ensureChannel();
		notification = buildNotification(sessions);
		if (Build.VERSION.SDK_INT >= 34) {
			startForeground(NOTIFICATION_ID, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
		} else {
			startForeground(NOTIFICATION_ID, notification);
		}
		return START_NOT_STICKY;
	}

	@Override
	public void onDestroy() {
		stopForeground(STOP_FOREGROUND_REMOVE);
		super.onDestroy();
	}

	@Override
	public IBinder onBind(Intent intent) {
		return null;
	}

	private void ensureChannel() {
		NotificationManager manager;
		NotificationChannel channel;

		manager = (NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
		if (manager == null) {
			return;
		}
		if (manager.getNotificationChannel(CHANNEL_ID) != null) {
			return;
		}
		channel = new NotificationChannel(CHANNEL_ID, stringNamed("service_channel_name", "Active sessions"), NotificationManager.IMPORTANCE_LOW);
		manager.createNotificationChannel(channel);
	}

	private Notification buildNotification(int sessions) {
		Intent openIntent;
		Intent stopIntent;
		PendingIntent openPending;
		PendingIntent stopPending;
		String text;
		int icon;

		openIntent = new Intent(this, MikutermActivity.class);
		openIntent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
		openPending = PendingIntent.getActivity(this, 0, openIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

		stopIntent = new Intent(this, MikutermActivity.class);
		stopIntent.setAction(ACTION_STOP_ALL);
		stopIntent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
		stopPending = PendingIntent.getActivity(this, 1, stopIntent, PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

		if (sessions == 1) {
			text = stringNamed("service_one_session", "1 active SSH session");
		} else {
			text = String.format(stringNamed("service_many_sessions", "%d active SSH sessions"), sessions);
		}
		icon = getApplicationInfo().icon;
		if (icon == 0) {
			icon = android.R.drawable.sym_def_app_icon;
		}

		return new Notification.Builder(this, CHANNEL_ID)
			.setSmallIcon(icon)
			.setContentTitle(stringNamed("service_title", "mikuterm"))
			.setContentText(text)
			.setOngoing(true)
			.setOnlyAlertOnce(true)
			.setContentIntent(openPending)
			.addAction(new Notification.Action.Builder(null, stringNamed("service_disconnect_all", "Disconnect all"), stopPending).build())
			.build();
	}

	@SuppressWarnings("deprecation")
	private String stringNamed(String name, String fallback) {
		int id;

		id = getResources().getIdentifier(name, "string", getPackageName());
		if (id == 0) {
			return fallback;
		}
		return getString(id);
	}
}

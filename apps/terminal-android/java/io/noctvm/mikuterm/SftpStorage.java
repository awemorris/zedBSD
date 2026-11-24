/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

package io.noctvm.mikuterm;

import android.app.Activity;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.UriPermission;
import android.database.Cursor;
import android.net.Uri;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.MediaStore;
import android.provider.OpenableColumns;

import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * Android Scoped Storage (MediaStore.Downloads) and SAF (ACTION_OPEN_DOCUMENT /
 * ACTION_OPEN_DOCUMENT_TREE) backend for mikuterm's SFTP local filesystem layer.
 */
final class SftpStorage {
	static final int REQ_SFTP_UPLOAD = 4101;
	static final int REQ_SFTP_DOWNLOAD_TREE = 4102;

	private static final Object LOCK = new Object();
	private static final Map<String, Uri> TREE_URIS = new HashMap<String, Uri>();
	private static final Map<String, String> TREE_IDS = new HashMap<String, String>();
	private static final Map<String, Uri> DOC_URIS = new HashMap<String, Uri>();
	private static int nextTreeId = 1;
	private static int nextDocId = 1;
	private static int pendingViewNumber;

	private SftpStorage() {
	}

	/** Launches the SAF document picker for SFTP upload. */
	static void launchUploadPicker(Activity activity, int viewNumber) {
		if (activity == null) {
			return;
		}
		synchronized (LOCK) {
			pendingViewNumber = viewNumber;
		}
		Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
		intent.addCategory(Intent.CATEGORY_OPENABLE);
		intent.setType("*/*");
		intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
		activity.startActivityForResult(intent, REQ_SFTP_UPLOAD);
	}

	/** Launches the SAF folder picker for changing the SFTP download directory. */
	static void launchDownloadDirPicker(Activity activity, int viewNumber) {
		if (activity == null) {
			return;
		}
		synchronized (LOCK) {
			pendingViewNumber = viewNumber;
		}
		Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
		intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
			| Intent.FLAG_GRANT_WRITE_URI_PERMISSION
			| Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
		activity.startActivityForResult(intent, REQ_SFTP_DOWNLOAD_TREE);
	}

	/** Handles Activity results for SAF upload and download-folder pickers. */
	static boolean onActivityResult(Activity activity, int requestCode, int resultCode, Intent data) {
		if (requestCode != REQ_SFTP_UPLOAD && requestCode != REQ_SFTP_DOWNLOAD_TREE) {
			return false;
		}
		int viewNumber;
		synchronized (LOCK) {
			viewNumber = pendingViewNumber;
		}
		if (resultCode != Activity.RESULT_OK || data == null) {
			if (requestCode == REQ_SFTP_DOWNLOAD_TREE) {
				MikutermBridge.nativeSftpDownloadDirPicked(viewNumber, null, null);
			}
			return true;
		}

		if (requestCode == REQ_SFTP_UPLOAD) {
			List<String> virtualPaths = new ArrayList<String>();
			ClipData clip = data.getClipData();
			if (clip != null) {
				for (int i = 0; i < clip.getItemCount(); i++) {
					Uri uri = clip.getItemAt(i).getUri();
					if (uri != null) {
						virtualPaths.add(registerDocUri(activity, uri));
					}
				}
			} else if (data.getData() != null) {
				virtualPaths.add(registerDocUri(activity, data.getData()));
			}
			if (!virtualPaths.isEmpty()) {
				MikutermBridge.nativeSftpUploadPicked(
					viewNumber,
					virtualPaths.toArray(new String[0]));
			}
			return true;
		}

		Uri treeUri = data.getData();
		if (treeUri != null) {
			int flags = Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION;
			try {
				activity.getContentResolver().takePersistableUriPermission(treeUri, flags);
			} catch (SecurityException e) {
				/* Proceed if transient permission was granted. */
			}
			String virtualDir = registerTreeUriInternal(treeUri);
			MikutermBridge.nativeSftpDownloadDirPicked(viewNumber, treeUri.toString(), virtualDir);
		}
		return true;
	}

	/** Registers a persisted SAF tree URI and returns its virtual "saf-tree://<id>" path, or null. */
	static String registerTreeUri(Context ctx, String uriString) {
		if (ctx == null || uriString == null || uriString.isEmpty()) {
			return null;
		}
		Uri uri;
		try {
			uri = Uri.parse(uriString);
		} catch (RuntimeException e) {
			return null;
		}
		List<UriPermission> perms = ctx.getContentResolver().getPersistedUriPermissions();
		boolean granted = false;
		for (UriPermission p : perms) {
			if (p.getUri().equals(uri) && p.isReadPermission() && p.isWritePermission()) {
				granted = true;
				break;
			}
		}
		if (!granted) {
			return null;
		}
		return registerTreeUriInternal(uri);
	}

	private static String registerTreeUriInternal(Uri uri) {
		String key = uri.toString();
		synchronized (LOCK) {
			String existing = TREE_IDS.get(key);
			if (existing != null) {
				return "saf-tree://" + existing;
			}
			String id = Integer.toString(nextTreeId++);
			TREE_URIS.put(id, uri);
			TREE_IDS.put(key, id);
			return "saf-tree://" + id;
		}
	}

	private static String registerDocUri(Context ctx, Uri uri) {
		String name = queryDisplayName(ctx, uri);
		String safeName = sanitizeFilename(name);
		String id;
		synchronized (LOCK) {
			id = Integer.toString(nextDocId++);
			DOC_URIS.put(id, uri);
		}
		return "saf-doc://" + id + "/" + safeName;
	}

	private static String queryDisplayName(Context ctx, Uri uri) {
		Cursor c = null;
		try {
			c = ctx.getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null);
			if (c != null && c.moveToFirst()) {
				String n = c.getString(0);
				if (n != null && !n.isEmpty()) {
					return n;
				}
			}
		} catch (RuntimeException e) {
			/* Fallback below. */
		} finally {
			if (c != null) {
				c.close();
			}
		}
		String last = uri.getLastPathSegment();
		return (last != null && !last.isEmpty()) ? last : "upload.bin";
	}

	private static String sanitizeFilename(String raw) {
		StringBuilder sb = new StringBuilder(raw.length());
		for (int i = 0; i < raw.length(); i++) {
			char ch = raw.charAt(i);
			if (ch == '/' || ch == '\\' || ch < 0x20 || ch == 0x7f) {
				sb.append('_');
			} else {
				sb.append(ch);
			}
		}
		String out = sb.toString();
		if (out.isEmpty() || ".".equals(out) || "..".equals(out)) {
			return "upload.bin";
		}
		return out;
	}

	/** Creates a pending MediaStore.Downloads entry (IS_PENDING=1) and returns its content URI string. */
	static String createPendingDownload(Context ctx, String subDir, String displayName) {
		if (ctx == null || displayName == null || displayName.isEmpty()) {
			return null;
		}
		String relPath = Environment.DIRECTORY_DOWNLOADS;
		if (subDir != null && !subDir.isEmpty()) {
			relPath = relPath + "/" + subDir;
		}
		ContentValues cv = new ContentValues();
		cv.put(MediaStore.Downloads.DISPLAY_NAME, displayName);
		cv.put(MediaStore.Downloads.RELATIVE_PATH, relPath);
		cv.put(MediaStore.Downloads.IS_PENDING, 1);
		try {
			Uri uri = ctx.getContentResolver().insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, cv);
			return uri != null ? uri.toString() : null;
		} catch (RuntimeException e) {
			return null;
		}
	}

	/** Marks a pending MediaStore.Downloads entry as visible (IS_PENDING=0). */
	static boolean commitPendingDownload(Context ctx, String uriString) {
		if (ctx == null || uriString == null) {
			return false;
		}
		try {
			Uri uri = Uri.parse(uriString);
			ContentValues cv = new ContentValues();
			cv.put(MediaStore.Downloads.IS_PENDING, 0);
			int rows = ctx.getContentResolver().update(uri, cv, null, null);
			return rows >= 0;
		} catch (RuntimeException e) {
			return false;
		}
	}

	/** Opens a content URI or registered doc ID and detaches its POSIX file descriptor. */
	static int openFd(Context ctx, String target, String mode) {
		if (ctx == null || target == null || mode == null) {
			return -1;
		}
		Uri uri = resolveTargetUri(target);
		if (uri == null) {
			return -1;
		}
		try {
			ParcelFileDescriptor pfd = ctx.getContentResolver().openFileDescriptor(uri, mode);
			if (pfd == null) {
				return -1;
			}
			return pfd.detachFd();
		} catch (FileNotFoundException | SecurityException e) {
			return -1;
		}
	}

	/** Deletes a MediaStore or SAF document URI. */
	static boolean deleteUri(Context ctx, String target) {
		if (ctx == null || target == null) {
			return false;
		}
		Uri uri = resolveTargetUri(target);
		if (uri == null) {
			return false;
		}
		try {
			if (DocumentsContract.isDocumentUri(ctx, uri)) {
				return DocumentsContract.deleteDocument(ctx.getContentResolver(), uri);
			}
		} catch (RuntimeException | FileNotFoundException e) {
			/* Fallback to ContentResolver.delete below. */
		}
		try {
			return ctx.getContentResolver().delete(uri, null, null) > 0;
		} catch (RuntimeException e) {
			return false;
		}
	}

	/** Stats a registered SAF upload document; returns [is_dir(0), size, mtime_sec] or null. */
	static long[] statSafDoc(Context ctx, String docId) {
		Uri uri;
		synchronized (LOCK) {
			uri = DOC_URIS.get(docId);
		}
		if (ctx == null || uri == null) {
			return null;
		}
		long size = 0L;
		Cursor c = null;
		try {
			c = ctx.getContentResolver().query(uri, new String[] { OpenableColumns.SIZE }, null, null, null);
			if (c != null && c.moveToFirst() && !c.isNull(0)) {
				size = c.getLong(0);
			}
		} catch (RuntimeException e) {
			return null;
		} finally {
			if (c != null) {
				c.close();
			}
		}
		return new long[] { 0L, size, 0L };
	}

	/** Stats an existing MediaStore.Downloads entry; returns [is_dir(0), size, mtime_sec] or null if absent. */
	static long[] statMediaStoreDownload(Context ctx, String relPath) {
		if (ctx == null || relPath == null || relPath.isEmpty()) {
			return null;
		}
		int slash = relPath.lastIndexOf('/');
		String name = (slash >= 0) ? relPath.substring(slash + 1) : relPath;
		String subDir = (slash > 0) ? relPath.substring(0, slash) : "";
		String expectedRel = Environment.DIRECTORY_DOWNLOADS + "/" + (subDir.isEmpty() ? "" : subDir + "/");
		String sel = MediaStore.Downloads.DISPLAY_NAME + "=? AND "
			+ MediaStore.Downloads.RELATIVE_PATH + "=? AND "
			+ MediaStore.Downloads.IS_PENDING + "=0";
		Cursor c = null;
		try {
			c = ctx.getContentResolver().query(
				MediaStore.Downloads.EXTERNAL_CONTENT_URI,
				new String[] { MediaStore.Downloads.SIZE, MediaStore.Downloads.DATE_MODIFIED },
				sel,
				new String[] { name, expectedRel },
				null);
			if (c != null && c.moveToFirst()) {
				long size = c.isNull(0) ? 0L : c.getLong(0);
				long mtime = c.isNull(1) ? 0L : c.getLong(1);
				return new long[] { 0L, size, mtime };
			}
		} catch (RuntimeException e) {
			return null;
		} finally {
			if (c != null) {
				c.close();
			}
		}
		return null;
	}

	/** Stats a path inside a SAF tree; returns [is_dir, size, mtime_sec] or null if absent. */
	static long[] statSafTree(Context ctx, String treeId, String relPath) {
		Uri treeUri = getTreeUri(treeId);
		if (ctx == null || treeUri == null) {
			return null;
		}
		if (relPath == null || relPath.isEmpty()) {
			return new long[] { 1L, 0L, 0L };
		}
		Uri docUri = findSafDocument(ctx, treeUri, relPath);
		if (docUri == null) {
			return null;
		}
		Cursor c = null;
		try {
			c = ctx.getContentResolver().query(
				docUri,
				new String[] {
					DocumentsContract.Document.COLUMN_MIME_TYPE,
					DocumentsContract.Document.COLUMN_SIZE,
					DocumentsContract.Document.COLUMN_LAST_MODIFIED
				},
				null, null, null);
			if (c != null && c.moveToFirst()) {
				String mime = c.getString(0);
				long isDir = DocumentsContract.Document.MIME_TYPE_DIR.equals(mime) ? 1L : 0L;
				long size = c.isNull(1) ? 0L : c.getLong(1);
				long mtime = c.isNull(2) ? 0L : (c.getLong(2) / 1000L);
				return new long[] { isDir, size, mtime };
			}
		} catch (RuntimeException e) {
			return null;
		} finally {
			if (c != null) {
				c.close();
			}
		}
		return null;
	}

	/** Creates a temporary document in a SAF tree directory and returns its document URI string. */
	static String createSafTemp(Context ctx, String treeId, String relDir, String finalName) {
		Uri treeUri = getTreeUri(treeId);
		if (ctx == null || treeUri == null || finalName == null || finalName.isEmpty()) {
			return null;
		}
		Uri parentUri = ensureSafDir(ctx, treeUri, relDir);
		if (parentUri == null) {
			return null;
		}
		String tempName = ".mikuterm.part." + finalName;
		try {
			Uri created = DocumentsContract.createDocument(
				ctx.getContentResolver(),
				parentUri,
				"application/octet-stream",
				tempName);
			return created != null ? created.toString() : null;
		} catch (RuntimeException | FileNotFoundException e) {
			return null;
		}
	}

	/** Commits a SAF temporary document by renaming it to finalName (replacing existing if needed). */
	static boolean commitSafTemp(Context ctx, String docUriString, String finalName) {
		if (ctx == null || docUriString == null || finalName == null) {
			return false;
		}
		try {
			Uri docUri = Uri.parse(docUriString);
			Uri renamed = DocumentsContract.renameDocument(ctx.getContentResolver(), docUri, finalName);
			return renamed != null;
		} catch (RuntimeException | FileNotFoundException e) {
			return false;
		}
	}

	/** Creates a directory (and parents) inside a SAF tree. */
	static boolean mkdirSafTree(Context ctx, String treeId, String relPath) {
		Uri treeUri = getTreeUri(treeId);
		if (ctx == null || treeUri == null) {
			return false;
		}
		return ensureSafDir(ctx, treeUri, relPath) != null;
	}

	/** Lists children of a SAF tree directory as flattened [name, is_dir, size, mtime_sec] strings. */
	static String[] listSafTree(Context ctx, String treeId, String relPath) {
		Uri treeUri = getTreeUri(treeId);
		if (ctx == null || treeUri == null) {
			return null;
		}
		Uri dirUri = (relPath == null || relPath.isEmpty())
			? DocumentsContract.buildDocumentUriUsingTree(treeUri, DocumentsContract.getTreeDocumentId(treeUri))
			: findSafDocument(ctx, treeUri, relPath);
		if (dirUri == null) {
			return null;
		}
		Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(
			treeUri,
			DocumentsContract.getDocumentId(dirUri));
		List<String> out = new ArrayList<String>();
		Cursor c = null;
		try {
			c = ctx.getContentResolver().query(
				childrenUri,
				new String[] {
					DocumentsContract.Document.COLUMN_DISPLAY_NAME,
					DocumentsContract.Document.COLUMN_MIME_TYPE,
					DocumentsContract.Document.COLUMN_SIZE,
					DocumentsContract.Document.COLUMN_LAST_MODIFIED
				},
				null, null, null);
			while (c != null && c.moveToNext()) {
				String name = c.getString(0);
				if (name == null || name.isEmpty()) {
					continue;
				}
				String mime = c.getString(1);
				boolean isDir = DocumentsContract.Document.MIME_TYPE_DIR.equals(mime);
				long size = c.isNull(2) ? 0L : c.getLong(2);
				long mtime = c.isNull(3) ? 0L : (c.getLong(3) / 1000L);
				out.add(name);
				out.add(isDir ? "1" : "0");
				out.add(Long.toString(size));
				out.add(Long.toString(mtime));
			}
		} catch (RuntimeException e) {
			return null;
		} finally {
			if (c != null) {
				c.close();
			}
		}
		return out.toArray(new String[0]);
	}

	private static Uri getTreeUri(String treeId) {
		synchronized (LOCK) {
			return TREE_URIS.get(treeId);
		}
	}

	private static Uri resolveTargetUri(String target) {
		if (target.startsWith("content://")) {
			return Uri.parse(target);
		}
		if (target.startsWith("saf-doc://")) {
			String rest = target.substring("saf-doc://".length());
			int slash = rest.indexOf('/');
			String id = (slash >= 0) ? rest.substring(0, slash) : rest;
			synchronized (LOCK) {
				return DOC_URIS.get(id);
			}
		}
		return null;
	}

	private static Uri findSafDocument(Context ctx, Uri treeUri, String relPath) {
		Uri cur = DocumentsContract.buildDocumentUriUsingTree(
			treeUri,
			DocumentsContract.getTreeDocumentId(treeUri));
		String[] parts = relPath.split("/");
		ContentResolver cr = ctx.getContentResolver();
		for (String part : parts) {
			if (part.isEmpty()) {
				continue;
			}
			Uri next = findChildByName(cr, treeUri, cur, part);
			if (next == null) {
				return null;
			}
			cur = next;
		}
		return cur;
	}

	private static Uri ensureSafDir(Context ctx, Uri treeUri, String relPath) {
		Uri cur = DocumentsContract.buildDocumentUriUsingTree(
			treeUri,
			DocumentsContract.getTreeDocumentId(treeUri));
		if (relPath == null || relPath.isEmpty()) {
			return cur;
		}
		ContentResolver cr = ctx.getContentResolver();
		String[] parts = relPath.split("/");
		for (String part : parts) {
			if (part.isEmpty()) {
				continue;
			}
			Uri next = findChildByName(cr, treeUri, cur, part);
			if (next == null) {
				try {
					next = DocumentsContract.createDocument(
						cr,
						cur,
						DocumentsContract.Document.MIME_TYPE_DIR,
						part);
				} catch (RuntimeException | FileNotFoundException e) {
					return null;
				}
			}
			if (next == null) {
				return null;
			}
			cur = next;
		}
		return cur;
	}

	private static Uri findChildByName(ContentResolver cr, Uri treeUri, Uri parentDocUri, String name) {
		Uri childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(
			treeUri,
			DocumentsContract.getDocumentId(parentDocUri));
		Cursor c = null;
		try {
			c = cr.query(
				childrenUri,
				new String[] {
					DocumentsContract.Document.COLUMN_DOCUMENT_ID,
					DocumentsContract.Document.COLUMN_DISPLAY_NAME
				},
				null, null, null);
			while (c != null && c.moveToNext()) {
				String docId = c.getString(0);
				String disp = c.getString(1);
				if (name.equals(disp) && docId != null) {
					return DocumentsContract.buildDocumentUriUsingTree(treeUri, docId);
				}
			}
		} catch (RuntimeException e) {
			return null;
		} finally {
			if (c != null) {
				c.close();
			}
		}
		return null;
	}
}

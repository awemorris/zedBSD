/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

package io.noctvm.mikuterm;

import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;

import java.io.IOException;
import java.security.GeneralSecurityException;
import java.security.KeyStore;
import java.util.Arrays;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/**
 * Keeps passphrases secret on the disk: the Keystore's AES-GCM key (made on first use, it never
 * leaves the Keystore) wraps a passphrase into 12 bytes of IV followed by the cipher text and its tag.
 *
 * The work runs one request at a time on a thread of its own and every answer goes to the native
 * code with the request's number (null bytes when it failed), so the native thread never waits.
 * The caller's array of a passphrase is wiped when it has been used.
 */
final class Secrets {
	/** The name of the Keystore's key. */
	private static final String ALIAS = "mikuterm-passphrase";

	/** The IV's length and the tag's length in bits. */
	private static final int IV_LENGTH = 12;
	private static final int TAG_BITS = 128;

	/** The thread the requests run on, in the order they came. */
	private static final ExecutorService WORKER = Executors.newSingleThreadExecutor();

	private Secrets() {
	}

	/** Wraps plain, and answers the request with IV followed by the cipher text. */
	static void wrap(final int request, final byte[] plain) {
		WORKER.execute(new Runnable() {
			@Override
			public void run() {
				byte[] result = null;

				try {
					result = encrypt(plain);
				} catch (GeneralSecurityException | IOException | RuntimeException e) {
					result = null;
				} finally {
					Arrays.fill(plain, (byte) 0);
				}
				MikutermBridge.nativeSecret(request, result);
			}
		});
	}

	/** Opens wrapped (IV followed by the cipher text), and answers the request with the passphrase. */
	static void unwrap(final int request, final byte[] wrapped) {
		WORKER.execute(new Runnable() {
			@Override
			public void run() {
				byte[] result = null;

				try {
					result = decrypt(wrapped);
				} catch (GeneralSecurityException | IOException | RuntimeException e) {
					result = null;
				}
				MikutermBridge.nativeSecret(request, result);
				if (result != null) {
					Arrays.fill(result, (byte) 0);
				}
			}
		});
	}

	/** The Keystore's key, made when there is none. */
	private static SecretKey key() throws GeneralSecurityException, IOException {
		KeyStore store;
		KeyGenerator generator;

		store = KeyStore.getInstance("AndroidKeyStore");
		store.load(null);
		if (!store.containsAlias(ALIAS)) {
			generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
			generator.init(new KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
				.setBlockModes(KeyProperties.BLOCK_MODE_GCM)
				.setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
				.setKeySize(256)
				.setUserAuthenticationRequired(false)
				.build());
			return generator.generateKey();
		}
		return (SecretKey) store.getKey(ALIAS, null);
	}

	/** IV followed by the cipher text of plain. */
	private static byte[] encrypt(byte[] plain) throws GeneralSecurityException, IOException {
		Cipher cipher;
		byte[] iv;
		byte[] body;
		byte[] out;

		cipher = Cipher.getInstance("AES/GCM/NoPadding");
		cipher.init(Cipher.ENCRYPT_MODE, key());
		iv = cipher.getIV();
		body = cipher.doFinal(plain);
		out = new byte[iv.length + body.length];
		System.arraycopy(iv, 0, out, 0, iv.length);
		System.arraycopy(body, 0, out, iv.length, body.length);
		return out;
	}

	/** The passphrase of IV followed by the cipher text. */
	private static byte[] decrypt(byte[] wrapped) throws GeneralSecurityException, IOException {
		Cipher cipher;

		if (wrapped == null || wrapped.length <= IV_LENGTH) {
			throw new GeneralSecurityException("too short");
		}
		cipher = Cipher.getInstance("AES/GCM/NoPadding");
		cipher.init(Cipher.DECRYPT_MODE, key(), new GCMParameterSpec(TAG_BITS, wrapped, 0, IV_LENGTH));
		return cipher.doFinal(wrapped, IV_LENGTH, wrapped.length - IV_LENGTH);
	}
}

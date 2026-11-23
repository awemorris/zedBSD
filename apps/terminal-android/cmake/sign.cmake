# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# The signing of the apk.  MIKUTERM_KEYSTORE names the key store to sign with and
# MIKUTERM_KEY_ALIAS the key in it; the store's password is read from the environment
# variable MIKUTERM_KS_PASS (it is never on a command line).  With no key store named, a
# debug key is made in the build folder, once, and the apk is signed with it: such an apk
# installs for trying the program, and each build folder has a key of its own.
#

set(MIKUTERM_KEYSTORE "" CACHE FILEPATH "The key store to sign the apk with (empty: a debug key in the build folder)")
set(MIKUTERM_KEY_ALIAS "mikuterm-debug" CACHE STRING "The key's alias in the key store")

if(MIKUTERM_KEYSTORE)
	set(MIKUTERM_SIGN_KEYSTORE ${MIKUTERM_KEYSTORE})
	set(MIKUTERM_SIGN_PASS env:MIKUTERM_KS_PASS)
	set(MIKUTERM_KEYSTORE_OUTPUT "")
else()
	set(MIKUTERM_SIGN_KEYSTORE ${CMAKE_BINARY_DIR}/debug.keystore)
	set(MIKUTERM_SIGN_PASS pass:android)
	set(MIKUTERM_KEYSTORE_OUTPUT ${MIKUTERM_SIGN_KEYSTORE})
	add_custom_command(
		OUTPUT ${MIKUTERM_SIGN_KEYSTORE}
		COMMAND ${KEYTOOL} -genkeypair -keystore ${MIKUTERM_SIGN_KEYSTORE}
			-alias ${MIKUTERM_KEY_ALIAS} -keyalg RSA -keysize 2048 -validity 10000
			-storepass android -keypass android -dname "CN=mikuterm debug"
		COMMENT "Making the debug key"
		VERBATIM)
endif()

set(MIKUTERM_APK ${CMAKE_BINARY_DIR}/mikuterm-${PROJECT_VERSION}.apk)
add_custom_target(apk ALL
	COMMAND ${APKSIGNER} sign --ks ${MIKUTERM_SIGN_KEYSTORE} --ks-key-alias ${MIKUTERM_KEY_ALIAS}
		--ks-pass ${MIKUTERM_SIGN_PASS} --v2-signing-enabled true --v3-signing-enabled true
		--out ${MIKUTERM_APK} ${MIKUTERM_ALIGNED_APK}
	COMMAND ${APKSIGNER} verify ${MIKUTERM_APK}
	COMMAND ${CMAKE_COMMAND} -DAPK=${MIKUTERM_APK} -P ${CMAKE_CURRENT_LIST_DIR}/write-sha256.cmake
	DEPENDS ${MIKUTERM_KEYSTORE_OUTPUT}
	COMMENT "Signing the apk"
	VERBATIM)
add_dependencies(apk aligned-apk)

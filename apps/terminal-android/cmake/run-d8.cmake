# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Runs d8 on every .class file under CLASSES_DIR (a command line cannot name them with a
# wildcard, and the list is only known when this runs).  Given with -D: CLASSES_DIR, D8,
# ANDROID_JAR and OUT_DIR, the folder that receives classes.dex.
#

file(GLOB_RECURSE classes ${CLASSES_DIR}/*.class)
execute_process(
	COMMAND ${D8} --release --min-api 28 --lib ${ANDROID_JAR} --output ${OUT_DIR} ${classes}
	RESULT_VARIABLE result)
if(NOT result EQUAL 0)
	message(FATAL_ERROR "d8 failed (${result})")
endif()

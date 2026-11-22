# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Fetches one file and checks it: cmake -DURL=<address> -DSHA256=<hash> -DOUTPUT=<path>
# [-DLOCAL=<a copy already downloaded>] -P fetch.cmake.  The local copy is taken when it is
# there (read, never changed), the address otherwise; either way the file must have the SHA-256,
# and only then is it put at OUTPUT.
#

if(NOT URL OR NOT SHA256 OR NOT OUTPUT)
	message(FATAL_ERROR "fetch.cmake needs URL, SHA256 and OUTPUT")
endif()

set(part ${OUTPUT}.part)
get_filename_component(directory ${OUTPUT} DIRECTORY)
file(MAKE_DIRECTORY ${directory})

# The local copy, or the download.
if(LOCAL AND EXISTS ${LOCAL})
	file(COPY_FILE ${LOCAL} ${part})
else()
	file(DOWNLOAD ${URL} ${part} TLS_VERIFY ON STATUS status)
	list(GET status 0 code)
	if(NOT code EQUAL 0)
		message(FATAL_ERROR "${URL}: the download failed: ${status}")
	endif()
endif()

# The check, before the file is given its name.
file(SHA256 ${part} found)
if(NOT found STREQUAL SHA256)
	message(FATAL_ERROR "${OUTPUT}: the SHA-256 is ${found}, not ${SHA256}")
endif()
file(RENAME ${part} ${OUTPUT})

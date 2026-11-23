# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Makes the folder of the apk's assets: the program's read-only data under share/ (each file as
# "name=source" in -DFILES), share.list (the names, one a line) and share.version (the program's
# version and a hash of the files' contents, so a changed file is unpacked again by the program).
# The folder is -DOUT=<dir>, made again each time.
#

file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT}/share)
set(names "")
set(hashes "")
foreach(pair IN LISTS FILES)
	string(REPLACE "=" ";" parts "${pair}")
	list(GET parts 0 name)
	list(GET parts 1 source)
	get_filename_component(directory ${OUT}/share/${name} DIRECTORY)
	file(MAKE_DIRECTORY ${directory})
	file(COPY_FILE ${source} ${OUT}/share/${name})
	file(SHA256 ${source} hash)
	string(APPEND names "${name}\n")
	string(APPEND hashes "${name} ${hash}\n")
endforeach()
file(WRITE ${OUT}/share.list "${names}")
string(SHA256 all "${hashes}")
string(SUBSTRING ${all} 0 16 short)
file(WRITE ${OUT}/share.version "${VERSION}-${short}")

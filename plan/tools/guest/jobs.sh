# The number of parallel jobs of the image and test builds, in one place.  It is 4: the host runs several agents'
# builds and tests at once, and each keeps to 4 threads (2026-11-12, the user's rule; it replaces the earlier 16).
# Sourced, from the repository's root, by the scripts that run make themselves; ZEDBSD_JOBS in the environment
# overrides it for one run.  A script that runs make passes it on as BUILD_JOBS too, so that the build tools make
# runs (cmake and the packages' own makes, mk/build-jobs.mk) keep to it.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
ZEDBSD_JOBS=${ZEDBSD_JOBS:-4}

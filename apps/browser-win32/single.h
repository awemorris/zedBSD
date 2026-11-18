/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * One OmniGrammaton at a time: a second start hands its URLs to the
 * window of the first and ends.
 */

#ifndef OMNIGRAMMATON_SINGLE_H
#define OMNIGRAMMATON_SINGLE_H

/*
 * Takes the program's place, or hands the command line's URLs (the words
 * that are not options; a file's path is made absolute) to the window of
 * the program already running.  Returns 1 when they were handed over (the
 * program then ends), 0 when this is the first start (or no window took
 * them: the program goes on).
 */
int single_start(int argc, char **argv);

#endif

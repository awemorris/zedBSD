/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Exit statuses with an agreed meaning.
 *
 * A program that fails can say only a number, and a number on its own says
 * nothing about what went wrong.  These are the names the BSDs gave to a
 * small set of them, so that a caller reading a status can tell a mistake
 * in the command line from a file that could not be opened from a host
 * that could not be reached.  Nothing enforces them; they are a convention,
 * and their value is that portable software already follows it.
 *
 * The range is deliberately above the statuses a program is likely to
 * choose for itself and below the ones a shell reserves for signals.
 */

#ifndef LIBC_SYSEXITS_H
#define LIBC_SYSEXITS_H

/* Nothing went wrong. */
#define EX_OK		0

/* The lowest and highest of the named statuses, for a caller that checks. */
#define EX_BASE		64
#define EX__BASE	64
#define EX__MAX		78

/* The command line was wrong: an unknown option, or the wrong number of them. */
#define EX_USAGE	64

/* The input was the right kind of thing but wrong in itself. */
#define EX_DATAERR	65

/* An input file did not exist, or could not be read. */
#define EX_NOINPUT	66

/* A user was named who does not exist. */
#define EX_NOUSER	67

/* A host was named who does not exist. */
#define EX_NOHOST	68

/* The service asked for is not available. */
#define EX_UNAVAILABLE	69

/* The program found itself in a state it does not allow for. */
#define EX_SOFTWARE	70

/* Something failed that belongs to the system rather than to the program. */
#define EX_OSERR	71

/* A file the system is expected to have is missing or wrong. */
#define EX_OSFILE	72

/* An output file could not be created. */
#define EX_CANTCREAT	73

/* A read or a write failed. */
#define EX_IOERR	74

/*
 * Nothing is wrong that will still be wrong later; the work may be tried
 * again.  A queue that holds the request rather than discarding it reports
 * this.
 */
#define EX_TEMPFAIL	75

/* The other end of a protocol did something the protocol does not allow. */
#define EX_PROTOCOL	76

/* Permission was refused, and not for a file. */
#define EX_NOPERM	77

/* Something is configured wrongly. */
#define EX_CONFIG	78

#endif

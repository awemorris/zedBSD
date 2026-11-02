// -*- coding: utf-8; tab-width: 8; indent-tabs-mode: t; -*-

/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The C++ part of a library of the dynamic loader's exit-handler test
 * (rtld-finalize.c, finalize-lib.c).  A static object with a destructor:
 * the compiler registers the destructor at the library's initialization
 * with __cxa_atexit(destructor, object, &__dso_handle), so the destructor
 * runs when the library is unloaded, before the library's own destructor,
 * or at exit for a library that stays.
 */

#ifndef FINALIZE_TAG
#error FINALIZE_TAG names the library in its records
#endif

// The program's record of what ran (rtld-finalize.c).
extern "C" void finalize_record(const char *event);

namespace {

// Records its destruction.
class Recorder {
public:
	explicit Recorder(const char *event);
	~Recorder();

private:
	const char *event_;
};

// Keeps the record the destructor gives.
Recorder::Recorder(
	const char *event)
	: event_(event)
{
}

// Records that the static object was destroyed.
Recorder::~Recorder()
{
	finalize_record(event_);
}

// The static object, constructed at the library's initialization.
Recorder recorder(FINALIZE_TAG " c++-static");

}

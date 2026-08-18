/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#ifndef ZEDBSD_USERLAND_SH_VARS_H
#define ZEDBSD_USERLAND_SH_VARS_H

int sh_var_name(const char *);
const char *sh_var_get(const char *);
int sh_var_set(const char *, const char *, int);
int sh_var_export(const char *);
int sh_var_readonly(const char *);
int sh_var_unset(const char *);

struct sh_var_snapshot {
	char *name;
	char *value;
	char *environment;
	int existed;
	int environment_existed;
	int exported;
	int readonly;
};

int sh_var_snapshot(const char *, struct sh_var_snapshot *);
int sh_var_restore(struct sh_var_snapshot *);

#endif

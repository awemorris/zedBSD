/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <readline/history.h>
#include <readline/readline.h>

#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *
edit(const char *input)
{
	int descriptors[2];
	int saved_input = dup(STDIN_FILENO);
	int saved_output = dup(STDOUT_FILENO);
	int null_output = open("/dev/null", O_WRONLY);
	char *result;
	assert(saved_input >= 0 && saved_output >= 0 && null_output >= 0);
	assert(pipe(descriptors) == 0);
	assert(write(descriptors[1], input, strlen(input)) ==
	    (ssize_t)strlen(input));
	close(descriptors[1]);
	assert(dup2(descriptors[0], STDIN_FILENO) == STDIN_FILENO);
	assert(dup2(null_output, STDOUT_FILENO) == STDOUT_FILENO);
	close(descriptors[0]);
	close(null_output);
	result = readline("test> ");
	assert(dup2(saved_input, STDIN_FILENO) == STDIN_FILENO);
	assert(dup2(saved_output, STDOUT_FILENO) == STDOUT_FILENO);
	close(saved_input);
	close(saved_output);
	return result;
}

int
main(void)
{
	char *line;
	using_history();
	line = edit("abc\033[DX\n");
	assert(line != NULL && strcmp(line, "abXc") == 0);
	add_history(line);
	free(line);
	line = edit("\033[A\001Z\005!\n");
	assert(line != NULL && strcmp(line, "ZabXc!") == 0);
	free(line);
	clear_history();
	return 0;
}

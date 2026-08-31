ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
BUILD := $(ROOT)/build/ws001-p015-host
TEST := $(BUILD)/unix-socket-publication-contract-test

.PHONY: run
run: $(TEST)
	cd $(ROOT) && $(TEST)

$(TEST): $(ROOT)/plan/ws001-posix/tests/unix-socket-publication-contract-test.c
	mkdir -p $(BUILD)
	$(CC) -std=c11 -O0 -Wall -Wextra -Werror $< -o $@

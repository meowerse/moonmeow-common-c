# Standalone tests for src/MeowProtocol.h. Not part of the library build.
#
#   make -C tests/meow                 # build with ASan/UBSan and run
#   make -C tests/meow SANITIZE=       # without sanitizers
#   make -C tests/meow BUILD_DIR=/var/tmp/meow-tests
#
# The codecs are header-only, so this compiles one file and links nothing from
# the library.

CC ?= cc
BUILD_DIR ?= build
SANITIZE ?= -fsanitize=address,undefined -fno-sanitize-recover=all
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter
SRC_DIR := $(dir $(lastword $(MAKEFILE_LIST)))../../src

TEST := $(BUILD_DIR)/test_meow_protocol

.PHONY: all run clean

all: run

$(TEST): test_meow_protocol.c $(SRC_DIR)/MeowProtocol.h $(SRC_DIR)/Limelight.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SANITIZE) -I$(SRC_DIR) -o $@ test_meow_protocol.c

run: $(TEST)
	$(TEST)

clean:
	rm -rf $(BUILD_DIR)

# RoomCheck - build system
#
# Targets:
#   make dev            local development build (glibc, dynamic)
#   make static         fully static musl build (musl-gcc)
#   make static-arm64   cross build with aarch64-linux-musl-gcc
#   make objs           compile application objects only
#   make test           run test suite (builds dev first)
#   make clean

CC ?= cc
AR ?= ar
CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=c11 -D_GNU_SOURCE
CPPFLAGS ?= -Ibackend/src -Ithird_party/sqlite
LDFLAGS ?=
LIBS ?= -lpthread -lm

BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
BIN := $(BUILD_DIR)/RoomCheck

APP_SRC := $(wildcard backend/src/*.c)
APP_OBJ := $(APP_SRC:backend/src/%.c=$(OBJ_DIR)/%.o)

SQLITE_SRC := third_party/sqlite/sqlite3.c
SQLITE_OBJ := $(OBJ_DIR)/sqlite3.o
SQLITE_FLAGS := -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
	-DSQLITE_DQS=0 -DSQLITE_DEFAULT_FOREIGN_KEYS=1 \
	-DSQLITE_ENABLE_COLUMN_METADATA=0 -DSQLITE_OMIT_DEPRECATED=0 \
	-DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
	-DSQLITE_LIKE_DOESNT_MATCH_BLOBS=1 -DSQLITE_MAX_VARIABLE_NUMBER=256

.PHONY: all dev static static-arm64 objs clean test

all: dev

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

objs: $(APP_OBJ)

$(OBJ_DIR)/%.o: backend/src/%.c backend/src/common.h | $(OBJ_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(SQLITE_OBJ): $(SQLITE_SRC) | $(OBJ_DIR)
	mkdir -p $(OBJ_DIR)/sqlite
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SQLITE_FLAGS) -c $< -o $@

dev: $(BIN)

$(BIN): $(APP_OBJ) $(SQLITE_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) $(APP_OBJ) $(SQLITE_OBJ) $(LIBS) -o $@

static:
	$(MAKE) clean
	$(MAKE) CC=musl-gcc LDFLAGS="-static -s -no-pie" $(BIN)
	@echo "Static build: $(BIN)"

static-arm64:
	$(MAKE) clean
	$(MAKE) CC=aarch64-linux-musl-gcc LDFLAGS="-static -s -no-pie" $(BIN)
	@echo "Static arm64 build: $(BIN)"

test: dev
	./tests/run_tests.sh

clean:
	rm -rf $(BUILD_DIR)

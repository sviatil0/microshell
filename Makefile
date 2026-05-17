# microshell - small but real Unix shell in C
# ----------------------------------------------------------------------
# Targets:
#   make all       -> ./microshell
#   make test      -> run tests/*.sh
#   make install   -> copy binary to $(PREFIX)/bin (default /usr/local)
#   make clean     -> remove build artifacts

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE
LDFLAGS ?=
PREFIX  ?= /usr/local

SRC := \
	src/shell.c    \
	src/parser.c   \
	src/executor.c \
	src/builtins.c \
	src/jobs.c     \
	src/history.c

OBJ := $(SRC:.c=.o)
BIN := microshell

.PHONY: all clean test install distclean

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

test: $(BIN)
	@echo "==> running tests/test_parser.sh"
	@MSH=./$(BIN) bash tests/test_parser.sh
	@echo "==> running tests/integration.sh"
	@MSH=./$(BIN) bash tests/integration.sh
	@echo "==> all tests passed"

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(OBJ) $(BIN)

distclean: clean
	rm -f tests/.out tests/.err tests/.in

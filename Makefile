CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror
CPPFLAGS += -Iinclude
LDFLAGS ?=
LDLIBS  ?=
PREFIX  ?= /usr/local

SRC     := src/chcl.c src/main.c
OBJ     := $(SRC:.c=.o)
BIN     := chcl

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c include/chcl.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(OBJ) $(BIN) $(BIN).exe tests/test_core tests/test_core.exe

tests/test_core: tests/test_core.c src/chcl.c include/chcl.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/test_core.c $(LDLIBS)

test: $(BIN) tests/test_core
	./tests/test_core
	bash tests/test_basic.sh

.PHONY: all clean install test

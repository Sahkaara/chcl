CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Iinclude
PREFIX  ?= /usr/local

SRC     := src/chcl.c src/main.c
OBJ     := $(SRC:.c=.o)
BIN     := chcl

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ)

%.o: %.c include/chcl.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)

clean:
	rm -f $(OBJ) $(BIN)

test: $(BIN)
	bash tests/test_basic.sh

.PHONY: all clean install test

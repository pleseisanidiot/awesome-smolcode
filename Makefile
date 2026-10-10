CC = cc
CPPFLAGS ?=
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS = -lcurl
PREFIX ?= /usr/local
PYTHON ?= python3
OBJECTS = main.o agent.o api.o tools.o json.o session.o

.PHONY: all clean install check check-python check-all
all: smolcode

smolcode: $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(OBJECTS): smolcode.h json.h config.h

clean:
	rm -f smolcode $(OBJECTS)

install: smolcode
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 smolcode $(DESTDIR)$(PREFIX)/bin/smolcode

check:
	CC="$(CC)" sh tests/run.sh

check-python:
	$(PYTHON) -m unittest discover -s tests_python -v

check-all: check check-python

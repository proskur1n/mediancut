CFLAGS := -Wall -Wextra -g
LIBS := -lm
PREFIX := /usr/local

all: mediancut

mediancut: main.c Makefile
	$(CC) -o $@ $(CFLAGS) main.c $(LIBS)

release: CFLAGS += -O2
release: clean
release: mediancut

install:
	cp mediancut $(PREFIX)/bin

uninstall:
	rm -f $(PREFIX)/bin/mediancut

clean:
	rm -f mediancut

.PHONY: all release install uninstall clean

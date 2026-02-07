CC = cc
CFLAGS = -Wall -Wextra -O2
FRAMEWORKS = -framework IOKit -framework CoreFoundation
PREFIX ?= /usr/local
VERSION ?= dev

coke: coke.c
	$(CC) $(CFLAGS) -DCOKE_VERSION='"$(VERSION)"' -o $@ $< $(FRAMEWORKS)

install: coke
	install -d $(PREFIX)/bin
	install -m 755 coke $(PREFIX)/bin/coke

uninstall:
	rm -f $(PREFIX)/bin/coke

clean:
	rm -f coke

.PHONY: install uninstall clean

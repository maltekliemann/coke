CC = cc
CFLAGS = -Wall -Wextra -O2
FRAMEWORKS = -framework IOKit -framework CoreFoundation -framework CoreGraphics
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

test:
	@set -eu; scratch=$$(mktemp -d); trap 'rm -rf "$$scratch"' EXIT; \
	$(CC) $(CFLAGS) -o "$$scratch/coke-test" tests/fake_power.c $(FRAMEWORKS); \
	python3 tests/test_coke.py "$$scratch/coke-test"

.PHONY: install uninstall clean test

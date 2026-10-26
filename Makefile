# Build:   make
# Install: make install   (per-user, ~/.config/geany/plugins)
CFLAGS  ?= -O2 -g
CFLAGS  += -fPIC -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags geany)
LDLIBS  += $(shell pkg-config --libs geany)
PLUGDIR ?= $(HOME)/.config/geany/plugins

favorites.so: favorites.c
	$(CC) $(CFLAGS) -shared -o $@ $< $(LDFLAGS) $(LDLIBS)

install: favorites.so
	install -Dm644 favorites.so $(PLUGDIR)/favorites.so

uninstall:
	rm -f $(PLUGDIR)/favorites.so

clean:
	rm -f favorites.so

.PHONY: install uninstall clean

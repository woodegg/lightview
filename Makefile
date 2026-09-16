CC ?= cc
PKG_CONFIG ?= pkg-config
PREFIX ?= /usr/local
PKGS = webkit2gtk-4.1 json-glib-1.0 gio-unix-2.0
ICON_SIZES = 16 24 32 48 64 128 256
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKGS))
CFLAGS ?= -Os -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Wformat=2
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS))

.PHONY: all check-deps check install clean
all: build/lightview

check-deps:
	@$(PKG_CONFIG) --atleast-version=2.40 webkit2gtk-4.1 || { echo 'Install WebKitGTK 4.1 development files (engine >= 2.40).'; exit 1; }
	@$(PKG_CONFIG) --exists $(PKGS)

build/lightview: src/lightview.c build/lightview-resources.o | check-deps
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/lightview.c build/lightview-resources.o $(LDLIBS)

build/lightview-resources.o: build/lightview-resources.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -Wno-overlength-strings -c -o $@ $<

build/lightview-resources.c: data/lightview.gresource.xml data/lightview.png
	mkdir -p build
	glib-compile-resources --sourcedir=data --generate-source --target=$@ $<

check: build/lightview
	dbus-run-session -- xvfb-run -a python3 tests/integration.py

install: build/lightview
	install -Dm755 build/lightview $(DESTDIR)$(PREFIX)/bin/lightview
	install -Dm755 tools/lightviewctl $(DESTDIR)$(PREFIX)/bin/lightviewctl
	install -Dm755 tools/lightview-memory $(DESTDIR)$(PREFIX)/bin/lightview-memory
	install -Dm644 data/lightview.desktop $(DESTDIR)$(PREFIX)/share/applications/lightview.desktop
	@for size in $(ICON_SIZES); do \
		install -Dm644 data/icons/lightview-$$size.png \
			$(DESTDIR)$(PREFIX)/share/icons/hicolor/$$size\x$$size/apps/lightview.png; \
	done
	install -Dm644 data/lightview.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/512x512/apps/lightview.png

clean:
	$(RM) build/lightview build/lightview-resources.c build/lightview-resources.o

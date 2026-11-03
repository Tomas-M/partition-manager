CC?=cc
PKGS=gtk+-3.0 json-glib-1.0 gio-unix-2.0
CPPFLAGS+=$(shell pkg-config --cflags $(PKGS))
CFLAGS?=-O2 -g
CFLAGS+=-std=c11 -Wall -Wextra -Wformat=2 -Wshadow -Wno-deprecated-declarations
LDLIBS+=$(shell pkg-config --libs $(PKGS)) -lm
PREFIX?=/usr

all: build/partition-manager

build:
	mkdir -p $@

build/resources.c: resources.xml style.css app-icon.png $(wildcard icons/*.svg) | build
	glib-compile-resources resources.xml --generate-source --target=$@ --sourcedir=.

build/partition-manager: main.c ui.c model.c engine.c partition-manager.h build/resources.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ main.c ui.c model.c engine.c build/resources.c $(LDFLAGS) $(LDLIBS)

build/partition-manager-test: tests.c model.c engine.c partition-manager.h | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests.c model.c engine.c $(LDFLAGS) $(LDLIBS)

build/partition-manager-ui-test: ui-tests.c ui.c model.c engine.c partition-manager.h build/resources.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ ui-tests.c ui.c model.c engine.c build/resources.c $(LDFLAGS) $(LDLIBS)

check: build/partition-manager-test
	./build/partition-manager-test

install: build/partition-manager
	install -Dm755 build/partition-manager $(DESTDIR)$(PREFIX)/bin/partition-manager
	install -Dm644 partition-manager.desktop $(DESTDIR)$(PREFIX)/share/applications/partition-manager.desktop
	install -Dm644 app-icon.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/128x128/apps/slax-partition-manager.png
	install -Dm644 icons/LICENSE.txt $(DESTDIR)$(PREFIX)/share/partition-manager/LICENSE-font-awesome.txt

clean:
	rm -rf build

.PHONY: all check install clean

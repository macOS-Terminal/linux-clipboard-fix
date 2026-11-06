# cbfix-bridge — Makefile
#
# Depends on: libwayland-client, libX11, libXfixes, wayland-scanner
# Debian/Ubuntu : sudo apt install build-essential libwayland-dev wayland-protocols \
#                                libx11-dev libxfixes-dev pkg-config
# Arch          : sudo pacman -S base-devel wayland wayland-protocols libx11 libxfixes
# Fedora        : sudo dnf install gcc make wayland-devel wayland-protocols-devel \
#                                libX11-devel libXfixes-devel pkgconf

CC      ?= cc
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
CFLAGS  ?= -O2 -g
CFLAGS  += -Wall -Wextra -Wno-unused-parameter -std=gnu11
PKGS     = wayland-client x11 xfixes

BUILD    = build
TARGET   = $(BUILD)/cbfix-bridge
PROTO_XML = protocols/ext-data-control-v1.xml
PROTO_H   = $(BUILD)/ext-data-control-v1-client-protocol.h
PROTO_C   = $(BUILD)/ext-data-control-v1-client-protocol.c
SRC       = src/cbfix-bridge.c

PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS))

.PHONY: all clean install uninstall check deps

all: $(TARGET)

$(BUILD):
	@mkdir -p $(BUILD)

$(PROTO_H): $(PROTO_XML) | $(BUILD)
	wayland-scanner client-header $< $@

$(PROTO_C): $(PROTO_XML) | $(BUILD)
	wayland-scanner private-code $< $@

$(TARGET): $(SRC) $(PROTO_C) $(PROTO_H) | $(BUILD)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -I$(BUILD) -o $@ $(SRC) $(PROTO_C) $(PKG_LIBS)

deps:
	@echo "--- required pkg-config modules ---"
	@for m in $(PKGS); do \
		if pkg-config --exists $$m; then echo "  OK      $$m $$(pkg-config --modversion $$m)"; \
		else echo "  MISSING $$m"; fi; done
	@printf "  %-7s wayland-scanner: " ""; command -v wayland-scanner || echo MISSING
	@printf "  %-7s protocol xml   : " ""; test -f $(PROTO_XML) && echo OK || echo MISSING

check: $(TARGET)
	./$(TARGET) --selftest

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/cbfix-bridge

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/cbfix-bridge

clean:
	rm -rf $(BUILD)

test: $(TARGET) build/x11-read
	bash tests/run-tests.sh
	bash tests/run-bulk-tests.sh

build/x11-read: tests/x11-read.c | $(BUILD)
	$(CC) -O2 -Wall -o $@ $< -lX11

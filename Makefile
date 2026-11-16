# lambdock -- hyper-hackable desktop dock for Wayland
#
# SPDX-License-Identifier: MIT
# Copyright © Josep Bigorra <jjbigorra@gmail.com>
#
# A shim, for the habits and the recipes that still type `make`. Everything here
# is cargo: the build is `cargo build`, the suite is `cargo test`, and the three
# jobs cargo does not do (install, translations, the source archive) are
# `cargo xtask`.

PREFIX  ?= /usr/local
DESTDIR ?=
CARGO   ?= cargo
FLAGS   ?= --offline --locked

# Compiled in: where the Scheme modules and the catalogues land.
export LAMBDOCK_SCHEME_DIR = $(PREFIX)/share/guile/site/3.0
export LAMBDOCK_LOCALEDIR = $(PREFIX)/share/locale

.PHONY: all build test check install uninstall i18n dist clean

all: build

build:
	$(CARGO) build $(FLAGS) --release

test:
	$(CARGO) test $(FLAGS)

check: test
	$(CARGO) clippy $(FLAGS) --all-targets

install: build
	$(CARGO) run $(FLAGS) -p xtask -- install --prefix "$(PREFIX)" --destdir "$(DESTDIR)"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/lambdock"
	rm -rf "$(DESTDIR)$(PREFIX)/share/guile/site/3.0/lambdock"

i18n:
	$(CARGO) run $(FLAGS) -p xtask -- i18n --prefix "$(PREFIX)" --destdir "$(DESTDIR)"

dist:
	$(CARGO) run $(FLAGS) -p xtask -- dist

clean:
	$(CARGO) clean

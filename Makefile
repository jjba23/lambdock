# lambdock -- hyper-hackable desktop dock for Wayland
#
# SPDX-License-Identifier: MIT
# Copyright © Josep Bigorra <jjbigorra@gmail.com>
#
# The whole build, in one file. `cargo` produces the binary -- resources, icons
# and the Wayland protocol interfaces included, through `rust/build.rs` -- and
# everything around it (translations, the desktop entry, the AppStream metainfo,
# the Scheme modules) is installed from the tree by the targets below.
#
# Nothing here assembles a command out of data: every step is a program with its
# arguments.

PREFIX        ?= /usr/local
DESTDIR       ?=
BINDIR        ?= $(PREFIX)/bin
DATADIR       ?= $(PREFIX)/share
APPDATADIR    ?= $(DATADIR)
SCHEMEDIR     ?= $(DATADIR)/guile/site/3.0
APPLICATIONID ?= org.jointhefreeworld.lambdock
VERSION       := $(shell sed -n 's/^version = "\(.*\)"/\1/p' rust/Cargo.toml)
CARGO         ?= cargo
CARGOFLAGS    ?= --offline --locked
TARGETDIR     ?= rust/target
BINARY        := $(TARGETDIR)/release/lambdock

# The X11 half is a build option: a GTK built for Wayland alone has no Xlib.
LAMBDOCK_HAVE_X11 ?= 1
export LAMBDOCK_HAVE_X11

.PHONY: all build test check install uninstall i18n clean dist

all: build

build:
	$(CARGO) build $(CARGOFLAGS) --release --manifest-path rust/Cargo.toml

# The suite needs no display: every test that would is a decision test.
test:
	$(CARGO) test $(CARGOFLAGS) --manifest-path rust/Cargo.toml

# What CI should run before a commit: the suite, the Scheme suite, and the lints.
check: test
	$(CARGO) clippy $(CARGOFLAGS) --all-targets --manifest-path rust/Cargo.toml
	sh tests/run_scheme_tests.sh

# Translations: po/*.po -> <lang>/LC_MESSAGES/lambdock.mo. `make i18n` also
# refreshes the template from the sources, so the catalogues stay in step.
i18n:
	cd po && xgettext --from-code=UTF-8 --language=C --keyword=tr:1 \
	  --keyword=trn:1,2 --add-comments=TRANSLATORS \
	  --output=lambdock.pot --files-from=POTFILES.in
	cd po && for po in *.po; do \
	  lang=$${po%.po}; \
	  mkdir -p "$(DESTDIR)$(DATADIR)/locale/$$lang/LC_MESSAGES"; \
	  msgfmt "$$po" -o "$(DESTDIR)$(DATADIR)/locale/$$lang/LC_MESSAGES/lambdock.mo"; \
	done

install: build
	install -Dm755 $(BINARY) "$(DESTDIR)$(BINDIR)/lambdock"
	install -Dm644 data/$(APPLICATIONID).desktop \
	  "$(DESTDIR)$(APPDATADIR)/applications/$(APPLICATIONID).desktop"
	install -Dm644 data/$(APPLICATIONID).metainfo.xml \
	  "$(DESTDIR)$(APPDATADIR)/metainfo/$(APPLICATIONID).metainfo.xml"
	install -Dm644 logo.svg \
	  "$(DESTDIR)$(APPDATADIR)/icons/hicolor/scalable/apps/lambdock.svg"
	for module in scheme/lambdock/*.scm; do \
	  install -Dm644 "$$module" \
	    "$(DESTDIR)$(SCHEMEDIR)/lambdock/$$(basename "$$module")"; \
	done
	$(MAKE) i18n DESTDIR="$(DESTDIR)" DATADIR="$(DATADIR)"

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/lambdock"
	rm -f "$(DESTDIR)$(APPDATADIR)/applications/$(APPLICATIONID).desktop"
	rm -f "$(DESTDIR)$(APPDATADIR)/metainfo/$(APPLICATIONID).metainfo.xml"
	rm -rf "$(DESTDIR)$(SCHEMEDIR)/lambdock"

clean:
	$(CARGO) clean --manifest-path rust/Cargo.toml

# A source archive with everything the build needs and nothing it does not.
dist:
	git archive --format=tar.gz --prefix=lambdock-$(VERSION)/ \
	  -o lambdock-$(VERSION).tar.gz HEAD

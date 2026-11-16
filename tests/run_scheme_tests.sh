#!/bin/sh
# lambdock -- hyper-hackable desktop dock for Wayland
#
# SPDX-License-Identifier: MIT
# Copyright © Josep Bigorra <jjbigorra@gmail.com>
#
# Runs the pure-Scheme (Guile) unit tests for the embedded configuration
# engine. These do not need a display server or the C runtime. Exits 0 on
# success, 1 on assertion failure, and 77 (Meson "skipped") if no Guile
# interpreter can be located.

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)

GUILE_BIN="${GUILE:-}"
if [ -z "$GUILE_BIN" ]; then
  GUILE_BIN=$(command -v guile 2>/dev/null || true)
fi
if [ -z "$GUILE_BIN" ] && [ -x "${HOME:-}/.guix-profile/bin/guile" ]; then
  GUILE_BIN="${HOME}/.guix-profile/bin/guile"
fi
if [ -z "$GUILE_BIN" ] || [ ! -x "$GUILE_BIN" ]; then
  echo "guile interpreter not found; skipping Scheme unit tests" >&2
  exit 77
fi

# Load modules straight from source and never write .go caches into the tree.
export GUILE_AUTO_COMPILE=0
exec "$GUILE_BIN" -L "$root/scheme" "$here/scheme/lambdock_tests.scm"

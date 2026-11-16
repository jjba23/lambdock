#!/bin/sh
# lambdock -- hyper-hackable desktop dock for Wayland
#
# SPDX-License-Identifier: MIT
# Copyright © Josep Bigorra <jjbigorra@gmail.com>
#
# Build the Rust archive where meson can declare it.
#
# `cargo build` always writes `<target-dir>/<profile>/liblambdock.a`, and a meson
# custom target may not declare an output inside a subdirectory, so the archive is
# copied next to the declared one. Everything is an argument: meson passes the
# manifest, the target directory, the profile and the output path, so no command
# here is ever assembled out of data.
#
# usage: cargo-build.sh <manifest> <target-dir> <profile> <output>

set -eu

manifest=$1
target_dir=$2
profile=$3
output=$4
shift 4

if [ "$profile" = release ]; then
  set -- --release "$@"
fi

cargo build --locked --offline \
  --manifest-path "$manifest" \
  --target-dir "$target_dir" \
  "$@"

cp "$target_dir/$profile/liblambdock.a" "$output"

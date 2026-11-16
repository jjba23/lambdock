// lambdock -- hyper-hackable desktop dock for Wayland
//
// SPDX-License-Identifier: MIT
// Copyright © Josep Bigorra <jjbigorra@gmail.com>

//! Links the crate against what its C ABI promises.
//!
//! The ported modules allocate with `g_strdup`/`g_strdup_printf` and return
//! strings the C side frees with `g_free`, and they resolve translations through
//! `gettext`. The staticlib gets those symbols from the executable that imports
//! it, but a `cargo test` binary links on its own, so it needs the same
//! libraries -- and pkg-config is the one source of those flags for both halves
//! of the build. Meson already requires it.

use std::process::Command;

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_PATH");
    println!("cargo:rerun-if-env-changed=PKG_CONFIG_LIBDIR");

    let output = Command::new("pkg-config")
        .args(["--libs", "glib-2.0"])
        .output()
        .expect("pkg-config is needed to place GLib's libraries");

    assert!(
        output.status.success(),
        "pkg-config --libs glib-2.0 failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );

    let flags = String::from_utf8(output.stdout).expect("pkg-config printed UTF-8");

    for flag in flags.split_whitespace() {
        if let Some(directory) = flag.strip_prefix("-L") {
            println!("cargo:rustc-link-search=native={directory}");
        } else if let Some(library) = flag.strip_prefix("-l") {
            println!("cargo:rustc-link-lib={library}");
        }
    }
}

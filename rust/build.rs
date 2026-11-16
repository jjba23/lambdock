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
    // The X11 window tracker is a build option, not a runtime one: a GTK built for
    // Wayland alone has no Xlib to link, and meson says so by not setting this.
    println!("cargo:rerun-if-env-changed=LAMBDOCK_HAVE_X11");
    println!("cargo:rustc-check-cfg=cfg(have_x11)");
    if std::env::var_os("LAMBDOCK_HAVE_X11").is_some() {
        println!("cargo:rustc-cfg=have_x11");
        link("x11");
    }

    // The crate borrows three libraries' contracts: GLib's allocator and printf
    // for the strings it hands back, libguile for the Scheme values it reads, and
    // libwayland-client for the window tracker's registry and proxies.
    link("glib-2.0");
    link("guile-3.0");
    link("wayland-client");
}

/// Asks pkg-config for one package's link flags and hands them to rustc.
fn link(package: &str) {
    let output = Command::new("pkg-config")
        .args(["--libs", package])
        .output()
        .unwrap_or_else(|error| panic!("pkg-config is needed for {package}: {error}"));

    assert!(
        output.status.success(),
        "pkg-config --libs {package} failed: {}",
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

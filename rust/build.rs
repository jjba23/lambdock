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
    println!("cargo:rerun-if-env-changed=LAMBDOCK_SCHEME_DIR");
    println!("cargo:rerun-if-env-changed=LAMBDOCK_VERSION");
    // The version meson stamps into the C half reaches the About dialog from
    // here, so there is still one version string in the project.
    if let Ok(version) = std::env::var("LAMBDOCK_VERSION") {
        println!("cargo:rustc-env=LAMBDOCK_PACKAGE_VERSION={version}");
    }
    // Compiled in, like the -D the C half used: where `(lambdock core)` lives.
    if let Ok(directory) = std::env::var("LAMBDOCK_SCHEME_DIR") {
        println!("cargo:rustc-env=LAMBDOCK_SCHEME_DIR={directory}");
    }
    println!("cargo:rustc-check-cfg=cfg(have_x11)");
    if std::env::var_os("LAMBDOCK_HAVE_X11").is_some() {
        println!("cargo:rustc-cfg=have_x11");
        link("x11");
    }

    // The crate borrows four libraries' contracts: GLib's allocator and printf
    // for the strings it hands back, libguile for the Scheme values it reads,
    // libwayland-client for the window tracker's registry and proxies, and
    // gtk4-layer-shell for the overlays it puts on the screen.
    //
    // gtk4-layer-shell goes before wayland-client, and the order is the point:
    // the library exports its own `wl_proxy_marshal_array_flags` and
    // `wl_proxy_destroy` so it can reach libwayland's private half, and it only
    // counts the display as supported -- and only turns a window into a layer
    // surface -- after one of those two has run its `dlsym`. A symbol resolves
    // to the first library that defines it, so linking wayland-client first
    // hides those wrappers for the whole process: the library never
    // initialises, `gtk_layer_is_supported()` answers no on a compositor that
    // has the protocol, and the dock comes up as an ordinary window.
    link("glib-2.0");
    link("guile-3.0");
    link("gtk4-layer-shell-0");
    link("wayland-client");

    // The two sources the C half used to bring to the link: the GLib resource
    // bundle (CSS, icons, the About logo) and the Wayland protocol's interface
    // descriptions. Generating them here is what lets the binary be built by
    // cargo alone -- meson still drives installation and the packaging, but no
    // longer contributes an object file.
    generate("resources");

    println!("cargo:rerun-if-changed=../resources/lambdock.gresource.xml");
    println!("cargo:rerun-if-changed=../protocols");
}

/// Compiles the two generated C sources and archives them, so `cargo build`
/// produces a binary that has its resources and its protocol data.
fn generate(_what: &str) {
    let out_dir = std::env::var("OUT_DIR").expect("cargo sets OUT_DIR");
    let source_root = std::path::Path::new(".");

    let resources = std::path::Path::new(&out_dir).join("lambdock-resources.c");
    run(
        "glib-compile-resources",
        &[
            "--generate-source",
            &format!("--target={}", resources.display()),
            "--sourcedir=../resources",
            "../resources/lambdock.gresource.xml",
        ],
        source_root,
    );

    let protocol = std::path::Path::new(&out_dir).join("wlr-foreign-toplevel.c");
    run(
        "wayland-scanner",
        &[
            "private-code",
            "../protocols/wlr-foreign-toplevel-management-unstable-v1.xml",
            &protocol.display().to_string(),
        ],
        source_root,
    );

    // One archive, compiled with the flags pkg-config names for the headers
    // these two files include.
    let cflags = Command::new("pkg-config")
        .args(["--cflags", "gio-2.0", "wayland-client"])
        .output()
        .expect("pkg-config is needed for the generated sources");
    let cflags = String::from_utf8(cflags.stdout).expect("pkg-config printed UTF-8");
    let cflags: Vec<&str> = cflags.split_whitespace().collect();

    // The toolchain's own compiler, or whichever the environment names: a sandbox
    // (Guix, Nix) puts it on PATH under a name of its choosing.
    let cc = std::env::var("CC").unwrap_or_else(|_| "cc".to_owned());

    let mut objects = Vec::new();
    for source in [&resources, &protocol] {
        let object = source.with_extension("o");
        let mut arguments = vec!["-fPIC", "-c", "-o"];
        let object_text = object.display().to_string();
        let source_text = source.display().to_string();
        arguments.push(&object_text);
        arguments.push(&source_text);
        arguments.extend(cflags.iter().copied());

        run(&cc, &arguments, source_root);
        objects.push(object);
    }

    // Linked as objects, not as an archive: the resource bundle registers
    // itself from a constructor, and a linker drops an archive member nobody
    // references -- which is exactly how the CSS went missing the first time.
    for object in &objects {
        println!("cargo:rustc-link-arg-bins={}", object.display());
    }

    // The libraries those two objects need, listed again *after* them. Link
    // arguments come after the crate's own `-l` flags, and with `--as-needed` a
    // library that appears before its first user is dropped: the resource bundle
    // references `g_static_resource_init` and the protocol code references
    // `wl_proxy_*`, so a target whose Rust side happens to reference neither
    // (a test binary is one) lost glib and libwayland and failed to link.
    for package in ["gio-2.0", "glib-2.0", "wayland-client"] {
        for flag in pkg_config_libs(package) {
            println!("cargo:rustc-link-arg-bins={flag}");
        }
    }
}

/// One package's `pkg-config --libs`, as the flags a linker takes.
fn pkg_config_libs(package: &str) -> Vec<String> {
    let output = Command::new("pkg-config")
        .args(["--libs", package])
        .output()
        .unwrap_or_else(|error| panic!("pkg-config is needed for {package}: {error}"));

    assert!(
        output.status.success(),
        "pkg-config --libs {package} failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );

    String::from_utf8(output.stdout)
        .expect("pkg-config printed UTF-8")
        .split_whitespace()
        .map(str::to_owned)
        .collect()
}

/// Runs a build-time tool, and fails the build when it does: a missing resource
/// bundle is not a warning.
fn run(program: &str, arguments: &[&str], directory: &std::path::Path) {
    let output = Command::new(program)
        .current_dir(directory)
        .args(arguments)
        .output()
        .unwrap_or_else(|error| panic!("{program} is needed for the build: {error}"));

    assert!(
        output.status.success(),
        "{program} failed: {}",
        String::from_utf8_lossy(&output.stderr)
    );
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

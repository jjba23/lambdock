;; lambdock -- hyper-hackable desktop dock for Wayland
;;
;; SPDX-License-Identifier: MIT
;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

(use-modules (guix packages)
             (gnu)
             (guix utils)
             (guix git-download)
             (guix download)
             (gnu packages guile)
             (gnu packages nss)
             (gnu packages admin)
             (gnu packages web)
             (gnu packages gnupg)
             (gnu packages guile-xyz)
             (gnu packages pkg-config)
             (guix build-system guile)
             (gnu packages linux)
             (gnu packages compression)
             (gnu packages texinfo)
             (gnu packages autotools)
             (gnu packages build-tools)
             (gnu packages cpp)
             (gnu packages gtk)
             (gnu packages glib)
             (gnu packages gnome)
             (gnu packages xorg)
             (gnu packages base)
             (gnu packages freedesktop)
             (gnu packages gettext)
             (gnu packages gdb)
             (gnu packages build-tools)
             (gnu packages commencement)
             (gnu packages rust)
             (gnu packages rust-apps)
             ((guix licenses)
              #:prefix license:))

(packages->manifest (list guile-3.0
                          ;; for production Guile 3.0 is used
                          ;; for dev Arei REPL use Guile Next
                          gcc-toolchain
                          ;; The Rust toolchain: `rustc' builds the crate and
                          ;; `cargo' runs its tests.  `rust' keeps cargo in its
                          ;; own output.
                          rust
                          (list rust "cargo")
                          ;; `cargo clippy' is the lints half of `maak test', and
                          ;; the tools output is where the toolchain keeps
                          ;; `cargo-clippy' and `clippy-driver'. The same output
                          ;; is the editor story: `rustfmt' and `rust-analyzer',
                          ;; the latter wrapped to find its sysroot.
                          ;;
                          ;; Having them here is not enough on its own: cargo
                          ;; looks for a subcommand in `$CARGO_HOME/bin' *before*
                          ;; the PATH, and rustup keeps the host's `cargo-clippy'
                          ;; in `~/.cargo/bin'. So `maak clippy' (and `maak test')
                          ;; pin CARGO_HOME inside the shell, which is what makes
                          ;; the shell's clippy -- the one matching this rustc --
                          ;; the one that runs.
                          (list rust "tools")
                          ;; rust-analyzer reads the standard library's sources
                          ;; from the sysroot; without this it shows signatures
                          ;; it inferred and no documentation.
                          (list rust "rust-src")
                          gettext-minimal
                          pkg-config
                          ccls
                          gtk
                          gtk4-layer-shell
                          libx11
                          texinfo
                          watchexec
                          guile-documenta
                          wlr-protocols
                          guile-ares-rs
                          ;; `maak debug-run' launches the dock under gdb, so the
                          ;; debugger comes from the shell rather than from
                          ;; whatever the host happens to have.
                          gdb
                          ;; `libtiff-4.pc' (pulled in through gdk-pixbuf)
                          ;; lists liblzma in its Requires.private, and
                          ;; pkg-config resolves those even for `--cflags'. The
                          ;; library is not propagated into the shell, so
                          ;; without xz the gtk4 dependency lookup aborts with
                          ;; "Package 'liblzma' ... not found".
                          xz
                          ;; `glib-compile-resources' lives in the "bin" output of
                          ;; glib, which is not what gtk propagates. Without it
                          ;; `build.rs' cannot compile the resource bundle.
                          (list glib "bin")
                          ;; An icon theme is what makes `utilities-terminal'
                          ;; resolvable: without it the dock item tests treat the
                          ;; icon fallback warning as a failure.
                          adwaita-icon-theme
                          ;; `cargo test' uses
                          ;; dirname/mktemp; do not depend on the host PATH.
                          coreutils))

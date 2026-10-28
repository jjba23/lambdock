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
             (gnu packages ninja)
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
             (gnu packages build-tools)
             (gnu packages commencement)
             (gnu packages rust-apps)
             ((guix licenses)
              #:prefix license:))

(packages->manifest (list guile-3.0
                          ;; for production Guile 3.0 is used
                          ;; for dev Arei REPL use Guile Next
                          gcc-toolchain
                          gettext-minimal
                          pkg-config
                          meson
                          ccls
                          gtk
                          gtk4-layer-shell
                          texinfo
                          watchexec
                          guile-documenta
                          wlr-protocols
                          ninja
                          guile-ares-rs
                          ;; `libtiff-4.pc' (pulled in through gdk-pixbuf)
                          ;; lists liblzma in its Requires.private, and
                          ;; pkg-config resolves those even for `--cflags'. The
                          ;; library is not propagated into the shell, so
                          ;; without xz the gtk4 dependency lookup aborts with
                          ;; "Package 'liblzma' ... not found" before meson can
                          ;; configure anything.
                          xz
                          ;; `glib-compile-resources' lives in the "bin" output of
                          ;; glib, which is not what gtk propagates. Without it
                          ;; meson aborts at gnome.compile_resources.
                          (list glib "bin")
                          ;; Headless display + X authority for the test stage.
                          xvfb-run
                          ;; An icon theme is what makes `utilities-terminal'
                          ;; resolvable: without it the dock item tests treat the
                          ;; icon fallback warning as a failure.
                          adwaita-icon-theme
                          ;; `tests/run_scheme_tests.sh' and `xvfb-run' use
                          ;; dirname/mktemp; do not depend on the host PATH.
                          coreutils))

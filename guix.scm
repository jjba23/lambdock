;; lambdock -- hyper-hackable desktop dock for Wayland
;;
;; SPDX-License-Identifier: MIT
;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

(use-modules (guix packages)
             (guix gexp)
             (guix git-download)
             (guix build-system gnu)
             ((guix licenses)
              #:prefix license:)
             (gnu packages commencement)
             (gnu packages gtk)
             (gnu packages glib)
             (gnu packages guile)
             (gnu packages gettext)
             (gnu packages freedesktop)
             (gnu packages pkg-config)
             (gnu packages rust)
             (gnu packages xorg))

(define %source-dir
  (dirname (current-filename)))

(define-public lambdock
  (package
    (name "lambdock")
    (version "dev")
    (source
     (local-file %source-dir
                 #:recursive? #t
                 #:select? (git-predicate %source-dir)))
    (build-system gnu-build-system)
    (arguments
     (list #:parallel-build? #t
           #:parallel-tests? #t
           ;; cargo builds, `cargo xtask' installs: the data files, the Scheme
           ;; modules and the compiled catalogues have no cargo target of their
           ;; own, which is the whole reason the task crate exists.
           #:phases #~(modify-phases %standard-phases
                        (delete 'configure)
                        (replace 'build
                          (lambda _
                            ;; Compiled in: the dock finds its Scheme modules and
                            ;; its catalogues by path, not through a wrapper, so
                            ;; a build that leaves these unset produces a binary
                            ;; that only runs from the source tree.
                            (setenv "LAMBDOCK_SCHEME_DIR"
                                    (string-append #$output
                                                   "/share/guile/site/3.0"))
                            (setenv "LAMBDOCK_LOCALEDIR"
                                    (string-append #$output "/share/locale"))
                            (invoke "cargo" "build" "--offline" "--locked"
                                    "--release")))
                        (replace 'check
                          (lambda* (#:key tests? #:allow-other-keys)
                            (when tests?
                              (invoke "cargo" "test" "--offline" "--locked"))))
                        (replace 'install
                          (lambda _
                            (invoke "cargo" "run" "--offline" "--locked"
                                    "-p" "xtask" "--" "install"
                                    "--prefix" #$output))))))
    (native-inputs (list (list glib "bin")
                         pkg-config
                         gettext-minimal
                         ;; `build.rs' compiles the generated resource bundle and
                         ;; protocol code with `cc', and archives them with `ar':
                         ;; both come from the toolchain, not from a bare `gcc'.
                         gcc-toolchain
                         ;; `rustc' and `cargo' build the crate and run the
                         ;; suite; `wayland-scanner' and `glib:bin' generate the
                         ;; protocol and the resource bundle.
                         rust
                         (list rust "cargo")))
    (inputs (list gtk
                  gtk4-layer-shell
                  guile-3.0
                  wlr-protocols
                  libx11))
    (home-page "https://github.com/jjba23/lambdock")
    (synopsis "Desktop dock customizable with GNU Guile Scheme")
    (description
     "Lambdock is a desktop dock built with GTK4.  On Wayland it anchors itself
with @code{gtk4-layer-shell} and tracks windows with the foreign toplevel
management protocol; on X11 it docks itself through EWMH.  It features
customizable CSS theming, fluid slide-out auto-hiding, multi-monitor placement,
and an embedded GNU Guile Scheme engine for live configuration, extensibility,
and REPL interaction.")
    (license license:expat)))

lambdock

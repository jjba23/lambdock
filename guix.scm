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
           ;; No configure script: `make' is the whole build, and `make install'
           ;; places the binary, the desktop entry, the metainfo, the Scheme
           ;; modules and the catalogues under the prefix.
           #:phases #~(modify-phases %standard-phases
                        (delete 'configure)
                        (replace 'check
                          (lambda* (#:key tests? #:allow-other-keys)
                            (when tests?
                              (invoke "make" "check")))))
           #:make-flags #~(list (string-append "PREFIX=" #$output))))
    (native-inputs (list (list glib "bin")
                         pkg-config
                         gettext-minimal
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

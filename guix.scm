;; lambdock -- hyper-hackable desktop dock for Wayland
;;
;; SPDX-License-Identifier: MIT
;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

(use-modules (guix packages)
             (guix gexp)
             (guix git-download)
             (guix build-system meson)
             ((guix licenses)
              #:prefix license:)
             (gnu packages gtk)
             (gnu packages glib)
             (gnu packages guile)
             (gnu packages gettext)
             (gnu packages freedesktop)
             (gnu packages pkg-config))

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
    (build-system meson-build-system)
    (arguments (list #:parallel-build? #t
                     #:parallel-tests? #t))
    (native-inputs (list (list glib "bin")
                         pkg-config
                         gettext-minimal))
    (inputs (list gtk
                  gtk4-layer-shell
                  guile-3.0
                  wlr-protocols))
    (home-page "https://github.com/jjba23/lambdock")
    (synopsis "Wayland desktop dock customizable with GNU Guile Scheme")
    (description
     "Lambdock is a Wayland-native desktop dock built with GTK4 and
@code{gtk4-layer-shell}.  It features dynamic window tracking using the Wayland
foreign toplevel management protocol, customizable CSS theming, fluid slide-out
auto-hiding, multi-monitor placement, and an embedded GNU Guile Scheme engine
for live configuration, extensibility, and REPL interaction.")
    (license license:expat)))

lambdock

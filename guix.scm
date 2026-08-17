;; lambdock

;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

;; lambdock is free software : you can redistribute it and / or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.

;; lambdock is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with lambdock.If not, see <https://www.gnu.org/licenses/>.

(use-modules (guix packages)
             (guix gexp)
             (guix git-download)
             (guix build-system meson)
             ((guix licenses)
              #:prefix license:)
             (gnu packages gtk)
             (gnu packages glib)
             (gnu packages guile)
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
                         pkg-config))
    (inputs (list gtk
                  gtk4-layer-shell
                  guile-3.0
                  wlr-protocols))
    (home-page "https://codeberg.org/jjba23/lambdock")
    (synopsis "Wayland desktop dock customizable with GNU Guile Scheme")
    (description
     "Lambdock is a Wayland-native desktop dock built with GTK4 and
@code{gtk4-layer-shell}.  It features dynamic window tracking using the Wayland
foreign toplevel management protocol, customizable CSS theming, fluid slide-out
auto-hiding, multi-monitor placement, and an embedded GNU Guile Scheme engine
for live configuration, extensibility, and REPL interaction.")
    (license license:gpl3+)))

lambdock

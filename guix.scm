;; lambdock

;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

;; lambdock is free software: you can redistribute it and/or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.

;; lambdock is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with lambdock.  If not, see <https://www.gnu.org/licenses/>.

(use-modules (guix packages)
             (guix gexp)
             (guix git-download)
             (guix build-system meson)
             ((guix licenses)
              #:prefix license:)
             (gnu packages gtk)
             (gnu packages guile)
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
    (native-inputs (list pkg-config))
    (inputs (list gtk gtk4-layer-shell guile-next))
    (home-page "https://github.com/yourusername/lambdock")
    (synopsis "Wayland-native GTK4 dock extensible in Guile Scheme")
    (description
     "Lambdock is a lightweight, Wayland-native desktop dock built with a C core,
GTK4 layer-shell, and dynamic configuration powered by GNU Guile Lisp.")
    (license license:gpl3+)))

lambdock

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

(define-module (maak)
  #:use-module (maak maak)
  #:use-module (ice-9 textual-ports)
  #:use-module (ice-9 popen)
  #:declarative? #t)

(define (run)
  "Run lambdock with Guix"
  (time-machine-program-shell (list "lambdock")
                              #:verbose? #t))

(define (repl)
  "Development REPL."
  (time-machine-manifest-shell '("guile -L ./src"
                                 "-c '((@ (ares server) run-nrepl-server))'")))

(define c-packages
  (make-parameter (string-join (map symbol->string
                                    '(gtk4 guile-3.0 gtk4-layer-shell-0
                                           wlr-protocols)) " ")))

(define (add-libs)
  (delete-file-recursively ".ccls")
  (time-machine-manifest-shell (list (~ "pkg-config --cflags ~a"
                                        (c-packages))
                                     " | tr ' ' '\n' >> .ccls")
                               #:verbose? #t)
  ($ '("cat .ccls")))

(define deb-packages
  (make-parameter (map symbol->string
                       '(build-essential debhelper
                                         devscripts
                                         meson
                                         ninja-build
                                         pkg-config
                                         libgtk-4-dev
                                         libgtk4-layer-shell-dev
                                         guile-3.0-dev
                                         libwayland-dev
                                         libglib2.0-dev-bin
                                         wayland-protocols))))

(define (deb-build)
  "Build .deb package (unsigned)"
  ($ '("sudo apt update")
     #:verbose? #t)
  ($ (cons "sudo apt install -y"
           (deb-packages))
     #:verbose? #t)
  ($ '("sudo mk-build-deps -i -r debian/control")
     #:verbose? #t)
  ($ '("debuild -b -uc -us")
     #:verbose? #t))

(define (arch-build)
  "Build Arch Linux package (.pkg.tar.zst)"
  ($ '("makepkg -si")
     #:verbose? #t)
  ($ '("namcap PKGBUILD")
     #:verbose? #t))

(define rpm-packages
  (make-parameter (map symbol->string
                       '(rpm-build
                         rpmdevtools
                         gcc
                         meson
                         ninja-build
                         pkgconfig
                         gtk4-devel
                         gtk4-layer-shell-devel
                         guile30-devel
                         libwayland-client-devel))))

(define (rpm-build)
  "Build RPM package for Fedora/RHEL"
  ($ '("rpmdev-setuptree") #:verbose? #t)
  ($ '("tar -czvf ~/rpmbuild/SOURCES/lambdock-0.1.0.tar.gz --transform 's/^\\./lambdock-0.1.0/' --exclude='.git' --exclude='build' .")
     #:verbose? #t)
  ($ '("rpmbuild -ba lambdock.spec") #:verbose? #t)
  ($ '("cp ~/rpmbuild/RPMS/*/*.rpm .") #:verbose? #t))

(define (nix-build)
  "Build Nix package"
  ($ '("nix --extra-experimental-features 'nix-command flakes' build")))

(define (nix-run)
  "Run Nix package"
  ($ '("nix --extra-experimental-features 'nix-command flakes' run .#")))

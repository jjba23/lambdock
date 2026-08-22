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
  #:declarative? #t
  #:use-module (ice-9 string-fun)
  #:use-module (ice-9 textual-ports)
  #:use-module (ice-9 popen)
  #:use-module (maak maak))

;; ===== Maak abstractions =====

(define (syscall-with-output cmd)
  (let* ((process (open-input-pipe cmd))
         (process-output (get-string-all process)))
    (close-pipe process) process-output))

;; ===== Maak tasks =======

(define project-name
  'lambdock)

(define (run)
  "Run lambdock with Guix"
  ($ '("pkill lambdock || true"))
  (time-machine-program-shell (list "lambdock")
                              #:verbose? #t))

(define (test)
  "Test the project"
  (time-machine-program-shell (list "meson setup build && ninja -C build test")
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
                                         cme
                                         lintian
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

  (dynamic-wind
   ;; Setup: create temporary symlink at project root
   (lambda ()
     ($ '("ln -s packaging/debian debian")
        #:verbose? #t))

   ;; Execution: run debian build commands against ./debian
   (lambda ()
     ($ '("sudo mk-build-deps -i -r debian/control")
        #:verbose? #t)
     ($ '("debuild -b -uc -us")
        #:verbose? #t))

   ;; Cleanup: remove symlink safely (leaves packaging/debian intact)
   (lambda ()
     ($ '("rm -rf debian")
        #:verbose? #t))))

(define (arch-build)
  "Build Arch Linux package (.pkg.tar.zst)"
  ($ '("makepkg -si")
     #:verbose? #t)
  ($ '("namcap packaging/arch/PKGBUILD")
     #:verbose? #t))

(define rpm-packages
  (make-parameter (map symbol->string
                       '(rpm-build rpmdevtools
                                   gcc
                                   meson
                                   ninja-build
                                   pkgconfig
                                   gtk4-devel
                                   gtk4-layer-shell-devel
                                   guile30-devel
                                   libwayland-client-devel))))

(define (rpm-build)
  "Build RPM package for openSUSE and Fedora/RHEL"
  (let* ((latest-tag (syscall-with-output "git describe --tags --abbrev=0"))
         (latest-tag-num (string-trim-both (string-drop latest-tag 1))))
    ($ '("rpmdev-setuptree")
       #:verbose? #t)
    ($ (list "git archive --format=tar.gz"
             (~ "--prefix=lambdock-~a/" latest-tag-num)
             (~ "-o ~~/rpmbuild/SOURCES/lambdock-~a.tar.gz" latest-tag-num)
             latest-tag)
       #:verbose? #t)
    ($ '("cp packaging/opensuse/lambdock.spec ~/rpmbuild/SPECS/")
       #:verbose? #t)
    ($ '("rpmbuild -ba ~/rpmbuild/SPECS/lambdock.spec")
       #:verbose? #t)
    ($ '("ls ~/rpmbuild/RPMS/x86_64/lambdock-[0-9]*.rpm"))
    (let* ((latest-rpm (syscall-with-output
                        "ls -t ~/rpmbuild/RPMS/x86_64/lambdock-[0-9]*.rpm | head -n 1"))
           (latest-src-rpm (syscall-with-output
                            "ls -t ~/rpmbuild/SRPMS/lambdock-[0-9]*.rpm | head -n 1")))
      ($ '("cp packaging/opensuse/lambdock-rpmlintrc ~/rpmbuild/RPMS/x86_64/")
         #:verbose? #t)
      ($ (list (~ "rpmlint ~a" latest-rpm))
         #:verbose? #t)
      ($ '("cp packaging/opensuse/lambdock-rpmlintrc ~/rpmbuild/SRPMS/")
         #:verbose? #t)
      ($ (list (~ "rpmlint ~a" latest-src-rpm))
         #:verbose? #t))))

(define (nix-test)
  "Run Nix flake checks and test phase"
  ($ '("nix --extra-experimental-features 'nix-command flakes' flake check")
     #:verbose? #t))

(define (nix-build)
  "Build Nix package"
  ($ '("nix --extra-experimental-features 'nix-command flakes' build")))

(define (nix-run)
  "Run Nix package"
  ($ '("nix --extra-experimental-features 'nix-command flakes' run .#")))

(define (container-build)
  "Build Container image"
  ($ '("podman build -t lambdock .")))

(define (container-run)
  "Run Container image"
  ($ '("podman run -d --rm --name lambdock" "--network host"
       "--userns=keep-id --ipc=host"
       "--device /dev/dri"
       "-e XDG_RUNTIME_DIR=/run/user/$(id -u)"
       "-e WAYLAND_DISPLAY=\"$WAYLAND_DISPLAY\""
       "-e HOME=\"$HOME\""
       "-v /run/user/$(id -u):/run/user/$(id -u):z"
       "-v \"$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY:/run/user/$(id -u)/$WAYLAND_DISPLAY:ro\""
       "-v \"$HOME:$HOME\""
       "-v /usr/share/icons:/usr/share/icons:ro"
       "-v /usr/share/applications:/usr/share/applications:ro"
       "lambdock:latest")
     #:verbose? #t))

(define (push-docker-latest)
  "Push the latest locally generated Docker image to Dockerhub."
  ($ (list (~ "podman push localhost/~a:latest" project-name)
           (~ "docker.io/jjba23/~a:latest" project-name))
     #:verbose? #t))

(define (push-docker-tag)
  "Push the locally generated Docker image to Dockerhub."
  ($ '("git fetch --all"))
  (let* ((latest-tag (syscall-with-output "git describe --tags --abbrev=0")))
    ($ (list (~ "podman push localhost/~a:latest" project-name)
             (~ "docker.io/jjba23/~a:~a" project-name latest-tag))
       #:verbose? #t)))

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
  (time-machine-program-shell (list "lambdock")
                              #:verbose? #t))

(define c-packages
  (make-parameter (string-join (map symbol->string
                                    '(gtk4 guile-3.0 gtk4-layer-shell-0)) " ")))

(define (add-libs)
  (delete-file-recursively ".ccls")
  (time-machine-manifest-shell (list (~ "pkg-config --cflags ~a"
                                        (c-packages))
                                     " | tr ' ' '\n' >> .ccls")
                               #:verbose? #t)
  ($ '("cat .ccls")))

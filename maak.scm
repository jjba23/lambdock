(define-module (maak)
  #:use-module (maak maak)
  #:use-module (ice-9 textual-ports)
  #:use-module (ice-9 popen)
  #:declarative? #t)

(define (run)
  (time-machine-program-shell (list "lambdock")
                              #:verbose? #t))

(define (add-libs)
  (delete-file-recursively ".ccls")
  (time-machine-manifest-shell (list
                                "pkg-config --cflags gtk4 guile-3.0 gtk4-layer-shell-0 | tr ' ' '
' >> .ccls")
                               #:verbose? #t)
  ($ '("cat .ccls")))

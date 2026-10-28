;; lambdock -- hyper-hackable desktop dock for Wayland
;;
;; SPDX-License-Identifier: MIT
;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

(define-module (lambdock logger)
  #:use-module (ice-9 format)
  #:export (log-debug log-info log-warn log-error))

;; ANSI Color Codes
(define %clr-reset
  "\x1b[0m")
(define %clr-dim
  "\x1b[2m")
(define %clr-green
  "\x1b[32m")
(define %clr-yellow
  "\x1b[33m")
(define %clr-red
  "\x1b[31m")
(define %clr-cyan
  "\x1b[36m")

(define (format-log level-str color subsystem msg args)
  (let ((formatted-msg (apply format #f msg args)))
    (format #t
            "~a[~a]~a[lambdock]~a[Scheme:~a]~a ~a~%"
            color
            level-str
            %clr-reset
            %clr-cyan
            subsystem
            %clr-reset
            formatted-msg)))

(define (log-debug subsystem msg . args)
  (format-log "DEBUG" %clr-dim subsystem msg args))

(define (log-info subsystem msg . args)
  (format-log "INFO" %clr-green subsystem msg args))

(define (log-warn subsystem msg . args)
  (format-log "WARN" %clr-yellow subsystem msg args))

(define (log-error subsystem msg . args)
  (format-log "ERROR" %clr-red subsystem msg args))

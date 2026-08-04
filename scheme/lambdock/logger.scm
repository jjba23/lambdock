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
            "~a[Scheme:~a]~a ~a[~a]~a ~a~%"
            %clr-cyan
            subsystem
            %clr-reset
            color
            level-str
            %clr-reset
            formatted-msg)))

(define (log-debug subsystem msg . args)
  (format-log "DEBUG" %clr-dim subsystem msg args))

(define (log-info subsystem msg . args)
  (format-log "INFO " %clr-green subsystem msg args))

(define (log-warn subsystem msg . args)
  (format-log "WARN " %clr-yellow subsystem msg args))

(define (log-error subsystem msg . args)
  (format-log "ERROR" %clr-red subsystem msg args))

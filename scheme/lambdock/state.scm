;; lambdock

;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

;; lambdock is free software : you can redistribute it and / or modify
;; it under the terms of the GNU General Public License as published by
;; the Free Software Foundation, either version 3 of the License, or
;; (at your option) any later version.

;; lambdock is distributed in the hope that it will be useful,
;; but WITHOUT ANY WARRANTY; without even the implied warranty of
;; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
;; GNU General Public License for more details.

;; You should have received a copy of the GNU General Public License
;; along with lambdock. If not, see <https://www.gnu.org/licenses/>.

(define-module (lambdock state)
  #:declarative? #f
  #:use-module (ice-9 format)
  #:use-module (lambdock logger)
  #:export ( ;Configuration Entry Point
             configure-dock!

            ;; Getters
            get-dock-items
            get-dock-position
            get-dock-theme
            get-dock-icon-size
            get-dock-auto-hide?
            get-dock-monitor
            get-dock-exclude-apps
            get-dock-enable-repl?
            get-dock-repl-socket-path
            get-dock-show-when-clear?
            get-dock-hover-animate?

            ;; Setters
            set-dock-items!
            set-dock-position!
            set-dock-theme!
            set-dock-icon-size!
            set-dock-auto-hide!
            set-dock-monitor!
            set-dock-exclude-apps!
            set-dock-enable-repl!
            set-dock-repl-socket-path!
            set-dock-show-when-clear!
            set-dock-hover-animate!))

(define-syntax define-config-field
  (syntax-rules ()
                ((_ var-name getter-name setter-name default-val validator)
                 (begin
                   (define var-name
                     default-val)
                   (define (getter-name)
                     var-name)
                   (define (setter-name val)
                     (when (validator val)
                       (set! var-name val)))))))

;; =============================================================================
;; Application State Definitions
;; =============================================================================

(define-config-field *dock-items* get-dock-items set-dock-items!
                     '() list?)

(define-config-field *dock-position* get-dock-position set-dock-position!
                     'bottom identity)

(define-config-field *dock-theme* get-dock-theme set-dock-theme!
                     'vanilla identity)

(define-config-field *dock-icon-size* get-dock-icon-size set-dock-icon-size!
                     48
                     (lambda (s)
                       (and (number? s)
                            (> s 0))))

(define-config-field *dock-auto-hide?* get-dock-auto-hide? set-dock-auto-hide!
                     #f boolean?)

(define-config-field *dock-show-when-clear?* get-dock-show-when-clear?
                     set-dock-show-when-clear! #f boolean?)

(define-config-field *dock-monitor* get-dock-monitor set-dock-monitor!
                     'default identity)

(define-config-field *dock-exclude-apps* get-dock-exclude-apps
                     set-dock-exclude-apps!
                     '() list?)

(define-config-field *dock-enable-repl?* get-dock-enable-repl?
                     set-dock-enable-repl! #f boolean?)

(define-config-field *dock-repl-socket-path* get-dock-repl-socket-path
                     set-dock-repl-socket-path! "/tmp/lambdock-repl.sock"
                     (lambda (p)
                       (and (string? p)
                            (not (string-null? p)))))

(define-config-field *dock-hover-animate?* get-dock-hover-animate?
                     set-dock-hover-animate! #f boolean?)

;; =============================================================================
;; Configuration Interface
;; =============================================================================

(define* (configure-dock! #:key (position 'bottom)
                          (icon-size 48)
                          (items '())
                          (auto-hide? #f)
                          (show-when-clear? #t)
                          (theme 'vanilla)
                          (monitor 'default)
                          (exclude-apps '())
                          (enable-repl? #f)
                          (repl-socket-path "/tmp/lambdock-repl.sock")
                          (hover-animate? #t))
  "Sets the primary configuration parameters for the dock."
 (log-info "State"
   "Registering: ~a items at ~apx, theme: ~a, auto-hide?: ~a, show-when-clear?: ~a, position: ~a, monitor: ~a, excluded items: ~a, repl?: ~a, repl-socket: ~a, hover-animate?: ~a"
   (if (list? items)
       (length items) 0)
   icon-size
   theme
   auto-hide?
   show-when-clear?
   position
   monitor
   (if (list? exclude-apps)
       (length exclude-apps) 0)
   enable-repl?
   repl-socket-path
   hover-animate?)

  (set-dock-position! position)
  (set-dock-theme! theme)
  (set-dock-icon-size! icon-size)
  (set-dock-items! items)
  (set-dock-auto-hide! auto-hide?)
  (set-dock-monitor! monitor)
  (set-dock-exclude-apps! exclude-apps)
  (set-dock-enable-repl! enable-repl?)
  (set-dock-repl-socket-path! repl-socket-path)
  (set-dock-show-when-clear! show-when-clear?)
  (set-dock-hover-animate! hover-animate?)

  (log-info "State" "Configuration loaded successfully."))

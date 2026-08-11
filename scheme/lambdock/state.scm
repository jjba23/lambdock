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
  #:use-module (srfi srfi-9)
  #:use-module (ice-9 format)
  #:use-module (lambdock logger)
  #:export ( ;Configuration Entry Point
             configure-dock!
            <dock-config>
            dock-config?

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

            ;; High-Level Setters
            set-dock-position!
            set-dock-theme!
            set-dock-icon-size!
            set-dock-auto-hide!
            set-dock-show-when-clear!
            set-dock-monitor!
            set-dock-hover-animate!))

;; =============================================================================
;; Dock Configuration Record
;; =============================================================================

(define-record-type <dock-config>
  (make-dock-config position
                    icon-size
                    items
                    auto-hide?
                    show-when-clear?
                    theme
                    monitor
                    exclude-apps
                    enable-repl?
                    repl-socket-path
                    hover-animate?)
  dock-config?
  (position dock-config-position set-dock-config-position!)
  (icon-size dock-config-icon-size set-dock-config-icon-size!)
  (items dock-config-items set-dock-config-items!)
  (auto-hide? dock-config-auto-hide? set-dock-config-auto-hide?!)
  (show-when-clear? dock-config-show-when-clear?
                    set-dock-config-show-when-clear?!)
  (theme dock-config-theme set-dock-config-theme!)
  (monitor dock-config-monitor set-dock-config-monitor!)
  (exclude-apps dock-config-exclude-apps set-dock-config-exclude-apps!)
  (enable-repl? dock-config-enable-repl? set-dock-config-enable-repl?!)
  (repl-socket-path dock-config-repl-socket-path
                    set-dock-config-repl-socket-path!)
  (hover-animate? dock-config-hover-animate? set-dock-config-hover-animate?!))

;; Global hashtable storing isolated <dock-config> records keyed by config path / symbol
(define *dock-configs*
  (make-hash-table))

;; Fallback default record when key is missing or unregistered
(define %default-fallback-config%
  (make-dock-config 'bottom
                    48
                    '()
                    #f
                    #t
                    'vanilla
                    'default
                    '()
                    #f
                    "/tmp/lambdock-repl.sock"
                    #t))

;; =============================================================================
;; Configuration Interface
;; =============================================================================

;; === FILE: state.scm (Excerpt: configure-dock!) ===
(define* (configure-dock! #:key (config-path 'default)
                          (position 'bottom)
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
  "Sets and registers configuration parameters for a specific dock instance key or file path."
  (let ((cfg (make-dock-config position
                               icon-size
                               items
                               auto-hide?
                               show-when-clear?
                               theme
                               monitor
                               exclude-apps
                               enable-repl?
                               repl-socket-path
                               hover-animate?)))
    ;; Store under exact file path / symbol key
    (hash-set! *dock-configs* config-path cfg)

    ;; ALWAYS update 'default key if this is the primary settings file or initial load
    (when (or (eq? config-path
                   'default)
              (string-suffix? "settings.scm"
                              (format #f "~a" config-path)))
      (hash-set! *dock-configs*
                 'default cfg))

    (log-info "State"
     "Registered config for [~a]: ~a items, pos: ~a, theme: ~a, auto-hide?: ~a, repl?: ~a (~a) (~a)"
     config-path
     (if (list? items)
         (length items) 0)
     position
     theme
     auto-hide?
     enable-repl?
     repl-socket-path
     cfg)))

(define (get-dock-config key)
  (or (hash-ref *dock-configs* key #f)
      (hash-ref *dock-configs*
                'default #f) %default-fallback-config%))

;; =============================================================================
;; Getters
;; =============================================================================

(define (get-dock-items . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-items cfg)))

(define (get-dock-position . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-position cfg)))

(define (get-dock-theme . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-theme cfg)))

(define (get-dock-icon-size . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-icon-size cfg)))

(define (get-dock-auto-hide? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-auto-hide? cfg)))

(define (get-dock-show-when-clear? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-show-when-clear? cfg)))

(define (get-dock-monitor . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-monitor cfg)))

(define (get-dock-exclude-apps . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-exclude-apps cfg)))

(define (get-dock-enable-repl? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-enable-repl? cfg)))

(define (get-dock-repl-socket-path . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-repl-socket-path cfg)))

(define (get-dock-hover-animate? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-hover-animate? cfg)))

;; =============================================================================
;; Setters
;; =============================================================================

(define (trigger-ui-reload!)
  "Helper to invoke C reload-dock! procedure if available."
  (catch #t
         (lambda ()
           (let ((mod (resolve-module '(lambdock core)
                                      #:ensure #f)))
             (when mod
               (let ((proc (module-ref mod
                                       'reload-dock! #f)))
                 (when (and proc
                            (procedure? proc))
                   (proc))))))
         (lambda _
           #f)))

(define (apply-to-targets proc args)
  (if (null? args)
      (hash-for-each (lambda (k cfg)
                       (proc cfg)) *dock-configs*)
      (let ((cfg (get-dock-config (car args))))
        (when cfg
          (proc cfg))))
  (trigger-ui-reload!))

(define (set-dock-position! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-position! cfg val)) args))

(define (set-dock-theme! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-theme! cfg val)) args))

(define (set-dock-icon-size! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-icon-size! cfg val)) args))

(define (set-dock-auto-hide! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-auto-hide?! cfg val)) args))

(define (set-dock-show-when-clear! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-show-when-clear?! cfg val)) args))

(define (set-dock-monitor! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-monitor! cfg val)) args))

(define (set-dock-hover-animate! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-hover-animate?! cfg val)) args))

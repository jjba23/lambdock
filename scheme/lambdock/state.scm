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

            ;; Getters (accept optional config-path argument)
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
            get-dock-hover-animate?))

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

;; =============================================================================
;; Configuration Interface
;; =============================================================================

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
    (hash-set! *dock-configs* config-path cfg)
    ;; Fall back 'default to the first configured dock if 'default is not set yet
    (when (not (hash-ref *dock-configs*
                         'default #f))
      (hash-set! *dock-configs*
                 'default cfg))
    (log-info "State"
     "Registered configuration for [~a]: ~a items, position: ~a, theme: ~a, repl?: ~a (~a)"
     config-path
     (if (list? items)
         (length items) 0)
     position
     theme
     enable-repl?
     repl-socket-path)))

(define (get-dock-config key)
  (or (hash-ref *dock-configs* key #f)
      (hash-ref *dock-configs*
                'default #f)
      ;; Fallback to the first available config in the hashtable
      (let ((keys (hash-fold (lambda (k v acc)
                               (cons k acc))
                             '() *dock-configs*)))
        (if (pair? keys)
            (hash-ref *dock-configs*
                      (car keys) #f) #f))))

;; =============================================================================
;; Getters
;; =============================================================================

(define (get-dock-items . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (if cfg
        (dock-config-items cfg)
        '())))

(define (get-dock-position . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (if cfg
        (dock-config-position cfg)
        'bottom)))

(define (get-dock-theme . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (if cfg
        (dock-config-theme cfg)
        'vanilla)))

(define (get-dock-icon-size . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (if cfg
        (dock-config-icon-size cfg) 48)))

(define (get-dock-auto-hide? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (if cfg
        (dock-config-auto-hide? cfg) #f)))

(define (get-dock-show-when-clear? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (if cfg
        (dock-config-show-when-clear? cfg) #t)))

(define (get-dock-monitor . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (if cfg
        (dock-config-monitor cfg)
        'default)))

(define (get-dock-exclude-apps . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (if cfg
        (dock-config-exclude-apps cfg)
        '())))

(define (get-dock-enable-repl? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (if cfg
        (dock-config-enable-repl? cfg) #f)))

(define (get-dock-repl-socket-path . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (if cfg
        (dock-config-repl-socket-path cfg) "/tmp/lambdock-repl.sock")))

(define (get-dock-hover-animate? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (if cfg
        (dock-config-hover-animate? cfg) #t)))

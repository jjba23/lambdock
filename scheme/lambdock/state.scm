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
            get-dock-config

            ;; Getters
            get-dock-items
            get-dock-position
            get-dock-theme
            get-dock-icon-size
            get-dock-item-spacing
            get-dock-auto-hide?
            get-dock-monitor
            get-dock-exclude-apps
            get-dock-enable-repl?
            get-dock-repl-socket-path
            get-dock-show-when-clear?
            get-dock-hover-animate?
            get-dock-track-windows?
            get-dock-mode
            get-dock-show-window-titles?
            get-dock-shelf-tilt
            get-dock-shelf-perspective
            get-dock-shelf-margin
            get-dock-shelf-front
            get-dock-reflection
            get-dock-reflection-intensity
            get-dock-reflection-blur

            ;; Setters
            set-dock-position!
            set-dock-theme!
            set-dock-icon-size!
            set-dock-item-spacing!
            set-dock-auto-hide!
            set-dock-show-when-clear!
            set-dock-monitor!
            set-dock-hover-animate!
            set-dock-track-windows!
            set-dock-mode!
            set-dock-show-window-titles!
            set-dock-shelf-tilt!
            set-dock-shelf-perspective!
            set-dock-shelf-margin!
            set-dock-shelf-front!
            set-dock-reflection!
            set-dock-reflection-intensity!
            set-dock-reflection-blur!))

;; =============================================================================
;; Dock Configuration Record
;; =============================================================================

(define-record-type <dock-config>
  (make-dock-config position
                    icon-size
                    item-spacing
                    items
                    auto-hide?
                    show-when-clear?
                    theme
                    monitor
                    exclude-apps
                    enable-repl?
                    repl-socket-path
                    hover-animate?
                    track-windows?
                    mode
                    show-window-titles?
                    shelf-tilt
                    shelf-perspective
                    shelf-margin
                    shelf-front
                    reflection
                    reflection-intensity
                    reflection-blur)
  dock-config?
  (position dock-config-position set-dock-config-position!)
  (icon-size dock-config-icon-size set-dock-config-icon-size!)
  (item-spacing dock-config-item-spacing set-dock-config-item-spacing!)
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
  (hover-animate? dock-config-hover-animate? set-dock-config-hover-animate?!)
  (track-windows? dock-config-track-windows? set-dock-config-track-windows?!)
  (mode dock-config-mode set-dock-config-mode!)
  (show-window-titles? dock-config-show-window-titles?
                       set-dock-config-show-window-titles?!)
  (shelf-tilt dock-config-shelf-tilt set-dock-config-shelf-tilt!)
  (shelf-perspective dock-config-shelf-perspective
                     set-dock-config-shelf-perspective!)
  (shelf-margin dock-config-shelf-margin set-dock-config-shelf-margin!)
  (shelf-front dock-config-shelf-front set-dock-config-shelf-front!)
  (reflection dock-config-reflection set-dock-config-reflection!)
  (reflection-intensity dock-config-reflection-intensity
                        set-dock-config-reflection-intensity!)
  (reflection-blur dock-config-reflection-blur
                   set-dock-config-reflection-blur!))

;; Global hashtable storing isolated <dock-config> records keyed by config path / symbol
(define *dock-configs*
  (make-hash-table))

;; Fallback default record when key is missing or unregistered
(define %default-fallback-config%
  (make-dock-config 'bottom
                    48
                    4       ; item spacing (px between dock items)
                    '()
                    #f
                    #t
                    'vanilla
                    'default
                    '()
                    #f
                    "/tmp/lambdock-repl.sock"
                    #t
                    #t
                    'dock
                    #f
                    0       ; shelf-tilt (degrees)
                    400     ; shelf-perspective (px)
                    24      ; shelf-margin (px, C's CAPSULE_PADDING_PX)
                    0       ; shelf-front (px)
                    0.0     ; reflection (band height / icon size)
                    0.55    ; reflection-intensity
                    0.05))  ; reflection-blur (blur radius / icon size)

;; =============================================================================
;; Configuration Interface
;; =============================================================================

;; === FILE: state.scm (Excerpt: configure-dock!) ===
(define* (configure-dock! #:key (config-path 'default)
                          (position 'bottom)
                          (icon-size 48)
                          (item-spacing 4)
                          (items '())
                          (auto-hide? #f)
                          (show-when-clear? #t)
                          (theme 'vanilla)
                          (monitor 'default)
                          (exclude-apps '())
                          (enable-repl? #f)
                          (repl-socket-path "/tmp/lambdock-repl.sock")
                          (hover-animate? #t)
                          (track-windows? #t)
                          (mode 'dock)
                          (show-window-titles? #f)
                          (shelf-tilt 0)
                          (shelf-perspective 400)
                          (shelf-margin 24)
                          (shelf-front 0)
                          (reflection 0.0)
                          (reflection-intensity 0.55)
                          (reflection-blur 0.05))
  "Sets and registers configuration parameters for a specific dock instance key or file path."
  (let ((cfg (make-dock-config position
                               icon-size
                               item-spacing
                               items
                               auto-hide?
                               show-when-clear?
                               theme
                               monitor
                               exclude-apps
                               enable-repl?
                               repl-socket-path
                               hover-animate?
                               track-windows?
                               mode
                               show-window-titles?
                               shelf-tilt
                               shelf-perspective
                               shelf-margin
                               shelf-front
                               reflection
                               reflection-intensity
                               reflection-blur)))
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
     "Registered config for [~a]: ~a items, pos: ~a, theme: ~a, auto-hide?: ~a, repl?: ~a (~a), track-windows?: ~a, mode: ~a, icon ~a/spacing ~a (shelf: tilt ~a, margin ~a, front ~a, reflection ~a)"
     config-path
     (if (list? items)
         (length items) 0)
     position
     theme
     auto-hide?
     enable-repl?
     repl-socket-path
     track-windows?
     mode
     icon-size
     item-spacing
     shelf-tilt
     shelf-margin
     shelf-front
     reflection)))

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

(define (get-dock-item-spacing . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-item-spacing cfg)))

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

(define (get-dock-track-windows? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-track-windows? cfg)))

(define (get-dock-mode . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-mode cfg)))

(define (get-dock-show-window-titles? . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-show-window-titles? cfg)))

;; --- Shelf geometry / reflection parameters ---------------------------------
;; The dock has no modes: these numbers are its whole shape. The defaults
;; describe the classic flat capsule.

(define (get-dock-shelf-tilt . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-shelf-tilt cfg)))

(define (get-dock-shelf-perspective . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-shelf-perspective cfg)))

(define (get-dock-shelf-margin . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-shelf-margin cfg)))

(define (get-dock-shelf-front . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-shelf-front cfg)))

(define (get-dock-reflection . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-reflection cfg)))

(define (get-dock-reflection-intensity . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-reflection-intensity cfg)))

(define (get-dock-reflection-blur . args)
  (let ((cfg (get-dock-config (if (null? args)
                                  'default
                                  (car args)))))
    (dock-config-reflection-blur cfg)))

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

(define (set-dock-item-spacing! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-item-spacing! cfg val)) args))

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

(define (set-dock-track-windows! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-track-windows?! cfg val)) args))

(define (set-dock-mode! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-mode! cfg val)) args))

(define (set-dock-show-window-titles! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-show-window-titles?! cfg val)) args))

;; --- Shelf geometry / reflection setters ------------------------------------

(define (set-dock-shelf-tilt! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-shelf-tilt! cfg val)) args))

(define (set-dock-shelf-perspective! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-shelf-perspective! cfg val)) args))

(define (set-dock-shelf-margin! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-shelf-margin! cfg val)) args))

(define (set-dock-shelf-front! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-shelf-front! cfg val)) args))

(define (set-dock-reflection! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-reflection! cfg val)) args))

(define (set-dock-reflection-intensity! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-reflection-intensity! cfg val)) args))

(define (set-dock-reflection-blur! val . args)
  (apply-to-targets (lambda (cfg)
                      (set-dock-config-reflection-blur! cfg val)) args))

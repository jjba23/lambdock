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

(define-module (lambdock core)
  #:declarative? #f
  #:use-module (ice-9 format)
  #:use-module (lambdock items)
  #:use-module (lambdock state)
  #:use-module (lambdock loader)
  #:use-module (lambdock logger)
  #:use-module (system repl repl)
  #:re-export (<dock-item> dock-item?
                           dock-item-type
                           dock-item-name
                           dock-item-icon
                           dock-item-exec
                           app-item
                           separator-item
                           configure-dock!
                           get-dock-items
                           get-dock-position
                           get-dock-theme
                           get-dock-monitor
                           get-icon-size
                           get-dock-auto-hide?
                           get-dock-exclude-apps
                           get-dock-enable-repl?
                           resolve-dock-configuration)
  #:export (init-dock!))

;; =============================================================================
;; Core Hooks
;; =============================================================================

(define (init-dock!)
  "Entry point invoked after C runtime initialization."
  (let ((config-file (get-config-path)))
    (if (and config-file
             (file-exists? config-file))
        (begin
          (log-info "Core" "Loading user settings from: ~a" config-file)
          (catch #t
                 (lambda ()
                   (load config-file)
                   (let* ((settings (resolve-dock-configuration)))
                     (configure-dock! #:position (assq-ref settings
                                                           'dock-position)
                                      #:icon-size (assq-ref settings
                                                            'dock-icon-size)
                                      #:items (assq-ref settings
                                                        'dock-items)
                                      #:theme (assq-ref settings
                                                        'dock-theme)
                                      #:monitor (assq-ref settings
                                                          'dock-monitor)
                                      #:auto-hide? (assq-ref settings
                                                             'dock-auto-hide?)
                                      #:exclude-apps (assq-ref settings
                                                               'dock-exclude-apps))))
                 (lambda (key . args)
                   (log-error "Core" "ERROR loading ~a: ~a ~a~%" config-file
                              key args))))
        (log-warn "Core" "No settings file found at: ~a~%" config-file)))

  ;; Load fallback default dock items if no items were registered
  (when (null? *dock-items*)
    (log-warn "Core"
              "No configured items found, loading default configuration...")
    (configure-dock! #:position 'bottom
                     #:icon-size 48
                     #:items (list (app-item #:name "Terminal"
                                             #:icon "Alacritty"
                                             #:exec "alacritty"))
                     #:auto-hide? #f)))

;; =============================================================================
;; REPL Management
;; =============================================================================

(define *repl-started?*
  #f)

(define (start-repl-if-enabled!)
  (when (and (get-dock-enable-repl?)
             (not *repl-started?*))
    (set! *repl-started?* #t)
    (log-info "Core" "Starting interactive Guile REPL background thread...")
    (call-with-new-thread (lambda ()
                            (start-repl)))))

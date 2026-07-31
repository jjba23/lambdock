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
  #:re-export (<dock-item> dock-item?
                           dock-item-type
                           dock-item-name
                           dock-item-icon
                           dock-item-exec
                           app-item
                           separator-item
                           terminal-item
                           configure-dock!
                           get-dock-items
                           get-dock-position
                           get-icon-size
                           get-dock-auto-hide?
                           resolve-dock-configuration)
  #:export (init-dock!))

;; =============================================================================
;; Core Hooks (Called by main.c)
;; =============================================================================

(define (init-dock!)
  "Entry point invoked after C runtime initialization."
  (format #t "[Scheme] Core dock module initialized.~%")

  ;; Attempt to load the user's configuration file
  (let ((config-file (get-config-path)))
    (if (and config-file
             (file-exists? config-file))
        (begin
          (format #t "[Scheme] Loading user settings from: ~a~%" config-file)
          (catch #t
                 (lambda ()
                   (load config-file)
                   (let* ((settings (resolve-dock-configuration)))
                     (write settings)
                     (configure-dock! #:position (assq-ref settings
                                                           'dock-position)
                                      #:icon-size (assq-ref settings
                                                            'dock-icon-size)
                                      #:items (assq-ref settings
                                                        'dock-items)
                                      #:auto-hide? (assq-ref settings
                                                             'dock-auto-hide?))))
                 (lambda (key . args)
                   (format #t "[Scheme] ERROR loading ~a: ~a ~a~%" config-file
                           key args))))
        (format #t "[Scheme] No settings file found at: ~a~%" config-file)))

  ;; Load fallback default dock items if no items were registered
  (when (null? *dock-items*)
    (format #t
     "[Scheme] No configured items found, loading default configuration...~%")
    (configure-dock! #:position 'bottom
                     #:icon-size 48
                     #:items (list (terminal-item))
                     #:auto-hide? #f))
  *dock-items*)

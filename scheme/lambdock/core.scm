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
  #:use-module (srfi srfi-9)
  #:use-module (ice-9 format)
  #:export (<dock-item> dock-item?
                        dock-item-type
                        dock-item-name
                        dock-item-icon
                        dock-item-exec

                        ;; Generic Constructors
                        app-item
                        separator-item

                        ;; Specialized App Helper Constructors
                        terminal-item
                        browser-item
                        files-item
                        editor-item
                        settings-item
                        audio-item
                        sysmon-item

                        ;; State & Configuration
                        configure-dock!
                        get-dock-items
                        get-dock-position
                        get-icon-size

                        ;; Core Initialization
                        init-dock!
                        resolve-dock-configuration))

(define* (resolve-dock-configuration #:key (module-name '(lambdock settings)))
  (let* ((mod (resolve-module module-name))
         (public-mod (module-public-interface mod))
         ;; Check if a public interface exists AND contains at least one export.
         (has-exports? (and public-mod
                            (not (null? (module-map (lambda (name var)
                                                      name) public-mod)))))
         ;; If it has exports, search only those. Otherwise, fall back to the whole module.
         (target-mod (if has-exports? public-mod mod))
         (settings (filter (lambda (x)
                             (not (equal? #f x)))
                           (module-map (lambda (name var)
                                         (cons name
                                               (module-ref mod name)))
                                       target-mod))))
    settings))

;; =============================================================================
;; Data Structures
;; =============================================================================

;; Internal record type definition with private accessor names
(define-record-type <dock-item>
  (make-dock-item type name icon exec)
  dock-item?
  (type %dock-item-type)
  (name %dock-item-name)
  (icon %dock-item-icon)
  (exec %dock-item-exec))

;; Public procedure wrappers
(define (dock-item-type item)
  (%dock-item-type item))

(define (dock-item-name item)
  (%dock-item-name item))

(define (dock-item-icon item)
  (%dock-item-icon item))

(define (dock-item-exec item)
  (%dock-item-exec item))

;; Generic constructor for custom app launchers
(define* (app-item #:key (name "Unknown")
                   (icon "application-x-executable")
                   (exec ""))
  (make-dock-item 'app name icon exec))

;; Generic constructor for separators
(define (separator-item)
  (make-dock-item 'separator "" "" ""))

;; =============================================================================
;; Specialized Helper Constructors
;; =============================================================================

(define* (terminal-item #:key (name "Terminal")
                        (icon "utilities-terminal")
                        (exec "alacritty"))
  "Constructs a terminal launcher."
  (app-item #:name name
            #:icon icon
            #:exec exec))

;; =============================================================================
;; Application State
;; =============================================================================

(define *dock-items*
  '())

(define *dock-position*
  'bottom)

(define *dock-icon-size*
  48)

;; =============================================================================
;; Configuration Interface
;; =============================================================================

(define* (configure-dock! #:key position icon-size items)
  "Sets the primary configuration parameters for the dock."
  (set! *dock-position* position)
  (set! *dock-icon-size* icon-size)
  (set! *dock-items* items)
  (format #t
   "[Scheme] Configuration loaded: ~a items registered at size ~apx.~%"
   (length *dock-items*) *dock-icon-size*))

(define (get-dock-items)
  *dock-items*)

(define (get-dock-position)
  *dock-position*)

(define (get-icon-size)
  *dock-icon-size*)

;; =============================================================================
;; Config File Resolution
;; =============================================================================

(define (get-config-path)
  "Resolves the user configuration path based on XDG guidelines."
  (let ((xdg-config (getenv "XDG_CONFIG_HOME"))
        (home (getenv "HOME")))
    (cond
      ((and xdg-config
            (not (string-null? xdg-config)))
       (string-append xdg-config "/lambdock/settings.scm"))
      (home (string-append home "/.config/lambdock/settings.scm"))
      (else #f))))

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
                                                        'dock-items))))
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
                     #:items (list (terminal-item))))
  *dock-items*)

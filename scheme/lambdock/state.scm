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

            ;; Setters
            set-dock-items!
            set-dock-position!
            set-dock-theme!
            set-dock-icon-size!
            set-dock-auto-hide!
            set-dock-monitor!
            set-dock-exclude-apps!
            set-dock-enable-repl!
            set-dock-repl-socket-path!))

;; =============================================================================
;; Application State Variables
;; =============================================================================

(define *dock-items*
  '())

(define *dock-position*
  'bottom)

(define *dock-theme*
  'vanilla)

(define *dock-icon-size*
  48)

(define *dock-auto-hide?*
  #f)

(define *dock-monitor*
  'default)

(define *dock-exclude-apps*
  '())

(define *dock-enable-repl?*
  #f)

(define *dock-repl-socket-path*
  "/tmp/lambdock-repl.sock")

;; =============================================================================
;; Getters
;; =============================================================================

(define (get-dock-items)
  *dock-items*)

(define (get-dock-position)
  *dock-position*)

(define (get-dock-theme)
  *dock-theme*)

(define (get-dock-icon-size)
  *dock-icon-size*)

(define (get-dock-auto-hide?)
  *dock-auto-hide?*)

(define (get-dock-monitor)
  *dock-monitor*)

(define (get-dock-exclude-apps)
  *dock-exclude-apps*)

(define (get-dock-enable-repl?)
  *dock-enable-repl?*)

(define (get-dock-repl-socket-path)
  *dock-repl-socket-path*)

;; =============================================================================
;; Setters
;; =============================================================================

(define (set-dock-items! items)
  (when (list? items)
    (set! *dock-items* items)))

(define (set-dock-position! pos)
  (when pos
    (set! *dock-position* pos)))

(define (set-dock-theme! theme)
  (when theme
    (set! *dock-theme* theme)))

(define (set-dock-icon-size! size)
  (when (and (number? size)
             (> size 0))
    (set! *dock-icon-size* size)))

(define (set-dock-auto-hide! auto-hide?)
  (when (boolean? auto-hide?)
    (set! *dock-auto-hide?* auto-hide?)))

(define (set-dock-monitor! monitor)
  (when monitor
    (set! *dock-monitor* monitor)))

(define (set-dock-exclude-apps! exclude-apps)
  (when (list? exclude-apps)
    (set! *dock-exclude-apps* exclude-apps)))

(define (set-dock-enable-repl! enable?)
  (when (boolean? enable?)
    (set! *dock-enable-repl?* enable?)))

(define (set-dock-repl-socket-path! path)
  (when (and (string? path)
             (not (string-null? path)))
    (set! *dock-repl-socket-path* path)))

;; =============================================================================
;; Configuration Interface
;; =============================================================================

(define* (configure-dock! #:key (position 'bottom)
                          (icon-size 48)
                          (items '())
                          (auto-hide? #f)
                          (theme 'vanilla)
                          (monitor 'default)
                          (exclude-apps '())
                          (enable-repl? #f)
                          (repl-socket-path "/tmp/lambdock-repl.sock"))
  "Sets the primary configuration parameters for the dock."
  (log-info "State"
   "Registering: ~a items at ~apx, theme: ~a, auto-hide?: ~a, position: ~a, monitor: ~a, excluded items: ~a, repl?: ~a, repl-socket: ~a"
   (if (list? items)
       (length items) 0)
   icon-size
   theme
   auto-hide?
   position
   monitor
   (if (list? exclude-apps)
       (length exclude-apps) 0)
   enable-repl?
   repl-socket-path)

  (set-dock-position! position)
  (set-dock-theme! theme)
  (set-dock-icon-size! icon-size)
  (set-dock-items! items)
  (set-dock-auto-hide! auto-hide?)
  (set-dock-monitor! monitor)
  (set-dock-exclude-apps! exclude-apps)
  (set-dock-enable-repl! enable-repl?)
  (set-dock-repl-socket-path! repl-socket-path)

  (log-info "State" "Configuration loaded successfully."))

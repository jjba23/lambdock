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
  #:export (configure-dock! get-dock-items
                            get-dock-position
                            get-icon-size
                            get-dock-auto-hide?
                            get-dock-theme
                            *dock-theme*
                            *dock-items*
                            *dock-position*
                            *dock-icon-size*
                            *dock-auto-hide?*))

;; =============================================================================
;; Application State
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

;; =============================================================================
;; Configuration Interface
;; =============================================================================

(define* (configure-dock! #:key position
                          icon-size
                          items
                          auto-hide?
                          theme)
  "Sets the primary configuration parameters for the dock."
  (set! *dock-position* position)
  (set! *dock-theme* theme)
  (set! *dock-icon-size* icon-size)
  (set! *dock-items* items)
  (set! *dock-auto-hide?* auto-hide?)

  (format #t
   "
[Scheme] Configuration loaded: ~a items registered at size ~apx, theme: ~a, auto-hide?: ~a, position: ~a.
"
   (length items)
   icon-size
   theme
   auto-hide?
   position))

(define (get-dock-items)
  *dock-items*)

(define (get-dock-position)
  *dock-position*)

(define (get-dock-theme)
  *dock-theme*)

(define (get-icon-size)
  *dock-icon-size*)

(define (get-dock-auto-hide?)
  *dock-auto-hide?*)

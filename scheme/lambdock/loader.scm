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

(define-module (lambdock loader)
  #:declarative? #t
  #:export (get-config-path resolve-dock-configuration))

;; =============================================================================
;; Config File Resolution & Dynamic Module Inspection
;; =============================================================================

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

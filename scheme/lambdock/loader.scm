;; lambdock -- hyper-hackable desktop dock for Wayland
;;
;; SPDX-License-Identifier: MIT
;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

(define-module (lambdock loader)
  #:declarative? #f
  #:use-module (srfi srfi-1)
  #:use-module (lambdock logger)
  #:use-module (lambdock looks)
  #:export (get-config-path resolve-dock-configuration))

;; =============================================================================
;; Config File Resolution & Isolated Module Inspection
;; =============================================================================

(define (resolve-dock-configuration file-path)
  "Evaluates FILE-PATH inside a fresh, isolated module and extracts defined symbols."
  (if (and file-path
           (string? file-path)
           (file-exists? file-path))
      (begin
        (log-debug "Loader" "Resolving isolated module evaluation for: ~a"
                   file-path)
        (let ((anon-mod (make-fresh-user-module)))
          (save-module-excursion (lambda ()
                                   (set-current-module anon-mod)
                                   ;; Shadow define-module so settings files don't switch away from anon-mod
                                   (eval '(define-syntax define-module
                                            (syntax-rules ()
                                                          ((_ . rest) #f)))
                                         anon-mod)
                                   (module-use! anon-mod
                                                (resolve-module '(lambdock
                                                                  core)))
                                   (module-use! anon-mod
                                                (resolve-module '(lambdock
                                                                  items)))
                                   (module-use! anon-mod
                                                (resolve-module '(lambdock
                                                                  presets)))
                                   (primitive-load file-path)))

          ;; Filter locally defined variables
          (let ((extracted (filter-map (lambda (sym)
                                         (let ((var (module-local-variable
                                                     anon-mod sym)))
                                           (if (and var
                                                    (variable-bound? var)
                                                    (not (eq? sym
                                                              'define-module)))
                                               (let ((val (variable-ref var)))
                                                 (log-debug "Loader"
                                                  "  [Key Found] ~a => ~a" sym
                                                  val)
                                                 (cons sym val)) #f)))
                                       (module-map (lambda (sym var)
                                                     sym) anon-mod))))
            
            (log-info "Loader" "Extracted ~a configuration keys from ~a"
                      (length extracted) file-path) extracted)))
      (begin
        (log-warn "Loader" "Target settings file path invalid or missing: ~a"
                  file-path)
        '())))

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

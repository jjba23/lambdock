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
  #:use-module (ice-9 threads)
  #:use-module (lambdock items)
  #:use-module (lambdock state)
  #:use-module (lambdock loader)
  #:use-module (lambdock logger)
  #:use-module (lambdock presets)
  #:use-module (system repl server)
  #:re-export ( ;Item structures
                <dock-item>
               dock-item?
               dock-item-type
               dock-item-name
               dock-item-icon
               dock-item-exec
               dock-item-poll-fn
               dock-item-poll-interval
               dock-item-hover-animate?
               app-item
               separator-item
               dynamic-item
               ;; Dynamic items
               get-free-ram-mb
               ram-item
               get-clock-text
               clock-item
               get-cpu-loadavg
               cpu-item
               get-battery-status
               battery-item
               get-cpu-temp
               cpu-temp-item

               ;; Primary Configurator
               configure-dock!

               ;; Getters
               get-dock-items
               get-dock-position
               get-dock-theme
               get-dock-icon-size
               get-dock-auto-hide?
               get-dock-show-when-clear?
               get-dock-monitor
               get-dock-exclude-apps
               get-dock-enable-repl?
               get-dock-repl-socket-path
               get-dock-hover-animate?

               ;; Config Loader
               resolve-dock-configuration
               ;; Alists
               preset-icons
               preset-launchers

               ;; Lookup & Convenience Functions
               preset-icon
               preset-launcher
               preset-launchers-for)
  #:export (init-dock! load-dock-config! start-repl!))

;; =============================================================================
;; Core Hooks
;; =============================================================================

(define* (load-dock-config! file-path)
  "Loads a specific settings file and registers its configuration record under its file-path."
  (if (and file-path
           (file-exists? file-path))
      (begin
        (log-info "Core" "Loading user settings from: ~a" file-path)
        (catch #t
               (lambda ()
                 (load file-path)
                 (let* ((settings (resolve-dock-configuration))
                        (cfg (lambda (key)
                               (assq-ref settings key))))
                   (configure-dock! #:config-path file-path
                                    #:position (cfg 'dock-position)
                                    #:icon-size (cfg 'dock-icon-size)
                                    #:items (cfg 'dock-items)
                                    #:theme (cfg 'dock-theme)
                                    #:monitor (cfg 'dock-monitor)
                                    #:auto-hide? (cfg 'dock-auto-hide?)
                                    #:show-when-clear? (cfg 'dock-show-when-clear?)
                                    #:exclude-apps (cfg 'dock-exclude-apps)
                                    #:enable-repl? (cfg 'dock-enable-repl?)
                                    #:repl-socket-path (cfg 'dock-repl-socket-path)
                                    #:hover-animate? (cfg 'dock-hover-animate?))))
               (lambda (key . args)
                 (log-error "Core" "ERROR loading ~a: ~a ~a~%" file-path key
                            args))))
      (log-warn "Core" "No settings file found at: ~a~%" file-path)))

(define (init-dock!)
  "Entry point invoked after C runtime initialization."
  (let ((config-file (get-config-path)))
    (when config-file
      (load-dock-config! config-file)))

  ;; Load fallback default dock items if no items were registered
  (when (null? (get-dock-items))
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

(define *active-repl-sockets*
  '())

(define* (start-repl! . args)
  (define (mkdir-p path)
    (unless (or (string-null? path)
                (file-exists? path))
      (let ((parent (dirname path)))
        (when (and (not (string=? parent path))
                   (not (string=? parent ".")))
          (mkdir-p parent))
        (catch 'system-error
               (lambda ()
                 (mkdir path))
               (lambda (key . args)
                 (unless (and (file-exists? path)
                              (eq? 'directory
                                   (stat:type (stat path))))
                   (apply throw key args)))))))

  (let* ((config-key (if (pair? args)
                         (car args)
                         'default))
         (socket-path (get-dock-repl-socket-path config-key))
         (enabled? (get-dock-enable-repl? config-key)))
    (when (and enabled?
               (not (member socket-path *active-repl-sockets*)))
      (set! *active-repl-sockets*
            (cons socket-path *active-repl-sockets*))
      (log-info "Core" "Starting interactive Guile REPL Unix socket at ~a..."
                socket-path)
      (call-with-new-thread (lambda ()
                              (catch #t
                                     (lambda ()
                                       (let ((dir (dirname socket-path)))
                                         (when (and (not (string-null? dir))
                                                    (not (file-exists? dir)))
                                           (mkdir-p dir)))
                                       (when (file-exists? socket-path)
                                         (delete-file socket-path))
                                       (let ((server-socket (make-unix-domain-server-socket
                                                             #:path
                                                             socket-path)))
                                         (spawn-server server-socket)))
                                     (lambda (key . args)
                                       (log-error "Core"
                                        "Failed to start REPL server on ~a (~a): ~a"
                                        socket-path key args))))))))

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

(define-module (lambdock presets)
  #:declarative? #t
  #:use-module (srfi srfi-1)
  #:use-module (lambdock items)
  #:use-module (lambdock logger)
  #:export ( ;Alists
             preset-icons
            preset-launchers

            ;; Lookup & Convenience Functions
            preset-icon
            preset-launcher
            preset-launchers-for))

;; =============================================================================
;; Lookup & Convenience Functions
;; =============================================================================

(define (preset-icon key)
  "Looks up and returns the preset icon string for KEY (symbol). Returns #f if unmapped."
  (assq-ref preset-icons key))

(define (preset-launcher key)
  "Looks up and returns the preset app-item for KEY (symbol). Returns #f if unmapped."
  (assq-ref preset-launchers key))

(define (preset-launchers-for . keys)
  "Retrieves a list of preset app-items for the specified KEY symbols.
Accepts variadic symbols e.g. (preset-launchers-for 'alacritty 'firefox)
or a single list e.g. (preset-launchers-for '(alacritty firefox))."
  (let ((target-keys (if (and (pair? keys)
                              (list? (car keys)))
                         (car keys) keys)))
    (filter-map (lambda (k)
                  (let ((item (preset-launcher k)))
                    (unless item
                      (log-warn "Presets"
                                "No preset launcher found for key: ~a" k))
                    item)) target-keys)))

;; =============================================================================
;; Association List: Preset Icons
;; =============================================================================

(define preset-icons
  '((google-chrome . "google-chrome") (firefox . "firefox")
    (chromium . "chromium")
    (brave . "brave-browser")
    (nyxt . "engineer.atlas.Nyxt")

    (alacritty . "Alacritty")
    (kitty . "kitty")
    (foot . "foot")
    (wezterm . "org.wezfurlong.wezterm")
    (gnome-terminal . "org.gnome.Terminal")

    (emacs . "emacs")
    (vscode . "code")
    (intellij . "jetbrains-idea")
    (bruno . "com.usebruno.Bruno")

    (nautilus . "org.gnome.Nautilus")
    (gnome-calendar . "org.gnome.Calendar")
    (gnome-calculator . "org.gnome.Calculator")
    (pavucontrol . "pavucontrol")
    (btop . "btop")
    (onepassword . "1password")

    (spotify . "com.spotify.Client")
    (zapzap . "com.rtosta.zapzap")
    (discord . "com.discordapp.Discord")
    (slack . "com.slack.Slack")
    (telegram . "org.telegram.desktop")
    (gimp . "gimp")
    (inkscape . "org.inkscape.Inkscape")
    (obs . "com.obsproject.Studio")))

;; =============================================================================
;; Association List: Preset App Launchers
;; =============================================================================

(define preset-launchers
  `((google-chrome unquote
                   (app-item #:name "Google Chrome"
                             #:exec "google-chrome"
                             #:icon "google-chrome"))
    (firefox unquote
             (app-item #:name "Firefox"
                       #:exec "firefox"
                       #:icon "firefox"))
    (chromium unquote
              (app-item #:name "Chromium"
                        #:exec "chromium"
                        #:icon "chromium"))
    (brave unquote
           (app-item #:name "Brave Browser"
                     #:exec "brave-browser"
                     #:icon "brave-browser"))
    (nyxt unquote
          (app-item #:name "Nyxt"
                    #:exec "flatpak run engineer.atlas.Nyxt"
                    #:icon "engineer.atlas.Nyxt"))

    (alacritty unquote
               (app-item #:name "Terminal (Alacritty)"
                         #:exec "alacritty"
                         #:icon "Alacritty"))
    (kitty unquote
           (app-item #:name "Kitty"
                     #:exec "kitty"
                     #:icon "kitty"))
    (foot unquote
          (app-item #:name "Foot"
                    #:exec "foot"
                    #:icon "foot"))
    (wezterm unquote
             (app-item #:name "WezTerm"
                       #:exec "wezterm"
                       #:icon "org.wezfurlong.wezterm"))
    (gnome-terminal unquote
                    (app-item #:name "GNOME Terminal"
                              #:exec "gnome-terminal"
                              #:icon "org.gnome.Terminal"))

    (emacs unquote
           (app-item #:name "Emacs"
                     #:exec "emacsclient -c"
                     #:icon "emacs"))
    (vscode unquote
            (app-item #:name "Visual Studio Code"
                      #:exec "code"
                      #:icon "code"))
    (intellij unquote
              (app-item #:name "IntelliJ IDEA"
                        #:exec "intellij-idea"
                        #:icon "jetbrains-idea"))
    (bruno unquote
           (app-item #:name "Bruno"
                     #:exec "flatpak run com.usebruno.Bruno"
                     #:icon "com.usebruno.Bruno"))

    (nautilus unquote
              (app-item #:name "Files"
                        #:exec "nautilus -w"
                        #:icon "org.gnome.Nautilus"))
    (gnome-calendar unquote
                    (app-item #:name "Agenda"
                              #:exec "gnome-calendar"
                              #:icon "org.gnome.Calendar"))
    (gnome-calculator unquote
                      (app-item #:name "Calculator"
                                #:exec "gnome-calculator"
                                #:icon "org.gnome.Calculator"))
    (pavucontrol unquote
                 (app-item #:name "Audio Manager"
                           #:exec "pavucontrol"
                           #:icon "pavucontrol"))
    (btop unquote
          (app-item #:name "System Monitor"
                    #:exec "alacritty -e btop"
                    #:icon "btop"))
    (onepassword unquote
                 (app-item #:name "1Password"
                           #:exec "1password"
                           #:icon "1password"))

    (spotify unquote
             (app-item #:name "Spotify"
                       #:exec "flatpak run com.spotify.Client"
                       #:icon "com.spotify.Client"))
    (zapzap unquote
            (app-item #:name "WhatsApp"
                      #:exec "flatpak run com.rtosta.zapzap"
                      #:icon "com.rtosta.zapzap"))
    (discord unquote
             (app-item #:name "Discord"
                       #:exec "flatpak run com.discordapp.Discord"
                       #:icon "com.discordapp.Discord"))
    (slack unquote
           (app-item #:name "Slack"
                     #:exec "slack"
                     #:icon "com.slack.Slack"))
    (telegram unquote
              (app-item #:name "Telegram"
                        #:exec "telegram-desktop"
                        #:icon "org.telegram.desktop"))
    (gimp unquote
          (app-item #:name "GIMP"
                    #:exec "gimp"
                    #:icon "gimp"))
    (inkscape unquote
              (app-item #:name "Inkscape"
                        #:exec "inkscape"
                        #:icon "org.inkscape.Inkscape"))
    (obs unquote
         (app-item #:name "OBS Studio"
                   #:exec "obs"
                   #:icon "com.obsproject.Studio"))))

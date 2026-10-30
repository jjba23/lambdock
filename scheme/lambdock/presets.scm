;; lambdock -- hyper-hackable desktop dock for Wayland
;;
;; SPDX-License-Identifier: MIT
;; Copyright © Josep Bigorra <jjbigorra@gmail.com>

(define-module (lambdock presets)
  #:declarative? #t
  #:use-module (srfi srfi-1)
  #:use-module (lambdock items)
  #:use-module (lambdock looks)
  #:use-module (lambdock logger)
  #:re-export (dock-looks
               look-aliases
               look-name
               preset-look
               look-settings
               look-setting
               look?)
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
  '( ;Web Browsers
     (google-chrome . "google-chrome")
    (google-chrome-flatpak . "com.google.Chrome")
    (firefox . "firefox")
    (firefox-flatpak . "org.mozilla.firefox")
    (chromium . "chromium")
    (chromium-flatpak . "org.chromium.Chromium")
    (brave . "brave-browser")
    (brave-flatpak . "com.brave.Browser")
    (nyxt . "engineer.atlas.Nyxt")
    (nyxt-flatpak . "engineer.atlas.Nyxt")
    (zen-browser . "zen-browser")
    (zen-browser-flatpak . "app.zen_browser.zen")
    (librewolf . "librewolf")
    (librewolf-flatpak . "io.gitlab.librewolf-community")

    ;; Terminal Emulators
    (alacritty . "Alacritty")
    (alacritty-flatpak . "org.alacritty.Alacritty")
    (kitty . "kitty")
    (kitty-flatpak . "org.kovidgoyal.kitty")
    (foot . "foot")
    (foot-flatpak . "org.codeberg.dnkl.foot")
    (wezterm . "org.wezfurlong.wezterm")
    (wezterm-flatpak . "org.wezfurlong.wezterm")
    (gnome-terminal . "org.gnome.Terminal")
    (gnome-terminal-flatpak . "org.gnome.Terminal")
    (ghostty . "com.mitchellh.ghostty")
    (ghostty-flatpak . "com.mitchellh.ghostty")

    ;; Editors & IDEs
    (emacs . "emacs")
    (emacs-flatpak . "org.gnu.emacs")
    (vscode . "code")
    (vscode-flatpak . "com.visualstudio.code")
    (intellij . "jetbrains-idea")
    (intellij-flatpak . "com.jetbrains.IntelliJ-IDEA-Community")
    (bruno . "com.usebruno.Bruno")
    (bruno-flatpak . "com.usebruno.Bruno")
    (zed . "dev.zed.Zed")
    (zed-flatpak . "dev.zed.Zed")
    (neovim . "nvim")
    (neovim-flatpak . "io.neovim.nvim")

    ;; System & Utilities
    (nautilus . "org.gnome.Nautilus")
    (nautilus-flatpak . "org.gnome.Nautilus")
    (gnome-calendar . "org.gnome.Calendar")
    (gnome-calendar-flatpak . "org.gnome.Calendar")
    (gnome-calculator . "org.gnome.Calculator")
    (gnome-calculator-flatpak . "org.gnome.Calculator")
    (pavucontrol . "pavucontrol")
    (pavucontrol-flatpak . "org.pulseaudio.pavucontrol")
    (btop . "btop")
    (btop-flatpak . "io.github.aristocratos.btop")
    (trash . "user-trash")
    (onepassword . "1password")
    (onepassword-flatpak . "com.onepassword.OnePassword")
    (thunderbird . "thunderbird")
    (thunderbird-flatpak . "org.mozilla.Thunderbird")
    (bitwarden . "bitwarden")
    (bitwarden-flatpak . "com.bitwarden.desktop")

    ;; Media, Graphics & Messaging
    (spotify . "spotify")
    (spotify-flatpak . "com.spotify.Client")
    (zapzap . "com.rtosta.zapzap")
    (zapzap-flatpak . "com.rtosta.zapzap")
    (discord . "discord")
    (discord-flatpak . "com.discordapp.Discord")
    (slack . "com.slack.Slack")
    (slack-flatpak . "com.slack.Slack")
    (telegram . "org.telegram.desktop")
    (telegram-flatpak . "org.telegram.desktop")
    (gimp . "gimp")
    (gimp-flatpak . "org.gimp.GIMP")
    (inkscape . "org.inkscape.Inkscape")
    (inkscape-flatpak . "org.inkscape.Inkscape")
    (obs . "com.obsproject.Studio")
    (obs-flatpak . "com.obsproject.Studio")
    (vlc . "vlc")
    (vlc-flatpak . "org.videolan.VLC")
    (steam . "steam")
    (steam-flatpak . "com.valvesoftware.Steam")
    (blender . "blender")
    (blender-flatpak . "org.blender.Blender")))

;; =============================================================================
;; Association List: Preset App Launchers
;; =============================================================================

(define preset-launchers
  `( ;Web Browsers
     (google-chrome unquote
                    (app-item #:name "Google Chrome"
                              #:exec "google-chrome"
                              #:icon "google-chrome"))
    (google-chrome-flatpak unquote
                           (app-item #:name "Google Chrome"
                                     #:exec "flatpak run com.google.Chrome"
                                     #:icon "com.google.Chrome"))
    (firefox unquote
             (app-item #:name "Firefox"
                       #:exec "firefox"
                       #:icon "firefox"))
    (firefox-flatpak unquote
                     (app-item #:name "Firefox"
                               #:exec "flatpak run org.mozilla.firefox"
                               #:icon "org.mozilla.firefox"))
    (chromium unquote
              (app-item #:name "Chromium"
                        #:exec "chromium"
                        #:icon "chromium"))
    (chromium-flatpak unquote
                      (app-item #:name "Chromium"
                                #:exec "flatpak run org.chromium.Chromium"
                                #:icon "org.chromium.Chromium"))
    (brave unquote
           (app-item #:name "Brave Browser"
                     #:exec "brave-browser"
                     #:icon "brave-browser"))
    (brave-flatpak unquote
                   (app-item #:name "Brave Browser"
                             #:exec "flatpak run com.brave.Browser"
                             #:icon "com.brave.Browser"))
    (nyxt unquote
          (app-item #:name "Nyxt"
                    #:exec "nyxt"
                    #:icon "engineer.atlas.Nyxt"))
    (nyxt-flatpak unquote
                  (app-item #:name "Nyxt"
                            #:exec "flatpak run engineer.atlas.Nyxt"
                            #:icon "engineer.atlas.Nyxt"))
    (zen-browser unquote
                 (app-item #:name "Zen Browser"
                           #:exec "zen-browser"
                           #:icon "zen-browser"))
    (zen-browser-flatpak unquote
                         (app-item #:name "Zen Browser"
                                   #:exec "flatpak run app.zen_browser.zen"
                                   #:icon "app.zen_browser.zen"))
    (librewolf unquote
               (app-item #:name "LibreWolf"
                         #:exec "librewolf"
                         #:icon "librewolf"))
    (librewolf-flatpak unquote
                       (app-item #:name "LibreWolf"
                                 #:exec
                                 "flatpak run io.gitlab.librewolf-community"
                                 #:icon "io.gitlab.librewolf-community"))

    ;; Terminal Emulators
    (alacritty unquote
               (app-item #:name "Terminal (Alacritty)"
                         #:exec "alacritty"
                         #:icon "Alacritty"))
    (alacritty-flatpak unquote
                       (app-item #:name "Terminal (Alacritty)"
                                 #:exec "flatpak run org.alacritty.Alacritty"
                                 #:icon "org.alacritty.Alacritty"))
    (kitty unquote
           (app-item #:name "Kitty"
                     #:exec "kitty"
                     #:icon "kitty"))
    (kitty-flatpak unquote
                   (app-item #:name "Kitty"
                             #:exec "flatpak run org.kovidgoyal.kitty"
                             #:icon "org.kovidgoyal.kitty"))
    (foot unquote
          (app-item #:name "Foot"
                    #:exec "foot"
                    #:icon "foot"))
    (foot-flatpak unquote
                  (app-item #:name "Foot"
                            #:exec "flatpak run org.codeberg.dnkl.foot"
                            #:icon "org.codeberg.dnkl.foot"))
    (wezterm unquote
             (app-item #:name "WezTerm"
                       #:exec "wezterm"
                       #:icon "org.wezfurlong.wezterm"))
    (wezterm-flatpak unquote
                     (app-item #:name "WezTerm"
                               #:exec "flatpak run org.wezfurlong.wezterm"
                               #:icon "org.wezfurlong.wezterm"))
    (gnome-terminal unquote
                    (app-item #:name "GNOME Terminal"
                              #:exec "gnome-terminal"
                              #:icon "org.gnome.Terminal"))
    (gnome-terminal-flatpak unquote
                            (app-item #:name "GNOME Terminal"
                                      #:exec "flatpak run org.gnome.Terminal"
                                      #:icon "org.gnome.Terminal"))
    (ghostty unquote
             (app-item #:name "Ghostty"
                       #:exec "ghostty"
                       #:icon "com.mitchellh.ghostty"))
    (ghostty-flatpak unquote
                     (app-item #:name "Ghostty"
                               #:exec "flatpak run com.mitchellh.ghostty"
                               #:icon "com.mitchellh.ghostty"))

    ;; Editors & IDEs
    (emacs unquote
           (app-item #:name "Emacs"
                     #:exec "emacsclient -c"
                     #:icon "emacs"))
    (emacs-flatpak unquote
                   (app-item #:name "Emacs"
                             #:exec "flatpak run org.gnu.emacs"
                             #:icon "org.gnu.emacs"))
    (vscode unquote
            (app-item #:name "Visual Studio Code"
                      #:exec "code"
                      #:icon "code"))
    (vscode-flatpak unquote
                    (app-item #:name "Visual Studio Code"
                              #:exec "flatpak run com.visualstudio.code"
                              #:icon "com.visualstudio.code"))
    (intellij unquote
              (app-item #:name "IntelliJ IDEA"
                        #:exec "idea"
                        #:icon "idea"))
    (intellij-flatpak unquote
                      (app-item #:name "IntelliJ IDEA"
                       #:exec
                       "flatpak run com.jetbrains.IntelliJ-IDEA-Community"
                       #:icon "com.jetbrains.IntelliJ-IDEA-Community"))
    (webstorm unquote
              (app-item #:name "WebStorm"
                        #:exec "webstorm"
                        #:icon "webstorm"))
    (webstorm-flatpak unquote
                      (app-item #:name "WebStorm"
                                #:exec "flatpak run com.jetbrains.WebStorm"
                                #:icon "com.jetbrains.WebStorm"))
    (datagrip unquote
              (app-item #:name "DataGrip"
                        #:exec "datagrip"
                        #:icon "datagrip"))
    (datagrip-flatpak unquote
                      (app-item #:name "DataGrip"
                                #:exec "flatpak run com.jetbrains.DataGrip"
                                #:icon "com.jetbrains.DataGrip"))
    (bruno unquote
           (app-item #:name "Bruno"
                     #:exec "bruno"
                     #:icon "com.usebruno.Bruno"))
    (bruno-flatpak unquote
                   (app-item #:name "Bruno"
                             #:exec "flatpak run com.usebruno.Bruno"
                             #:icon "com.usebruno.Bruno"))
    (zed unquote
         (app-item #:name "Zed"
                   #:exec "zed"
                   #:icon "dev.zed.Zed"))
    (zed-flatpak unquote
                 (app-item #:name "Zed"
                           #:exec "flatpak run dev.zed.Zed"
                           #:icon "dev.zed.Zed"))
    (neovim unquote
            (app-item #:name "Neovim"
                      #:exec "alacritty -e nvim"
                      #:icon "nvim"))
    (neovim-flatpak unquote
                    (app-item #:name "Neovim"
                              #:exec "flatpak run io.neovim.nvim"
                              #:icon "io.neovim.nvim"))

    ;; System & Utilities
    (ram unquote
         (ram-item))
    (clock unquote
           (clock-item))
    (cpu unquote
         (cpu-item))
    (battery unquote
             (battery-item))
    (cpu-temp unquote
              (cpu-temp-item))
    (trash unquote
           (trash-item))
    (nautilus unquote
              (app-item #:name "Files"
                        #:exec "nautilus -w"
                        #:icon "org.gnome.Nautilus"))
    (nautilus-flatpak unquote
                      (app-item #:name "Files"
                                #:exec "flatpak run org.gnome.Nautilus"
                                #:icon "org.gnome.Nautilus"))
    (gnome-calendar unquote
                    (app-item #:name "Agenda"
                              #:exec "gnome-calendar"
                              #:icon "org.gnome.Calendar"))
    (gnome-calendar-flatpak unquote
                            (app-item #:name "Agenda"
                                      #:exec "flatpak run org.gnome.Calendar"
                                      #:icon "org.gnome.Calendar"))
    (gnome-calculator unquote
                      (app-item #:name "Calculator"
                                #:exec "gnome-calculator"
                                #:icon "org.gnome.Calculator"))
    (gnome-calculator-flatpak unquote
                              (app-item #:name "Calculator"
                                        #:exec
                                        "flatpak run org.gnome.Calculator"
                                        #:icon "org.gnome.Calculator"))
    (pavucontrol unquote
                 (app-item #:name "Audio Manager"
                           #:exec "pavucontrol"
                           #:icon "pavucontrol"))
    (pavucontrol-flatpak unquote
                         (app-item #:name "Audio Manager"
                                   #:exec
                                   "flatpak run org.pulseaudio.pavucontrol"
                                   #:icon "org.pulseaudio.pavucontrol"))
    (btop unquote
          (app-item #:name "System Monitor"
                    #:exec "alacritty -e btop"
                    #:icon "btop"))
    (btop-flatpak unquote
                  (app-item #:name "System Monitor"
                            #:exec "flatpak run io.github.aristocratos.btop"
                            #:icon "io.github.aristocratos.btop"))
    (onepassword unquote
                 (app-item #:name "1Password"
                           #:exec "1password"
                           #:icon "1password"))
    (onepassword-flatpak unquote
                         (app-item #:name "1Password"
                                   #:exec
                                   "flatpak run com.onepassword.OnePassword"
                                   #:icon "com.onepassword.OnePassword"))
    (thunderbird unquote
                 (app-item #:name "Thunderbird"
                           #:exec "thunderbird"
                           #:icon "thunderbird"))
    (thunderbird-flatpak unquote
                         (app-item #:name "Thunderbird"
                                   #:exec
                                   "flatpak run org.mozilla.Thunderbird"
                                   #:icon "org.mozilla.Thunderbird"))
    (bitwarden unquote
               (app-item #:name "Bitwarden"
                         #:exec "bitwarden"
                         #:icon "bitwarden"))
    (bitwarden-flatpak unquote
                       (app-item #:name "Bitwarden"
                                 #:exec "flatpak run com.bitwarden.desktop"
                                 #:icon "com.bitwarden.desktop"))

    ;; Media, Graphics & Messaging
    (spotify unquote
             (app-item #:name "Spotify"
                       #:exec "spotify"
                       #:icon "spotify"))
    (spotify-flatpak unquote
                     (app-item #:name "Spotify"
                               #:exec "flatpak run com.spotify.Client"
                               #:icon "com.spotify.Client"))
    (zapzap unquote
            (app-item #:name "WhatsApp"
                      #:exec "zapzap"
                      #:icon "com.rtosta.zapzap"))
    (zapzap-flatpak unquote
                    (app-item #:name "WhatsApp"
                              #:exec "flatpak run com.rtosta.zapzap"
                              #:icon "com.rtosta.zapzap"))
    (discord unquote
             (app-item #:name "Discord"
                       #:exec "discord"
                       #:icon "discord"))
    (discord-flatpak unquote
                     (app-item #:name "Discord"
                               #:exec "flatpak run com.discordapp.Discord"
                               #:icon "com.discordapp.Discord"))
    (slack unquote
           (app-item #:name "Slack"
                     #:exec "slack"
                     #:icon "com.slack.Slack"))
    (slack-flatpak unquote
                   (app-item #:name "Slack"
                             #:exec "flatpak run com.slack.Slack"
                             #:icon "com.slack.Slack"))
    (telegram unquote
              (app-item #:name "Telegram"
                        #:exec "telegram-desktop"
                        #:icon "org.telegram.desktop"))
    (telegram-flatpak unquote
                      (app-item #:name "Telegram"
                                #:exec "flatpak run org.telegram.desktop"
                                #:icon "org.telegram.desktop"))
    (gimp unquote
          (app-item #:name "GIMP"
                    #:exec "gimp"
                    #:icon "gimp"))
    (gimp-flatpak unquote
                  (app-item #:name "GIMP"
                            #:exec "flatpak run org.gimp.GIMP"
                            #:icon "org.gimp.GIMP"))
    (inkscape unquote
              (app-item #:name "Inkscape"
                        #:exec "inkscape"
                        #:icon "org.inkscape.Inkscape"))
    (inkscape-flatpak unquote
                      (app-item #:name "Inkscape"
                                #:exec "flatpak run org.inkscape.Inkscape"
                                #:icon "org.inkscape.Inkscape"))
    (obs unquote
         (app-item #:name "OBS Studio"
                   #:exec "obs"
                   #:icon "com.obsproject.Studio"))
    (obs-flatpak unquote
                 (app-item #:name "OBS Studio"
                           #:exec "flatpak run com.obsproject.Studio"
                           #:icon "com.obsproject.Studio"))
    (vlc unquote
         (app-item #:name "VLC Media Player"
                   #:exec "vlc"
                   #:icon "vlc"))
    (vlc-flatpak unquote
                 (app-item #:name "VLC Media Player"
                           #:exec "flatpak run org.videolan.VLC"
                           #:icon "org.videolan.VLC"))
    (steam unquote
           (app-item #:name "Steam"
                     #:exec "steam"
                     #:icon "steam"))
    (steam-flatpak unquote
                   (app-item #:name "Steam"
                             #:exec "flatpak run com.valvesoftware.Steam"
                             #:icon "com.valvesoftware.Steam"))
    (blender unquote
             (app-item #:name "Blender"
                       #:exec "blender"
                       #:icon "blender"))
    (blender-flatpak unquote
                     (app-item #:name "Blender"
                               #:exec "flatpak run org.blender.Blender"
                               #:icon "org.blender.Blender"))))

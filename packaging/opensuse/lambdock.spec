#
# spec file for package lambdock
#
# Copyright (c) 2026 SUSE LLC and contributors
#
# All modifications and additions to the file contributed by third parties
# remain the property of their copyright owners, unless otherwise agreed
# upon. The license for this file, and modifications and additions to the
# file, is the same license as for the pristine package itself (unless the
# license for the pristine package is not an Open Source License, in which
# case the license is the MIT License). An "Open Source License" is a
# license that conforms to the Open Source Definition (Version 1.9)
# published by the Open Source Initiative.

# Please submit bugfixes or comments via http://bugs.opensuse.org/
#

Name:           lambdock
Version:        0.13.3
Release:        0
Summary:        Wayland desktop dock customizable with GNU Guile Scheme
License:        GPL-3.0-or-later
URL:            https://codeberg.org/jjba23/lambdock
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  gcc
BuildRequires:  gettext
BuildRequires:  meson
BuildRequires:  ninja
BuildRequires:  pkgconfig
BuildRequires:  pkgconfig(glib-2.0)
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(gtk4-layer-shell-0)
BuildRequires:  pkgconfig(guile-3.0)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-protocols)
Requires:       gtk4
Requires:       guile
Requires:       hicolor-icon-theme

%description
Lambdock is a Wayland-native desktop dock built with GTK4,
gtk4-layer-shell, and GNU Guile Scheme.

It provides dynamic Wayland top-level window tracking,
multi-monitor support, configurable application launchers,
smooth auto-hide animations, GTK CSS themes, hot-reloading,
multiple dock instances, and an embedded Guile Scheme runtime
with an optional interactive REPL.

Lambdock is designed for Wayland compositors implementing the
wlr-layer-shell protocol, including Sway, Hyprland, River,
Wayfire, Niri, and other compatible compositors.

%prep
%autosetup

%build
%meson
%meson_build

%install
%meson_install
%find_lang %{name}

%check
%meson_test

%files -f %{name}.lang
%license COPYING
%doc README.org NEWS.org
%{_bindir}/lambdock
%{_datadir}/applications/org.jointhefreeworld.lambdock.desktop
%{_datadir}/metainfo/org.jointhefreeworld.lambdock.metainfo.xml
%{_datadir}/icons/hicolor/scalable/apps/org.jointhefreeworld.lambdock.svg
%dir %{_datadir}/guile
%dir %{_datadir}/guile/site
%dir %{_datadir}/guile/site/3.0
%{_datadir}/guile/site/3.0/lambdock

%changelog
* Tue Oct 27 2026 Josep Bigorra <jjbigorra@gmail.com> - 0.13.3
- The launchpad shows one tile per application again, and a tile whose
  application is installed more than once offers which copy to start
- The launchpad backdrop is denser, so its labels read over a busy desktop
* Mon Oct 26 2026 Josep Bigorra <jjbigorra@gmail.com> - 0.13.2
- Right-click menu offers browser and music player actions for those apps
* Mon Oct 26 2026 Josep Bigorra <jjbigorra@gmail.com> - 0.13.1
- Copy data/ into the container build so the image builds again
* Mon Oct 26 2026 Josep Bigorra <jjbigorra@gmail.com> - 0.13.0
- Keyboard and screen-reader accessibility with a reduce-motion setting
- Rebuilt stack fan with live thumbnails in the fan, grid and list views
- Configurable launchpad tiles, columns and frosted backdrop with a close button
- Roomier preferences window with exact-value spin buttons
